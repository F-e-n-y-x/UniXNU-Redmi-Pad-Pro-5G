#!/usr/bin/env python3
"""adhoc-resign.py TREE OUT.cdhash: make every Mach-O in TREE ad-hoc signed, in place, and write their CDHashes (one per line).

pacstrip rewrites code but leaves Apple's CMS signature, which no longer matches, so XNU refuses the binaries as "completely
unsigned code" (boot 5). For every slice with LC_CODE_SIGNATURE this recomputes each CodeDirectory's code-page hashes, sets
CS_ADHOC, empties the CMS wrapper (slot 0x10000), and records the SHA-256 CDHash (first 20 bytes) of the slot-0 CodeDirectory.
Those hashes go into the trust bundle (trust-append-module.py), so the kernel treats the binaries as platform code.
Entitlements, requirements and Info.plist blobs are untouched, so their special-slot hashes stay valid.
"""
import hashlib
import struct
import sys
from pathlib import Path

CSMAGIC_EMBEDDED = 0xfade0cc0
CSMAGIC_CD = 0xfade0c02
CSMAGIC_WRAPPER = 0xfade0b01
CS_ADHOC = 0x2
HASHES = {1: hashlib.sha1, 2: hashlib.sha256, 3: hashlib.sha256, 4: hashlib.sha384}


def resign_slice(d: bytearray, base: int) -> bytes | None:
    """Re-sign the thin Mach-O at d[base:]; returns the slot-0 CDHash, or None without a signature."""
    if struct.unpack_from('<I', d, base)[0] != 0xfeedfacf:
        return None
    ncmds = struct.unpack_from('<I', d, base + 16)[0]
    off, sig = base + 32, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x1d:
            sig = struct.unpack_from('<II', d, off + 8)
        off += size
    if not sig:
        return None
    return resign_superblob(d, base, base + sig[0])


def resign_superblob(d: bytearray, base: int, sb: int) -> bytes | None:
    """Re-sign the superblob at d[sb:] whose code pages start at d[base:]; returns the CDHash the kernel uses."""
    magic, _, count = struct.unpack_from('>III', d, sb)
    if magic != CSMAGIC_EMBEDDED:
        raise ValueError(f'superblob magic {magic:#x}')
    cdhash = None
    for i in range(count):
        slot, boff = struct.unpack_from('>II', d, sb + 12 + 8 * i)
        blob = sb + boff
        bmagic, blen = struct.unpack_from('>II', d, blob)
        if bmagic == CSMAGIC_WRAPPER:
            struct.pack_into('>II', d, blob, CSMAGIC_WRAPPER, 8)   # empty CMS: ad-hoc
        elif bmagic == CSMAGIC_CD:
            _, flags, hoff, _, _, ncode, limit, hsize, htype, _, pshift = struct.unpack_from('>IIIIIIIBBBB', d, blob + 8)
            h, page = HASHES[htype], 1 << pshift
            for n in range(ncode):
                start = base + n * page
                end = base + min(limit, (n + 1) * page)
                d[blob + hoff + n * hsize:blob + hoff + (n + 1) * hsize] = h(d[start:end]).digest()[:hsize]
            struct.pack_into('>I', d, blob + 12, flags | CS_ADHOC)
            if slot == 0:
                cdhash = hashlib.sha256(d[blob:blob + blen]).digest()[:20] if htype == 2 else None
            elif slot >= 0x1000 and htype == 2:   # alternate SHA-256 CodeDirectory: that is the hash the kernel uses
                cdhash = hashlib.sha256(d[blob:blob + blen]).digest()[:20]
    return cdhash


def resign_file(path: Path) -> list[bytes]:
    d = bytearray(path.read_bytes())
    if len(d) < 32:
        return []
    magic = struct.unpack_from('>I', d, 0)[0]
    if d[:7] == b'dyld_v1':   # a dyld shared (sub)cache: its signature covers the file up to codeSignatureOffset
        sig_off, sig_size = struct.unpack_from('<QQ', d, 0x28)
        h = resign_superblob(d, 0, sig_off) if sig_size else None
        if h:
            path.write_bytes(d)
        return [h] if h else []
    if magic == 0xcafebabe:
        n = struct.unpack_from('>I', d, 4)[0]
        bases = [struct.unpack_from('>IIIII', d, 8 + 20 * i)[2] for i in range(n)]
    elif struct.unpack_from('<I', d, 0)[0] == 0xfeedfacf:
        bases = [0]
    else:
        return []
    hashes = [h for h in (resign_slice(d, b) for b in bases) if h]
    if hashes:
        path.write_bytes(d)
    return hashes


def self_test():
    """A fake one-page Mach-O with a CD and a CMS wrapper: code hash, ad-hoc flag, empty CMS and CDHash come out right."""
    page, code = 4096, bytes(range(256)) * 16
    cd = bytearray(struct.pack('>IIIIIIIIIBBBBI', CSMAGIC_CD, 0, 0x20400, 0, 0, 0, 0, 1, page, 32, 2, 0, 12, 0))
    cd += bytes(8)                       # padding; the code hash goes right after it
    hoff = len(cd)
    struct.pack_into('>I', cd, 16, hoff)
    struct.pack_into('>I', cd, 4, hoff + 32)
    cd += bytes(32)
    cms = struct.pack('>II', CSMAGIC_WRAPPER, 64) + bytes(56)
    sb = struct.pack('>III', CSMAGIC_EMBEDDED, 12 + 16 + len(cd) + len(cms), 2) + struct.pack('>II', 0, 28) + \
        struct.pack('>II', 0x10000, 28 + len(cd))
    hdr = struct.pack('<IiiIIIII', 0xfeedfacf, 0x100000c, 0, 2, 1, 16, 0, 0) + struct.pack('<IIII', 0x1d, 16, page, len(sb) + len(cd) + len(cms))
    d = bytearray(hdr + code[len(hdr):page] + sb + cd + cms)
    h = resign_slice(d, 0)
    blob = page + 28
    assert d[blob + hoff:blob + hoff + 32] == hashlib.sha256(d[:page]).digest()
    assert struct.unpack_from('>I', d, blob + 12)[0] & CS_ADHOC
    assert struct.unpack_from('>II', d, page + 28 + len(cd)) == (CSMAGIC_WRAPPER, 8)
    assert h == hashlib.sha256(d[blob:blob + hoff + 32]).digest()[:20]


if __name__ == '__main__':
    self_test()
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    tree, out = Path(sys.argv[1]), Path(sys.argv[2])
    hashes, files = [], 0
    for p in sorted(tree.rglob('*')):
        if p.is_file() and not p.is_symlink():
            got = resign_file(p)
            files += bool(got)
            hashes += got
    out.write_text(''.join(h.hex() + '\n' for h in sorted(set(hashes))))
    print(f'{files} files re-signed ad hoc, {len(set(hashes))} CDHashes -> {out}')
