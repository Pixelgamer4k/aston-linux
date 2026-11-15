#!/usr/bin/env python3
"""Build an init_boot image that keeps the stock kernel and vendor ramdisk.

The new generic ramdisk contains only /init and /bin/busybox. Android merges
the vendor ramdisk from vendor_boot underneath it at boot, which is where the
kernel modules live. This script does not flash anything.
"""

import os
import struct
import subprocess
import sys
from pathlib import Path

PAGE = 4096
ROOT = Path(__file__).resolve().parents[1]
BACKUP = ROOT / "recovery" / "los-init_boot.img"
BUSYBOX = ROOT / "ramdisk" / "bin" / "busybox"
INIT = ROOT / "ramdisk" / "init"
OUT = ROOT / "out" / "init_boot_debian.img"


def align(n: int) -> int:
    return (n + PAGE - 1) & ~(PAGE - 1)


def cpio_newc(files: list[tuple[str, bytes, int]]) -> bytes:
    blob = bytearray()
    for name, data, mode in files:
        name_b = name.encode() + b"\x00"
        header = (
            f"070701"
            f"{0:08x}{mode:08x}{0:08x}{0:08x}{1:08x}"
            f"{0:08x}{len(data):08x}"
            f"{0:08x}{0:08x}{0:08x}{0:08x}"
            f"{len(name_b):08x}{0:08x}"
        ).encode()
        blob += header
        blob += name_b
        blob += b"\x00" * ((-len(blob)) % 4)
        blob += data
        blob += b"\x00" * ((-len(blob)) % 4)
    trailer_name = b"TRAILER!!!\x00"
    header = (
        f"070701"
        f"{0:08x}{0:08x}{0:08x}{0:08x}{1:08x}"
        f"{0:08x}{0:08x}"
        f"{0:08x}{0:08x}{0:08x}{0:08x}"
        f"{len(trailer_name):08x}{0:08x}"
    ).encode()
    blob += header
    blob += trailer_name
    blob += b"\x00" * ((-len(blob)) % 4)
    return bytes(blob)


def collect_ramdisk() -> list[tuple[str, bytes, int]]:
    root = ROOT / "ramdisk"
    files: list[tuple[str, bytes, int]] = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = Path(dirpath).relative_to(root)
        for name in sorted(dirnames):
            rel = (rel_dir / name).as_posix()
            if rel == ".":
                continue
            files.append((rel, b"", 0o040755))
        for name in sorted(filenames):
            path = Path(dirpath) / name
            rel = (rel_dir / name).as_posix()
            mode = 0o100755 if os.access(path, os.X_OK) or name in {"init", "sgdisk", "busybox"} else 0o100644
            files.append((rel, path.read_bytes(), mode))
    for needed in ("proc", "sys", "dev", "mnt", "tmp"):
        if not any(item[0] == needed for item in files):
            files.append((needed, b"", 0o040755))
    return files


def main() -> int:
    files = collect_ramdisk()
    init = next((data for name, data, _mode in files if name == "init"), b"")
    if not init.startswith(b"#!"):
        print("ramdisk/init must be a script", file=sys.stderr)
        return 1
    cpio = cpio_newc(files)
    work = ROOT / "out"
    work.mkdir(parents=True, exist_ok=True)
    raw = work / "ramdisk.cpio"
    comp = work / "ramdisk.lz4"
    raw.write_bytes(cpio)
    subprocess.run(["lz4", "-l", "-9", "-f", str(raw), str(comp)], check=True)
    ramdisk = comp.read_bytes()

    src = BACKUP.read_bytes()
    if src[:8] != b"ANDROID!":
        print("backup init_boot is not an Android boot image", file=sys.stderr)
        return 1
    header_version = struct.unpack_from("<I", src, 40)[0]
    if header_version != 4:
        print(f"unexpected header version {header_version}", file=sys.stderr)
        return 1
    header_size = struct.unpack_from("<I", src, 20)[0]
    # kernel_size is 0. Ramdisk begins at the first page.
    image = bytearray(src[:PAGE])
    struct.pack_into("<I", image, 12, len(ramdisk))
    image += ramdisk
    image += b"\x00" * (align(len(image)) - len(image))
    # Keep the 8 MiB partition image size so fastboot can write it later.
    part = 8 * 1024 * 1024
    if len(image) > part:
        print(f"ramdisk does not fit in init_boot ({len(image)} > {part})", file=sys.stderr)
        return 1
    image += b"\x00" * (part - len(image))
    OUT.write_bytes(image)
    print(f"wrote {OUT} header_size={header_size} ramdisk={len(ramdisk)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
