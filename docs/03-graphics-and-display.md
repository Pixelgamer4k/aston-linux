# 3. Graphics and display

## Two separate kernel devices

On this downstream kernel the GPU and the display are **separate drivers**:

| Job | Driver | Userspace view |
|---|---|---|
| 3D / compute | `msm_kgsl` (Android's KGSL) | `/dev/kgsl-3d0`, **not** a DRM device |
| Scanout | `msm_drm` (Qualcomm SDE) | `/dev/dri/card0`, KMS/atomic |

Buffers go from KGSL to SDE as dma-bufs. Upstream Mesa's Freedreno/Turnip mainly target the
mainline `msm` DRM driver. Turnip also has a **KGSL backend** (`-Dfreedreno-kmds=msm,kgsl`),
which is what makes this work.

## GPU driver: Turnip on KGSL

- Mesa main (26.3-devel), built with `-Dvulkan-drivers=freedreno -Dgallium-drivers=zink,freedreno -Dfreedreno-kmds=msm,kgsl`.
  The full meson line is in [04-steamos.md](04-steamos.md).
- Vulkan 1.4 with 224 extensions. OpenGL 4.6 through **Zink** on top of Turnip
  (`MESA_LOADER_DRIVER_OVERRIDE=zink`).
- The native Freedreno GL driver (gallium `freedreno`) rendered fine, but the compositor
  could not scan its buffers out. Zink was kept.
- **GL and Vulkan must come from the same Mesa build.** Mixing an older libGL with a newer
  Turnip produced buffers that Gamescope's Xwayland couldn't import, and Steam's CEF UI then
  crash-looped.
- Put the driver in the default loader paths (`/etc/ld.so.conf.d`, ICD JSON, `/etc/environment`)
  so *every* process uses it, not only scripts that set environment variables.

### Patch: `VK_EXT_physical_device_drm` on KGSL

[`patches/mesa-turnip-kgsl-physical-device-drm.patch`](../patches/mesa-turnip-kgsl-physical-device-drm.patch)

Gamescope uses `VK_EXT_physical_device_drm` to match the Vulkan device with the DRM node that
will display its frames. It **rejects client frames** if the extension is missing. KGSL isn't
DRM, so Turnip didn't expose it. The patch reports `/dev/dri/card0` (SDE) and `renderD128` as
the associated nodes, which is true in practice because SDE imports Turnip's dma-bufs.
After this, Gamescope accepted game frames, and per-game resolution with FSR upscaling worked.

## Display quirks

1. **Phantom virtual connectors.** `msm_drm` exposes two "virtual" connectors that report
   *connected*. Every compositor tried to light them and failed, which led to the false
   belief that "the panel rejects the normal display path". Disable them at boot:
   `for c in /sys/class/drm/card0-Virtual-*; do echo off > $c/status; done`.
   After that, KWin and Gamescope drive the DSI panel directly at 120 Hz and 10-bit.
2. **No `IN_FORMATS` property on planes.** SDE accepts LINEAR buffers through
   AddFB2-with-modifiers but doesn't publish the list. Gamescope aborts without it.
3. **Plane alpha is 0–255**, not the standard 0–65535. Gamescope's values are out of range.
4. **No hardware rotation.** Gamescope needs `--force-composition-rotation` to rotate the
   portrait panel to landscape in its own compositor.
5. **Touch on SPI.** Gamescope only ties a touchscreen to the internal panel, and rotates its
   touches with it, when the touchscreen's parent bus is I²C. This one is on SPI.

[`tools/drm-informats-shim.c`](../tools/drm-informats-shim.c) is an `LD_PRELOAD` library for
Gamescope that fixes 2, 3 and 5. It fakes an `IN_FORMATS` blob with LINEAR for each plane,
rescales alpha in atomic requests, and reports `spi` parents as `i2c`.
[`tools/nohwmon-shim.c`](../tools/nohwmon-shim.c) answers MangoHud's scan of the missing
`/sys/class/hwmon` (this kernel has no hwmon) with an empty directory.

## Compositors

| Compositor | Result |
|---|---|
| Weston (CPU renderer) hosting KWin (CPU) hosting Plasma at 316×695 ×4 | What the first attempt shipped: blurry and laggy. Avoid |
| KWin Wayland directly on `card0` | **Works**: native 1264×2780, 120 Hz, 10-bit, 3× scale (Plasma Mobile) or 2× landscape (Desktop Mode) |
| Gamescope nested (inside KWin) | Works for per-game scaling and FSR. **Touch never reaches the game**, because nested Gamescope ignores touch |
| Gamescope as the whole session (DRM backend) | **Works** with the shim, `--force-composition-rotation` and Steam Deck session env vars. Steam's UI at full 2780×1264 |
| Sway | Not tried after the virtual-connector fix |

Steam asks Gamescope for 1920×1080 unless the Steam Deck session environment is set.
The variables are in [`tools/aston-gamescope-session`](../tools/aston-gamescope-session).
Without them the UI is 16:9 with side bars.

## Screenshots

Neither the KDE portal nor Gamescope's screenshot path was usable early on.
[`tools/drmshot.c`](../tools/drmshot.c) reads the scanout buffer straight from KMS as root and
works under any compositor. It shows tearing bands on busy screens because it reads while
the panel updates.
