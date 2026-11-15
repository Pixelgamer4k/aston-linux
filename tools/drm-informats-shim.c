// Preload library: give KMS planes an IN_FORMATS property when the driver has none.
//
// Qualcomm's downstream SDE display driver (msm_drm on Android kernels) accepts
// linear buffers through AddFB2-with-modifiers but never publishes the
// IN_FORMATS list that says so.  Gamescope's DRM backend requires that list to
// create its output images and aborts without it.  This library answers the
// three libdrm calls involved with a synthetic IN_FORMATS blob listing every
// format of the plane with the LINEAR modifier, which is what the driver
// really supports.
//
// The same driver measures plane opacity ("alpha") on a 0-255 scale where the
// standard property is 0-65535.  Gamescope sends the standard value, which is
// out of range here, so the property is presented with the standard range and
// values are scaled down on their way into an atomic request.
//
// Gamescope also only ties a touchscreen to the built-in display (and so rotates
// its touches with the picture) when the touchscreen is an I2C device.  This
// phone's is on SPI, so an "spi" parent is reported to it as "i2c".
//
// Build: gcc -shared -fPIC -O2 -o libaston-drm-informats.so drm-informats-shim.c $(pkg-config --cflags libdrm) -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define FAKE_PROP_ID   0x7fff0001u
#define FAKE_BLOB_BASE 0x7ffe0000u      /* + plane id */

static drmModeObjectPropertiesPtr (*real_get_props)(int, uint32_t, uint32_t);
static drmModePropertyPtr (*real_get_prop)(int, uint32_t);
static drmModePropertyBlobPtr (*real_get_blob)(int, uint32_t);

static int (*real_atomic_add)(drmModeAtomicReqPtr, uint32_t, uint32_t, uint64_t);

#define MAX_ALPHA_PROPS 8
static uint32_t alpha_props[MAX_ALPHA_PROPS];
static int n_alpha_props;

static int is_alpha_prop(uint32_t id)
{
    for (int i = 0; i < n_alpha_props; i++)
        if (alpha_props[i] == id)
            return 1;
    return 0;
}

/* A driver "alpha" range property topping out at 255: remember it. */
static int note_alpha_prop(drmModePropertyPtr prop)
{
    if (!prop || strcmp(prop->name, "alpha") || !(prop->flags & DRM_MODE_PROP_RANGE) ||
        prop->count_values != 2 || prop->values[1] != 255)
        return 0;
    if (!is_alpha_prop(prop->prop_id) && n_alpha_props < MAX_ALPHA_PROPS)
        alpha_props[n_alpha_props++] = prop->prop_id;
    return 1;
}

static void init(void)
{
    if (real_get_props)
        return;
    real_atomic_add = dlsym(RTLD_NEXT, "drmModeAtomicAddProperty");
    real_get_props = dlsym(RTLD_NEXT, "drmModeObjectGetProperties");
    real_get_prop = dlsym(RTLD_NEXT, "drmModeGetProperty");
    real_get_blob = dlsym(RTLD_NEXT, "drmModeGetPropertyBlob");
}

/* Also rescales the current value of 0-255 alpha properties to 0-65535. */
static int has_in_formats(int fd, drmModeObjectPropertiesPtr p)
{
    int found = 0;
    for (uint32_t i = 0; i < p->count_props; i++) {
        drmModePropertyPtr prop = real_get_prop(fd, p->props[i]);
        if (prop && !strcmp(prop->name, "IN_FORMATS"))
            found = 1;
        if (note_alpha_prop(prop))
            p->prop_values[i] = p->prop_values[i] * 0xffff / 255;
        drmModeFreeProperty(prop);
    }
    return found;
}

drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t object_id, uint32_t object_type)
{
    init();
    drmModeObjectPropertiesPtr p = real_get_props(fd, object_id, object_type);
    if (!p || object_type != DRM_MODE_OBJECT_PLANE || has_in_formats(fd, p))
        return p;

    uint32_t n = p->count_props;
    uint32_t *props = malloc((n + 1) * sizeof(*props));
    uint64_t *values = malloc((n + 1) * sizeof(*values));
    if (!props || !values) {
        free(props);
        free(values);
        return p;
    }
    memcpy(props, p->props, n * sizeof(*props));
    memcpy(values, p->prop_values, n * sizeof(*values));
    props[n] = FAKE_PROP_ID;
    values[n] = FAKE_BLOB_BASE + object_id;
    free(p->props);
    free(p->prop_values);
    p->props = props;
    p->prop_values = values;
    p->count_props = n + 1;
    return p;
}

drmModePropertyPtr drmModeGetProperty(int fd, uint32_t property_id)
{
    init();
    if (property_id != FAKE_PROP_ID) {
        drmModePropertyPtr real = real_get_prop(fd, property_id);
        if (note_alpha_prop(real))
            real->values[1] = 0xffff;
        return real;
    }
    drmModePropertyPtr prop = calloc(1, sizeof(*prop));
    if (!prop)
        return NULL;
    prop->prop_id = FAKE_PROP_ID;
    prop->flags = DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_BLOB;
    strcpy(prop->name, "IN_FORMATS");
    return prop;
}

drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t blob_id)
{
    init();
    if (blob_id < FAKE_BLOB_BASE || blob_id >= FAKE_PROP_ID)
        return real_get_blob(fd, blob_id);

    drmModePlanePtr plane = drmModeGetPlane(fd, blob_id - FAKE_BLOB_BASE);
    if (!plane)
        return NULL;
    uint32_t nfmt = plane->count_formats;
    uint32_t nmod = (nfmt + 63) / 64;           /* one LINEAR entry per 64 formats */
    size_t fmt_off = sizeof(struct drm_format_modifier_blob);
    size_t mod_off = (fmt_off + nfmt * sizeof(uint32_t) + 7) & ~(size_t)7;
    size_t len = mod_off + nmod * sizeof(struct drm_format_modifier);

    drmModePropertyBlobPtr blob = calloc(1, sizeof(*blob));
    uint8_t *data = calloc(1, len);
    if (!blob || !data) {
        free(blob);
        free(data);
        drmModeFreePlane(plane);
        return NULL;
    }
    struct drm_format_modifier_blob *hdr = (void *)data;
    hdr->version = FORMAT_BLOB_CURRENT;
    hdr->count_formats = nfmt;
    hdr->formats_offset = fmt_off;
    hdr->count_modifiers = nmod;
    hdr->modifiers_offset = mod_off;
    memcpy(data + fmt_off, plane->formats, nfmt * sizeof(uint32_t));
    struct drm_format_modifier *mods = (void *)(data + mod_off);
    for (uint32_t i = 0; i < nmod; i++) {
        uint32_t left = nfmt - i * 64;
        mods[i].formats = left >= 64 ? ~0ull : ((1ull << left) - 1);
        mods[i].offset = i * 64;
        mods[i].modifier = DRM_FORMAT_MOD_LINEAR;
    }
    drmModeFreePlane(plane);
    blob->id = blob_id;
    blob->length = len;
    blob->data = data;
    return blob;
}

int drmModeAtomicAddProperty(drmModeAtomicReqPtr req, uint32_t object_id, uint32_t property_id, uint64_t value)
{
    init();
    if (is_alpha_prop(property_id))
        value = value * 255 / 0xffff;
    return real_atomic_add(req, object_id, property_id, value);
}

struct udev_device;
const char *udev_device_get_subsystem(struct udev_device *dev)
{
    static const char *(*real)(struct udev_device *);
    if (!real)
        real = dlsym(RTLD_NEXT, "udev_device_get_subsystem");
    const char *s = real ? real(dev) : NULL;
    return s && !strcmp(s, "spi") ? "i2c" : s;
}
