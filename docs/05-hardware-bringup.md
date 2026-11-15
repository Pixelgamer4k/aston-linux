# 5. Hardware bring-up notes

Everything below runs on the LineageOS kernel with the prebuilt vendor modules. Firmware
comes from the phone's own Android `vendor`/`odm` partitions, mounted **read-only** from
Linux. [`tools/aston-lpmap`](../tools/aston-lpmap) maps a logical partition out of `super`
without dmsetup tooling.

## Device nodes and services

- The stock LineageOS kernel has **no devtmpfs**. The first port used `mdev` and hand-made
  `mknod`s. That reset `/dev/null` to mode 600 during boot and **silently broke D-Bus**, and
  with it NetworkManager, logind and upower. Rebuild with `DEVTMPFS` (see [02](02-boot-and-kernel.md)).
- Several systemd services (upower, power-profiles-daemon) failed on this kernel's sandbox
  limits. Relax `ProtectKernel*`/namespace options in drop-ins.
- `serial-getty@hvc0` crash-loops (thousands of restarts). Mask it.
- No **nftables**. `ufw` falls back to a half-loaded ruleset and **blocks all inbound
  traffic, including SSH over USB.** Don't enable a firewall without `NF_TABLES`.

## Wi-Fi

WCN7850 through `cnss2` + `qca_cld3_kiwi_v2`, with firmware from the vendor partition. It works
with NetworkManager + wpa_supplicant. **iwd doesn't work.** Hotspot (AP mode) worked under Debian.

## Bluetooth: [`tools/aston-btfw`](../tools/aston-btfw)

Same chip as Wi-Fi, on `/dev/ttyHS0`. Three problems had to be solved:

1. **The 5.15 kernel's `hci_qca` loader predates WCN7850** and can't parse its replies.
   `aston-btfw` powers the chip through `btpower`, reads its version, raises the UART to
   3 Mbaud, downloads the rampatch and NVM from user space, waiting for the chip's
   acknowledgement of every block, and resets it. The UART then speaks plain H4.
2. **The UART powers down after 150 ms idle** and cuts the link. In-band and deep sleep are
   turned off in the NVM image, and the bridge keeps the UART awake.
3. **The chip advertises a command it then rejects**, and the kernel aborts setup. With
   `--bridge`, the kernel's H4 driver is attached to a pty and `aston-btfw` relays traffic,
   rewriting that one reply.

Result: BlueZ works with a stable address, and an Xbox-compatible controller pairs and
connects. Toggling Bluetooth off and on reloads firmware, which takes about 10 s. Clone Xbox
pads switch identity (and MAC) between modes, so re-pairing may be needed after a mode change.

## Touch

Synaptics TCM2 on SPI. The axis ranges it reports are wrong. A **hwdb** entry fixes them;
avoid the old `LD_PRELOAD` touch shims. For Gamescope, see the SPI/I²C note in [03](03-graphics-and-display.md).

## Haptics: [`tools/aston-buzz`](../tools/aston-buzz)

The PMIC high-voltage haptics block shows up as a force-feedback input device.
- A plain `FF_CONSTANT`/`FF_PERIODIC` gives a generic buzz.
- **6 predefined OnePlus effects** (click, double, tick, thud, pop, heavy) and 9 primitives
  play crisp, Android-like clicks.
- `FF_CUSTOM` waveform streaming is supported. OnePlus's richer haptics service is closed source.

## Video encode and decode (`msm_vidc`)

- Loading `msm_vidc` **without its firmware crashes the kernel** (driver bug). Copy the iris
  firmware from the vendor partition first, then load `videocc-kalama` and `msm_vidc`.
- It's a V4L2 M2M driver that **only accepts DMABUF memory on both queues** (allocate from
  `/dev/dma_heap/system`). That's why stock ffmpeg and GStreamer V4L2 elements fail at buffer
  setup.
- [`tools/vdectest.c`](../tools/vdectest.c) decoded 300 frames of 1080p H.264 in 0.25 s.
  [`tools/venctest.c`](../tools/venctest.c) encoded 180 frames of 1080p HEVC in about 0.5 s.
- [`tools/aston-gamerec.c`](../tools/aston-gamerec.c) + [`aston-record`](../tools/aston-record)
  form a **gameplay recorder**. It takes Gamescope's PipeWire stream as NV12 and encodes it
  in hardware to H.265, at 1918×872 60 fps with 0 dropped frames. The audio track was never
  finished.
- Not done: VA-API/V4L2-request glue so Firefox, mpv or Steam can use it.

## Audio: not working

- Loading the 26 audio modules in order probes both **TFA9873** amps, and the APR/GPR link to
  the ADSP appears, but **no ALSA card ever registers**. The sound card waits for the ADSP's
  audio service to report ready. On Android that needs the service locator (`pd-mapper`-like)
  and a closed component-loader daemon.
- Even with a card, routing, gain and speaker protection are done by Android's closed audio
  HAL with calibration blobs. A real fix is either a libhybris/Halium-style bridge to the
  Android audio HAL (as Droidian does) or a full AudioReach/q6apm topology for this board.
  That is several days of work.
- Bluetooth audio and USB-C audio (host mode) avoid all of this.

## USB host mode

`echo host > /sys/bus/platform/devices/a600000.ssusb/mode` works and switches back cleanly.
**While in host mode the USB-network link to your computer is gone.** Use Wi-Fi SSH.

## Power and thermals: [`tools/aston-powerd`](../tools/aston-powerd)

Android's thermal HAL and `thermal-engine` don't run, so this daemon does that job:

- **CPU governor:** `walt` (load-following). Don't leave all cores on `performance`. That
  was the main cause of a 50 °C skin temperature.
- **OnePlus factory tables:** 10 CPU levels (per-cluster max frequencies) and 4 GPU levels,
  copied from the phone's own `/odm/etc/temperature_profile/sys_thermal_control_config.xml`.
  There's a default policy and a more permissive *gaming* policy, which switches automatically
  while a game runs. The mapping *gear = skin °C − 35* is inferred: it fits the file's 40/44/49 °C
  thresholds, but it isn't documented. A board sensor stands in for Android's computed "shell"
  temperature.
- **Chip limits:** soft CPU caps from 85 °C and GPU caps from 80 °C, before the kernel's own
  95 °C+ protection.
- **Dual-cell power maths:** the gauge reports one cell's voltage, so multiply V×I by about 2.
  This was measured: the gauge's charge counter fell at 1284 mA while it reported 636 mA, a
  ratio of 2.02. Idle with the screen on is about 1.4–3 W, and 3 loaded cores about 5.1 W.
- **TDP:** the daemon holds whole-phone draw to a wattage on battery by stepping CPU and GPU
  levels. It's exposed in Steam's Quick Access TDP slider (see [04](04-steamos.md)), on a
  desktop slider, and through `aston tdp N`.
- **Charge limit** works through the OnePlus charger sysfs. SuperVOOC isn't negotiated, so
  charging is plain USB rate.

## Suspend: not working

`echo mem > /sys/power/state` starts and **aborts within a fraction of a second**. A device
vetoes it, and which one was never found. Look at `/sys/power/suspend_stats` and
`pm_debug_messages`.

## Sensors, camera, fingerprint, modem

- Sensors sit behind the SSC/sensor co-processor with a proprietary protocol.
- Cameras and fingerprint need Android's closed HALs and the secure world.
- The modem boots, but the data path isn't QMI-WWAN/MBIM in a form ModemManager supports.
