# 2. Boot chain, partitions and kernel

## Dual-boot design

```
                 ┌──────────── slot B ─────────────┐   ┌──────────── slot A ────────────────────┐
 bootloader ───► │ stock OxygenOS: boot_b, init_boot_b,│   │ boot_a      = LineageOS kernel (rebuilt)│
 (OnePlus abl,   │ vendor_boot_b, super(*_b), userdata │   │ vendor_boot_a = LineageOS vendor ramdisk │
  unlocked)      └─────────────────────────────────────┘   │ init_boot_a = custom ramdisk: busybox +  │
                                                          │               boot/init (pid 1)          │
                                                          │ rootfs      = GPT partition "linux"      │
                                                          └──────────────────────────────────────────┘
```

- The bootloader stays unlocked and is never modified. Switching OS means
  `fastboot --set-active=a|b`.
- `vbmeta_a` was flashed with flags = 3 (verity and verification disabled). On an unlocked
  bootloader this is enough for unsigned slot-A images.
- `init_boot_a` keeps the **LineageOS** kernel's vendor ramdisk (from `vendor_boot_a`)
  underneath. Android's first-stage merging gives pid 1 the matching `/lib/modules`.

## Partition split

`userdata` on `sda` (LUN 0, 4096-byte sectors) was cut in half and a new GPT entry named
`linux` was created after it:

| # | Name | First LBA | Last LBA | Size |
|---|---|---|---|---|
| 14 | userdata | 4162284 | 32747379 | 109 GiB |
| 15 | linux | 32747380 | 61332474 | 109 GiB (ext4) |

Notes:
- Stock `userdata` spans 4162284–61332474 (218 GiB).
- **Shrinking userdata means formatting it.** It is encrypted f2fs, so it can't be resized
  in place. Back up first.
- `sgdisk` worked from a booted Linux (from Debian on slot A over SSH). It **failed
  from rooted Android** ("Unable to save backup partition table") because Android holds the
  block device. Always keep an `sgdisk --backup` copy of the original table off the phone.
- Reverting is `sgdisk --load-backup=<original> /dev/sda` followed by `fastboot -w`.

## pid 1: [`boot/init`](../boot/init)

A busybox shell script. In order, it:

1. Mounts proc/sys, then tries `devtmpfs` (only present in our rebuilt kernel) and falls back
   to tmpfs + `mdev`.
2. Loads vendor modules from `modules.load` with its own dependency resolver. It skips
   `qcom-dload-mode`, because otherwise every panic drops into the Qualcomm crash-dump mode
   and needs a key combo to escape.
3. Finds the partition whose `PARTNAME` is `linux` and mounts it.
4. Reads `/boot-target` on that partition. `steamos` bind-mounts `/steamos` as the new root;
   anything else boots the partition root (Debian). Several systems can share one partition.
5. **Writes `ANDROID_BOOT_COMPLETED` to `/proc/phoenix`.** OnePlus's boot watchdog ("phoenix")
   otherwise forces a reboot at 240 s.
6. Brings up a USB ECM gadget at `169.254.42.1/16` (loads the eUSB2/dwc3 modules and sets the
   ssusb role to peripheral), so the phone is reachable over SSH from first boot. It also starts
   a fallback `sshd` on port 2222 in case the distro's SSH fails.
7. `switch_root` to `/sbin/init` (systemd).

If something fails, it saves logs into `/var/log/aston-boot.log` and reboots to the bootloader.
It never lets pid 1 exit.

Image building: [`tools/pack_init_boot.py`](../tools/pack_init_boot.py) builds the v4 init_boot
with only `/init` and `/bin/busybox`. [`tools/pack_boot.py`](../tools/pack_boot.py) swaps only
the kernel inside an existing boot image and keeps the header, OS version and cmdline.

## The slot fallback trap

Qualcomm A/B state lives in the **GPT attribute bits of `boot_a`/`boot_b` on `sde`**: priority,
active, retry count, successful, unbootable. A slot that is never marked *successful* loses
one retry per boot and is abandoned after 7. The phone then silently boots Android.
On Android, `update_verifier`/`boot_control` sets the flag. Plain Linux has neither.

[`tools/slotflag.py`](../tools/slotflag.py) reads the flag and can set *successful* on a slot.
It rewrites only the affected entry sector and header of each GPT copy, and only if the CRCs
were valid. A oneshot service runs it on every good boot. The main and backup GPT on `sde`
did not match each other, so generic tools like sgdisk were deliberately not used there.

## Kernel

Base: `LineageOS/android_kernel_oneplus_sm8550` at **`3caf896e6bc8`**, the exact commit of the
lineage-23.2 boot image. Its config came from the shipped image with
`scripts/extract-ikconfig`.

Why not the stock OnePlus kernel? It was tried. Linux booted, mounted the rootfs and started
SSH, but the **stock display driver never produced a frame** outside Android, so the
LineageOS kernel and modules were kept.

Config changes ([`patches/kernel-aston.config`](../patches/kernel-aston.config)):

| Option | Why |
|---|---|
| `USER_NS`, `PID_NS` | Steam's pressure-vessel runtime, Flatpak, bubblewrap |
| `DEVTMPFS` | Real device nodes; without it `/dev/null` was reset to root-only at boot and silently killed D-Bus |
| `FHANDLE` | systemd |
| `SYSVIPC`, `POSIX_MQUEUE` | **Steam crashes silently without System V semaphores** |

### Keeping 721 prebuilt vendor modules loadable

The vendor modules (display, GPU, Wi-Fi, touch…) are prebuilt against the Android KABI with
`MODVERSIONS`. A rebuilt kernel must keep **every exported symbol CRC** the same:

- Namespaces, devtmpfs and fhandle don't change any struct the modules see.
- `SYSVIPC` adds `sysvsem`/`sysvshm` to `struct task_struct`, which changes its layout and
  CRCs. [`patches/kernel-sysvipc-kabi-reserve.patch`](../patches/kernel-sysvipc-kabi-reserve.patch)
  moves those two fields into the `ANDROID_KABI_RESERVE(2..4)` padding at the end of
  `task_struct` and hides them from genksyms with `__GENKSYMS__`. Size, offsets and CRCs stay
  unchanged.
- **Check before flashing:** compare every `__versions` entry required by every `.ko` against
  the new `Module.symvers`. About 40,000 references across 721 modules matched with zero
  mismatches on all three builds.

Build: clang/LLVM 16, `make O=out ARCH=arm64 LLVM=1 LLVM_IAS=1`. On a memory-limited VM the
LTO link step ran out of memory until swap was added and the link parallelism lowered.
A different compiler from LineageOS's own worked fine in practice.
