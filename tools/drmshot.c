// Screenshot by reading the framebuffer currently on the panel's display plane.
// Works under any compositor (no screenshot portal needed); must run as root.
// Writes a binary PPM to stdout in the panel's native orientation.
// Build: gcc -O2 -static -o drmshot drmshot.c
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("card0"); return 1; }
    struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
    ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);

    uint32_t planes[64];
    struct drm_mode_get_plane_res res = { .plane_id_ptr = (uintptr_t)planes, .count_planes = 64 };
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res)) { perror("planes"); return 1; }
    uint32_t fb_id = 0;
    for (uint32_t i = 0; i < res.count_planes && i < 64 && !fb_id; i++) {
        struct drm_mode_get_plane p = { .plane_id = planes[i] };
        if (!ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p))
            fb_id = p.fb_id;
    }
    if (!fb_id) { fprintf(stderr, "no active framebuffer\n"); return 1; }

    struct drm_mode_fb_cmd2 fb = { .fb_id = fb_id };
    if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb)) { perror("GETFB2"); return 1; }
    struct drm_prime_handle ph = { .handle = fb.handles[0], .flags = O_RDONLY | O_CLOEXEC };
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph)) { perror("export"); return 1; }
    size_t size = (size_t)fb.pitches[0] * fb.height;
    uint8_t *map = mmap(NULL, size, PROT_READ, MAP_SHARED, ph.fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }

    fprintf(stderr, "fb %u: %ux%u format %.4s pitch %u\n", fb_id, fb.width, fb.height, (char *)&fb.pixel_format, fb.pitches[0]);
    printf("P6\n%u %u\n255\n", fb.width, fb.height);
    uint8_t *row = malloc(fb.width * 3);
    for (uint32_t y = 0; y < fb.height; y++) {
        const uint32_t *src = (const uint32_t *)(map + (size_t)y * fb.pitches[0]);
        for (uint32_t x = 0; x < fb.width; x++) {
            uint32_t v = src[x];
            uint8_t r, g, b;
            switch (fb.pixel_format) {
            case DRM_FORMAT_ABGR2101010: case DRM_FORMAT_XBGR2101010:
                r = (v >> 2) & 0xff; g = (v >> 12) & 0xff; b = (v >> 22) & 0xff; break;
            case DRM_FORMAT_ARGB2101010: case DRM_FORMAT_XRGB2101010:
                b = (v >> 2) & 0xff; g = (v >> 12) & 0xff; r = (v >> 22) & 0xff; break;
            case DRM_FORMAT_ABGR8888: case DRM_FORMAT_XBGR8888:
                r = v & 0xff; g = (v >> 8) & 0xff; b = (v >> 16) & 0xff; break;
            default: /* XRGB8888 / ARGB8888 */
                b = v & 0xff; g = (v >> 8) & 0xff; r = (v >> 16) & 0xff; break;
            }
            row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
        }
        fwrite(row, 3, fb.width, stdout);
    }
    return 0;
}
