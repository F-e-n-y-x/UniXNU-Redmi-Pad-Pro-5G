#!/usr/bin/env python3
# Minimal dyld shared cache (split) reader: vm->file mapping, image symtabs.
import struct, sys, os, glob, mmap

class Cache:
    def __init__(self, base):
        self.base = base
        self.files = []  # (path, mm, mappings[(addr,size,fileoff)])
        paths = [base] + sorted(p for p in glob.glob(base + '.*')
                                if not p.endswith(('.map', '.atlas')))
        for p in paths:
            fd = open(p, 'rb'); mm = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_READ)
            if mm[:4] != b'dyld':
                continue
            moff, mcnt = struct.unpack_from('<II', mm, 0x10)
            maps = [struct.unpack_from('<QQQ', mm, moff + i * 32) for i in range(mcnt)]
            self.files.append((p, mm, maps))
        self.hdr = self.files[0][1]

    def v2f(self, addr):
        for p, mm, maps in self.files:
            for a, s, fo in maps:
                if a <= addr < a + s:
                    return p, mm, fo + addr - a
        raise KeyError(hex(addr))

    def read(self, addr, n):
        p, mm, o = self.v2f(addr); return mm[o:o + n]

    def images(self):
        mm = self.hdr
        ioff, icnt = struct.unpack_from('<II', mm, 0x1c0)
        for i in range(icnt):
            addr, mt, ino, poff, pad = struct.unpack_from('<QQQII', mm, ioff + i * 32)
            e = mm.find(b'\0', poff); yield addr, mm[poff:e].decode()

    def symbols(self, mh):
        h = self.read(mh, 32)
        ncmds = struct.unpack_from('<I', h, 16)[0]
        cmds = self.read(mh + 32, struct.unpack_from('<I', h, 20)[0])
        off = 0; segs = {}; symtab = None
        for _ in range(ncmds):
            cmd, sz = struct.unpack_from('<II', cmds, off)
            if cmd == 0x19:
                name = cmds[off + 8:off + 24].rstrip(b'\0').decode()
                vm, vs, fo = struct.unpack_from('<QQQ', cmds, off + 24)
                segs[name] = (vm, fo)
            elif cmd == 0x2:
                symtab = struct.unpack_from('<IIII', cmds, off + 8)
            off += sz
        le_vm, le_fo = segs['__LINKEDIT']
        symoff, nsyms, stroff, strsize = symtab
        syms = self.read(le_vm + symoff - le_fo, nsyms * 16)
        strs = self.read(le_vm + stroff - le_fo, strsize)
        for i in range(nsyms):
            sx, ty, sect, desc, val = struct.unpack_from('<IBBHQ', syms, i * 16)
            if ty & 0xe0 == 0 and (ty & 0x0e) == 0x0e and val:
                e = strs.find(b'\0', sx); yield val, strs[sx:e].decode(errors='replace')

if __name__ == '__main__':
    c = Cache(sys.argv[1])
    print('symbolFileUUID', c.hdr[0x190:0x1a0].hex(), 'localSyms', struct.unpack_from('<QQ', c.hdr, 0x48))
    want = sys.argv[2:]
    for mh, path in c.images():
        if any(path.endswith('/' + w) for w in want):
            for v, n in sorted(c.symbols(mh)):
                print(path.split('/')[-1], hex(v), n)
