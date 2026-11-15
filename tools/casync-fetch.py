#!/usr/bin/env python3
"""Rebuild a disk image from a casync index (.caibx) and an HTTP chunk store.

Same job as `casync extract`, but fetches chunks in parallel, which matters
for images made of tens of thousands of small chunks.

Usage: casync-fetch.py INDEX.caibx STORE_URL OUTPUT.img [--sha256 HEX] [--workers N]
"""
import argparse
import collections
import concurrent.futures
import hashlib
import os
import struct
import sys
import threading
import time

import requests
import zstandard

ap = argparse.ArgumentParser()
ap.add_argument("index")
ap.add_argument("store")
ap.add_argument("output")
ap.add_argument("--sha256", default="")
ap.add_argument("--workers", type=int, default=32)
a = ap.parse_args()

d = open(a.index, "rb").read()
if struct.unpack_from("<Q", d, 8)[0] != 0x96824D9C7B129FF9 or struct.unpack_from("<Q", d, 56)[0] != 0xE75B9E112F17417D:
    sys.exit("not a casync blob index")
entries = (len(d) - 48 - 16 - 40) // 40
where = collections.defaultdict(list)
prev = 0
for i in range(entries):
    off = 64 + i * 40
    end = struct.unpack_from("<Q", d, off)[0]
    where[d[off + 8:off + 40].hex()].append((prev, end - prev))
    prev = end
size = prev
print("image %.2f GiB, %d chunks, %d unique" % (size / 2**30, entries, len(where)), flush=True)

fd = os.open(a.output, os.O_RDWR | os.O_CREAT, 0o644)
os.ftruncate(fd, size)
done_path = a.output + ".done"
done = set(open(done_path).read().split()) if os.path.exists(done_path) else set()
done_log = open(done_path, "a")
lock = threading.Lock()
local = threading.local()
count = [len(done), 0]


def fetch(digest):
    if digest in done:
        return
    s = getattr(local, "s", None)
    if s is None:
        s = local.s = requests.Session()
        s.headers["User-Agent"] = "casync-fetch/1.0"
    url = "%s/%s/%s.cacnk" % (a.store.rstrip("/"), digest[:4], digest)
    for attempt in range(8):
        try:
            r = s.get(url, timeout=60)
            r.raise_for_status()
            data = zstandard.ZstdDecompressor().decompress(r.content, max_output_size=64 << 20)
            break
        except Exception as e:
            if attempt == 7:
                raise RuntimeError("%s: %s" % (url, e))
            time.sleep(min(8, 2 ** attempt))
    for off, length in where[digest]:
        if len(data) != length:
            raise RuntimeError("chunk %s: wrong length" % digest)
        if data.count(0) != length:
            os.pwrite(fd, data, off)
    with lock:
        done_log.write(digest + "\n")
        count[0] += 1
        count[1] += len(r.content)
        if count[0] % 4000 == 0:
            done_log.flush()
            print("%d/%d chunks, %.0f MiB fetched" % (count[0], len(where), count[1] / 2**20), flush=True)


with concurrent.futures.ThreadPoolExecutor(a.workers) as ex:
    for f in concurrent.futures.as_completed([ex.submit(fetch, k) for k in where]):
        f.result()
done_log.close()
os.fsync(fd)
os.close(fd)
if a.sha256:
    h = hashlib.sha256()
    with open(a.output, "rb") as f:
        for block in iter(lambda: f.read(8 << 20), b""):
            h.update(block)
    if h.hexdigest() != a.sha256:
        sys.exit("sha256 mismatch: " + h.hexdigest())
    print("sha256 ok", flush=True)
print("done", flush=True)
