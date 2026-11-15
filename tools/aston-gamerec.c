// aston-gamerec: record what Gamescope shows with the phone's hardware encoder.
//
// Gamescope publishes its composited picture as a PipeWire video stream and can
// hand it over already converted to NV12 on the GPU. This program takes that
// stream and feeds it to the Qualcomm video encoder (msm_vidc), which only
// accepts DMABUF memory and so cannot be driven by stock ffmpeg or GStreamer.
// Frames are encoded at a constant rate (the newest frame is repeated when the
// picture has not changed), so the raw stream can be muxed with a fixed rate.
//
// Build: gcc -O2 -o aston-gamerec aston-gamerec.c $(pkg-config --cflags --libs libpipewire-0.3)
// Usage: aston-gamerec OUT.h265 [WIDTH HEIGHT [FPS [BITRATE [hevc|h264]]]]
// Stops cleanly on SIGINT or SIGTERM.
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE      /* raw frames in */
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE     /* bitstream out */
#define MAXBUF 16
#define ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))
/* Gamescope's own format property: the size the consumer would like. */
#define SPA_FORMAT_VIDEO_requested_size 0x70000

struct dbuf { int fd; size_t size; uint8_t *map; };

static struct {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_hook listener;
    struct spa_source *timer;
    struct spa_video_info_raw info;

    const char *path;
    FILE *out;
    uint32_t want_w, want_h, codec;
    int fps, bitrate;

    /* newest frame from Gamescope, tightly packed NV12 */
    uint8_t *latest;
    int have_frame;

    /* encoder */
    int vfd, heap, ready, stopping;
    int stride, scan;
    size_t raw_size, bs_size;
    struct dbuf raw[MAXBUF], bs[MAXBUF];
    int nraw, nbs, rfree[MAXBUF], nfree;
    uint64_t fed, packets, bytes, repeated, dropped;
} g = { .vfd = -1, .heap = -1 };

static struct dbuf alloc(size_t size)
{
    struct dma_heap_allocation_data d = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
    struct dbuf b = { -1, size, NULL };
    if (ioctl(g.heap, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
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
    return ioctl(g.vfd, VIDIOC_QBUF, &vb);
}

static void set_ctrl(uint32_t id, int value)
{
    struct v4l2_control c = { .id = id, .value = value };
    ioctl(g.vfd, VIDIOC_S_CTRL, &c);
}

static int encoder_start(uint32_t w, uint32_t h)
{
    g.vfd = open("/dev/video33", O_RDWR | O_NONBLOCK);
    g.heap = open("/dev/dma_heap/qcom,system", O_RDWR);
    if (g.heap < 0)
        g.heap = open("/dev/dma_heap/system", O_RDWR);
    if (g.vfd < 0 || g.heap < 0) {
        perror("open encoder");
        return -1;
    }
    struct v4l2_format cf = { .type = CAP };
    cf.fmt.pix_mp.pixelformat = g.codec;
    cf.fmt.pix_mp.width = w;
    cf.fmt.pix_mp.height = h;
    cf.fmt.pix_mp.num_planes = 1;
    struct v4l2_format of = { .type = OUT };
    of.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    of.fmt.pix_mp.width = w;
    of.fmt.pix_mp.height = h;
    of.fmt.pix_mp.num_planes = 1;
    if (ioctl(g.vfd, VIDIOC_S_FMT, &cf) || ioctl(g.vfd, VIDIOC_S_FMT, &of) || ioctl(g.vfd, VIDIOC_G_FMT, &cf)) {
        perror("encoder format");
        return -1;
    }
    g.stride = of.fmt.pix_mp.plane_fmt[0].bytesperline ? of.fmt.pix_mp.plane_fmt[0].bytesperline : ALIGN(w, 128);
    g.scan = ALIGN(h, 32);
    g.raw_size = of.fmt.pix_mp.plane_fmt[0].sizeimage;
    g.bs_size = cf.fmt.pix_mp.plane_fmt[0].sizeimage;

    struct v4l2_streamparm parm = { .type = OUT };
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = g.fps;
    ioctl(g.vfd, VIDIOC_S_PARM, &parm);
    set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, g.bitrate);
    set_ctrl(V4L2_CID_MPEG_VIDEO_GOP_SIZE, g.fps * 2);
    set_ctrl(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1);

    struct v4l2_requestbuffers rb = { .count = 6, .type = OUT, .memory = V4L2_MEMORY_DMABUF };
    struct v4l2_requestbuffers crb = { .count = 6, .type = CAP, .memory = V4L2_MEMORY_DMABUF };
    if (ioctl(g.vfd, VIDIOC_REQBUFS, &rb) || ioctl(g.vfd, VIDIOC_REQBUFS, &crb)) {
        perror("encoder buffers");
        return -1;
    }
    g.nraw = rb.count > MAXBUF ? MAXBUF : rb.count;
    g.nbs = crb.count > MAXBUF ? MAXBUF : crb.count;
    for (int i = 0; i < g.nraw; i++) { g.raw[i] = alloc(g.raw_size); g.rfree[g.nfree++] = i; }
    for (int i = 0; i < g.nbs; i++) g.bs[i] = alloc(g.bs_size);
    int t = OUT;
    if (ioctl(g.vfd, VIDIOC_STREAMON, &t)) { perror("encoder start (raw)"); return -1; }
    t = CAP;
    if (ioctl(g.vfd, VIDIOC_STREAMON, &t)) { perror("encoder start (bitstream)"); return -1; }
    for (int i = 0; i < g.nbs; i++)
        qbuf(CAP, i, &g.bs[i], 0, 0);
    g.ready = 1;
    fprintf(stderr, "recording %ux%u at %d fps, %d kbit/s %s -> %s\n", w, h, g.fps, g.bitrate / 1000,
            g.codec == V4L2_PIX_FMT_HEVC ? "H.265" : "H.264", g.path);
    return 0;
}

/* Collect finished packets and used raw buffers. Returns 1 once the last packet is out. */
static int encoder_drain(void)
{
    int last = 0;
    struct v4l2_plane pl = { 0 };
    struct v4l2_buffer vb = { .type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
    while (ioctl(g.vfd, VIDIOC_DQBUF, &vb) == 0)
        g.rfree[g.nfree++] = vb.index;
    memset(&pl, 0, sizeof(pl));
    struct v4l2_buffer cb = { .type = CAP, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
    while (ioctl(g.vfd, VIDIOC_DQBUF, &cb) == 0) {
        if (pl.bytesused) {
            fwrite(g.bs[cb.index].map + pl.data_offset, 1, pl.bytesused, g.out);
            g.bytes += pl.bytesused;
            g.packets++;
        }
        if (cb.flags & V4L2_BUF_FLAG_LAST)
            last = 1;
        else
            qbuf(CAP, cb.index, &g.bs[cb.index], 0, 0);
    }
    return last;
}

static void encode_latest(void)
{
    uint32_t w = g.info.size.width, h = g.info.size.height;
    if (!g.nfree) {
        g.dropped++;
        return;
    }
    int i = g.rfree[--g.nfree];
    uint8_t *dst = g.raw[i].map;
    for (uint32_t y = 0; y < h; y++)
        memcpy(dst + (size_t)y * g.stride, g.latest + (size_t)y * w, w);
    uint8_t *duv = dst + (size_t)g.stride * g.scan, *suv = g.latest + (size_t)w * h;
    for (uint32_t y = 0; y < (h + 1) / 2; y++)
        memcpy(duv + (size_t)y * g.stride, suv + (size_t)y * w, w);
    if (qbuf(OUT, i, &g.raw[i], g.raw_size, g.fed * 1000000 / g.fps) == 0)
        g.fed++;
    else
        g.rfree[g.nfree++] = i;
}

static void on_timer(void *data, uint64_t expirations)
{
    if (g.stopping || !g.have_frame)
        return;
    if (!g.ready && encoder_start(g.info.size.width, g.info.size.height) < 0) {
        pw_main_loop_quit(g.loop);
        return;
    }
    encoder_drain();
    /* One frame per tick; if ticks were missed, repeat to keep the rate constant. */
    for (uint64_t n = 0; n < expirations && n < 4; n++) {
        encode_latest();
        if (n)
            g.repeated++;
    }
}

static void on_process(void *data)
{
    struct pw_buffer *b = NULL, *next;
    /* Only the newest queued frame matters. */
    while ((next = pw_stream_dequeue_buffer(g.stream))) {
        if (b)
            pw_stream_queue_buffer(g.stream, b);
        b = next;
    }
    if (!b)
        return;
    struct spa_data *d = &b->buffer->datas[0];
    if (g.latest && d->data && d->chunk && !(d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
        uint32_t w = g.info.size.width, h = g.info.size.height;
        uint32_t stride = d->chunk->stride > 0 ? (uint32_t)d->chunk->stride : w;
        const uint8_t *src = (const uint8_t *)d->data + d->chunk->offset;
        if ((size_t)stride * (h + (h + 1) / 2) <= d->maxsize) {
            for (uint32_t y = 0; y < h; y++)
                memcpy(g.latest + (size_t)y * w, src + (size_t)y * stride, w);
            for (uint32_t y = 0; y < (h + 1) / 2; y++)
                memcpy(g.latest + (size_t)w * h + (size_t)y * w, src + (size_t)stride * h + (size_t)y * stride, w);
            g.have_frame = 1;
        }
    }
    pw_stream_queue_buffer(g.stream, b);
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    if (!param || id != SPA_PARAM_Format)
        return;
    if (spa_format_video_raw_parse(param, &g.info) < 0 || g.info.format != SPA_VIDEO_FORMAT_NV12) {
        fprintf(stderr, "Gamescope did not offer NV12\n");
        pw_main_loop_quit(g.loop);
        return;
    }
    uint32_t w = g.info.size.width, h = g.info.size.height;
    /* Gamescope first offers its full size and then renegotiates to the size
     * asked for, so the encoder is only started when the first frame arrives
     * (see on_timer). A change after that would break the recording. */
    if (g.ready) {
        fprintf(stderr, "stream size changed to %ux%u; stopping\n", w, h);
        pw_main_loop_quit(g.loop);
        return;
    }
    free(g.latest);
    g.have_frame = 0;
    g.latest = calloc(1, (size_t)w * (h + (h + 1) / 2));
    if (!g.latest) {
        pw_main_loop_quit(g.loop);
        return;
    }
    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    pw_stream_update_params(g.stream, params, 1);

    struct timespec interval = { 0, 1000000000L / g.fps }, start = { 0, 1 };
    pw_loop_update_timer(pw_main_loop_get_loop(g.loop), g.timer, &start, &interval, false);
}

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "stream error: %s\n", error ? error : "?");
        pw_main_loop_quit(g.loop);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

static void on_signal(void *data, int sig)
{
    pw_main_loop_quit(g.loop);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s OUT.h265 [WIDTH HEIGHT [FPS [BITRATE [hevc|h264]]]]\n", argv[0]);
        return 1;
    }
    g.path = argv[1];
    g.want_w = argc > 3 ? atoi(argv[2]) : 1920;
    g.want_h = argc > 3 ? atoi(argv[3]) : 872;
    g.fps = argc > 4 ? atoi(argv[4]) : 60;
    g.bitrate = argc > 5 ? atoi(argv[5]) : 12000000;
    g.codec = (argc > 6 && !strcmp(argv[6], "h264")) ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_HEVC;
    g.out = fopen(g.path, "wb");
    if (!g.out) {
        perror(g.path);
        return 1;
    }

    pw_init(&argc, &argv);
    g.loop = pw_main_loop_new(NULL);
    struct pw_loop *l = pw_main_loop_get_loop(g.loop);
    pw_loop_add_signal(l, SIGINT, on_signal, NULL);
    pw_loop_add_signal(l, SIGTERM, on_signal, NULL);
    g.timer = pw_loop_add_timer(l, on_timer, NULL);

    const char *target = getenv("ASTON_GAMEREC_NODE");
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_TARGET_OBJECT, target ? target : "gamescope-drm", NULL);
    g.stream = pw_stream_new_simple(l, "aston-gamerec", props, &stream_events, NULL);

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_rectangle def = SPA_RECTANGLE(g.want_w, g.want_h), min = SPA_RECTANGLE(16, 16), max = SPA_RECTANGLE(8192, 8192);
    struct spa_fraction fdef = SPA_FRACTION(0, 1), fmin = SPA_FRACTION(0, 1), fmax = SPA_FRACTION(1000, 1);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def, &min, &max),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&fdef, &fmin, &fmax),
        SPA_FORMAT_VIDEO_requested_size, SPA_POD_Rectangle(&def),
        SPA_FORMAT_VIDEO_colorMatrix, SPA_POD_Id(SPA_VIDEO_COLOR_MATRIX_BT709),
        SPA_FORMAT_VIDEO_colorRange, SPA_POD_Id(SPA_VIDEO_COLOR_RANGE_16_235));
    if (pw_stream_connect(g.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
        fprintf(stderr, "cannot connect to PipeWire\n");
        return 1;
    }
    pw_main_loop_run(g.loop);

    /* Finish: tell the encoder to stop and collect what is still inside it. */
    if (g.ready) {
        g.stopping = 1;
        struct v4l2_encoder_cmd ec = { .cmd = V4L2_ENC_CMD_STOP };
        ioctl(g.vfd, VIDIOC_ENCODER_CMD, &ec);
        for (int i = 0; i < 100 && !encoder_drain(); i++)
            usleep(10000);
    }
    fclose(g.out);
    fprintf(stderr, "frames %llu, packets %llu, %llu bytes, repeated %llu, dropped %llu\n",
            (unsigned long long)g.fed, (unsigned long long)g.packets, (unsigned long long)g.bytes,
            (unsigned long long)g.repeated, (unsigned long long)g.dropped);
    pw_stream_destroy(g.stream);
    pw_main_loop_destroy(g.loop);
    pw_deinit();
    return g.packets ? 0 : 2;
}
