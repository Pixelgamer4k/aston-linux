// Hardware H.264/HEVC encode test for the downstream msm_vidc driver.
// Like the decoder, the encoder only accepts DMABUF memory on both queues.
// This feeds generated NV12 frames (a moving test pattern) from dma-heap
// buffers and writes the elementary stream it gets back.
// Build: gcc -O2 -o venctest venctest.c
// Usage: venctest OUT.h265 WIDTH HEIGHT FRAMES [hevc|h264] [BITRATE] [FPS]
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE      /* raw frames in */
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE     /* bitstream out */
#define MAXBUF 32
#define ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))

static int vfd, heap;
struct dbuf { int fd; size_t size; uint8_t *map; };

static int xioctl(const char *name, unsigned long req, void *arg)
{
    int r = ioctl(vfd, req, arg);
    if (r < 0)
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
    return r;
}

static struct dbuf alloc(size_t size)
{
    struct dma_heap_allocation_data d = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
    struct dbuf b = { -1, size, NULL };
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
        perror("dma-heap alloc");
        exit(1);
    }
    b.fd = d.fd;
    b.map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
    return b;
}

static int qbuf(int type, int index, struct dbuf *b, size_t used, uint64_t ts_us)
{
    struct v4l2_plane pl = { .m.fd = b->fd, .length = b->size, .bytesused = used };
    struct v4l2_buffer vb = { .type = type, .memory = V4L2_MEMORY_DMABUF, .index = index,
                              .m.planes = &pl, .length = 1 };
    vb.timestamp.tv_sec = ts_us / 1000000;
    vb.timestamp.tv_usec = ts_us % 1000000;
    return ioctl(vfd, VIDIOC_QBUF, &vb);
}

static void set_ctrl(const char *name, uint32_t id, int value)
{
    struct v4l2_control c = { .id = id, .value = value };
    if (ioctl(vfd, VIDIOC_S_CTRL, &c) < 0)
        fprintf(stderr, "control %s: %s\n", name, strerror(errno));
}

// Moving diagonal bars: enough motion and detail to make the encoder work.
static void draw(uint8_t *p, int w, int h, int stride, int scan, int n)
{
    for (int y = 0; y < h; y++) {
        uint8_t *row = p + (size_t)y * stride;
        for (int x = 0; x < w; x++)
            row[x] = (uint8_t)(((x + y + n * 6) >> 3) * 37 + (x ^ y));
    }
    uint8_t *uv = p + (size_t)stride * scan;
    for (int y = 0; y < h / 2; y++) {
        uint8_t *row = uv + (size_t)y * stride;
        for (int x = 0; x < w; x += 2) {
            row[x] = (uint8_t)(128 + 90 * ((x + n * 4) % 512 < 256 ? 1 : -1));
            row[x + 1] = (uint8_t)(128 + ((y * 2 + n * 3) & 127) - 64);
        }
    }
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s OUT WIDTH HEIGHT FRAMES [hevc|h264] [BITRATE] [FPS]\n", argv[0]);
        return 1;
    }
    int w = atoi(argv[2]), h = atoi(argv[3]), total = atoi(argv[4]);
    uint32_t codec = (argc > 5 && !strcmp(argv[5], "h264")) ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_HEVC;
    int bitrate = argc > 6 ? atoi(argv[6]) : 12000000;
    int fps = argc > 7 ? atoi(argv[7]) : 60;
    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror(argv[1]); return 1; }

    vfd = open("/dev/video33", O_RDWR | O_NONBLOCK);
    heap = open("/dev/dma_heap/qcom,system", O_RDWR);
    if (heap < 0)
        heap = open("/dev/dma_heap/system", O_RDWR);
    if (vfd < 0 || heap < 0) { perror("open device"); return 1; }

    struct v4l2_format cf = { .type = CAP };
    cf.fmt.pix_mp.pixelformat = codec;
    cf.fmt.pix_mp.width = w;
    cf.fmt.pix_mp.height = h;
    cf.fmt.pix_mp.num_planes = 1;
    if (xioctl("S_FMT bitstream", VIDIOC_S_FMT, &cf)) return 1;

    struct v4l2_format of = { .type = OUT };
    of.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    of.fmt.pix_mp.width = w;
    of.fmt.pix_mp.height = h;
    of.fmt.pix_mp.num_planes = 1;
    if (xioctl("S_FMT raw", VIDIOC_S_FMT, &of)) return 1;
    xioctl("G_FMT bitstream", VIDIOC_G_FMT, &cf);
    int stride = of.fmt.pix_mp.plane_fmt[0].bytesperline ? of.fmt.pix_mp.plane_fmt[0].bytesperline : ALIGN(w, 128);
    int scan = ALIGN(h, 32);
    size_t raw_size = of.fmt.pix_mp.plane_fmt[0].sizeimage, bs_size = cf.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("raw %ux%u stride %d, %zu bytes/frame; bitstream buffers %zu bytes\n",
           of.fmt.pix_mp.width, of.fmt.pix_mp.height, stride, raw_size, bs_size);

    struct v4l2_streamparm parm = { .type = OUT };
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = fps;
    xioctl("S_PARM", VIDIOC_S_PARM, &parm);
    set_ctrl("bitrate", V4L2_CID_MPEG_VIDEO_BITRATE, bitrate);
    set_ctrl("gop", V4L2_CID_MPEG_VIDEO_GOP_SIZE, fps * 2);
    set_ctrl("prepend headers", V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1);

    struct v4l2_requestbuffers rb = { .count = 8, .type = OUT, .memory = V4L2_MEMORY_DMABUF };
    if (xioctl("REQBUFS raw", VIDIOC_REQBUFS, &rb)) return 1;
    int nraw = rb.count > MAXBUF ? MAXBUF : rb.count;
    struct v4l2_requestbuffers crb = { .count = 8, .type = CAP, .memory = V4L2_MEMORY_DMABUF };
    if (xioctl("REQBUFS bitstream", VIDIOC_REQBUFS, &crb)) return 1;
    int nbs = crb.count > MAXBUF ? MAXBUF : crb.count;

    struct dbuf raw[MAXBUF], bs[MAXBUF];
    int rfree[MAXBUF], nfree = 0;
    for (int i = 0; i < nraw; i++) { raw[i] = alloc(raw_size); rfree[nfree++] = i; }
    for (int i = 0; i < nbs; i++) bs[i] = alloc(bs_size);

    int t = OUT;
    if (xioctl("STREAMON raw", VIDIOC_STREAMON, &t)) return 1;
    t = CAP;
    if (xioctl("STREAMON bitstream", VIDIOC_STREAMON, &t)) return 1;
    for (int i = 0; i < nbs; i++)
        if (qbuf(CAP, i, &bs[i], 0, 0)) perror("QBUF bitstream");

    int fed = 0, got = 0, stopped = 0, done = 0;
    size_t bytes = 0;
    double t0 = now(), last = t0, draw_time = 0;
    while (!done && now() - last < 5.0) {
        while (nfree && fed < total) {
            int i = rfree[--nfree];
            double d0 = now();
            draw(raw[i].map, w, h, stride, scan, fed);
            draw_time += now() - d0;
            if (qbuf(OUT, i, &raw[i], raw_size, (uint64_t)fed * 1000000 / fps)) { perror("QBUF raw"); return 1; }
            fed++;
        }
        if (fed >= total && !stopped) {
            struct v4l2_encoder_cmd ec = { .cmd = V4L2_ENC_CMD_STOP };
            xioctl("encoder stop", VIDIOC_ENCODER_CMD, &ec);
            stopped = 1;
        }
        struct pollfd p = { .fd = vfd, .events = POLLIN | POLLOUT | POLLPRI };
        if (poll(&p, 1, 500) <= 0)
            continue;
        if (p.revents & POLLOUT) {
            struct v4l2_plane pl = { 0 };
            struct v4l2_buffer vb = { .type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
            while (ioctl(vfd, VIDIOC_DQBUF, &vb) == 0)
                rfree[nfree++] = vb.index;
        }
        if (p.revents & POLLIN) {
            struct v4l2_plane pl = { 0 };
            struct v4l2_buffer vb = { .type = CAP, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
            while (ioctl(vfd, VIDIOC_DQBUF, &vb) == 0) {
                if (pl.bytesused) {
                    fwrite(bs[vb.index].map + pl.data_offset, 1, pl.bytesused, out);
                    bytes += pl.bytesused;
                    got++;
                    last = now();
                }
                if (vb.flags & V4L2_BUF_FLAG_LAST) { done = 1; break; }
                qbuf(CAP, vb.index, &bs[vb.index], 0, 0);
            }
        }
        if (p.revents & POLLERR) { fprintf(stderr, "device reported an error\n"); break; }
    }
    double secs = last - t0;
    fclose(out);
    printf("fed %d frames, got %d packets, %zu bytes in %.2f s (%.0f fps; %.2f s of that drawing test frames)\n",
           fed, got, bytes, secs, secs > 0 ? got / secs : 0, draw_time);
    return got > 0 ? 0 : 2;
}
