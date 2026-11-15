// Hardware H.264 decode test for the downstream msm_vidc driver.
// The driver only accepts DMABUF memory on both queues, which is why stock
// ffmpeg/GStreamer V4L2 decoders fail on it.  This feeds an Annex-B stream
// (one access unit per buffer, split on AUD NALs) using dma-heap buffers and
// reports how many frames come out and how fast.
// Build: gcc -O2 -o vdectest vdectest.c
// Usage: vdectest FILE.h264 WIDTH HEIGHT
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define NOUT 4
#define MAXCAP 32

static int vfd, heap;
struct dbuf { int fd; size_t size; void *map; };

static int xioctl(const char *name, unsigned long req, void *arg)
{
    int r = ioctl(vfd, req, arg);
    if (r < 0)
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
    return r;
}

static struct dbuf alloc(size_t size, int map)
{
    struct dma_heap_allocation_data d = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
    struct dbuf b = { -1, size, NULL };
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
        perror("dma-heap alloc");
        exit(1);
    }
    b.fd = d.fd;
    if (map)
        b.map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
    return b;
}

static int qbuf(int type, int index, struct dbuf *b, size_t used, uint64_t ts, uint32_t flags)
{
    struct v4l2_plane pl = { .m.fd = b->fd, .length = b->size, .bytesused = used };
    struct v4l2_buffer vb = { .type = type, .memory = V4L2_MEMORY_DMABUF, .index = index,
                              .m.planes = &pl, .length = 1, .flags = flags };
    vb.timestamp.tv_sec = ts / 1000000;
    vb.timestamp.tv_usec = ts % 1000000;
    return ioctl(vfd, VIDIOC_QBUF, &vb);
}

// Next access unit: from `pos` up to the next AUD NAL (type 9) start code.
static size_t next_au(const uint8_t *d, size_t len, size_t pos)
{
    for (size_t i = pos + 4; i + 4 < len; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1 && (d[i + 4] & 0x1f) == 9)
            return i;
    return len;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s FILE.h264 WIDTH HEIGHT\n", argv[0]);
        return 1;
    }
    int w = atoi(argv[2]), h = atoi(argv[3]);
    int in = open(argv[1], O_RDONLY);
    struct stat st;
    if (in < 0 || fstat(in, &st)) { perror(argv[1]); return 1; }
    const uint8_t *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, in, 0);
    size_t len = st.st_size, pos = 0;

    vfd = open("/dev/video32", O_RDWR | O_NONBLOCK);
    heap = open("/dev/dma_heap/qcom,system", O_RDWR);
    if (heap < 0)
        heap = open("/dev/dma_heap/system", O_RDWR);
    if (vfd < 0 || heap < 0) { perror("open device"); return 1; }

    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    xioctl("subscribe source change", VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = V4L2_EVENT_EOS;
    xioctl("subscribe eos", VIDIOC_SUBSCRIBE_EVENT, &sub);

    struct v4l2_format f = { .type = OUT };
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.num_planes = 1;
    if (xioctl("S_FMT output", VIDIOC_S_FMT, &f)) return 1;
    size_t out_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;

    struct v4l2_format cf = { .type = CAP };
    cf.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    cf.fmt.pix_mp.width = w;
    cf.fmt.pix_mp.height = h;
    cf.fmt.pix_mp.num_planes = 1;
    xioctl("S_FMT capture", VIDIOC_S_FMT, &cf);

    struct v4l2_requestbuffers rb = { .count = NOUT, .type = OUT, .memory = V4L2_MEMORY_DMABUF };
    if (xioctl("REQBUFS output", VIDIOC_REQBUFS, &rb)) return 1;
    struct dbuf ob[NOUT];
    int ofree[NOUT], nfree = 0;
    for (int i = 0; i < NOUT; i++) { ob[i] = alloc(out_size, 1); ofree[nfree++] = i; }
    printf("output: %d buffers of %zu bytes\n", rb.count, out_size);

    int t = OUT;
    if (xioctl("STREAMON output", VIDIOC_STREAMON, &t)) return 1;

    struct dbuf cb[MAXCAP];
    int ncap = 0, cap_on = 0, frames = 0, fed = 0, eos_sent = 0, done = 0;
    double t0 = now(), last = t0;

    while (!done && now() - last < 5.0) {
        // Feed bitstream while output buffers are free.
        while (nfree && pos < len) {
            size_t end = next_au(data, len, pos), n = end - pos;
            int i = ofree[--nfree];
            if (n > out_size) { fprintf(stderr, "access unit too large (%zu)\n", n); return 1; }
            memcpy(ob[i].map, data + pos, n);
            if (qbuf(OUT, i, &ob[i], n, (uint64_t)fed * 33333, 0)) { perror("QBUF output"); return 1; }
            pos = end;
            fed++;
        }
        if (pos >= len && !eos_sent) {
            struct v4l2_decoder_cmd dc = { .cmd = V4L2_DEC_CMD_STOP };
            xioctl("decoder stop", VIDIOC_DECODER_CMD, &dc);
            eos_sent = 1;
        }

        struct pollfd p = { .fd = vfd, .events = POLLIN | POLLOUT | POLLPRI };
        if (poll(&p, 1, 500) <= 0)
            continue;

        if (p.revents & POLLPRI) {
            struct v4l2_event ev;
            while (ioctl(vfd, VIDIOC_DQEVENT, &ev) == 0) {
                if (ev.type == V4L2_EVENT_EOS) { done = 1; break; }
                if (ev.type != V4L2_EVENT_SOURCE_CHANGE)
                    continue;
                // Stream parameters are known: (re)build the capture queue.
                t = CAP;
                if (cap_on) { ioctl(vfd, VIDIOC_STREAMOFF, &t); cap_on = 0; }
                struct v4l2_format g = { .type = CAP };
                xioctl("G_FMT capture", VIDIOC_G_FMT, &g);
                struct v4l2_control mc = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
                ioctl(vfd, VIDIOC_G_CTRL, &mc);
                int want = mc.value > 0 ? mc.value + 4 : 12;
                if (want > MAXCAP) want = MAXCAP;
                struct v4l2_requestbuffers crb = { .count = want, .type = CAP, .memory = V4L2_MEMORY_DMABUF };
                if (xioctl("REQBUFS capture", VIDIOC_REQBUFS, &crb)) return 1;
                for (int i = 0; i < ncap; i++) close(cb[i].fd);
                ncap = crb.count;
                size_t csz = g.fmt.pix_mp.plane_fmt[0].sizeimage;
                printf("capture: %ux%u fourcc %.4s, %d buffers of %zu bytes (driver minimum %d)\n",
                       g.fmt.pix_mp.width, g.fmt.pix_mp.height, (char *)&g.fmt.pix_mp.pixelformat, ncap, csz, mc.value);
                for (int i = 0; i < ncap; i++) {
                    cb[i] = alloc(csz, 0);
                    if (qbuf(CAP, i, &cb[i], 0, 0, 0)) perror("QBUF capture");
                }
                if (xioctl("STREAMON capture", VIDIOC_STREAMON, &t)) return 1;
                cap_on = 1;
            }
        }
        if (p.revents & POLLOUT) {
            struct v4l2_plane pl = { 0 };
            struct v4l2_buffer vb = { .type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
            while (ioctl(vfd, VIDIOC_DQBUF, &vb) == 0)
                ofree[nfree++] = vb.index;
        }
        if (p.revents & POLLIN) {
            struct v4l2_plane pl = { 0 };
            struct v4l2_buffer vb = { .type = CAP, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
            while (ioctl(vfd, VIDIOC_DQBUF, &vb) == 0) {
                if (pl.bytesused) { frames++; last = now(); }
                if (vb.flags & V4L2_BUF_FLAG_LAST) { done = 1; break; }
                qbuf(CAP, vb.index, &cb[vb.index], 0, 0, 0);
            }
        }
        if (p.revents & POLLERR) { fprintf(stderr, "device reported an error\n"); break; }
    }
    double secs = last - t0;
    printf("fed %d access units, decoded %d frames in %.2f s (%.0f fps)\n", fed, frames, secs, secs > 0 ? frames / secs : 0);
    return frames > 0 ? 0 : 2;
}
