# 1. The device

OnePlus 12R, Indian variant `CPH2585IN`. Codename `aston`, project `OP5D35L1` / 23861,
fastboot product `kalama`. Every value below was read from a running unit.

## SoC

| Item | Value |
|---|---|
| Chip | Snapdragon 8 Gen 2, SM8550 (`kalama`). Same SoC as the AYN Odin 2, the reference for most SM8550 Linux work |
| Prime core | 1 × Cortex-X3, 595–3187 MHz |
| Performance | 2 × A715 + 2 × A710, 499–2803 MHz |
| Efficiency | 3 × A510, 307–2016 MHz |
| CPU features | AES, SHA1/2/3/512, CRC32, LSE atomics, RCPC, FlagM/FlagM2, dotprod, FP16, i8mm, BF16, PAC, BTI. **No SVE/SVE2** exposed, **no TSO** mode |
| RAM | 16 GB (15.1 GB usable) |
| Page size / VA bits | 4 KB / **39-bit** (stock GKI config). Standalone FEX prefers 48-bit; changing it breaks all vendor modules |
| GPU | Adreno 740 v2, 124–680 MHz (9 steps) |
| Storage | UFS 4.0. **4096-byte logical sectors.** `sda` = LUN 0 (userdata etc.), `sde` holds the boot chain and the A/B slot attribute bits |

## Display

| Item | Value |
|---|---|
| Panel | 1264×2780, command-mode DSI with DSC |
| Modes | 60 / 90 / 120 Hz |
| Depth | 10-bit scanout (`ABGR2101010`) works |
| HDR / WCG / VRR | Reported as incapable by the downstream driver |
| Driver | Qualcomm downstream `msm_drm` (SDE), **not** mainline DPU |

## Radios and peripherals

| Item | Chip / driver |
|---|---|
| Wi-Fi 7 + Bluetooth | WCN7850 (`qca_cld3_kiwi_v2`, `cnss2`, `btpower`) |
| Touch | Synaptics TCM2 on **SPI** (`synaptics_tcm2`, `oplus_bsp_tp_custom`) |
| Haptics | OnePlus linear motor, force-feedback input device with 6 predefined effects and 9 primitives |
| Speaker amps | 2 × NXP TFA9873 (top and bottom) |
| Video | Qualcomm `msm_vidc` (iris): H.264, HEVC, VP9 and AV1 decode; H.264 and HEVC encode |
| Battery | **Dual-cell** pack, about 5500 mAh total. The fuel gauge reports the voltage of one cell (about 4 V) next to the pack current, so V×I is half the real power |
| USB | dwc3-msm with eUSB2 repeater. Role switch through `/sys/bus/platform/devices/a600000.ssusb/mode` |
| Modem | Runs, but the data path isn't something ModemManager understands |

## Firmware versions seen

| Item | Value |
|---|---|
| OxygenOS | 16.0.5.1001 → 16.0.5.1301 (EX01), Android 16 |
| Stock kernel | 5.15.180/5.15.197 `android13-8` GKI |
| LineageOS kernel used for Linux | 5.15.211 from `LineageOS/android_kernel_oneplus_sm8550` (commit `3caf896e6bc8`), lineage-23.2 nightly 2026-09-25 |

## Thermal

108 thermal zones and 65 cooling devices are readable from sysfs. OnePlus's thermal policy
lives on the Android side in `odm`/`vendor`: `sys_thermal_control_config.xml`, high-temperature
protection tables, and per-level FPS caps. [05-hardware-bringup.md](05-hardware-bringup.md)
describes how those tables were reused.
