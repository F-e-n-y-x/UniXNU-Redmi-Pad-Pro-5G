#!/usr/bin/env python3
"""Rebuild build/sheng-ufs/libShengUfs.a for the plain arm64 macOS kernel (archive step after component:ShengUfs).

component:ShengUfs compiles NativeUfs.cpp against the iOS ABI and leaves IOBlockStorageDevice imports for
build-ufs-original-bindings.py, which only knows the iPadOS 17 kernelcache. Here, as UniXNU sheng's source storage does:
  - refer/IOStorageFamily/IOBlockStorageDevice.cpp is compiled into the kernel, its metaclass renamed (UniXNUBlockStorageDevice)
    and parented to the original class at run time (native_source_storage_parent, package/RuanMacUfs.cpp);
  - it and NativeUfs.cpp are compiled with TARGET_OS_OSX=1 (macOS IOStorageFamily vtable: 3 more slots) and without their
    static constructors (the __mod_init_func section), which RuanMacUfs.cpp runs once IOStorageFamily is loaded;
  - package/RuanMacUfs.cpp starts NativeUfs from bsd_init (patches/xnu-12377/ruan-mac-ufs-start.patch).
Uses ShengUfs's own compile command (build/sheng-ufs/compile-command.json), only swapping source, output and the macOS header.
"""
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
out = ROOT / 'build/sheng-ufs'
base_command = json.loads((out / 'compile-command.json').read_text())
# the command's relative includes (meta_features.h) resolve in the kernel object directory, as build-arm-driver.py runs it
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                    .read_text().strip().rstrip(','))
compiler_dir = Path(base_command[0]).parent

osx = out / 'macos-target.h'
osx.write_text('/* Generated (build-ruan-mac-ufs.py): the macOS IOStorageFamily ABI on the iOS-triple kernel. */\n'
               '#include <TargetConditionals.h>\n#undef TARGET_OS_OSX\n#define TARGET_OS_OSX 1\n')

macro = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
original = (ROOT / 'refer/IOStorageFamily/IOBlockStorageDevice.cpp').read_text()
assert original.count(macro) == 1, 'IOBlockStorageDevice registration macro changed'
base = out / 'IOBlockStorageDevice.cpp'
base.write_text(original.replace(macro, '''
extern "C" const OSMetaClass *native_source_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("UniXNUBlockStorageDevice", native_source_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
'''))

# NativeUfs.cpp, patched copy (the checkout is shared with the iPadOS variant): its DMA buffers (host region, page tables,
# command lists) are cleared with bzero, which faults on the macOS kernel ("Unaligned kernel data abort" in bzero from
# hostContiguous, UFS boot 1) when they are mapped uncached or write-combined. Every bzero becomes a clear with aligned 64-bit
# stores, or byte stores where pointer or length are not 8-byte aligned: both are valid on every memory type.
ufs_src = (ROOT / 'drivers/ShengUfs/NativeUfs.cpp').read_text()
assert ufs_src.count('bzero (') >= 5, 'NativeUfs bzero calls changed'
safe = (
    '#include <stdint.h>\n#include <stddef.h>\n'
    'static inline void ruan_safe_bzero (void *p, size_t n)\n{\n'
    '  if (!((uintptr_t) p & 7) && !(n & 7)) { volatile uint64_t *q = (volatile uint64_t *) p; for (size_t i = 0; i < n / 8; ++i) q[i] = 0; }\n'
    '  else { volatile uint8_t *q = (volatile uint8_t *) p; for (size_t i = 0; i < n; ++i) q[i] = 0; }\n}\n')
ufs_copy = out / 'NativeUfs.cpp'
safe += ('static inline void *ruan_safe_memcpy (void *d, const void *s, size_t n)\n{\n'
         '  volatile uint8_t *q = (volatile uint8_t *) d; const volatile uint8_t *r = (const volatile uint8_t *) s;\n'
         '  if (!((uintptr_t) d & 7) && !((uintptr_t) s & 7) && !(n & 7)) {\n'
         '    volatile uint64_t *q8 = (volatile uint64_t *) d; const volatile uint64_t *r8 = (const volatile uint64_t *) s;\n'
         '    for (size_t i = 0; i < n / 8; ++i) q8[i] = r8[i];\n  } else for (size_t i = 0; i < n; ++i) q[i] = r[i];\n  return d;\n}\n')
# UFS boot 2: "Unaligned kernel data abort" in hostIssue (UPIU fields in an uncached/write-combined UCD, which this kernel maps as
# Device memory): the copy is also built with -mstrict-align, and its memcpy calls use the copy above.
ufs_copy.write_text(safe + ufs_src.replace('bzero (', 'ruan_safe_bzero (').replace('memcpy (', 'ruan_safe_memcpy ('))
# NativeRamBlock (UniXNU RamBlock): the USB-sent Data container (/chosen/memory-map RAMDisk) as an IOMedia, so APFS publishes its volume
# (XNU's md0 is a plain memdev that APFS never probes: full-OS boot 2). Same treatment as NativeUfs: macOS ABI, late registration.
ramblock = ROOT / 'drivers/RamBlock/NativeRamBlock.cpp'
sources = [base, ufs_copy, ramblock, ROOT / 'kits/ruan/RuanMacUfs.cpp']
source_index = next(i for i, a in enumerate(base_command) if a.endswith('/NativeUfs.cpp'))
output_index = base_command.index('-o') + 1
objcopy = next((str(p) for p in (compiler_dir / 'llvm-objcopy', Path('/opt/unixnu-linux/llvm/bin/llvm-objcopy'))
                if p.exists()), shutil.which('llvm-objcopy'))
assert objcopy, 'llvm-objcopy not found'
objects = []
for source in sources:
    obj = out / (source.stem + '.mac.o')
    command = list(base_command)
    command[source_index], command[output_index] = str(source), str(obj)
    command += ['-include', str(osx), '-I', str(ROOT / 'drivers/ShengUfs'), '-I', str(ROOT / 'drivers/RamBlock')]   # quoted includes
    if source == ufs_copy:
        command.append('-mstrict-align')
    if source == base:
        command.append('-Wno-error')   # Apple's file, built as is
    result = subprocess.run(command, cwd=record['directory'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / (source.stem + '.mac.log')).write_text(result.stdout)
    if result.returncode:
        raise SystemExit(f'{source.name}:\n{result.stdout[-5000:]}')
    if source.name in ('IOBlockStorageDevice.cpp', 'NativeUfs.cpp', 'NativeRamBlock.cpp'):
        late = obj.with_suffix('.late.o')
        subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func',
                        f'--globalize-symbol=__GLOBAL__sub_I_{source.name}', obj, late], check=True)
        obj = late
    objects.append(obj)

# RuanDummyEth (package/RuanDummyEth.cpp): the virtual built-in en0 TimeSync needs before WindowServer draws. An IOEthernetController
# subclass: compiled against the macOS SDK's IONetworking headers, its initializer run late (RuanMacUfs.cpp), and its
# IONetworkingFamily imports bound to that kext's addresses in this kernel collection (as UniXNU-tb375fc's build_desktop_tier3.py;
# the plain-arm64 kernel has no PAC vtable diversity to match). The addresses come from the previous kernel image: the kext's
# place in Apple's collection does not change between our builds.
import os as _os, sys as _sys
sdk_kernel = _os.environ.get('MACOS_SDK', '/root/unixnu-work/apple-trees/MacOSX26.sdk') + '/System/Library/Frameworks/Kernel.framework/Headers'
eth_src = ROOT / 'kits/ruan/RuanDummyEth.cpp'
if eth_src.is_file() and _os.path.isdir(sdk_kernel):
    eth_obj = out / 'RuanDummyEth.mac.o'
    command = list(base_command)
    command[source_index], command[output_index] = str(eth_src), str(eth_obj)
    command += ['-include', str(osx), f'-isystem{sdk_kernel}', '-Wno-unused-parameter', '-Wno-error']
    result = subprocess.run(command, cwd=record['directory'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / 'RuanDummyEth.mac.log').write_text(result.stdout)
    if result.returncode:
        raise SystemExit('RuanDummyEth.cpp:\n' + result.stdout[-5000:])
    eth_late = out / 'RuanDummyEth.mac.late.o'
    subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_RuanDummyEth.cpp',
                    eth_obj, eth_late], check=True)
    undefined = subprocess.run(['xcrun', 'nm', '-u', '-j', eth_late], text=True, stdout=subprocess.PIPE, check=True).stdout.split()
    wanted = sorted({u for u in undefined if any(t in u for t in ('IONetwork', 'IOEthernet', 'IOMbuf', 'IOOutputQueue'))})
    kc_path = Path(_os.environ.get('RUAN_KC_FOR_BINDINGS', '/root/unixnu-work/macos-build/out-gpu/kernel.macho'))
    _sys.path.insert(0, str(Path('/root/unixnu-work/UniXNU-macos/kits/XnuPrelink')))
    from kc_image import Image
    raw = kc_path.read_bytes()
    kc = Image(raw)
    net = {}
    for kext, (_va, off) in kc.entries.items():
        if kext == 'com.apple.iokit.IONetworkingFamily':
            for va, names in Image(raw, off).symbols().items():
                for n in names:
                    net[n] = va
    assert net, f'{kc_path}: no IONetworkingFamily symbols'
    text, addrs = '.text\n.p2align 2\n', ''
    for sym in wanted:
        if sym in net:
            if sym.endswith('gMetaClassE') or sym.startswith(('__ZTV', '__ZTI')):
                addrs += f'.globl {sym}\n.set {sym}, {net[sym]:#x}\n'
            else:
                addrs += f'.globl {sym}.original\n.set {sym}.original, {net[sym]:#x}\n'
                text += f'.globl {sym}\n{sym}:\n bti c\n b {sym}.original\n'
        else:
            # padding slots (_RESERVEDIONetworkController*) are never called
            assert '_RESERVED' in sym, f'IONetworkingFamily symbol missing from {kc_path}: {sym}'
            text += f'.globl {sym}\n{sym}:\n bti c\n ret\n'
    for name, body in (('eth-bindings', text), ('eth-addresses', addrs)):
        (out / f'{name}.S').write_text(body)
        subprocess.run([base_command[0], '-target', 'arm64-apple-ios17.0', '-c', out / f'{name}.S', '-o', out / f'{name}.o'],
                       check=True)
        objects.append(out / f'{name}.o')
    objects.append(eth_late)
    print(f'RuanDummyEth: {len(wanted)} IONetworkingFamily bindings from {kc_path.name}')
nm = subprocess.run(['xcrun', 'nm', '-u', '-j', *objects], text=True, stdout=subprocess.PIPE, check=True).stdout.split()
defined = set(subprocess.run(['xcrun', 'nm', '-U', '-j', *objects], text=True, stdout=subprocess.PIPE, check=True).stdout.split())
left = sorted({n for n in nm if 'IOBlockStorageDevice' in n} - defined)
assert not left, 'IOBlockStorageDevice imports left: ' + ', '.join(left[:5])
archive = out / 'libShengUfs.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
print(f'PASS: {archive.name} = source IOBlockStorageDevice + NativeUfs + NativeRamBlock + RuanMacUfs (macOS ABI, late registration)')
