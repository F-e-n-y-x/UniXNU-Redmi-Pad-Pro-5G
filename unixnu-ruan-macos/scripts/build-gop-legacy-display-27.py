#!/usr/bin/env python3
"""Bind the owned GOP backend to the original IOMobileFramebufferLegacy of the iPadOS 27 (24A446) fileset.

The class ABI (vtable targets and diversities, instance size, field offsets, original bindings) comes from the reviewed fixture
legacy-display-24A446.json. The kernel is arm64e: every vtable entry of the owned subclass must carry the diversity the original
callers authenticate with, so every original slot is declared under a name whose discriminator is that slot's diversity. Original
code is bound at its fileset address plus the collection slide; no original instruction or vtable is changed.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from kernel_target import ARCH, IOS_VERSION, TRIPLE
# The OS profile's build selects the class fixtures: the reviewed iPadOS 27 ones, or the macOS 27.3 ones that
# gen-board-abi.py writes from the symbols of the macOS collection.
BUILD = os.environ.get('UNIXNU_IPSW_BUILD') or '24A446'
MACOS = os.environ.get('UNIXNU_XNU_TARGET') == 'macosx'
if ARCH not in ('arm64', 'arm64e') or IOS_VERSION.split('.')[0] != '27' or (BUILD, MACOS) not in (('24A446', False), ('26C5035r', True)):
    raise SystemExit('This Legacy display builder supports arm64e iPadOS 27 (24A446) and macOS 27.3 (26C5035r).')
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
ABI_PATH = Path(__file__).resolve().with_name(f'legacy-display-{BUILD}.json')
abi = json.loads(ABI_PATH.read_text())
# IOService's vtable length in this kernel and the pure-virtual target of the collection: fixed for iPadOS 27, recorded
# in the fixture for macOS.
BASE = abi.get('base_slots', 168)
slide = int(abi['collection_slide'], 16)
# Original targets are fixed collection addresses. XnuPrelink recomputes the
# BTI/B adapter displacement after placing native text; no prior build is needed.
slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
D, R, N = abi['display'], abi['request'], abi.get('notify', {})


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=300, **kw)


def require(v, why):
    if not v:
        raise SystemExit(why)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def discriminator(name):
    """clang ptrauth_string_discriminator: SipHash-2-4 with the fixed key, modulo 0xffff, plus one."""
    mask = (1 << 64) - 1
    rotl = lambda x, b: ((x << b) | (x >> (64 - b))) & mask  # noqa: E731
    key = bytes([0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79, 0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4])
    k0, k1 = struct.unpack('<2Q', key)
    v = [0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1, 0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1]

    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & mask; v[1] = rotl(v[1], 13) ^ v[0]; v[0] = rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & mask; v[3] = rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & mask; v[3] = rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & mask; v[1] = rotl(v[1], 17) ^ v[2]; v[2] = rotl(v[2], 32)
    data = name.encode()
    full = len(data) & ~7
    for i in range(0, full, 8):
        m = struct.unpack_from('<Q', data, i)[0]
        v[3] ^= m; rounds(2); v[0] ^= m
    b = len(data) << 56 & mask
    for i, c in enumerate(data[full:]):
        b |= c << (8 * i)
    v[3] ^= b; rounds(2); v[0] ^= b; v[2] ^= 0xff; rounds(4)
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) % 0xffff + 1


# Prototypes of the original slots the owned backend overrides or calls; every other slot is an opaque dispatch target.
PROTOTYPES = {
    'isMainDisplay': 'bool isMainDisplay()', 'getDotPitch': 'IOReturn getDotPitch(uint32_t *)',
    'supportedFrameInfo': 'IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0',
    'getDisplayDimensions': 'void getDisplayDimensions(uint32_t *,uint32_t *)', 'startHardware': 'bool startHardware()',
    'kickSwapQueue': 'void kickSwapQueue()', 'completeCurrentSwap': 'void completeCurrentSwap()',
    'validateSwap': 'bool validateSwap(const OriginalLegacySwapRequest *)',
    'getRefreshAttributes': 'bool getRefreshAttributes(uint64_t *,bool)',
    'collectCompletionReports': 'IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0',
    'applyStateRequest': 'int applyStateRequest(OriginalLegacyStateRequest *,bool)',
    # Slot 366: swap_wait_gated calls it (this only, result logged) after a swap waited 5 s, then cancels its own queue (slot 367).
    'cancelSwapsAfterTimeout': 'bool cancelSwapsAfterTimeout()',
    # Slot 316 initializes optional family state and returns void; x0 is not an IOSurface result.
    'create_default_fb_surface': 'void create_default_fb_surface()'}
# A macOS fixture carries its own prototypes (the reference labels plus the family hooks it implements).
PROTOTYPES = abi.get('prototypes', PROTOTYPES)
by_slot = {slot: name for name, slot in abi['methods'].items()}
PURE_TARGET = int(abi['pure_target'], 16) if 'pure_target' in abi else slots[214][0]
pure = {i for i, (t, _) in enumerate(slots) if t == PURE_TARGET}
renames = []
for base, (name, suffix) in abi['names'].items():
    mangled = f'_ZN25IOMobileFramebufferLegacy{len(name)}{name}{suffix}'
    slot = abi['methods'].get(base) or int(base[len('opaque'):])
    require(discriminator(mangled) == slots[slot][1], f'Reviewed name {name} has the diversity of slot {slot}')
    renames.append(f'#define {base} {name}\n')
leaves = ' '.join(f'LEAF({i})' for i in abi['leaves'])
# The macOS family's swap request and notification fields (gen-board-abi.py); the iPadOS header has none of them.
MACOS_CONSTANTS = ('#define NATIVE_LEGACY_MACOS 1\n'
    f"static constexpr unsigned kLegacyCurrentSwap={D['current']};\n"
    f"static constexpr unsigned kRequestDropReasons={R.get('drop_reasons', 0)},kRequestSwapInfoA={R.get('swap_info_a', 0)},kRequestSwapInfoB={R.get('swap_info_b', 0)};\n"
    f"static constexpr unsigned kLegacySwapInfoTime={D.get('swap_info_time', 0)},kLegacySwapInfoHeld={D.get('swap_info_held', 0)};\n"
    # swap_complete_gated stores the completed swap's ID here (0: not in the fixture, the source's swap_wait rule stays off).
    f"static constexpr unsigned kLegacyLastCompletedSwap={D.get('last_completed_swap', 0)};\n"
    f"static constexpr unsigned kNotifyAsyncReference={N.get('async_reference', 0)},kNotifyMask={N.get('mask', 0)},kNotifyLastTime={N.get('last_time', 0)};\n"
    ) if MACOS else ''
header = f'''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the original iPadOS 27 (24A446) class, generated from {ABI_PATH.name}. Each original slot is declared
// under a name whose pointer-authentication discriminator is the original slot's diversity (the names below), so original
// callers authenticate the owned subclass's entries. Only qualified base prototypes may be called from source.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
extern "C" OSObject *native_legacy_default_surface_create(OSObject *,uint32_t,uint32_t);
struct OriginalLegacySwapRequest;
struct OriginalLegacyStateRequest {{uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];}};
static constexpr unsigned kLegacyVsyncClients={D['vsync_clients']};
static constexpr unsigned kRequestRect0={R['rect0']},kRequestRect1={R['rect1']},kRequestMasks={R['masks']},kRequestOptions={R['options']},kRequestDims={R['dims']},kRequestSurfaces={R['surfaces']};
#define NATIVE_LEGACY_LEAVES(LEAF) {leaves}
#define NATIVE_LEGACY_DEFAULT_SURFACE 1
#define NATIVE_LEGACY_CANCEL_SWAPS 1
{MACOS_CONSTANTS}static constexpr unsigned kLegacyDefaultSurface={D['default_surface']};
{''.join(renames)}class IOMobileFramebufferLegacy : public IOService {{
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[{abi['instance_size']}-sizeof(IOService)];
public:
 bool installDefaultSurface(uint32_t width,uint32_t height) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  OSObject *root=nullptr,*surface=nullptr;
  memcpy(&surface,self+kLegacyDefaultSurface,sizeof(surface));
  if(surface)return true;
  memcpy(&root,self+{D['surface_root']},sizeof(root));
  if(!root)return false;
  surface=native_legacy_default_surface_create(root,width,height);
  if(!surface)return false;
  // Transfer the factory reference to the family; its stop path releases this field.
  memcpy(self+kLegacyDefaultSurface,&surface,sizeof(surface));
  return true;
 }}
 void installBootDimensions(uint32_t width,uint32_t height) {{
  memcpy(reinterpret_cast<unsigned char *>(this)+{D['width']},&width,4);memcpy(reinterpret_cast<unsigned char *>(this)+{D['height']},&height,4);
 }}
 const OriginalLegacySwapRequest *pendingSwapRequest() const {{
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+{D['head']},sizeof(request));return request;
 }}
 // Tail queue: head {D['head']}, tail pointer {D['tail']}, each request's next {R['next']} and previous-link {R['prev']}; the head moves
 // to the current-swap slot {D['current']} for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+{D['current']},8);memcpy(&head,self+{D['head']},8);memcpy(&tail,self+{D['tail']},8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+{R['next']},8);memcpy(&previous,bytes+{R['prev']},8);
  if(previous!=self+{D['head']})return false;
  if(next) {{
   const void *link=self+{D['head']};
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+{R['prev']},&link,8);
  }} else if(tail!=bytes+{R['next']})return false;
  memcpy(self+{D['current']},&request,8);
  if(next) {{memcpy(self+{D['head']},&next,8);}}
  else {{const void *empty=nullptr,*emptyTail=self+{D['head']};memcpy(self+{D['head']},&empty,8);memcpy(self+{D['tail']},&emptyTail,8);}}
  return true;
 }}
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(BASE, len(slots)):
    if i in by_slot:
        header += f' virtual {PROTOTYPES[by_slot[i]]};\n'
    elif i in pure:
        header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:
        header += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header += f'}};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)=={abi["instance_size"]},"Actual original instance extent");\n'
(OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeLegacyDisplay'], text=True), end='')



def macho(data):
    """Load commands, segments and sections of an arm64e relocatable object."""
    magic, cpu, _, _, count = struct.unpack_from('<5I', data)
    require(magic == 0xfeedfacf and cpu == 0x100000c, 'arm64 Mach-O object')
    commands, segments, offset = [], [], 32
    for _ in range(count):
        command, size = struct.unpack_from('<2I', data, offset)
        commands.append((command, offset, size))
        if command == 0x19:
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            sections = []
            for i in range(nsects):
                at = offset + 72 + 80 * i
                va, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
                sections.append({'name': data[at:at + 16].rstrip(b'\0').decode(), 'va': va, 'size': length,
                                 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    return {'commands': commands, 'segments': segments}


def owned_vtable(obj, vtable_symbol, length):
    """The object's symbols and its class vtable in slot order: (target symbol, stored word) per entry."""
    raw = obj.read_bytes()
    image = macho(raw)
    st = next(p for c, p, _ in image['commands'] if c == 2)
    sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    symbols = []
    for i in range(count):
        ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
        symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
    vtable = next(va for name, va, _ in symbols if name == vtable_symbol)
    entries = {}
    for segment in image['segments']:
        for sec in segment['sections']:
            if sec['va'] <= vtable < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                    # extern, 8 bytes, ARM64_RELOC_AUTHENTICATED_POINTER (arm64e) or ARM64_RELOC_UNSIGNED (plain arm64 kernel, ruan: no PAC)
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == (11 if ARCH == 'arm64e' else 0):
                        word = struct.unpack_from('<Q', raw, sec['fileoff'] + offset)[0]
                        entries[sec['va'] + offset] = (symbols[bits & 0xffffff][0], word)
    compiled = [entries.get(vtable + 16 + i * 8) for i in range(length)]
    require(all(compiled) and vtable + 16 + length * 8 not in entries, f'Exact owned {length}-entry vtable of {vtable_symbol}')
    return symbols, compiled


obj = OUT / 'NativeLegacyDisplay.o'
symbols, compiled = owned_vtable(obj, '__ZTV19NativeLegacyDisplay', len(slots))
for i, (name, word) in enumerate(compiled):
    require(ARCH == 'arm64' or word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and (word >> 32) & 0xffff == slots[i][1],
            f'Slot {i} ({name}) signed with IA, address diversity and the original diversity {slots[i][1]:#x}')
family = range(*(int(x, 16) for x in abi['family_text'])) if 'family_text' in abi else range(0xfffffe000a27c000, 0xfffffe000a29a000)
bindings = {('_native_legacy_surface_stride_raw' if name == '_native_legacy_surface_stride' else name): int(va, 16) + slide for name, va in abi['bindings'].items()}
owned, native = [], []
for i, (name, _) in enumerate(compiled):
    if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
        owned.append(i)
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure, f'No unresolved original abstract target at slot {i}')
        require(bindings.get(name, slots[i][0] + slide) == slots[i][0] + slide, 'One original address per name')
        bindings[name] = slots[i][0] + slide
    else:
        require(i < BASE and slots[i][0] not in family, f'Slot {i} ({name}) is inherited unchanged by the original family')
        native.append(i)
require(pure <= set(owned), 'Every original abstract slot is implemented by the owned backend')
undefined = {n for n, _, k in symbols if k == 1 and 'IOMobileFramebufferLegacy' in n}
require(undefined <= set(bindings), 'Every original-class import is bound: ' + ', '.join(sorted(undefined - set(bindings))))

sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()


def write_bindings(prefix, bindings, data):
    """<prefix>original-bindings.o and <prefix>original-addresses.o for one class's original bindings."""
    # Absolute symbols get no fixup: a pointer to one is neither signed nor moved. Every original function is therefore reached through
    # an owned stub in kernel text (the vtable signs and moves it like any native code), whose branch lands on the original. Data
    # symbols are reached through GOT entries, which keep their absolute value.
    text, addresses = '.text\n.p2align 2\n', ''
    for name, va in sorted(bindings.items()):
        if name in data:
            # Reached through GOT entries: plain pointers that the emitter does not move.
            addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
        else:
            # The absolute target lives in the other object, so the branch stays a relocation the linker resolves.
            addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
            text += f'.globl {name}\n{name}:\n hint #34\n b {name}.original\n'  # bti c
    for kind, body in (('bindings', text), ('addresses', addresses)):
        (OUT / f'{prefix}original-{kind}.S').write_text(body)
        run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c', OUT / f'{prefix}original-{kind}.S', '-o', OUT / f'{prefix}original-{kind}.o'])


write_bindings('', bindings, {'__ZN25IOMobileFramebufferLegacy10gMetaClassE', '__ZTV25IOMobileFramebufferLegacy'})
llvm_objcopy = shutil.which('llvm-objcopy') or '/opt/unixnu-linux/llvm/bin/llvm-objcopy'
late = OUT / 'NativeLegacyDisplay.late.o'
run([llvm_objcopy, '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp', obj, late])

# The board probe and the owned support code, compiled with the kernel's own C++ flags.
r = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
source = ROOT / 'kits/ruan/LegacyDisplayProbe.cpp' if (ROOT / 'kits/ruan/LegacyDisplayProbe.cpp').is_file() else (ROOT / 'package/LegacyDisplayProbe.cpp' if (ROOT / 'package/LegacyDisplayProbe.cpp').is_file() else ROOT / 'kits/sheng/LegacyDisplayProbe.cpp')


def compile_cpp(src, dst, extra=()):
    cmd, args, i = [], r['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == r['file']:
            cmd.append(str(src))
        elif value == '-o':
            cmd += ['-o', str(dst)]
            i += 1
        elif value in ('-MF', '-MT', '-MJ'):
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif value.startswith(('--target=', '-mios-version-min=', '-miphoneos-version-min=')):
            pass
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    cmd += [f'--target={TRIPLE}', f'-mios-version-min={IOS_VERSION}',
            '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
            '-Wno-overriding-option', '-Wno-missing-designated-field-initializers', '-Wno-unused-but-set-global',
            '-Wno-deprecated-volatile',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(ROOT / 'kits/ruan'), '-I', str(ROOT / 'package'), *extra]
    run(cmd, cwd=r['directory'])


def compile_c(src, dst, recipe='osfmk/DEVELOPMENT/startup.o.json', extra=()):
    """A C board core with the flags of one kernel C file (osfmk for the cores, bsd for the VFS readers)."""
    rc = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / recipe).read_text().strip().rstrip(','))
    cmd, args, i = [], rc['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == rc['file']:
            cmd.append(str(src))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                cmd += ['-o', str(dst)]
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif value.startswith(('--target=', '-mios-version-min=', '-miphoneos-version-min=')):
            pass
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    cmd += [f'--target={TRIPLE}', f'-mios-version-min={IOS_VERSION}',
            '-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only',
            '-Wno-overriding-option', '-Wno-missing-designated-field-initializers', '-Wno-unused-but-set-global',
            '-Wno-deprecated-volatile',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(ROOT / 'kits/ruan'), '-I', str(ROOT / 'package'), *extra]
    run(cmd, cwd=rc['directory'])


compile_cpp(source, OUT / 'LegacyDisplayProbe.o')
# ---- Touch and buttons (macOS): the HID publisher (package/NativeHidInput.cpp) bound to the unchanged original IOHIDDevice of this
# collection, and its start-up (package/HidInputProbe.cpp; LegacyDisplayProbe.cpp calls native_hid_input_probe). The class ABI is
# hid-device-26C5035r.json (from the IOHIDFamily symbols of the collection): 334 slots over a 272-slot IOService. iPadOS 21H461
# (build-gop-legacy-display.py) has 207 over 168, so every IOHIDDevice slot here is the iPadOS one + 104: handleStart 272 (168),
# newTransportString 275 (171), newProductString 277 (173), newPrimaryUsageNumber/PageNumber 282/283 (178/179, declared only here),
# newReportDescriptor 284 (180, abstract), handleReport 285 (181). macOS IOHIDDevice also overrides IOService::didTerminate. The
# instance is 0xd0 as on iPadOS (IOHIDDevice::MetaClass C1 0xfffffe000ab3539c passes 0xd0 to OSMetaClass), so HidInputProbe's
# native_hid_class_sizes[4]==208 guard is unchanged. iPadOS 27 (24A446) has no HID fixture: its support stub below starts nothing.
hid_objects = []
if MACOS:
    hid_abi = json.loads(ABI_PATH.with_name(f'hid-device-{BUILD}.json').read_text())
    require(hid_abi['kernelcache_sha256'] == abi['kernelcache_sha256'] and hid_abi['collection_slide'] == abi['collection_slide'],
            'HID and display fixtures come from the same collection')
    hid_slots = [int(t, 16) for t, _ in hid_abi['slots']]
    hid_base, hid_pure = hid_abi['base_slots'], int(hid_abi['pure_target'], 16)
    hid_family = range(*(int(x, 16) for x in hid_abi['family_text']))
    HID_PROTOTYPES = {'handleStart': 'bool handleStart(IOService *)', 'newTransportString': 'OSString *newTransportString() const',
                      'newProductString': 'OSString *newProductString() const',
                      'newPrimaryUsageNumber': 'OSNumber *newPrimaryUsageNumber() const',
                      'newPrimaryUsagePageNumber': 'OSNumber *newPrimaryUsagePageNumber() const',
                      'newReportDescriptor': 'IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0',
                      'handleReport': 'IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits)'}
    hid_named = {slot: HID_PROTOTYPES[name] for name, slot in hid_abi['methods'].items()}
    require({i for i, t in enumerate(hid_slots) if t == hid_pure} == {hid_abi['methods']['newReportDescriptor']},
            'newReportDescriptor is the only abstract IOHIDDevice slot')
    hid_header = f"""// Generated by scripts/build-gop-legacy-display-27.py from hid-device-{BUILD}.json; opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSNumber.h>
#define NATIVE_HID_MACOS 1
class IOHIDDevice : public IOService {{
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[{hid_abi['instance_size']}-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 bool didTerminate(IOService *,IOOptionBits,bool *) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
"""
    for i in range(hid_base, len(hid_slots)):
        hid_header += (f' virtual {hid_named[i]};\n' if i in hid_named else
                       f' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();\n')
    hid_header += f'}};\nstatic_assert(sizeof(IOHIDDevice)=={hid_abi["instance_size"]},"Original device instance size");\n'
    (OUT / 'OriginalHidDeviceABI.h').write_text(hid_header)
    hid_obj = OUT / 'NativeHidInput.o'
    compile_cpp(source.parent / 'NativeHidInput.cpp', hid_obj, extra=('-I', str(OUT)))
    hid_symbols, hid_compiled = owned_vtable(hid_obj, '__ZTV14NativeHidInput', len(hid_slots))
    hid_bindings = {name: int(va, 16) + slide for name, va in hid_abi['bindings'].items()}
    hid_owned = []
    for i, (name, _) in enumerate(hid_compiled):
        if name.startswith(('__ZN14NativeHidInput', '__ZNK14NativeHidInput')):
            hid_owned.append(i)
        elif '11IOHIDDevice' in name:
            require(hid_slots[i] in hid_family, f'HID slot {i} ({name}) is an original IOHIDDevice method')
            require(hid_bindings.get(name, hid_slots[i] + slide) == hid_slots[i] + slide, 'One original HID address per name')
            hid_bindings[name] = hid_slots[i] + slide
        else:
            # Also catches an IOHIDDevice override of an IOService slot missing from the header above.
            require(i < hid_base and hid_slots[i] not in hid_family, f'HID slot {i} ({name}) is inherited unchanged from IOService')
    # destructors, getMetaClass, free, getWorkLoop, start, stop; transport/product strings, primary usage pair, report descriptor
    require(hid_owned == [0, 1, 7, 22, 190, 191, 215, 275, 277, 282, 283, 284], f'Only qualified HID methods overridden: {hid_owned}')
    hid_undefined = {n for n, _, k in hid_symbols if k == 1 and '11IOHIDDevice' in n}
    require(hid_undefined <= set(hid_bindings), 'Every original HID import is bound: ' + ', '.join(sorted(hid_undefined - set(hid_bindings))))
    write_bindings('hid-', hid_bindings, {'__ZN11IOHIDDevice10gMetaClassE', '__ZTV11IOHIDDevice'})
    hid_late = OUT / 'NativeHidInput.late.o'
    run([llvm_objcopy, '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp', hid_obj, hid_late])
    compile_cpp(source.parent / 'HidInputProbe.cpp', OUT / 'HidInputProbe.o')
    hid_objects = [hid_late, OUT / 'hid-original-bindings.o', OUT / 'hid-original-addresses.o', OUT / 'HidInputProbe.o']
    # Its producers, as iPadOS builds them (build-ram-root-probe.py): RuanInput.c (native_touch_* and the sinks), the HakiOS touch and
    # button drivers and RuanBattery.c (ruan_input_start starts all three; RuanMacBootProgress.c calls it, each gated by its boot-args).
    hakios_c = ('-I', str(source.resolve().parent.parent / '.cache/hakios-fw'), '-Wno-error', '-Wno-unused-function',
                '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-sign-compare',
                '-Wno-missing-prototypes')
    for name, extra in (('RuanInput', ()), ('RuanTouch', hakios_c), ('RuanButtons', hakios_c), ('RuanBattery', hakios_c)):
        compile_c(source.parent / f'{name}.c', OUT / f'{name}.o', recipe='bsd/DEVELOPMENT/kern_exec.o.json', extra=extra)
        hid_objects.append(OUT / f'{name}.o')
    # RuanBattery.c reports through HakiosBattery.cpp: an instance of the kernel's own IOPMPowerSource class (the class powerd
    # matches; no class of its own), so it needs no OSMetaClass registration and no static initializer (none would run: this
    # archive's __mod_init_func sections are never called). package/RuanBacklight.cpp (SY7758 over I2C) is built the same way:
    # an IOService with backlight-control for BrightnessControl, started by LegacyDisplayProbe.cpp's ruan_backlight_start;
    # RUAN_MACOS adds the macOS keys (BrightnessMilliNits, force-brightness-control, brightness-nits).
    for name, extra in (('HakiosBattery', ()), ('RuanBacklight', ('-DRUAN_MACOS=1',))):
        compile_cpp(source.parent / f'{name}.cpp', OUT / f'{name}.o', extra=extra)
        require(b'__mod_init_func' not in (OUT / f'{name}.o').read_bytes(), f'{name}.o has no static initializer')
        hid_objects.append(OUT / f'{name}.o')
    print(f'NativeHidInput bound to the original IOHIDDevice: owned {hid_owned}, {len(hid_bindings)} bindings')
support = OUT / 'LegacyDisplaySupport27.cpp'
support.write_text('''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Generated by build-gop-legacy-display-27.py. The iPadOS 27 IOSurface has no allocation-size getter: bound the copy by the
// surface's memory descriptor instead. Board probes not yet built for iPadOS 27 start nothing.
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" OSObject *native_legacy_surface_create(OSObject *,void *,OSDictionary *);
extern "C" IOReturn native_legacy_surface_allocate(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" OSObject *native_legacy_default_surface_create(OSObject *root,uint32_t width,uint32_t height) {
    if(!root || !width || !height || width>16384 || height>16384)return nullptr;
    const uint64_t stride=(uint64_t(width)*4+127)&~uint64_t(127);
    const uint64_t bytes=(stride*height+4095)&~uint64_t(4095);
    OSDictionary *properties=OSDictionary::withCapacity(6);
    if(!properties)return nullptr;
    const char *keys[]={"IOSurfaceWidth","IOSurfaceHeight","IOSurfaceBytesPerRow",
                        "IOSurfaceBytesPerElement","IOSurfacePixelFormat","IOSurfaceAllocSize"};
    const uint64_t values[]={width,height,stride,4,0x42475241,bytes};
    bool valid=true;
    for(unsigned i=0;i<6 && valid;++i) {
        OSNumber *number=OSNumber::withNumber(values[i],64);
        valid=number && properties->setObject(keys[i],number);
        if(number)number->release();
    }
    OSObject *surface=valid?native_legacy_surface_create(root,nullptr,properties):nullptr;
    properties->release();
    if(!surface){printf("sheng-fb: default surface factory failed\\n");return nullptr;}
    IOReturn status=native_legacy_surface_allocate(surface);
    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
    valid=status==kIOReturnSuccess && memory && memory->getLength()>=bytes &&
          native_legacy_surface_width(surface)==width && native_legacy_surface_height(surface)==height &&
          native_legacy_surface_stride(surface)==stride && native_legacy_surface_format(surface)==0x42475241 &&
          native_legacy_surface_planes(surface)==0;
    printf("sheng-fb: default surface %p status 0x%x valid %u %ux%u stride %llu bytes %llu\\n",
           surface,status,valid,width,height,stride,bytes);
    if(!valid){surface->release();return nullptr;}
    return surface;
}
extern "C" uint64_t native_legacy_surface_allocation(OSObject *surface) {
    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);return memory?memory->getLength():0;
}
// The board diagnostic page is not drawn on iPadOS 27: the keystore service's extension rows go nowhere.
extern "C" {char boot_progress_ext[8][48];}
// No boot progress animation runs on iPadOS 27: the scanout is always free and no pulse anchors to the framebuffer.
extern "C" int boot_progress_prepare_scanout(void) {return 1;}
extern "C" void boot_progress_cancel_scanout(void) {}
extern "C" int boot_progress_pulse_anchor(uintptr_t,uint64_t,unsigned,unsigned,unsigned) {return 0;}
// native_hid_input_probe: package/HidInputProbe.cpp on macOS; a no-op here on iPadOS 27 (appended below).
// ruan_gpu_* come from package/RuanGpuPower.c (compiled below), the accelerator from build-adreno740-accelerator.py.
extern "C" void ruan_wifi_init(void) {}
extern "C" void ruan_audio_init(void) {}
// unixnu_kernel_stack_shift: defined by the kernel (patches/xnu-12377/ruan-stack-shift.patch, osfmk/kern/stack.c).
// arm64_platform_sleep_hook: package/RuanAdrenoGlue.cpp (libRuanAdreno.a).
extern "C" void boot_progress_early_write(int, const uint64_t *) {}
extern "C" void boot_progress_pid1_syscall(int, int, const uint64_t *, uint64_t) {}
extern "C" void native_exit_trace(int, const char *, int) {}
typedef int dev_t;
#ifndef NODEV
#define NODEV ((dev_t)-1)
#endif
extern "C" {
volatile int native_owner_root_accepted = 0;
volatile dev_t native_owner_root_mount_device = NODEV;
}
class IOUserClient;
// The macOS NativeAdrenoAccelerator.cpp has no stand-in user client: the driver's (libRuanAdreno.a, ruan_adreno_driver_new_user_client)
// always answers, so LegacyDisplayProbe.cpp's fallback never runs.
extern "C" IOUserClient *native_adreno_user_client_create(task_t, void *, UInt32, OSDictionary *) { return nullptr; }
''' + ('' if MACOS else 'extern "C" void native_hid_input_probe(void) {}\n') + (
# macOS links package/RuanBacklight.cpp but not RuanBacklightSvc.cpp (ruan-bl-svc=1, iPadOS only): its entry points just fail.
'''extern "C" bool ruan_bl_svc_start(volatile uint32_t *) { return false; }
extern "C" int ruan_bl_svc_set_level(uint32_t) { return -1; }
extern "C" int ruan_bl_svc_reg(uint8_t, int) { return -1; }
extern "C" void ruan_bl_svc_static_init(void) __asm__("__GLOBAL__sub_I_RuanBacklightSvc.cpp");
extern "C" void ruan_bl_svc_static_init(void) {}
''' if MACOS else 'extern "C" bool ruan_backlight_start(void) { return false; }\n'))
compile_cpp(support, OUT / 'LegacyDisplaySupport27.o')
# The keystore state service (AppleKeyStoreState) and its probe, unchanged from iPadOS 17.
keystore_source = ROOT / 'kits/ruan/KeyStoreProbe.cpp' if (ROOT / 'kits/ruan/KeyStoreProbe.cpp').is_file() else (ROOT / 'package/KeyStoreProbe.cpp' if (ROOT / 'package/KeyStoreProbe.cpp').is_file() else ROOT / 'kits/sheng/KeyStoreProbe.cpp')
compile_cpp(keystore_source, OUT / 'KeyStoreProbe.o')
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeKeyStoreEvents'], text=True), end='')
keyevents = ROOT / 'build/apple-key-store-state'
keyevents_late = keyevents / 'NativeKeyStoreEvents.late.o'
run([llvm_objcopy, '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',
     keyevents / 'NativeKeyStoreEvents.o', keyevents_late])
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeGOPPresenter'], text=True), end='')
gop = ROOT / 'build/native-gop-presenter'
gop_late = gop / 'NativeGOPPresenter.late.o'
run([llvm_objcopy, '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp',
     gop / 'NativeGOPPresenter.o', gop_late])

board_objects = [late, OUT / 'original-bindings.o', OUT / 'original-addresses.o', OUT / 'LegacyDisplayProbe.o', OUT / 'LegacyDisplaySupport27.o', gop_late,
                 OUT / 'KeyStoreProbe.o', keyevents_late, *hid_objects]
# ruan USB debug terminal (package/UsbDebugCdc.cpp, ruan-usbdbg=1; started early by patches/xnu-12377/ruan-usbdbg-early.patch) and the
# boot-progress hooks that feed the ramoops zone it streams (package/RuanMacBootProgress.c, ruan-ramoops=1).
compile_cpp(ROOT / 'kits/ruan/UsbDebugCdc.cpp', OUT / 'UsbDebugCdc.o')
compile_c(ROOT / 'kits/ruan/RuanMacBootProgress.c', OUT / 'RuanMacBootProgress.o')
# reboot.fastboot (ruan-usbdbg-reboot=1): the PMIC reboot-reason cell + PSCI reset (package/BootReset.c, as on iPadOS)
compile_c(ROOT / 'kits/ruan/BootReset.c', OUT / 'BootReset.o')
board_objects += [OUT / 'UsbDebugCdc.o', OUT / 'RuanMacBootProgress.o', OUT / 'BootReset.o']
# EPSS CPU clock vote (package/RuanCpuFreq.c), on by default here: ruan-cpufreq=2 would cost 15 bytes of the 1000-byte line
compile_c(ROOT / 'kits/ruan/RuanCpuFreq.c', OUT / 'RuanCpuFreq.o', extra=('-DRUAN_CPUFREQ_DEFAULT=2',))
board_objects.append(OUT / 'RuanCpuFreq.o')
# Adreno 710 (ruan-gpu=17/18, as iPadOS): GMU boot, GPU pool, SMMU, zap (package/RuanGpuPower.c + AdrenoHfi.c, built by
# build-ram-root-probe.py on iPadOS); the driver itself is libRuanAdreno.a (build-ruan-adreno.py, force-loaded by the defconfig).
# The GPU pool is Device memory on this kernel: no unaligned accesses (package/RuanDeviceMem.h, full-OS boot 13).
compile_c(ROOT / 'kits/ruan/RuanGpuPower.c', OUT / 'RuanGpuPower.o', extra=('-mstrict-align', '-DRUAN_DEVICE_SAFE_MEM=1'))
compile_c(ROOT / 'kits/ruan/AdrenoHfi.c', OUT / 'AdrenoHfi.o', extra=('-mstrict-align',))
board_objects += [OUT / 'RuanGpuPower.o', OUT / 'AdrenoHfi.o']
# The accelerator for Metal discovery (MetalPluginName=NativeAdrenoMetal), bound to IOGraphicsFamily's IOAccelerator of this
# collection (scripts/data/ioaccelerator-base-abi-26C5035r.json); LegacyDisplayProbe.cpp runs its staged initializer and registers it.
# Its binding objects share basenames with the display's, so they are copied under their own names (ar replaces by name).
print(run(['python3', ROOT / 'scripts/build-adreno740-accelerator.py'], text=True), end='')
accel = ROOT / 'build/adreno740-accelerator'
for name in ('original-bindings.o', 'original-addresses.o'):
    shutil.copy2(accel / name, OUT / ('accel-' + name))
board_objects += [accel / 'NativeAdrenoAccelerator.late.o', OUT / 'accel-original-bindings.o', OUT / 'accel-original-addresses.o']
# UsbDebugCdc.cpp weak-imports iPadOS-only counters and drivers (GPU, backlight, battery, Wi-Fi...). A weak import that stays undefined
# fails the static kernel link, and an absolute 0 does not work either (direct BL out of range), so each one nothing else defines
# gets a weak no-op definition: functions return 0 (the *_str ones a "not in this build" text), data is 4 KiB of zeros.
def nm(paths, flags):
    out = subprocess.run(['llvm-nm', *flags, '-j', *map(str, paths)], capture_output=True, text=True).stdout
    return {line.strip() for line in out.splitlines() if line.strip() and not line.endswith(':')}
weak_refs = {line.split()[-1] for line in subprocess.run(['llvm-nm', '-m', '-u', str(OUT / 'UsbDebugCdc.o')], capture_output=True,
             text=True).stdout.splitlines() if 'weak' in line}
kernel_objects = list((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837').rglob('*.o'))
defined = nm(board_objects + kernel_objects + [a for a in (ROOT / 'build').rglob('*.a') if a.parent != OUT], ['--defined-only', '-g'])
absent = sorted(weak_refs - defined)
import re
usb_sources = '\n'.join(p.read_text() for p in (ROOT / 'kits/ruan').glob('UsbDebug*.[ch]*'))


def absent_definition(symbol):
    name = symbol[1:]   # Mach-O C names carry a leading underscore
    if not re.search(r'\b' + re.escape(name) + r'\s*\(', usb_sources):
        return f'__attribute__((weak, aligned(16))) char {name}[4096];\n'
    if name.endswith('_str'):
        return f'__attribute__((weak)) const char *{name}(void) {{ return "not in the macOS build"; }}\n'
    return f'__attribute__((weak)) long {name}(void) {{ return 0; }}\n'


(OUT / 'usbdbg-absent.c').write_text('/* generated by build-gop-legacy-display-27.py: iPadOS-only symbols UsbDebugCdc.cpp weak-imports */\n'
                                     + ''.join(absent_definition(n) for n in absent))
run(['xcrun', 'clang', '-target', TRIPLE, '-mkernel', '-c', OUT / 'usbdbg-absent.c', '-o', OUT / 'usbdbg-absent.o'])
board_objects.append(OUT / 'usbdbg-absent.o')
print(f'UsbDebugCdc: {len(weak_refs)} weak imports, {len(absent)} absent (weak no-op definitions): ' + ' '.join(absent))
print(f'Board drivers: {len(board_objects)} objects')
archive = OUT / 'libNativeLegacyDisplay.late.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, *board_objects])
sources = [ROOT / 'drivers/GopFramebuffer/NativeLegacyDisplay.cpp', ROOT / 'drivers/GopFramebuffer/NativeLegacyRequest.h',
           ROOT / 'drivers/GopFramebuffer/NativeGOPPresenter.cpp', ROOT / 'drivers/GopFramebuffer/NativeGOPWire.h',
           ROOT / 'drivers/GopFramebuffer/NativeHalfCodes.h', source,
           ROOT / 'scripts' / ABI_PATH.name, keystore_source, ROOT / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',
           ROOT / 'kits/ruan/RuanGpuPower.c', ROOT / 'kits/ruan/RuanMacBootProgress.c', ROOT / 'drivers/Adreno740/NativeAdrenoAccelerator.cpp']
if MACOS:
    sources += [source.parent / 'NativeHidInput.cpp', source.parent / 'HidInputProbe.cpp', ABI_PATH.with_name(f'hid-device-{BUILD}.json'),
                OUT / 'OriginalHidDeviceABI.h']


def source_name(path):
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


(OUT / 'build-report.json').write_text(json.dumps(dict(
    source_sha256={source_name(p): sha(p) for p in sources}, generated_abi_sha256=sha(OUT / 'OriginalLegacyFramebufferABI.h'),
    kernelcache_sha256=abi['kernelcache_sha256'], compiled_object_sha256=sha(obj), archive_sha256=sha(archive),
    bindings={n: hex(v) for n, v in bindings.items()}, virtual_entries=len(slots), owned_entries=owned,
    inherited_native_entries=native, original_bindings='fixed-collection-addresses', original_instructions_modified=False), indent=2) + '\n')
print(f'Built the owned Legacy backend on the {BUILD} class: {len(owned)} owned, {len(native)} native, {len(bindings)} original bindings')
