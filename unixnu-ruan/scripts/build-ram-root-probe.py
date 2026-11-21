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


def compile_like(record_name, source, obj, extra=()):
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
    command += ['-I', str(OUT), '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', *extra]
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

compile_cxx_like('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', DEVICE / 'package/RuanBacklightSvc.cpp', OUT / 'RuanBacklightSvc.o')
subprocess.run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_RuanBacklightSvc.cpp',
                OUT / 'RuanBacklightSvc.o', OUT / 'RuanBacklightSvc.late.o'], check=True)

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

# Qualcomm WCN6750 Wi-Fi (macOS/docs/research/wifi/port; filipnavara/UniXNU-asteroids 9f07af3's stack): ruan-wifi=1|2 (WPSS survey, boot +
# the board's QRTR router) is always built (small, no network). The module (default on; RUAN_WIFI_MODULE=0 leaves it out) adds ruan-wifi=3-5: drivers/Ath11kWifi (Linux ath11k +
# mac80211 + cfg80211, several MB of kernel code; fetched into .cache/Ath11kWifi by scripts/fetch-ath11k-wifi.sh, its own tools fetch the
# pinned Linux sources), RuanWifi.cpp (the board worker) and WifiNet.cpp (en0; its classes staged late as RuanBacklightSvc's).
WIFI_MODULE = os.environ.get('RUAN_WIFI_MODULE', '1') == '1'
IOKIT_RECORD = 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'


def wifi_module_objects():
    if not WIFI_MODULE:
        return []
    ath = Path(os.environ.get('ATH11K_WIFI_DIR', DEVICE / '.cache/Ath11kWifi'))
    if not (ath / 'include/nw_abi.h').is_file():
        subprocess.run(['bash', DEVICE / 'scripts/fetch-ath11k-wifi.sh', ath], check=True)
    include = ['-I', str(ath / 'include')]
    compile_cxx_like(IOKIT_RECORD, DEVICE / 'package/WifiNet.cpp', OUT / 'WifiNet.o', include)
    subprocess.run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_WifiNet.cpp',
                    OUT / 'WifiNet.o', OUT / 'WifiNet.late.o'], check=True)
    built = subprocess.run(['python3', DEVICE / 'scripts/build-ath11k-wifi.py', ath, OUT / 'ath11k-wifi'], check=True,
                           capture_output=True, text=True).stdout
    module = [Path(line[4:]) for line in built.splitlines() if line.startswith('OBJ ')]
    if not module:
        raise SystemExit('build-ath11k-wifi.py built no objects')
    return [compile_cxx_like(IOKIT_RECORD, DEVICE / 'package/RuanWifi.cpp', OUT / 'RuanWifi.o', include), OUT / 'WifiNet.late.o',
            compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/AsteroidsWifiFiles.c', OUT / 'AsteroidsWifiFiles.o'),
            *module]


QCA_BT = Path(os.environ.get('QCA_BLUETOOTH_DIR', DEVICE / '.cache/QcaBluetooth'))


def bluetooth_include():
    if not (QCA_BT / 'out/linux/btqca.c').is_file():
        subprocess.run(['bash', DEVICE / 'scripts/fetch-qca-bluetooth.sh', QCA_BT], check=True)
    return [f'-I{QCA_BT / d}' for d in ('include', 'src', 'shim', 'out/linux')]


def bluetooth_objects():
    built = subprocess.run(['python3', DEVICE / 'scripts/build-qca-bluetooth.py', QCA_BT, OUT / 'qca-bluetooth'], check=True,
                           capture_output=True, text=True).stdout
    return [Path(line[4:]) for line in built.splitlines() if line.startswith('OBJ ')]


# The iris2 VPU (macOS/docs/research/video/port): ruan-video=1-4 (survey, power, firmware boot, one decoder session); default built,
# RUAN_VIDEO=0 leaves it out. drivers/QcomIris of UniXNU-sheng (GPL-2.0-only) is fetched into .cache/QcomIris by scripts/fetch-qcom-iris.sh
# with patches/qcom-iris-ruan.patch; its own tools fetch the pinned Linux iris files. The ArmSmmu4K kit objects come from build-ruan-adreno.
# RuanVideo.o is pulled by BootProgress.c's ruan_video_start (built with -DRUAN_VIDEO=1).
VIDEO = os.environ.get('RUAN_VIDEO', '1') == '1'


def video_objects():
    if not VIDEO:
        return []
    iris = Path(os.environ.get('QCOM_IRIS_DIR', DEVICE / '.cache/QcomIris'))
    if not (iris / 'include/QcomIrisCore.h').is_file():
        subprocess.run(['bash', DEVICE / 'scripts/fetch-qcom-iris.sh', iris], check=True)
    subprocess.run(['python3', iris / 'tools/fetch-sources.py'], check=True, stdout=subprocess.DEVNULL)
    linux = iris / 'deps/linux/drivers/media/platform/qcom/iris'
    kit = ROOT / 'kits/ArmSmmu4K'
    common = ['-I', str(iris / 'include'), '-I', str(iris / 'shim'), '-I', str(linux), '-I', str(kit)]
    out = OUT / 'video'
    out.mkdir(exist_ok=True)
    # sheng's build-qcom-iris.py flags: -Os (text room), unused parameters of the Linux-shaped ops allowed; ruan's UBWC bank bit 14
    iris_flags = ['-Os', '-Wno-unused-parameter', '-DQCOM_IRIS_UBWC_HBB=14', *common, '-I', str(iris / 'src'),
                  '-I', str(ROOT / '.cache/xnu-work/osfmk')]
    return [compile_cxx_like(IOKIT_RECORD, DEVICE / 'package/RuanVideo.cpp', out / 'RuanVideo.o', common),
            compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/RuanVideoCc.c', out / 'RuanVideoCc.o'),
            compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/Sm8550AppsSmmu.c', out / 'Sm8550AppsSmmu.o'),
            *[compile_like('osfmk/DEVELOPMENT/startup.o.json', s, out / (s.stem + '.o'), iris_flags)
              for s in [*sorted((iris / 'src').glob('*.c')), linux / 'iris_vpu_buffer.c']]]


# HakiOS's C is warning-clean under its own flags, not under -Wextra -Werror; its firmware headers live outside git.
HAKIOS_C = ['-I', str(DEVICE / '.cache/hakios-fw'), '-Wno-error', '-Wno-unused-function', '-Wno-unused-parameter',
            '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-sign-compare', '-Wno-missing-prototypes']
objects = [ramblock / 'NativeRamBlock.o', ramblock / 'original-bindings.o',
           compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/RamBlockNoStore.c', OUT / 'RamBlockNoStore.o'),
           compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/RamRootProbe.c', OUT / 'RamRootProbe.o'),
           compile_cxx_like('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', DEVICE / 'package/KeyStoreProbe.cpp',
                            OUT / 'KeyStoreProbe.o'),
           OUT / 'NativeKeyStoreEvents.late.o',
           compile_like('bsd/DEVELOPMENT/kern_trustcache.o.json', DEVICE / 'package/RamRestoreTrust.c', OUT / 'RamRestoreTrust.o'),
           # the boot_progress_* hooks (patches/xnu/ruan-boot-progress.patch), with the flags of the startup.c that calls them
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/BootProgress.c', OUT / 'BootProgress.o',
                        [f'-DRUAN_VIDEO={int(VIDEO)}']),
           # ruan-autoreset's reboot into fastboot (package/BootReset.c, from UniXNU-Dizi-Ruan)
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/BootReset.c', OUT / 'BootReset.o'),
           # EL0 fault records and the SIGABRT reason scan for the crash banner (package/BootFaultProbe.c, from UniXNU-fuxi)
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/BootFaultProbe.c', OUT / 'BootFaultProbe.o'),
           # Touch and buttons (HakiOS drivers, proven on ruan with iOS 16.5) feeding NativeHidInput: package/RuanInput.c.
           # The NT36532 firmware header is generated from stock vendor firmware and stays out of git (.cache/hakios-fw).
           compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/RuanInput.c', OUT / 'RuanInput.o'),
           compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/RuanTouch.c', OUT / 'RuanTouch.o', HAKIOS_C),
           compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/RuanButtons.c', OUT / 'RuanButtons.o', HAKIOS_C),
           # Battery / PowerSource subsystem (SPMI PM7250B / PMK8350 vadc & IOPMPowerSource)
           compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/RuanBattery.c', OUT / 'RuanBattery.o', HAKIOS_C),
           compile_cxx_like('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', DEVICE / 'package/HakiosBattery.cpp', OUT / 'HakiosBattery.o'),
           # Backlight subsystem (MIPI DSI DCS 0x51 & AppleARMBacklight conformance)
           compile_cxx_like('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json', DEVICE / 'package/RuanBacklight.cpp', OUT / 'RuanBacklight.o'),
           # ruan-bl-svc=1: the non-blocking service (package/RuanBacklightSvc.cpp + GeniI2c.c, from UniXNU-Dizi-Ruan sheng), its class
           # staged late as the key store's: registered by RuanBacklight.cpp between preModLoad and postModLoad
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/GeniI2c.c', OUT / 'GeniI2c.o'),
           OUT / 'RuanBacklightSvc.late.o',
           # ruan-cpufreq: EPSS frequency vote (package/RuanCpuFreq.c, layout from UniXNU-Dizi-Ruan CpuFreqProbe.cpp)
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/RuanCpuFreq.c', OUT / 'RuanCpuFreq.o'),
           # ruan-120hz=1|2: panel 60 -> 120 Hz by the vertical front porch (docs/research/120hz/plan.md)
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/RuanPanel120.c', OUT / 'RuanPanel120.o'),
           # Adreno 710 bring-up stage S1 (gpu-dev): gated GCC/GPUCC reads, CX GDSC power-up, GPU SMMU survey (ruan-gpu=<1-3>)
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/RuanGpuPower.c', OUT / 'RuanGpuPower.o'),
           # HFI queue table for gpu 6 (UniXNU kits/AdrenoFirmware/AdrenoHfi.c, copied into package/)
           compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/AdrenoHfi.c', OUT / 'AdrenoHfi.o'),
           # Qualcomm WCN6750 Wi-Fi (see WIFI_MODULE above): the WPSS host thread and WpssProbe (asteroids', ported), the Qcom* helpers and
           # the firmware reader (asteroids', unchanged); RuanWifiWpss.o is pulled by BootProgress.c's ruan_wifi_start. Replaces the
           # unused RuanWifiProbe.c (same ruan_wifi_init / ruan_wifi_status_str names).
           compile_cxx_like(IOKIT_RECORD, DEVICE / 'package/RuanWifiWpss.cpp', OUT / 'RuanWifiWpss.o',
                            ['-DRUAN_WIFI_MODULE=1'] if WIFI_MODULE else []),
           compile_cxx_like(IOKIT_RECORD, DEVICE / 'package/WpssProbe.cpp', OUT / 'WpssProbe.o'),
           *[compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / f'package/{n}.c', OUT / f'{n}.o')
             for n in ('QcomGlink', 'QcomMdt', 'QcomQmp', 'QcomQrtr', 'QcomRpmh', 'QcomSmem', 'QcomSmp2p', 'FatVolume')],
           # clang 24's -Wunused-but-set-global flags asteroids' unchanged gPart (kept for the partition's lifetime)
           compile_like('bsd/DEVELOPMENT/kern_exec.o.json', DEVICE / 'package/AdspFirmware.c', OUT / 'AdspFirmware.o',
                        ['-Wno-unused-but-set-global']),
           *wifi_module_objects(),
           # Qualcomm WCN6750 Bluetooth (macOS/docs/research/bluetooth/port; asteroids' QcaBluetooth for the WCN6750): ruan-bt=1-4. RuanBluetooth.o is
           # pulled by BootProgress.c's ruan_bt_start; QcaBluetooth (fetched into .cache/QcaBluetooth, Linux btqca.c staged by its own tool),
           # RuanGeniUart.c and BluetoothFirmware.c come from build-qca-bluetooth.py with kern_exec.c's flags.
           compile_cxx_like(IOKIT_RECORD, DEVICE / 'package/RuanBluetooth.cpp', OUT / 'RuanBluetooth.o', bluetooth_include()),
           *bluetooth_objects(),
           *video_objects(),
           # ruan-audio=N: speaker path stages (docs/research/audio): the ADSP host (UniXNU-sheng AdspProbe.cpp, ported), GPR and AudioReach
           # packets (sheng's, + codec DMA RX), the staged bring-up. Same ruan_audio_init / ruan_audio_status_str names as the old
           # RuanAudioProbe.c status stub it replaces (the stub's files can be deleted). Exclusive with ruan-wifi >= 2 (one IPCC/QMP/TCS owner).
           compile_cxx_like(IOKIT_RECORD, DEVICE / 'package/RuanAudio.cpp', OUT / 'RuanAudio.o'),
           compile_cxx_like(IOKIT_RECORD, DEVICE / 'package/RuanAdsp.cpp', OUT / 'RuanAdsp.o'),
           *[compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / f'package/{n}.c', OUT / f'{n}.o') for n in ('ShengGpr', 'ShengAudioReach')]]
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
