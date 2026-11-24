"""featscan.py restricted to the instruction sections of the collection's fileset entries: {entry: {mnemonic: n}}."""
import bisect, collections, struct, sys
sys.path.insert(0, '/root/unixnu-work/UniXNU-ruan-macos/tools')
from featscan import disassemble, UNKNOWN
path = sys.argv[1]
d = open(path, 'rb').read()
def cmds(base):
    n, = struct.unpack_from('<I', d, base + 16); o = base + 32
    for _ in range(n):
        c, l = struct.unpack_from('<II', d, o); yield c, o; o += l
ranges = []
for c, o in cmds(0):
    if c != 0x80000035: continue
    hdr, = struct.unpack_from('<Q', d, o + 16); na, = struct.unpack_from('<I', d, o + 24)
    name = d[o + na:d.index(b'\0', o + na)].decode()
    for c2, o2 in cmds(hdr):
        if c2 != 0x19: continue
        ns, = struct.unpack_from('<I', d, o2 + 64)
        for i in range(ns):
            s = o2 + 72 + 80 * i
            addr, size, off = struct.unpack_from('<QQI', d, s + 32); fl, = struct.unpack_from('<I', d, s + 64)
            if fl & 0x80000400 and off: ranges.append((addr, addr + size, name))
ranges.sort(); starts = [r[0] for r in ranges]
def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return ranges[i][2] if i >= 0 and a < ranges[i][1] else None
have, want = disassemble(path, 'cortex-a78'), disassemble(path, 'apple-a13')
print('disassembled words', len(have), 'in entries', sum(1 for (_, a) in have if owner(a)))
res = collections.defaultdict(collections.Counter)
for key, m in have.items():
    src = want.get(key, UNKNOWN)
    if src not in (UNKNOWN, m):
        o = owner(key[1])
        res[o or '(outside entries)'][src] += 1
for k, v in sorted(res.items(), key=lambda kv: -sum(kv[1].values())): print(k, dict(v))
