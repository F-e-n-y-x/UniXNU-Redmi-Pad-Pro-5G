#!/usr/bin/env python3
"""make-macos-full-tree.py FULL CACHE_DIR: the full macOS 27.3 root tree for sda22, from the decrypted IPSW volumes.

FULL is the folder extract-apfs-tree.py filled (FULL/os + os.manifest: the OS system volume "Macintosh HD"; FULL/systemos +
systemos.manifest: the SystemOS cryptex). CACHE_DIR holds the pacstripped dyld shared cache of the same build
(macos-extract/26C5035r__Mac17,11). Writes FULL/root, FULL/root.manifest and FULL/root.cdhash:

1. root = os, with systemos at /System/Volumes/Preboot/Cryptexes/OS: the system volume links into the cryptex through
   /System/Cryptexes/OS -> ../../System/Volumes/Preboot/Cryptexes/OS (Safari.framework, WebKit …), which on a Mac is the Preboot
   volume's copy; here it is a directory of the root. (Copying systemos over / fails on exactly those links.) REUSE_ROOT=1 keeps
   an existing FULL/root copy of os and removes what an earlier overlay of systemos left in it.
2. /System/Library/dyld = the pacstripped cache (the cryptex's own cache is arm64e with PAC; dyld finds the cache here, as on
   the recovery root); the cryptex's dyld directory becomes a link to it, so either path gives the stripped cache.
3. Every other Mach-O is pacstripped in place (UniXNU-ruan-macos/tools/macho.py, the tool the recovery root went through;
   a binary without PAC instructions comes out unchanged), in parallel.
4. Every Mach-O is ad-hoc re-signed (adhoc-resign.py); its CDHashes go to root.cdhash for the trust cache.
The tree lives on the ext4 build image (host-work/apple-trees): NTFS cannot hold its names or modes.
"""
import concurrent.futures
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PACSTRIP_TOOLS = Path(os.environ.get('PACSTRIP_TOOLS', '/root/unixnu-work/UniXNU-ruan-macos/tools'))
MACHO = {b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'}
CRYPTEX = 'System/Volumes/Preboot/Cryptexes/OS'
CACHE_DIRS = ('System/Library/dyld', 'System/DriverKit/System/Library/dyld', CRYPTEX + '/System/Library/dyld',
              CRYPTEX + '/System/DriverKit/System/Library/dyld')


def merge_manifests(base, over):
    entries = {}
    for path in (base, over):
        for line in path.read_text().splitlines():
            if line:
                entries[line.split('\t')[3]] = line
    return entries


def strip_one(path):
    sys.path.insert(0, str(PACSTRIP_TOOLS))
    from macho import strip  # noqa: E402  (pacstrip's Mach-O rewriter)
    with open(path, 'rb') as f:
        if f.read(4) not in MACHO:
            return None
    data = bytearray(Path(path).read_bytes())
    try:
        report = strip(data)
    except Exception as error:          # not an arm64e image (x86_64-only, i386 …): left as it is
        return (path, -1, str(error)[:120])
    if report.patched:
        Path(path).write_bytes(data)
    return (path, report.patched, len(report.traps))


def link_cryptex(root, entries, full):
    """dyld finds a cryptex-only library (/usr/lib/libLaunchServicesSupport.dylib, most PrivateFrameworks) through the OS
    cryptex's search path, which libignition sets up; ruan boots without it (dyld_flags=8), so loginwindow failed with
    "Library not loaded" (full-OS boot 11). Each highest path that exists only in the cryptex gets a link to it at its
    normal place: /<path> -> /System/Cryptexes/OS/<path>."""
    os_paths = {l.split('\t')[3] for l in (full / 'os.manifest').read_text().splitlines() if l}
    sys_paths = sorted(l.split('\t')[3] for l in (full / 'systemos.manifest').read_text().splitlines() if l)
    linked = []
    for rel in sys_paths:
        if rel in os_paths or rel.startswith('System/Library/dyld') or any(rel.startswith(t + '/') for t in linked):
            continue
        parent = os.path.dirname(rel)
        if parent and parent not in os_paths:
            continue                    # its parent is linked or created above
        target = '/System/Cryptexes/OS/' + rel
        path = root / rel
        if path.is_symlink() or path.exists():
            continue
        os.symlink(target, path)
        entries[rel] = f'120755\t0\t0\t{rel}\t{target}'
        linked.append(rel)
    print(f'cryptex: {len(linked)} links at / into /System/Cryptexes/OS')


def stub_apfs_boot_util(root, entries):
    """launchd's mount phases run apfs_boot_util, which fails on our read-only root container (it creates a VM volume): a byte
    copy of /usr/bin/true (already re-signed, its CDHash already in the trust cache) succeeds instead, as UniXNU's boot shim does."""
    rel = 'System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_boot_util'
    shutil.copyfile(root / 'usr/bin/true', root / rel)
    (root / rel).chmod(0o755)
    print('apfs_boot_util: copy of /usr/bin/true')


def main():
    full, cache = Path(sys.argv[1]), Path(sys.argv[2])
    root = full / 'root'
    if os.environ.get('POST_ONLY'):          # only the steps after re-signing, on an existing tree and manifest
        entries = {l.split('\t')[3]: l for l in (full / 'root.manifest').read_text().splitlines() if l}
        link_cryptex(root, entries, full)
        stub_apfs_boot_util(root, entries)
        (full / 'root.manifest').write_text('\n'.join(entries[p] for p in sorted(entries)) + '\n')
        return
    os_paths = {l.split('\t')[3] for l in (full / 'os.manifest').read_text().splitlines() if l}
    sys_lines = [l for l in (full / 'systemos.manifest').read_text().splitlines() if l]
    if os.environ.get('REUSE_ROOT') and root.is_dir():
        stale = sorted({l.split('\t')[3] for l in sys_lines} - os_paths, reverse=True)
        for rel in stale:                       # deepest first: files, then their now-empty directories
            path = root / rel
            if path.is_symlink() or path.is_file():
                path.unlink()
            elif path.is_dir():
                shutil.rmtree(path)
        print(f'reused {root}: {len(stale)} overlay leftovers removed')
    else:
        if root.exists():
            shutil.rmtree(root)
        subprocess.run(['cp', '-a', f'{full / "os"}/.', root], check=True)
    cryptex = root / CRYPTEX
    shutil.rmtree(cryptex, ignore_errors=True)
    cryptex.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cp', '-a', f'{full / "systemos"}', cryptex], check=True)
    entries = merge_manifests(full / 'os.manifest', full / 'systemos.manifest')
    for rel in {l.split('\t')[3] for l in sys_lines} - os_paths:   # the overlay's paths are not at / any more
        entries.pop(rel, None)
    for d in ('System/Volumes/Preboot', 'System/Volumes/Preboot/Cryptexes', CRYPTEX):
        entries.setdefault(d, f'40755\t0\t0\t{d}\t')
    for line in sys_lines:
        f = line.split('\t')
        f[3] = f'{CRYPTEX}/{f[3]}'
        entries[f[3]] = '\t'.join(f)

    dyld = root / 'System/Library/dyld'
    shutil.rmtree(dyld, ignore_errors=True)
    dyld.mkdir(parents=True)
    for c in sorted(cache.glob('dyld_shared_cache_arm64e*')):
        if not c.name.endswith(('.symbols', '.map')):
            shutil.copyfile(c, dyld / c.name)
            entries[f'System/Library/dyld/{c.name}'] = f'100644\t0\t0\tSystem/Library/dyld/{c.name}\t'
    for gone in [p for p in entries if p.startswith('System/Library/dyld/') and not (root / p).exists()]:
        del entries[gone]
    cdyld = cryptex / 'System/Library/dyld'
    if cdyld.exists() or cdyld.is_symlink():
        shutil.rmtree(cdyld) if cdyld.is_dir() and not cdyld.is_symlink() else cdyld.unlink()
    for p in [p for p in entries if p.startswith(f'{CRYPTEX}/System/Library/dyld/')]:
        del entries[p]
    if (cryptex / 'System/Library').is_dir():
        os.symlink('../../../../../../../System/Library/dyld', cdyld)   # -> /System/Library/dyld
        entries[f'{CRYPTEX}/System/Library/dyld'] = f'120755\t0\t0\t{CRYPTEX}/System/Library/dyld\t../../../../../../../System/Library/dyld'

    # Bring-up: launchd's boot tasks (fips, mount-phase-1/2, fsck, data-protection …) come from a plist inside launchd itself;
    # a RequireSuccess task that fails stops the boot. Renaming the key in that XML (same length, so nothing moves) makes them
    # all optional: a failure is logged and the boot goes on. BOOT_TASKS_REQUIRED=1 keeps Apple's plist.
    if not os.environ.get('BOOT_TASKS_REQUIRED'):
        launchd = root / 'sbin/launchd'
        data = launchd.read_bytes()
        key = b'<key>RequireSuccess</key>'
        print(f'launchd: {data.count(key)} RequireSuccess boot-task keys made optional')
        launchd.write_bytes(data.replace(key, b'<key>RequireSuccesz</key>'))

    files = []
    for top, dirs, names in os.walk(root):
        rel = os.path.relpath(top, root)
        dirs[:] = [d for d in dirs if os.path.normpath(os.path.join(rel, d)) not in CACHE_DIRS]
        files += [os.path.join(top, n) for n in names if not os.path.islink(os.path.join(top, n))]
    patched = traps = skipped = 0
    with concurrent.futures.ProcessPoolExecutor() as pool:
        for result in pool.map(strip_one, files, chunksize=64):
            if result is None:
                continue
            if result[1] < 0:
                skipped += 1
            else:
                patched += result[1] > 0
                traps += result[2]
    print(f'pacstrip: {patched} Mach-O files rewritten, {traps} instructions left to the emulator, {skipped} not arm64e')

    subprocess.run([sys.executable, HERE / 'adhoc-resign.py', root, full / 'root.cdhash'], check=True)
    link_cryptex(root, entries, full)
    stub_apfs_boot_util(root, entries)
    (full / 'root.manifest').write_text('\n'.join(entries[p] for p in sorted(entries)) + '\n')
    print(f'{root}: {len(entries)} entries, {len((full / "root.cdhash").read_text().split())} CDHashes')


if __name__ == '__main__':
    main()
