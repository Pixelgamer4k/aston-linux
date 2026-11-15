# 8. Open problems worth attacking

Rough order of value against effort. Each item points to the doc with the details.

1. **Reproducible build.** Turn `steamos-provision.sh` + `steamos-gamemode-install.sh` into
   a single script or `mkosi`/`archiso`-style recipe that takes Valve's image and the phone's
   own firmware and produces an ext4 rootfs. Today it's two scripts run by hand over SSH.
2. **Boot menu.** Switching OS needs a computer (`fastboot --set-active`). A Linux-side tool
   that flips the A/B attributes in `sde`'s GPT safely, using the same approach as `slotflag.py`, would remove that.
3. **Suspend.** Find the device that aborts suspend (`/sys/power/suspend_stats`,
   `pm_debug_messages`, `wakeup_sources`). Without suspend there's no handheld-style battery life.
4. **Touch in nested Gamescope / Steam Deck controller emulation.** InputPlumber, or a small
   touch-to-pointer bridge.
5. **x86 Turnip-KGSL build** for Valve's translated-program GPU path, so native Linux x86
   games work.
6. **Hardware video for apps.** A V4L2 M2M wrapper that allocates DMABUF from
   `/dev/dma_heap/system`, either as an ffmpeg patch or a VA-API driver. The test programs
   in `tools/` show the exact ioctl sequence that works.
7. **Audio.** Either a libhybris/Halium bridge to the Android audio HAL (Droidian's approach)
   or AudioReach (q6apm) topology bring-up for the TFA9873 amps. This is the hardest item on the list.
8. **Mainline.** A mainline device tree for `aston`, based on the AYN Odin 2 / SM8550 HDK
   work upstream, would make most of the shims above unnecessary. It means writing a panel
   driver for the DSC command-mode panel, plus touch and PMIC-charger work.

## Facts that will save you time

- The phantom virtual DRM connectors explain "the panel rejects normal display output".
- `/proc/phoenix` must get `ANDROID_BOOT_COMPLETED`, or the phone reboots at 240 s.
- `qcom-dload-mode` turns panics into crash-dump mode. Leave it out while experimenting.
- `msm_vidc` crashes the kernel if its firmware is missing.
- Steam needs SysV semaphores. Steam on the `vr` variant shows the headset UI.
- Don't enable ufw without nftables, or you'll lock yourself out of SSH.
- The fuel gauge reports single-cell voltage on this dual-cell phone.
