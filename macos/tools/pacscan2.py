import struct, sys, collections
sys.path.insert(0, '/root/unixnu-work/UniXNU-ruan-macos/tools')
from insn import rewrite
d = open(sys.argv[1], 'rb').read()
def cmds(base):
    n, = struct.unpack_from('<I', d, base + 16); o = base + 32
    for _ in range(n):
        c, l = struct.unpack_from('<II', d, o); yield c, o; o += l
def count(start, size):
    n = 0
    for (w,) in struct.iter_unpack('<I', d[start:start + size - size % 4]):
        if rewrite(w)[0] != 'keep': n += 1
    return n
for c, o in cmds(0):
    if c != 0x80000035: continue
    hdr, = struct.unpack_from('<Q', d, o + 16); na, = struct.unpack_from('<I', d, o + 24)
    name = d[o + na:d.index(b'\0', o + na)].decode()
    secs = segs = 0; other = []
    for c2, o2 in cmds(hdr):
        if c2 != 0x19: continue
        sn = d[o2+8:o2+24].rstrip(b'\0').decode(); fo, fs = struct.unpack_from('<QQ', d, o2 + 40); ip, = struct.unpack_from('<I', d, o2 + 60)
        if ip & 4: segs += count(fo, fs)
        ns, = struct.unpack_from('<I', d, o2 + 64)
        for i in range(ns):
            s = o2 + 72 + 80 * i
            size, off = struct.unpack_from('<QI', d, s + 40); fl, = struct.unpack_from('<I', d, s + 64)
            if fl & 0x80000400 and off: secs += count(off, size)
            elif ip & 4: other.append((d[s:s+16].rstrip(b'\0').decode(), hex(fl)))
    if segs != secs or other: print(name, segs, secs, other)
