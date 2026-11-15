# 4. SteamOS for ARM on this phone

## Which SteamOS

In 2026 Valve published SteamOS for ARM with the **Steam Frame** headset (Arch based, image
codename `deckard`, version 0.5.0, build 20260921). It contains Gamescope, the Steam
client for ARM64, Proton (ARM64) with FEX for x86 translation, and KDE Plasma for Desktop Mode.
Community projects had already put it on SM8550 handhelds with mainline kernels, for example
[MaSieS4Fun/SteamOS-ARM-SM8550](https://github.com/MaSieS4Fun/SteamOS-ARM-SM8550).
This port does the same on the **Android downstream kernel**, because there is no mainline
kernel for the 12R.

Official download page: <https://store.steampowered.com/steamos/download/?ver=steamframe>.
The image is distributed as a **casync** chunked disk image (`.caibx` index plus a chunk
store). [`tools/casync-fetch.py`](../tools/casync-fetch.py) rebuilds it with parallel chunk
downloads and checks it against Valve's published SHA-256. The root filesystem tree was then
copied into `/steamos` on the `linux` partition, next to the earlier Debian, and selected with
`/boot-target` (see [02](02-boot-and-kernel.md)).

Valve's images and Steam aren't redistributable, so this repo has no image. You fetch it yourself.

## Adapting it: [`tools/steamos-provision.sh`](../tools/steamos-provision.sh)

It runs as root from the other system on the same partition (Debian at the time) and can be run
again safely. Main points:

- **`VARIANT_ID=steamdeck`** in os-release. The Steam client won't start its console UI on
  the `vr` variant.
- **Storage:** single ext4 partition. Masks Valve's partition-set units (`etc.mount`,
  `home.mount`, `efi.mount`, `systemd-repart`, growfs…).
- **Masks Valve-hardware services:** `deckard-*` (fan, FPGA, LEDs, audio setup, charger…),
  `steamvr-*`, MAC-address setters.
- **Copies the device layer in:** vendor modules for the running kernel, firmware, the KGSL
  Mesa build, and our services and tools.
- **Firmware conflict:** SteamOS ships its own `qca` Bluetooth firmware for its hardware,
  and it overrode the phone's. Use the phone's files from the Android vendor partition.
- **Wi-Fi backend pinned.** Steam told NetworkManager to switch to the iwd backend, which
  doesn't work with `qca_cld3` and dropped Wi-Fi mid-download. Pin `wpa_supplicant`.
- **The system updater is replaced** with a stub that says "no update available". Valve's
  updater (RAUC/A-B partition sets) would overwrite the adaptation. Steam's OOBE update step
  loops on "Try again" if the updater simply fails, so a stub is needed rather than masking it.
- SSH key-only, wheel sudo, `steamos` user with an empty password (Valve's default).

## Steam client fixes

| Problem | Fix |
|---|---|
| Steam crashes silently at start | Kernel needs `SYSVIPC` (see [02](02-boot-and-kernel.md)) |
| Steam shows the **headset** UI; QR sign-in pop-ups never appear (drawn into a VR overlay) | Remove the two headset-only flags from Valve's launch script. Steam then downloads the Steam Deck client variant |
| Two Steam instances fight | Disable Valve's desktop autostart when the session already starts Steam |
| "Fatal error, no network" at boot | Order Steam after `network-online.target` |
| Steam's FPS overlay (MangoHud) makes games exit within 6 s | Valve's MangoHud build + this driver; also needs `nohwmon-shim` |
| Proton's own resolution scaling env vars | Ignored by this Proton build; use Gamescope's `-w/-h` + FSR instead |

## Gaming Mode: [`tools/steamos-gamemode-install.sh`](../tools/steamos-gamemode-install.sh)

On a Steam Deck, SDDM starts `gamescope-session`. **SDDM can't run here** because the kernel
has no virtual terminals. The replacement is:

- [`aston-session-start`](../tools/aston-session-start) starts Gaming Mode (Gamescope +
  Steam) or Desktop Mode (Plasma) from a systemd service. It honours the one-shot choice
  Steam writes when you press *Switch to Desktop*, which is normally SDDM's job.
- [`steamos-session-select`](../tools/steamos-session-select) is a drop-in replacement that
  Steam calls. A *Return to Gaming Mode* desktop icon goes back.
- [`aston-gamescope-session`](../tools/aston-gamescope-session) runs Gamescope on the DRM
  backend with the shims preloaded, `--force-composition-rotation`, and the Steam Deck
  session environment, so Steam uses the real 2780×1264.
- Steam keeps running minimised in Desktop Mode, so the controller works as a mouse.
  Valve's Xbox "Desktop Configuration" needs Steam Input turned on for Xbox pads.

## Steam's Quick Access menu controls: [`tools/aston-steamosd`](../tools/aston-steamosd)

Steam talks to `steamos-manager` over D-Bus (`com.steampowered.SteamOSManager1`). Its stock
backends expect Valve hardware (RyzenAdj, AMD GPU sysfs) and couldn't parse this phone's
governors. `aston-steamosd` implements the interfaces Steam uses and maps them to this phone:

| Steam control | Implementation |
|---|---|
| TDP limit 3–15 W | Whole-phone power target enforced by `aston-powerd` (measured from the fuel gauge) |
| Manual GPU clock | KGSL `max_gpuclk` ceiling (124–680 MHz) |
| Performance profile | CPU governor and frequency caps |
| Charge limit | OnePlus charger sysfs |

Config: [`steamos-manager-remote-aston.toml`](../tools/steamos-manager-remote-aston.toml)
and the D-Bus policy [`com.aston.SteamOS.conf`](../tools/com.aston.SteamOS.conf).
The Quick Access menu opens with **Volume Up + Volume Down** through
[`aston-keysd`](../tools/aston-keysd), because there's no edge-swipe gesture.

## What ran

- *The Looker* (DX11 through DXVK, Proton 11 ARM64): runs. Playable at 1390×632 + FSR in nested Gamescope.
- *Just Cause 3*: ran in Gaming Mode.
- GTA V and RDR 2 were never tested. Anti-cheat online games won't work.
- Native **Linux x86** games are expected to fail. Valve's x86 Mesa copy for translated
  programs doesn't support KGSL, so an x86 Turnip-KGSL build would be needed.

## Mesa build line used (inside an Arch/SteamOS chroot)

```
meson setup build -Dprefix=/usr -Dlibdir=lib -Dbuildtype=release \
  -Dplatforms=x11,wayland -Dgallium-drivers=zink,freedreno -Dvulkan-drivers=freedreno \
  -Dfreedreno-kmds=msm,kgsl -Dglx=dri -Degl=enabled -Dgbm=enabled -Dgles1=disabled \
  -Dgles2=enabled -Dllvm=disabled -Dgallium-va=disabled -Dgallium-vdpau=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false \
  -Dvideo-codecs= -Dtools=
ninja -C build && DESTDIR=/src/dest ninja -C build install
```
