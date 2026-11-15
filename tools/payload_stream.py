#!/usr/bin/env python3
"""Streaming extractor for Android A/B full-OTA payload.bin.
Reads payload from stdin (sequentially), writes <outdir>/<partition>.img.
Usage: payload_stream.py list < payload.bin
       payload_stream.py extract OUTDIR [part,part...] < payload.bin
Verifies every operation's data sha256 and each partition's final sha256."""
import sys, struct, hashlib, lzma, bz2, subprocess, os

def rd(f, n):
    b = bytearray()
    while len(b) < n:
        c = f.read(n - len(b))
        if not c: raise EOFError("short read")
        b += c
    return bytes(b)

def varint(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if x < 0x80: return r, i

def fields(b):
    i = 0; out = []
    while i < len(b):
        k, i = varint(b, i); fn, wt = k >> 3, k & 7
        if wt == 0: v, i = varint(b, i)
        elif wt == 2:
            l, i = varint(b, i); v = b[i:i+l]; i += l
        elif wt == 1: v = b[i:i+8]; i += 8
        elif wt == 5: v = b[i:i+4]; i += 4
        else: raise ValueError(wt)
        out.append((fn, v))
    return out

def extent(b):
    d = dict(fields(b)); return d.get(1, 0), d.get(2, 0)

def parse_manifest(m):
    bs = 4096; parts = []
    for fn, v in fields(m):
        if fn == 3: bs = v
        elif fn == 13:
            p = {'name': None, 'ops': [], 'size': None, 'hash': None}
            for f2, v2 in fields(v):
                if f2 == 1: p['name'] = v2.decode()
                elif f2 == 7:
                    d = dict(fields(v2)); p['size'] = d.get(1); p['hash'] = d.get(2)
                elif f2 == 8:
                    op = {'type': 0, 'off': 0, 'len': 0, 'dst': [], 'sha': None}
                    for f3, v3 in fields(v2):
                        if f3 == 1: op['type'] = v3
                        elif f3 == 2: op['off'] = v3
                        elif f3 == 3: op['len'] = v3
                        elif f3 == 6: op['dst'].append(extent(v3))
                        elif f3 == 8: op['sha'] = v3
                    p['ops'].append(op)
            parts.append(p)
    return bs, parts

def main():
    f = sys.stdin.buffer
    hdr = rd(f, 24)
    assert hdr[:4] == b'CrAU', 'bad magic'
    ver, msize = struct.unpack('>QQ', hdr[4:20]); sigsize, = struct.unpack('>I', hdr[20:24])
    manifest = rd(f, msize); rd(f, sigsize)
    bs, parts = parse_manifest(manifest)
    mode = sys.argv[1]
    if mode == 'list':
        tot = 0
        for p in parts:
            types = sorted({o['type'] for o in p['ops']})
            print(f"{p['name']:24s} {p['size']:>14,d}  ops={len(p['ops'])} types={types}")
            tot += p['size']
        print(f"TOTAL {tot:,d} bytes, block {bs}")
        return
    outdir = sys.argv[2]; want = set(sys.argv[3].split(',')) if len(sys.argv) > 3 else None
    os.makedirs(outdir, exist_ok=True)
    allops = []; fh = {}
    for p in parts:
        if want and p['name'] not in want: continue
        if any(o['type'] not in (0, 1, 6, 8, 14) for o in p['ops']):
            sys.exit(f"{p['name']}: non-full operation, not a full OTA")
        out = open(os.path.join(outdir, p['name'] + '.img'), 'wb'); out.truncate(p['size']); fh[p['name']] = out
        for o in p['ops']: allops.append((o['off'], p['name'], o))
    allops.sort(key=lambda x: (x[2]['len'] == 0, x[0]))
    pos = 0; done = 0
    for off, name, o in allops:
        out = fh[name]
        if o['type'] == 6:  # ZERO: file already zero (sparse)
            continue
        if off < pos: sys.exit(f"out of order data at {off} < {pos}")
        while pos < off:
            n = min(1 << 20, off - pos); rd(f, n); pos += n
        data = rd(f, o['len']); pos += o['len']
        if o['sha'] and hashlib.sha256(data).digest() != o['sha']:
            sys.exit(f"{name}: op data hash mismatch at {off}")
        t = o['type']
        if t == 1: data = bz2.decompress(data)
        elif t == 8: data = lzma.decompress(data)
        elif t == 14: data = subprocess.run(['zstd', '-dc'], input=data, capture_output=True, check=True).stdout
        i = 0
        for sb, nb in o['dst']:
            n = nb * bs; out.seek(sb * bs); out.write(data[i:i+n]); i += n
        done += 1
        if done % 2000 == 0:
            print(f"  {done}/{len(allops)} ops, {pos/1e9:.2f} GB read", file=sys.stderr, flush=True)
    for name, out in fh.items(): out.close()
    bad = False
    for p in parts:
        if p['name'] not in fh: continue
        h = hashlib.sha256()
        with open(os.path.join(outdir, p['name'] + '.img'), 'rb') as g:
            while True:
                c = g.read(1 << 22)
                if not c: break
                h.update(c)
        ok = h.digest() == p['hash']
        bad |= not ok
        print(f"{'OK ' if ok else 'BAD'} {p['name']} {h.hexdigest()}")
    sys.exit(1 if bad else 0)

main()
