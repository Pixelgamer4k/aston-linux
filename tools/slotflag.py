#!/usr/bin/env python3
"""Inspect or set the Qualcomm A/B "boot successful" flag for a slot.

The bootloader keeps per-slot state in the GPT attribute bits of boot_a /
boot_b (byte 6 of the attribute field): priority (bits 0-1), active (bit 2),
retry count (bits 3-5), successful (bit 6), unbootable (bit 7).  A slot that
is never marked successful loses one retry per boot and is abandoned at zero.

Usage:
  slotflag.py DEVICE_OR_IMAGE                 show state, verify checksums
  slotflag.py DEVICE_OR_IMAGE --mark a        mark slot a successful

Only the one entry sector and the header sector of each GPT copy are
rewritten, and only when that copy's checksums were valid beforehand.
"""
import struct
import sys
import zlib

SECTOR = 4096
SUCCESSFUL, UNBOOTABLE, RETRY_MASK, ACTIVE = 0x40, 0x80, 0x38, 0x04


def read_at(f, lba, count=1):
    f.seek(lba * SECTOR)
    return bytearray(f.read(count * SECTOR))


class Gpt:
    def __init__(self, f, header_lba, label):
        self.f, self.header_lba, self.label = f, header_lba, label
        self.hdr = read_at(f, header_lba)
        self.ok_sig = self.hdr[:8] == b"EFI PART"
        if not self.ok_sig:
            return
        (self.hsize, self.hcrc) = struct.unpack_from("<II", self.hdr, 12)
        (self.my_lba, self.alt_lba) = struct.unpack_from("<QQ", self.hdr, 24)
        (self.ent_lba, self.nent, self.esize, self.ecrc) = struct.unpack_from("<QIII", self.hdr, 72)
        self.ent_sectors = -(-self.nent * self.esize // SECTOR)
        self.ents = read_at(f, self.ent_lba, self.ent_sectors)
        self.ok_hcrc = self.calc_hcrc() == self.hcrc
        self.ok_ecrc = self.calc_ecrc() == self.ecrc

    def calc_hcrc(self):
        h = bytearray(self.hdr[: self.hsize])
        h[16:20] = b"\0\0\0\0"
        return zlib.crc32(bytes(h)) & 0xFFFFFFFF

    def calc_ecrc(self):
        return zlib.crc32(bytes(self.ents[: self.nent * self.esize])) & 0xFFFFFFFF

    def valid(self):
        return self.ok_sig and self.ok_hcrc and self.ok_ecrc

    def find(self, name):
        for i in range(self.nent):
            e = self.ents[i * self.esize : (i + 1) * self.esize]
            if e[56:128].decode("utf-16-le").rstrip("\0") == name:
                return i
        return None

    def flags(self, idx):
        return self.ents[idx * self.esize + 48 + 6]

    def describe(self):
        if not self.ok_sig:
            return f"{self.label}: no GPT signature at LBA {self.header_lba}"
        out = [f"{self.label}: header LBA {self.header_lba}, entries LBA {self.ent_lba}, "
               f"{self.nent} x {self.esize} bytes, header crc {'ok' if self.ok_hcrc else 'BAD'}, "
               f"entries crc {'ok' if self.ok_ecrc else 'BAD'}"]
        for slot in "ab":
            i = self.find("boot_" + slot)
            if i is None:
                out.append(f"  boot_{slot}: not found")
                continue
            b = self.flags(i)
            out.append(f"  boot_{slot} (entry {i}): 0x{b:02x} priority={b & 3} active={bool(b & ACTIVE)} "
                       f"retry={(b & RETRY_MASK) >> 3} successful={bool(b & SUCCESSFUL)} unbootable={bool(b & UNBOOTABLE)}")
        return "\n".join(out)

    def mark(self, slot):
        i = self.find("boot_" + slot)
        off = i * self.esize + 48 + 6
        new = (self.ents[off] | SUCCESSFUL | RETRY_MASK) & ~UNBOOTABLE
        if new == self.ents[off]:
            return False
        self.ents[off] = new
        struct.pack_into("<I", self.hdr, 88, self.calc_ecrc())
        struct.pack_into("<I", self.hdr, 16, 0)
        struct.pack_into("<I", self.hdr, 16, self.calc_hcrc())
        sec = off // SECTOR
        self.f.seek((self.ent_lba + sec) * SECTOR)
        self.f.write(self.ents[sec * SECTOR : (sec + 1) * SECTOR])
        self.f.seek(self.header_lba * SECTOR)
        self.f.write(self.hdr)
        return True


def main():
    if len(sys.argv) not in (2, 4) or (len(sys.argv) == 4 and (sys.argv[2] != "--mark" or sys.argv[3] not in "ab")):
        sys.exit(__doc__)
    path, slot = sys.argv[1], sys.argv[3] if len(sys.argv) == 4 else None
    with open(path, "r+b" if slot else "rb") as f:
        primary = Gpt(f, 1, "primary")
        copies = [primary]
        if primary.ok_sig:
            copies.append(Gpt(f, primary.alt_lba, "backup"))
        for g in copies:
            print(g.describe())
        if not slot:
            return
        if not primary.valid():
            sys.exit("primary GPT is not valid; refusing to write")
        for g in copies:
            if not g.valid():
                print(f"{g.label}: checksums not valid, left untouched")
                continue
            if g.find("boot_" + slot) is None:
                print(f"{g.label}: boot_{slot} not found, left untouched")
                continue
            print(f"{g.label}: {'updated' if g.mark(slot) else 'already marked'}")
        f.flush()
    with open(path, "rb") as f:
        print("--- after")
        primary = Gpt(f, 1, "primary")
        print(primary.describe())
        if primary.ok_sig:
            print(Gpt(f, primary.alt_lba, "backup").describe())


if __name__ == "__main__":
    main()
