#!/usr/bin/env python3
"""[PART=i/n] extract-apfs-tree.py IMAGE DEST MANIFEST [VOLUME]: copy an APFS volume (raw container or .dmg) to DEST, plus a mkapfs
manifest of every entry (octal mode with type bits, uid, gid, path, symlink target), so owners survive the copy.

Uses HakiOS tools/apfs/apfs.py (read only). Sealed volumes (the decrypted IPSW OS/SystemOS images) store their file-system
tree nodes with a zeroed object header (integrity comes from the seal's hashes), which apfs.py's checksum check rejects; such
blocks are accepted here. Hard links become separate files (mkapfs makes none); device nodes, fifos and sockets are skipped.
"""
import os
import stat
import struct
import sys

sys.path.insert(0, os.environ.get('APFS_TOOLS', '/root/hakios-garnet/tools/apfs'))
import apfs  # noqa: E402

_checksum_ok = apfs.checksum_ok
apfs.checksum_ok = lambda b: _checksum_ok(b) or b[:32] == bytes(32)

# In a sealed volume's hashed file-system tree an index entry's child is stored relative to the tree's root object id
# (verified on 26C5035r SystemOS: stored + root oid resolves to the block whose SHA-256 the entry holds; absolute ids land on
# unrelated nodes).
BTREE_HASHED = 0x80
_btree_init = apfs.Btree.__init__


def _btree_init_rel(self, blocks, root_oid, resolve=None, name='btree'):
    _btree_init(self, blocks, root_oid, resolve, name)
    self.child_base = root_oid if (self.bt_flags & BTREE_HASHED and not self.physical and resolve) else 0


apfs.Btree.__init__ = _btree_init_rel
apfs.Btree.child = lambda self, node, i: self.node(node.child_oid(i) + self.child_base)

# Sealed volumes keep file extents out of the file-system tree, in a physical "fext" tree (apfs_fext_tree_oid at
# superblock offset 0x408; keys: private id, logical offset; values: length and flags, physical block), so apfs.py's
# extents() finds none and every data stream reads as zeros.
_extents = apfs.Volume.extents


def _extents_sealed(self, stream_id):
    if not self.sealed:
        return _extents(self, stream_id)
    if getattr(self, '_fext', None) is None:
        oid = struct.unpack_from('<Q', self.c.blocks.get(self.addr), 0x408)[0]
        self._fext = apfs.Btree(self.c.blocks, oid, name='fext')
    out = []
    for k, v in self._fext.iter_from(lambda k: struct.unpack_from('<Q', k, 0)[0] < stream_id):
        sid, logical = struct.unpack_from('<QQ', k, 0)
        if sid != stream_id:
            break
        len_flags, phys = struct.unpack_from('<QQ', v, 0)
        out.append((logical, len_flags & apfs.J_FILE_EXTENT_LEN_MASK, phys))
    return out


apfs.Volume.extents = _extents_sealed

# LZVN (7/8), LZFSE (11/12) and LZBITMAP (13/14) through macos/tools/decmp (libdecmp.so: lzfse + apfs-fuse's LZBITMAP).
# Resource-fork variants of these types start with a little-endian uint32 offset table, one entry per 64 KiB chunk plus the
# end (as apfs-fuse reads them), not zlib's HFS resource-fork layout. A chunk whose first byte is the type's "stored" marker
# (0x06 LZVN, 0xff LZBITMAP/LZFSE) holds the bytes as they are. Every chunk must decode to exactly its expected length.
_lib = None
_STORED = {7: 0x06, 8: 0x06, 11: 0xff, 12: 0xff, 13: 0xff, 14: 0xff}


def _decoder(ctype):
    global _lib
    if _lib is None:
        import ctypes
        _lib = ctypes.CDLL(os.environ.get('DECMP_LIB', '/root/unixnu-work/macos-build/libdecmp.so'))
        for f in (_lib.decmp_lzvn, _lib.decmp_lzfse, _lib.decmp_lzbitmap):
            f.restype = ctypes.c_size_t
            f.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    return {7: _lib.decmp_lzvn, 8: _lib.decmp_lzvn, 11: _lib.decmp_lzfse, 12: _lib.decmp_lzfse,
            13: _lib.decmp_lzbitmap, 14: _lib.decmp_lzbitmap}[ctype]


def _chunk(ctype, src, n):
    import ctypes
    if src[:1] == bytes([_STORED[ctype]]):
        out = src[1:1 + n]
    else:
        buf = ctypes.create_string_buffer(n + 4096)      # slack: the decoders may run a few bytes past while copying
        got = _decoder(ctype)(buf, n, src, len(src))
        out = buf.raw[:got]
    if len(out) != n:
        raise apfs.ApfsError(f'type {ctype}: chunk decoded to {len(out)} bytes, expected {n}')
    return out


_decompress = apfs.Inode.decompress


def _decompress_more(self, hdr):
    _magic, ctype, usize = struct.unpack_from('<IIQ', hdr, 0)
    if ctype in (7, 11, 13):
        return _chunk(ctype, hdr[16:], usize)
    if ctype in (8, 10, 12, 14):
        rsrc = self.vol.xattr_bytes(self.obj_id, apfs.RSRC_FORK_XATTR_NAME, self.xattrs)
        if rsrc is None:
            raise apfs.ApfsError(f'inode {self.obj_id}: type {ctype} without a resource fork')
        count = -(-usize // 0x10000)
        offs = struct.unpack_from(f'<{count + 1}I', rsrc, 0)
        out = bytearray()
        for k in range(count):
            n = min(0x10000, usize - len(out))
            src = rsrc[offs[k]:offs[k + 1]]
            out += src[1:1 + n] if ctype == 10 else _chunk(ctype, src, n)
        return bytes(out)
    return _decompress(self, hdr)


apfs.Inode.decompress = _decompress_more


def extract(image, dest, manifest, want=None, part=0, parts=1):
    """With parts > 1 this process writes only the files with file id % parts == part (every process walks the whole tree
    and makes the directories; part 0 also writes the manifest and the symlinks), so several can run side by side."""
    c = apfs.Container(image)
    v = apfs.pick_volume(c, want)
    os.makedirs(dest, exist_ok=True)
    lines, dirs, skipped = [], [], 0
    for p, _name, fid, _dt in apfs.walk(v, apfs.ROOT_DIR_INO_NUM):
        ino = v.inode(fid)
        if ino is None:
            skipped += 1
            continue
        rel = p.lstrip('/')
        out = os.path.join(dest, rel)
        target = ''
        if ino.is_dir:
            os.makedirs(out, exist_ok=True)
            dirs.append((out, ino.mode & 0o7777))
        elif ino.is_link:
            target = (ino.symlink_target_bytes() or b'').decode('utf-8', 'surrogateescape')
            if part == 0:
                if os.path.lexists(out):
                    os.unlink(out)
                os.symlink(target, out)
        elif ino.is_reg:
            if fid % parts != part:
                lines.append(f'{ino.mode:o}\t{ino.owner}\t{ino.group}\t{rel}\t')
                continue
            with open(out, 'wb') as f:
                f.write(ino.contents())
            os.chmod(out, (ino.mode & 0o7777) | 0o200)
        else:
            skipped += 1
            continue
        lines.append(f'{ino.mode:o}\t{ino.owner}\t{ino.group}\t{rel}\t{target}')
    if part == 0:
        for out, mode in reversed(dirs):
            os.chmod(out, mode | 0o700)
        with open(manifest, 'w') as f:
            f.write('\n'.join(lines) + '\n')
    print(f'volume {v.volname!r} -> {dest}: {len(lines)} entries, {skipped} skipped')
    c.close()


def self_test():
    assert apfs.checksum_ok(bytes(4096))          # a sealed-volume node with a zeroed header
    assert not apfs.checksum_ok(b'\1' * 4096)


if __name__ == '__main__':
    self_test()
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    a = sys.argv[1:]
    part, parts = (int(x) for x in os.environ.get('PART', '0/1').split('/'))   # PART=i/n: one of n side-by-side runs
    extract(a[0], a[1], a[2], a[3] if len(a) > 3 else None, part, parts)
