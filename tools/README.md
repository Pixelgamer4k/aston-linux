# tools/

"Phone" means it runs on the phone as root. "Host" means it runs on your computer.

## Boot, partitions, flashing (host)

| File | What it does |
|---|---|
| `pack_init_boot.py` | Builds a v4 `init_boot` with only `/init` (from `boot/init`) and busybox. Expects a LineageOS `init_boot` as template |
| `pack_boot.py` | Swaps the kernel inside an existing boot image and keeps the header and cmdline |
| `slotflag.py` | Reads or sets the Qualcomm A/B "successful" GPT attribute (run on the phone against `/dev/block/sde`) |
| `payload_stream.py` | Streaming, hash-verified extractor for full-OTA `payload.bin`. `list` or `extract OUT [parts]` |
| `casync-fetch.py` | Parallel casync (`.caibx`) image rebuilder, used for Valve's SteamOS image |
| `sshs`, `sshp` | SSH helpers: USB link `169.254.42.1`, optional Wi-Fi (`ASTON_WIFI_IP`), fallback port 2222. They need your own key in `keys/` |
| `shot` | Screenshot the phone through `drmshot` |

## SteamOS adaptation (phone)

| File | What it does |
|---|---|
| `steamos-provision.sh` | Adapts Valve's SteamOS ARM tree in `/steamos` to this phone. Can be re-run |
| `steamos-gamemode-install.sh` | Gaming Mode: Gamescope session, Mesa paths, Quick Access backend, shims |
| `aston-session-start`, `aston-gamescope-session`, `aston-gamescope-steam`, `steamos-session-select` | Session start and switching without SDDM |
| `gamescope.conf` | Default game resolution for Gamescope |
| `aston-steamosd` (+ `.service`, `com.aston.SteamOS.conf`, `steamos-manager-remote-aston.toml`) | Backend for Steam's TDP, GPU clock, profile and charge-limit sliders |
| `aston-keysd` (+ `.service`) | Volume Up + Down opens the Quick Access menu |
| `steam-firstrun-reset.sh` | Puts Steam back into first-run (OOBE) state without deleting anything |

## Device bring-up (phone)

| File | What it does |
|---|---|
| `aston` | Status and control CLI: temps, power, profile, TDP, USB role, Bluetooth, screenshots |
| `aston-powerd` | Thermal policy (OnePlus factory tables), power profiles, TDP, charge limit |
| `aston-tdp-slider` | Desktop power-limit slider (kdialog) |
| `aston-btfw` | WCN7850 Bluetooth firmware loader and H4 bridge |
| `aston-buzz` | Haptics, including OnePlus's shaped effects |
| `aston-lpmap` | Maps an Android logical partition out of `super`, read-only |
| `aston-record`, `aston-gamerec.c`, `record.conf`, `aston-video.service` | Hardware H.265 gameplay recorder |
| `drm-informats-shim.c` | Gamescope `LD_PRELOAD`: fake `IN_FORMATS`, alpha 0–255 rescale, SPI→I²C touch |
| `nohwmon-shim.c` | MangoHud `LD_PRELOAD`: empty `/sys/class/hwmon` |
| `touchinject.py` | Injects a test swipe through a virtual touchscreen |

## Test programs (phone)

| File | What it does |
|---|---|
| `drmtest.c`, `atomictest.c` | Legacy and atomic KMS tests on the DSI connector |
| `drmshot.c` | Reads the scanout buffer as a PPM (screenshots under any compositor) |
| `vdectest.c`, `venctest.c` | `msm_vidc` V4L2 M2M decode and encode tests with DMABUF heaps |
| `chroot-check.sh`, `chroot-more.sh`, `more-check.sh`, `net-check.sh` | Early probes run from Android into a Debian chroot |
