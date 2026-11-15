# 7. Safety, reverting, and relocking

**Read this before you flash anything on a OnePlus 12R.**

## Anti-rollback can brick the phone permanently

OxygenOS/ColorOS **16.0.3.50x and later** burn an anti-rollback **e-fuse**. Flashing older
firmware than the fuse allows hard-bricks the device, and only an authorised service tool
can recover it.

- Never flash OS images, `abl`, `xbl` or other firmware older than what is on the phone.
  Check the version first.
- Linux experiments only touch slot-A `boot`/`init_boot`/`vendor_boot`/`dtbo`/`vbmeta` and
  a new GPT partition. That's fine. The danger is when you "restore" with an old firmware
  package.

## Before you start

1. Back up the original GPT of `sda`: `sgdisk --backup=sda-gpt.bak /dev/sda`. Copy it off the phone.
2. Dump every slot-A and slot-B boot-chain image you'll touch (`boot`, `init_boot`,
   `vendor_boot`, `dtbo`, `vbmeta`, `recovery`) and keep checksums.
3. Note the exact OxygenOS version and baseband.
4. Leave **OEM unlocking** on in Developer options for the whole project.

## Reverting to stock (this was done on the author's phone)

1. **Restore the partition table** from a booted Linux, not rooted Android:
   `sgdisk --load-backup=sda-gpt.bak /dev/sda`. Then format data: `fastboot -w`, or let recovery do it.
2. **Get verified stock images for your exact build.** For each image, check AVB: the vbmeta
   RSA signature, the hash descriptors of `boot`/`init_boot`, and the chained partitions.
   Also check that the build fingerprint and security patch match the installed OS. If a
   `vbmeta` backup has flags = 3, zero bytes 120–123 to set flags back to 0. The OnePlus
   signature still verifies, because flags aren't covered by the signature.
3. Flash `boot`, `init_boot`, `vendor_boot`, `dtbo`, `recovery`, `vbmeta` to **both** slots.
4. **Virtual A/B leftovers:** after the experiment, `super` still held `*_b-cow` snapshot
   partitions (about 5.6 GB). `snapshot-update-status` was `none` because `/metadata` had been
   wiped. The OTA app then failed with "Try again" and logged nothing. The fix in fastbootd:
   `fastboot delete-logical-partition system_b-cow` (and `system_ext_b-cow`, `vendor_b-cow`,
   `product_b-cow`, `my_product_b-cow`, `odm_b-cow`), then flash a full OTA (below).
5. **Local "install from file" is blocked while the bootloader is unlocked** ("The
   bootloader is unlocked. Unable to install from local file"). Install the full OTA from a
   computer instead:
   - [`tools/payload_stream.py`](../tools/payload_stream.py) streams `payload.bin` out of the
     OTA zip, verifies every operation's SHA-256 and every partition's final hash, and writes
     the images. It can read straight from the phone, so the 8 GB zip doesn't need a local copy:
     `adb exec-out 'tail -c +<payload offset+1> ota.zip | head -c <size>' | python3 payload_stream.py extract OUT`.
   - Flash all partitions in **fastbootd** (`fastboot reboot fastboot`). `modem` returns
     "Operation not permitted" there. Flash it from the bootloader instead.
   - To bring the *other* slot's boot chain up to date: the bootloader refuses critical
     partitions (`abl`, `xbl*`, `tz`, `hyp`, `aop*`, `devcfg`, `keymaster`, `qupfw`,
     `uefisecapp`, `imagefv`) with "Flashing is not allowed for Critical Partitions", but
     fastbootd accepts `fastboot flash abl_a abl.img`.

## Relocking

Only relock when **every** partition on the active slot is official and verified, and the
inactive slot's boot chain is too. Then:

1. `fastboot flashing get_unlock_ability` must return **1**. That's your way back.
2. `fastboot flashing lock`, then confirm on the phone with the volume keys. This **wipes data**.
3. After setup, the orange "unlocked" warning should be gone.

Notes from the author's relock:
- After the experiment, verity reported `ro.boot.veritymode=eio`. That means a dm-verity
  corruption had been recorded at some point. A full read of every file on all system
  partitions found no I/O errors. The bootloader stores this state, and relocking resets it.
- A small post-flash "C20P01" update for the same version was an app/module (SOTA) update. It
  didn't switch slots.
