#!/usr/bin/env python3
"""Compile ruan's diagnostics (package/RamRootProbe.c, RamRestoreTrust.c, BootProgress.c) and the AppleKeyStore stand-in
(package/KeyStoreProbe.cpp + component AppleKeyStoreState) into build/ram-root-probe/libRamRootProbe.a.

Run as a [kernel] archives step (cwd = the UniXNU tree), with the flags of the actual XNU compile records. The pins are
the IPSW's unchanged RestoreRamDisk 044-75545-038.dmg (im4p-unwrapped raw APFS = ramroot/rrd/out/root-usb.apfs) and
its /sbin/launchd, hashed here on 2026-11-14; they equal UniXNU-fuxi diagnostics/ram-root/pins.json.
The RestoreTrustCache (BuildManifest RestoreTrustCache = Firmware/044-75545-038.dmg.trustcache, im4p payload) is read
from $RAM_RESTORE_TRUSTCACHE and embedded in the kernel; without the file the kernel is built without it and
ram-restore-trust=1 panics with that. Apple bytes stay in build/, never in this repository.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path('.').resolve()
OUT = ROOT / 'build/ram-root-probe'
OUT.mkdir(parents=True, exist_ok=True)
DEVICE = Path(os.environ.get('UNIXNU_DEVICE', Path(__file__).resolve().parents[1]))

IMAGE_BYTES = 184549376
IMAGE_SHA256 = 'aadb59ed21a8f0218a348f070913ab5a7679805c58408534d811170871f1fc05'
LAUNCHD_BYTES = 561856
LAUNCHD_SHA256 = '8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25'
# RAM_ROOT_IMAGE / RAM_ROOT_LAUNCHD pin another root (default: the Apple RestoreRamDisk pins above).
for env, kind in (('RAM_ROOT_IMAGE', 'IMAGE'), ('RAM_ROOT_LAUNCHD', 'LAUNCHD')):
    if os.environ.get(env):
        data = Path(os.environ[env]).read_bytes()
        globals()[kind + '_BYTES'], globals()[kind + '_SHA256'] = len(data), hashlib.sha256(data).hexdigest()
RESTORE_BYTES, RESTORE_ENTRIES = 5898, 267
RESTORE_SHA256 = '54e80085f2de19d76f870aa63366ef520a905913dc0208381010864f7d1fbb09'
RESTORE = Path(os.environ.get('RAM_RESTORE_TRUSTCACHE', '/root/unixnu-work/ramroot/rrd/restore-tc/restore.trustcache'))


def array(name, data):
    return f'static const uint8_t {name}[{len(data)}] __attribute__((aligned(4))) = {{' + ','.join(hex(b) for b in data) + '};\n'


(OUT / 'RamRootProbePins.h').write_text(
    f'#define RAM_ROOT_IMAGE_BYTES UINT64_C({IMAGE_BYTES})\n#define RAM_ROOT_FILE_BYTES {LAUNCHD_BYTES}U\n'
    f'#define RAM_RESTORE_ENTRIES {RESTORE_ENTRIES}U\n'
    + array('ram_root_image_sha256', bytes.fromhex(IMAGE_SHA256)) + array('ram_root_file_sha256', bytes.fromhex(LAUNCHD_SHA256)))

restore = RESTORE.read_bytes() if RESTORE.is_file() else None
if restore is not None and (len(restore) != RESTORE_BYTES or struct.unpack_from('<I', restore)[0] != 1 or
                            struct.unpack_from('<I', restore, 20)[0] != RESTORE_ENTRIES or
                            hashlib.sha256(restore).hexdigest() != RESTORE_SHA256):
    raise SystemExit(f'{RESTORE}: not the unchanged original RestoreTrustCache v1 ({RESTORE_SHA256})')
(OUT / 'RamRestoreTrustPins.h').write_text(
    '#define RAM_RESTORE_EMBEDDED 1\n' + array('ram_restore_original', restore) if restore else '#define RAM_RESTORE_EMBEDDED 0\n')


def compile_like(record_name, source, obj):
    record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / record_name)
                        .read_text().strip().rstrip(','))
    command, arguments, i = [], record['arguments'], 0
    while i < len(arguments):
        value = arguments[i]
        if value == record['file']:
            command.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                command += ['-o', str(obj)]
            i += 1
        elif value == '-Xclang' and arguments[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += ['-I', str(OUT), '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror']
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    (OUT / (obj.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    if struct.unpack_from('<4I', obj.read_bytes()) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit(f'{obj}: expected an ordinary ARM64 Mach-O object')
    return obj


def compile_cxx_like(record_name, source, obj, extra=()):
    """A C++ board file with an IOKit compile record (UniXNU-gts7xlwifi build-gop-legacy-display.py's recipe): the record's
    -Wno- suppressions are kept, since XNU's own headers need them under -Wextra."""
    record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / record_name)
                        .read_text().strip().rstrip(','))
    command, suppressions, arguments, i = [], [], record['arguments'], 0
    while i < len(arguments):
        value = arguments[i]
        if value == record['file']:
            command.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                command += ['-o', str(obj)]
            i += 1
        elif value == '-Xclang' and arguments[i + 1].startswith('-W'):
            i += 1
        elif value.startswith('-Wno-'):
            suppressions.append(value)
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-reorder-init-list',
                '-Wno-deprecated-enum-enum-conversion', '-I', str(record['directory']),
                '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(OUT), *extra, *suppressions]
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    (OUT / (obj.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    return obj


# The AppleKeyStore stand-in (UniXNU component AppleKeyStoreState, aks=1 offline-key-events=1), as UniXNU-gts7xlwifi,
# UniXNU-j716f and unixnu-fuxi build it: the class object with its module initialiser removed and the initialiser
# globalised, so it registers only when package/KeyStoreProbe.cpp asks (preModLoad/postModLoad), plus the probe.
subprocess.run(['python3', ROOT / 'scripts/build-arm-driver.py', 'AppleKeyStoreState'], check=True)
keystore = ROOT / 'build/apple-key-store-state'
subprocess.run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',
                keystore / 'NativeKeyStoreEvents.o', OUT / 'NativeKeyStoreEvents.late.o'], check=True)

# The Data volume's disk (UniXNU component RamBlock, NativeRamBlock): a real IOMedia, which APFS needs for a mount ("invalid
# fsindex: -1" on a bare md node, test 2026-11-14 14:39; "fs iokit node was not found" with a devfs alias, unixnu-fuxi v30).
# ruan has one loader RAM disk (DarwinBoot takes one RAMDisk), so the driver serves the part from ram-data-split=<offset> on
# (ruan-ram-data-split.patch), and refuses to start without it, so it never serves the root that md0 holds.
ramblock = OUT / 'ram-block'
ramblock.mkdir(exist_ok=True)
for header in (ROOT / 'drivers/RamBlock').glob('*.h'):
    (ramblock / header.name).write_bytes(header.read_bytes())
text = (ROOT / 'drivers/RamBlock/NativeRamBlock.cpp').read_text()
anchor = """  map->release();
  // Boot memory belongs to the loader/kernel; never allocate or free it here.
"""
assert text.count(anchor) == 1, 'NativeRamBlock.cpp changed: re-check the ram-data-split hook'
text = text.replace(anchor, """  map->release();
  // ruan: the RAMDisk holds root (md0) + Data; serve only the Data container from ram-data-split=<offset> on.
  if (!packedInput) {
    uint64_t split = 0;
    if (!PE_parse_boot_argn("ram-data-split", &split, sizeof split) || !split || (split & 4095) || split >= bytes) return false;
    physical += split;
    bytes -= split;
  }
""" + anchor.split('\n', 1)[1])
(ramblock / 'NativeRamBlock.cpp').write_text(text)
compile_cxx_like('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', ramblock / 'NativeRamBlock.cpp', ramblock / 'NativeRamBlock.o',
                 ['-I', str(ROOT / 'build/storage-abi/include'), '-I', str(ramblock)])
# IOBlockStorageDevice lives in the original IOStorageFamily kext: bind its symbols to that kext's addresses in the previous
# ruan kernel (the same prelink plan places the original kexts the same way; build-ramroot.sh checks it after the build).
subprocess.run(['python3', ROOT / 'scripts/build-ufs-original-bindings.py', '--component', 'NativeRamBlock',
                '--objects-dir', ramblock, '--candidate', ROOT / 'build/xnu-prelink/ruan-candidate.macho'], check=True)

objects = [ramblock / 'NativeRamBlock.o', ramblock / 'original-bindings.o',
           compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/RamBlockNoStore.c', OUT / 'RamBlockNoStore.o'),
           compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/RamRootProbe.c', OUT / 'RamRootProbe.o'),
           compile_cxx_like('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', DEVICE / 'package/KeyStoreProbe.cpp',
                            OUT / 'KeyStoreProbe.o'),
           OUT / 'NativeKeyStoreEvents.late.o',
           compile_like('bsd/DEVELOPMENT/kern_trustcache.o.json', DEVICE / 'package/RamRestoreTrust.c', OUT / 'RamRestoreTrust.o'),
           # the boot_progress_* hooks (patches/xnu/ruan-boot-progress.patch), with the flags of the startup.c that calls them
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/BootProgress.c', OUT / 'BootProgress.o'),
           # ruan-autoreset's reboot into fastboot (package/BootReset.c, from UniXNU-Dizi-Ruan)
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/BootReset.c', OUT / 'BootReset.o'),
           # EL0 fault records and the SIGABRT reason scan for the crash banner (package/BootFaultProbe.c, from UniXNU-fuxi)
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/BootFaultProbe.c', OUT / 'BootFaultProbe.o')]
archive = OUT / 'libRamRootProbe.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
# The kernel build does not track this archive (XNU_EXTRA_LD_ARCHIVES): drop the linked kernel when it changes.
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
stamp = OUT / 'archive.sha256'
if not stamp.is_file() or stamp.read_text().strip() != digest:
    (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped').unlink(missing_ok=True)
    stamp.write_text(digest + '\n')
print(f'PASS: libRamRootProbe.a {digest} restore_trust_embedded={int(bool(restore))}')
