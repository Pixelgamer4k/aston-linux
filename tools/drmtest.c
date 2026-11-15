/* Paint a test pattern on the first connected DRM connector using a dumb buffer. */
#include <drm.h>
#include <drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

int main(int argc, char **argv)
{
    int hold = argc > 1 ? atoi(argv[1]) : 30;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    if (xioctl(fd, DRM_IOCTL_SET_MASTER, 0)) perror("set master");

    struct drm_mode_card_res res = {0};
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("getres"); return 1; }
    uint32_t *conns = calloc(res.count_connectors, 4);
    uint32_t *crtcs = calloc(res.count_crtcs, 4);
    uint32_t *encs = calloc(res.count_encoders, 4);
    uint32_t *fbs = calloc(res.count_fbs + 1, 4);
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = (uintptr_t)fbs;
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("getres2"); return 1; }

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector c = {.connector_id = conns[i]};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
        if (c.connection != 1 || c.count_modes == 0 || c.connector_type != DRM_MODE_CONNECTOR_DSI) continue;
        struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof(*modes));
        uint32_t *cencs = calloc(c.count_encoders, 4);
        uint32_t *props = calloc(c.count_props, 4);
        uint64_t *pvals = calloc(c.count_props, 8);
        c.modes_ptr = (uintptr_t)modes;
        c.encoders_ptr = (uintptr_t)cencs;
        c.props_ptr = (uintptr_t)props;
        c.prop_values_ptr = (uintptr_t)pvals;
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;

        struct drm_mode_modeinfo *m = &modes[0];
        for (uint32_t k = 0; k < c.count_modes; k++)
            if (modes[k].vrefresh == 60) { m = &modes[k]; break; }
        printf("connector %u mode %s %ux%u@%u\n", c.connector_id, m->name, m->hdisplay, m->vdisplay, m->vrefresh);

        uint32_t crtc_id = 0;
        for (uint32_t e = 0; e < c.count_encoders && !crtc_id; e++) {
            struct drm_mode_get_encoder enc = {.encoder_id = cencs[e]};
            if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc)) continue;
            if (enc.crtc_id) { crtc_id = enc.crtc_id; break; }
            for (uint32_t k = 0; k < res.count_crtcs; k++)
                if (enc.possible_crtcs & (1u << k)) { crtc_id = crtcs[k]; break; }
        }
        if (!crtc_id) { fprintf(stderr, "no crtc\n"); return 1; }

        struct drm_mode_create_dumb cd = {.width = m->hdisplay, .height = m->vdisplay, .bpp = 32};
        if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create dumb"); return 1; }
        struct drm_mode_fb_cmd fbc = {.width = cd.width, .height = cd.height, .pitch = cd.pitch,
                                      .bpp = 32, .depth = 24, .handle = cd.handle};
        if (xioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbc)) { perror("addfb"); return 1; }
        struct drm_mode_map_dumb md = {.handle = cd.handle};
        if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map dumb"); return 1; }
        uint8_t *px = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
        if (px == MAP_FAILED) { perror("mmap"); return 1; }

        static const uint32_t bars[] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                        0xff00ff, 0xff0000, 0x0000ff, 0x000000};
        for (uint32_t y = 0; y < cd.height; y++) {
            uint32_t *row = (uint32_t *)(px + y * cd.pitch);
            for (uint32_t x = 0; x < cd.width; x++)
                row[x] = y < cd.height / 2 ? bars[x * 8 / cd.width]
                                           : (((x / 80) + (y / 80)) & 1 ? 0x202020 : 0x3050a0);
        }

        struct drm_mode_crtc sc = {.crtc_id = crtc_id, .fb_id = fbc.fb_id,
                                   .set_connectors_ptr = (uintptr_t)&c.connector_id,
                                   .count_connectors = 1, .mode = *m, .mode_valid = 1};
        if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) { perror("setcrtc"); return 1; }
        printf("crtc %u showing fb %u, holding %d s\n", crtc_id, fbc.fb_id, hold);
        fflush(stdout);
        /* Command-mode panels only refresh when the framebuffer is marked dirty. */
        struct drm_mode_fb_dirty_cmd dirty = {.fb_id = fbc.fb_id};
        for (int s = 0; s < hold * 10; s++) {
            ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
            usleep(100000);
        }
        return 0;
    }
    fprintf(stderr, "no connected connector\n");
    return 1;
}
