// Atomic KMS test for the aston panel (downstream msm_drm / SDE).
// Does one atomic modeset on the DSI connector, then page-flips between two
// dumb buffers with a moving bar, and reports the achieved frame rate.
// Build: gcc -O2 -o atomictest atomictest.c $(pkg-config --cflags --libs libdrm)
// Usage: atomictest [refresh_hz] [frames]
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct buf {
    uint32_t handle, fb, pitch;
    uint64_t size;
    uint32_t *map;
};

static int fd;

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; props && i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && !strcmp(p->name, name))
            id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    if (!id)
        fprintf(stderr, "missing property %s on object %u\n", name, obj);
    return id;
}

static int make_buf(struct buf *b, uint32_t w, uint32_t h)
{
    struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c))
        return -errno;
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = c.size;
    uint32_t handles[4] = { c.handle }, pitches[4] = { c.pitch }, offsets[4] = { 0 };
    if (drmModeAddFB2(fd, w, h, 0x34325258 /* XR24 */, handles, pitches, offsets, &b->fb, 0))
        return -errno;
    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
        return -errno;
    b->map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    return b->map == MAP_FAILED ? -errno : 0;
}

static void paint(struct buf *b, uint32_t w, uint32_t h, int frame)
{
    uint32_t bar = (frame * 12) % h;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)b->map + (size_t)y * b->pitch);
        uint32_t base = 0xff101820 | ((y * 96 / h) << 16);
        uint32_t c = (y >= bar && y < bar + 80) ? 0xff30c060 : base;
        for (uint32_t x = 0; x < w; x++)
            row[x] = c;
    }
}

static int flipped;
static void flip_handler(int f, unsigned seq, unsigned s, unsigned us, void *d)
{
    (void)f; (void)seq; (void)s; (void)us; (void)d;
    flipped = 1;
}

int main(int argc, char **argv)
{
    int want_hz = argc > 1 ? atoi(argv[1]) : 60;
    int frames = argc > 2 ? atoi(argv[2]) : 240;

    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card0"); return 1; }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        perror("atomic cap");
        return 1;
    }

    drmModeRes *res = drmModeGetResources(fd);
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c && c->connector_type == DRM_MODE_CONNECTOR_DSI && c->connection == DRM_MODE_CONNECTED) {
            conn = c;
            break;
        }
        drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "no DSI connector\n"); return 1; }

    drmModeModeInfo *mode = NULL;
    for (int i = 0; i < conn->count_modes; i++)
        if ((int)conn->modes[i].vrefresh == want_hz) { mode = &conn->modes[i]; break; }
    if (!mode) mode = &conn->modes[0];
    uint32_t w = mode->hdisplay, h = mode->vdisplay;

    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[0]);
    uint32_t crtc = 0;
    int crtc_idx = -1;
    for (int i = 0; i < res->count_crtcs; i++)
        if (enc->possible_crtcs & (1u << i)) { crtc = res->crtcs[i]; crtc_idx = i; break; }

    drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
    uint32_t plane = 0;
    for (uint32_t i = 0; i < pres->count_planes && !plane; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pres->planes[i]);
        if (p && (p->possible_crtcs & (1u << crtc_idx)))
            plane = p->plane_id;
        drmModeFreePlane(p);
    }
    printf("connector %u crtc %u plane %u mode %ux%u@%u\n", conn->connector_id, crtc, plane, w, h, mode->vrefresh);

    struct buf bufs[2];
    for (int i = 0; i < 2; i++) {
        int r = make_buf(&bufs[i], w, h);
        if (r) { fprintf(stderr, "buffer %d: %s\n", i, strerror(-r)); return 1; }
        paint(&bufs[i], w, h, i);
    }

    uint32_t blob = 0;
    drmModeCreatePropertyBlob(fd, mode, sizeof(*mode), &blob);

    uint32_t p_fb = prop_id(plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, conn->connector_id, prop_id(conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), crtc);
    drmModeAtomicAddProperty(req, crtc, prop_id(crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), blob);
    drmModeAtomicAddProperty(req, crtc, prop_id(crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
    drmModeAtomicAddProperty(req, plane, p_fb, bufs[0].fb);
    drmModeAtomicAddProperty(req, plane, prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), crtc);
    drmModeAtomicAddProperty(req, plane, prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_X"), 0);
    drmModeAtomicAddProperty(req, plane, prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_Y"), 0);
    drmModeAtomicAddProperty(req, plane, prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_W"), (uint64_t)w << 16);
    drmModeAtomicAddProperty(req, plane, prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_H"), (uint64_t)h << 16);
    drmModeAtomicAddProperty(req, plane, prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_X"), 0);
    drmModeAtomicAddProperty(req, plane, prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y"), 0);
    drmModeAtomicAddProperty(req, plane, prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_W"), w);
    drmModeAtomicAddProperty(req, plane, prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_H"), h);

    int r = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    printf("atomic TEST_ONLY modeset: %s\n", r ? strerror(-r) : "ok");
    r = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    printf("atomic modeset commit:    %s\n", r ? strerror(-r) : "ok");
    drmModeAtomicFree(req);
    if (r) return 2;

    drmEventContext ev = { .version = 2, .page_flip_handler = flip_handler };
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int done = 0, fails = 0;
    for (int f = 1; f <= frames; f++) {
        struct buf *b = &bufs[f & 1];
        paint(b, w, h, f);
        req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, plane, p_fb, b->fb);
        r = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL);
        drmModeAtomicFree(req);
        if (r) {
            if (!fails++) printf("flip commit failed: %s\n", strerror(-r));
            usleep(2000);
            continue;
        }
        flipped = 0;
        while (!flipped) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            if (poll(&pfd, 1, 1000) <= 0) { printf("flip event timeout at frame %d\n", f); goto out; }
            drmHandleEvent(fd, &ev);
        }
        done++;
    }
out:
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("flips: %d ok, %d failed, %.2f s, %.1f fps (includes CPU repaint)\n", done, fails, secs, done / secs);
    return 0;
}
