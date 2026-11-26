#!/usr/bin/env python3
"""apply-swrender.py SRC_DYLD_DIR OUT_DIR PATCHES_TXT [POLICY_PREFIX ...]

Copy each subcache that patches.txt names from SRC_DYLD_DIR to OUT_DIR (SRC is only read), check every
`before` (and that vmaddr maps to fileoff in that subcache), write `after`, and recompute the CodeDirectory
code-slot hashes the same way UniXNU-ruan-macos/tools/pacstrip-cache.py does. Re-running on OUT_DIR is
refused (before bytes no longer match), so a half-patched cache cannot happen silently.
With POLICY_PREFIX args only lines whose policy starts with one of them are applied.
"""
import hashlib, os, shutil, struct, sys

sys.path.insert(0, '/root/unixnu-work/UniXNU-ruan-macos/tools')
from importlib.machinery import SourceFileLoader
pacstrip = SourceFileLoader('pacstrip', '/root/unixnu-work/UniXNU-ruan-macos/tools/pacstrip-cache.py').load_module()


def f2v_ok(data, fileoff, vmaddr):
    moff, mcnt = struct.unpack_from('<II', data, 0x10)
    for i in range(mcnt):
        a, s, fo = struct.unpack_from('<QQQ', data, moff + i * 32)
        if fo <= fileoff < fo + s:
            return a + fileoff - fo == vmaddr
    return False


def main():
    src, out, ptxt, *only = sys.argv[1:]
    patches = {}
    for line in open(ptxt):
        if not line.strip() or line.startswith('#'):
            continue
        f, off, before, after, vm, policy = line.rstrip('\n').split(',', 5)
        if only and not any(policy.startswith(p) for p in only):
            continue
        b, a = bytes.fromhex(before), bytes.fromhex(after)
        assert len(b) == len(a), policy
        patches.setdefault(f, []).append((int(off, 16), b, a, int(vm, 16), policy))
    os.makedirs(out, exist_ok=True)
    for f, plist in sorted(patches.items()):
        dst = os.path.join(out, f)
        if not os.path.exists(dst):
            shutil.copyfile(os.path.join(src, f), dst)
        data = bytearray(open(dst, 'rb').read())
        # check all before writing anything
        for off, b, a, vm, pol in plist:
            assert f2v_ok(data, off, vm), '%s: vmaddr %x != fileoff %x (%s)' % (f, vm, off, pol)
            assert data[off:off + len(b)] == b, '%s+%x: have %s, want %s (%s)' % (f, off, data[off:off + len(b)].hex(), b.hex(), pol)
        pages = set()
        for off, b, a, vm, pol in plist:
            data[off:off + len(a)] = a
            pages.update(range(off >> 12, (off + len(a) - 1 >> 12) + 1))
            print('  %s+%x %s -> %s  %s' % (f, off, b.hex(), a.hex(), pol))
        slots = pacstrip.rehash(data)
        assert pacstrip.rehash(data) == 0  # signature now consistent with every page
        with open(dst, 'r+b') as fh:
            fh.write(data)
        print('%s: %d edits, %d code-slot hashes updated, size %d, sha256 %s'
              % (f, len(plist), slots, len(data), hashlib.sha256(data).hexdigest()))


if __name__ == '__main__':
    main()
