#!/usr/bin/env python3
"""Build a boot image from an existing one, swapping only the kernel.

Keeps the Android boot header (v4) of the source image, so OS version,
patch level and command line stay as the bootloader expects them.
This script does not flash anything.

Usage: pack_boot.py SOURCE_BOOT.img NEW_KERNEL_IMAGE OUTPUT.img
"""
import struct
import sys

PAGE = 4096
src, kernel, out = sys.argv[1:4]
img = open(src, "rb").read()
if img[:8] != b"ANDROID!" or struct.unpack_from("<I", img, 40)[0] != 4:
    sys.exit("source is not a v4 Android boot image")
old_ksize, rsize = struct.unpack_from("<II", img, 8)
if rsize or struct.unpack_from("<I", img, 1580)[0]:
    sys.exit("source has a ramdisk or signature; not handled")
k = open(kernel, "rb").read()
if k[:2] != b"MZ" or k[56:60] != b"ARM\x64":
    sys.exit("new kernel is not an uncompressed arm64 Image")
hdr = bytearray(img[:PAGE])
struct.pack_into("<I", hdr, 8, len(k))
pad = (-len(k)) % PAGE
with open(out, "wb") as f:
    f.write(hdr + k + b"\0" * pad)
print("kernel %d -> %d bytes, image %d bytes" % (old_ksize, len(k), PAGE + len(k) + pad))
