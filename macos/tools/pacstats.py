import struct, sys, collections
sys.path.insert(0, '/root/unixnu-work/UniXNU-ruan-macos/tools')
from insn import STRATEGIES
d = open(sys.argv[1], 'rb').read()
names = ['RETA','ERETA','BRA','BRAZ','BLRA','BLRAZ','PACDP','PACGA','LDRA','FJCVTZS']
def cmds(base):
    n, = struct.unpack_from('<I', d, base + 16); o = base + 32
    for _ in range(n):
        c, l = struct.unpack_from('<II', d, o); yield c, o; o += l
tot = collections.Counter(); rare = []
for c, o in cmds(0):
    if c != 0x80000035: continue
    hdr, = struct.unpack_from('<Q', d, o + 16); na, = struct.unpack_from('<I', d, o + 24)
    name = d[o + na:d.index(b'\0', o + na)].decode()
    for c2, o2 in cmds(hdr):
        if c2 != 0x19: continue
        ns, = struct.unpack_from('<I', d, o2 + 64)
        for i in range(ns):
            s = o2 + 72 + 80 * i
            size, off = struct.unpack_from('<QI', d, s + 40); fl, = struct.unpack_from('<I', d, s + 64)
            if not (fl & 0x80000400 and off): continue
            for k, (w,) in enumerate(struct.iter_unpack('<I', d[off:off + size - size % 4])):
                for j, (m, v, h) in enumerate(STRATEGIES):
                    if w & m == v:
                        r = h(w)
                        if r[0] != 'keep':
                            tot[names[j]] += 1
                            if names[j] in ('PACGA','LDRA','FJCVTZS') or (names[j]=='PACDP' and (w>>10&0x3f) >= 0x10): rare.append((name, off + 4*k, hex(w), names[j]))
                        break
print(dict(tot)); print(len(rare))
for r in [x for x in rare if x[3]=="PACGA"]: print(r)
for r in collections.Counter((a, n) for a, _, _, n in rare).most_common(40): print(r)
