#!/usr/bin/env python3
"""Count non-HINT PAC instructions in a kernel collection: per fileset entry code sections and over every executable root segment."""
import struct, sys, collections
sys.path.insert(0, '/root/unixnu-work/UniXNU-ruan-macos/tools')
from insn import STRATEGIES, rewrite, PATCH, TRAP
d = open(sys.argv[1], 'rb').read()
def cmds(base):
    n, = struct.unpack_from('<I', d, base + 16); o = base + 32
    for _ in range(n):
        c, l = struct.unpack_from('<II', d, o); yield c, o; o += l
def count(start, size):
    c = collections.Counter()
    for (w,) in struct.iter_unpack('<I', d[start:start + size - size % 4]):
        s, _ = rewrite(w)
        if s != 'keep': c[s] += 1
    return c
print('top subtype %#x' % struct.unpack_from('<I', d, 8))
tot = collections.Counter(); seg = collections.Counter()
for c, o in cmds(0):
    if c == 0x80000035:
        hdr, = struct.unpack_from('<Q', d, o + 16); na, = struct.unpack_from('<I', d, o + 24)
        name = d[o + na:d.index(b'\0', o + na)].decode()
        sub, = struct.unpack_from('<I', d, hdr + 8)
        e = collections.Counter()
        for c2, o2 in cmds(hdr):
            if c2 == 0x19:
                ns, = struct.unpack_from('<I', d, o2 + 64)
                for i in range(ns):
                    s = o2 + 72 + 80 * i
                    size, off = struct.unpack_from('<QI', d, s + 40); fl, = struct.unpack_from('<I', d, s + 64)
                    if fl & 0x80000400 and off: e += count(off, size)
        tot += e
        if name == 'com.apple.kernel' or e: print(f'{name:60s} sub={sub:#x} {dict(e)}') if '-v' in sys.argv or name == 'com.apple.kernel' else None
    if c == 0x19:
        segname = d[o + 8:o + 24].rstrip(b'\0').decode()
        fo, fs = struct.unpack_from('<QQ', d, o + 40); ip, = struct.unpack_from('<I', d, o + 60)
        if ip & 4 and fs: seg[segname] = count(fo, fs)
print('entries code sections:', dict(tot))
for k, v in seg.items(): print('root exec segment', k, dict(v))
