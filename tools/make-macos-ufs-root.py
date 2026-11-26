#!/usr/bin/env python3
"""make-macos-ufs-root.py EXTRACT OUT_IMAGE: the "unixnu" partition image (sda22) with the macOS root, for fastboot flash unixnu.

Root = EXTRACT/recovery-tree-signed (rebuild-macos-root.py: the 26C5035r recovery root, pacstripped and ad-hoc signed) plus the
pacstripped dyld shared cache (EXTRACT/26C5035r__Mac17,11/dyld_shared_cache_arm64e*) as real files in /System/Library/dyld,
one of dyld's own cache paths, so the cache is on the process' root volume (the kernel's shared-region check). The cache is
ad-hoc re-signed here (adhoc-resign.py) and every CDHash (root + cache) goes into EXTRACT/trust.bundle as one module on top of
trust.bundle.orig. The tree is assembled on the ext4 image (host-work/apple-trees), not on NTFS.

Layout as the iPadOS make-ufs-root.py (SHNGROOT manifest, OS window = GPT disk with the container at LBA 34). SystemOS window:
a small placeholder GPT disk with an empty APFS container until the decrypted SystemOS cryptex is available; the macOS kernel
has no SystemOS union patch, so nothing mounts it. Prints the root volume UUID (root_match_volume_uuid) and the image size.
"""
import hashlib
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import uuid
import zlib
from pathlib import Path

W = Path('/root/unixnu-work')
HERE = Path(__file__).resolve().parent
MIB = 1 << 20
spec = importlib.util.spec_from_file_location('make_ramroot', W / 'UniXNU-ruan/scripts/make-ramroot.py')
mr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mr)


def run(*cmd):
    subprocess.run([str(c) for c in cmd], check=True)


def mkapfs(tree, manifest, image, slack=16384):
    plain = [a for d in ('sbin', 'usr/lib', 'usr/libexec') for a in ('--plain', d)]
    run(sys.executable, '-I', '-c', mr.MKAPFS_APPLE_RSRC, mr.MKAPFS, '-o', image, '-z', *plain, '--slack', slack,
        f'System={tree},manifest={manifest}')
    mr.fix_spaceman(image)
    size = image.stat().st_size
    if size % 4096:
        with open(image, 'ab') as f:
            f.write(bytes(4096 - size % 4096))
    return image.stat().st_size


def volume_uuid(container):
    with open(container, 'rb') as f:
        data = f.read(64 * MIB)
    hit = next(i for i in range(0, len(data), 4096) if data[i + 32:i + 36] == b'APSB')
    return str(uuid.UUID(bytes=data[hit + 240:hit + 256])).upper()


def gpt(lbas, first, last):
    """Protective MBR + primary and backup GPT (512-byte sectors) with one APFS partition first..last (make-ufs-root.py)."""
    entry = (uuid.UUID('7C3457EF-0000-11AA-AA11-00306543ECAC').bytes_le + uuid.uuid4().bytes_le + struct.pack('<QQQ', first, last, 0)
             + 'System'.encode('utf-16le').ljust(72, b'\0'))
    table = entry.ljust(128 * 128, b'\0')
    disk = uuid.uuid4().bytes_le

    def header(mine, other, entries_lba):
        h = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, mine, other, 34, lbas - 34, disk,
                                  entries_lba, 128, 128, zlib.crc32(table)))
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h)))
        return bytes(h).ljust(512, b'\0')
    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\0\x02\0', 0xEE, b'\xff\xff\xff', 1, min(lbas - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'
    return bytes(mbr) + header(1, lbas - 1, 2) + table, table + header(lbas - 1, 1, lbas - 33)


def window(container, size):
    """A GPT disk of the container: (bytes of the window, generator of its blocks)."""
    window_bytes = -(-(34 * 512 + size + 33 * 512) // 4096) * 4096
    head, tail = gpt(window_bytes // 512, 34, 34 + size // 512 - 1)

    def blocks():
        yield head + bytes(34 * 512 - len(head))
        with open(container, 'rb') as src:
            yield from iter(lambda: src.read(16 * MIB), b'')
        pad = window_bytes - 34 * 512 - size - len(tail)
        yield bytes(pad) + tail
    return window_bytes, blocks


def build_recovery_tree(x, work):
    """The recovery root plus the pacstripped cache (the default without FULL)."""
    tree = work / 'root'
    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir(parents=True)
    run('cp', '-a', f'{x / "recovery-tree-signed"}/.', tree)
    dyld = tree / 'System/Library/dyld'
    dyld.mkdir(parents=True, exist_ok=True)
    caches = sorted((x / '26C5035r__Mac17,11').glob('dyld_shared_cache_arm64e*'))
    caches = [c for c in caches if not c.name.endswith('.symbols') and not c.name.endswith('.map')]
    for c in caches:
        shutil.copyfile(c, dyld / c.name)
    run(sys.executable, HERE / 'adhoc-resign.py', dyld, work / 'cache.cdhash')

    manifest = work / 'root.manifest'
    lines = (x / 'recovery.manifest').read_text().splitlines()
    present = {l.split('\t')[3] for l in lines}
    for d in ('System', 'System/Library', 'System/Library/dyld'):
        if d not in present:
            lines.append(f'40755\t0\t0\t{d}\t')
    lines += [f'100644\t0\t0\tSystem/Library/dyld/{c.name}\t' for c in caches]
    manifest.write_text('\n'.join(lines) + '\n')
    hashes = (x / 'recovery-tree-signed.cdhash').read_text().split() + (work / 'cache.cdhash').read_text().split()
    return tree, manifest, hashes


def main():
    x, out = Path(sys.argv[1]), Path(sys.argv[2])
    work = W / 'apple-trees/macos-ufs'
    work.mkdir(parents=True, exist_ok=True)
    full = os.environ.get('FULL')   # make-macos-full-tree.py's folder: its root tree is used as it is
    if full:
        full = Path(full)
        manifest, tree = full / 'root.manifest', full / 'root'
        hashes = (full / 'root.cdhash').read_text().split()
    else:
        tree, manifest, hashes = build_recovery_tree(x, work)
    root = work / 'root.apfs'
    root_size = mkapfs(tree, manifest, root)
    root_uuid = volume_uuid(root)

    empty = work / 'systemos-empty'
    shutil.rmtree(empty, ignore_errors=True)
    empty.mkdir()
    (work / 'systemos.manifest').write_text('')
    sysimg = work / 'systemos.apfs'
    run(sys.executable, '-I', mr.MKAPFS, '-o', sysimg, '--slack', 256, f'System={empty},manifest={work / "systemos.manifest"}')
    sys_container = sysimg.stat().st_size

    os_bytes, os_blocks = window(root, root_size)
    sys_bytes, sys_blocks = window(sysimg, -(-sys_container // 4096) * 4096)
    os_at = MIB
    sys_at = os_at + -(-os_bytes // MIB) * MIB
    total = sys_at + -(-sys_bytes // MIB) * MIB
    entries = []
    with out.open('wb') as target:
        target.truncate(total)
        for name, offset, size, blocks in (('OS', os_at, os_bytes, os_blocks), ('SystemOS', sys_at, sys_bytes, sys_blocks)):
            digest, done = hashlib.sha256(), 0
            target.seek(offset)
            for data in blocks():
                digest.update(data)
                target.write(data)
                done += len(data)
            if done < size:   # the placeholder container may be shorter than its 4 KiB-rounded size
                digest.update(bytes(size - done)); target.write(bytes(size - done)); done = size
            assert done == size, f'{name}: {done} bytes, expected {size}'
            entries.append((name, offset, size, digest.digest()))
        header = bytearray(4096)
        header[0:8] = b'SHNGROOT'
        struct.pack_into('<IIQ', header, 8, 1, len(entries), total)
        for index, (name, offset, size, digest) in enumerate(entries):
            struct.pack_into('<32sQQII32s40s', header, 24 + 128 * index, name.encode(), offset, size, 512, 1, digest, bytes(40))
        header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
        target.seek(0)
        target.write(header)
    with out.open('rb') as f:
        h = f.read(4096)
        assert h[:8] == b'SHNGROOT' and hashlib.sha256(h[:4064]).digest() == h[4064:]
        f.seek(os_at + 512); assert f.read(8) == b'EFI PART'
        f.seek(os_at + 34 * 512 + 32); assert f.read(4) == b'NXSB'
    assert total <= 34359717888, f'{total} bytes do not fit the 32 GiB unixnu partition'

    orig = x / 'trust.bundle.orig'
    run(sys.executable, W / 'UniXNU/scripts/trust-append-module.py', orig, x / 'trust.bundle', *hashes)
    (out.with_suffix('.uuid')).write_text(root_uuid + '\n')
    print(f'{out}: {total} bytes ({total / 2**30:.2f} GiB), root volume {root_uuid}, {len(hashes)} CDHashes in the trust bundle')


if __name__ == '__main__':
    main()
