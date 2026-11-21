#!/usr/bin/env python3
"""Compile check of the video port with the exact flags of ruan's last XNU build (its compile records), the recipes of
unixnu-ruan/scripts/build-ram-root-probe.py: C with startup.o's flags, C++ with IOPlatformExpert.cpo's, -Wall -Wextra -Werror,
-mgeneral-regs-only; QcomIris's C with sheng's build-qcom-iris.py additions (-Os, -Wno-unused-parameter). Objects go to a temp
directory and are checked to be arm64 Mach-O; nothing is linked, nothing is written into either tree. Then the host test of
RuanVideoCc.c runs.

usage: compile-check.py [TREE] [QCOM_IRIS]
  TREE       the kernel build tree (default /root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/tree)
  QCOM_IRIS  drivers/QcomIris fetched and patched by scripts/fetch-qcom-iris.sh, with its Linux deps (default /tmp/vcheck/QcomIris)
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
DEVICE = Path(__file__).resolve().parents[5] / 'unixnu-ruan'       # read only: QcomMdt.h, QcomRpmh.h, QcomScm.h, RuanZap.h
TREE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/tree')
IRIS = Path(sys.argv[2]) if len(sys.argv) > 2 else Path('/tmp/vcheck/QcomIris')
RECORDS = TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
LINUX_IRIS = IRIS / 'deps/linux/drivers/media/platform/qcom/iris'
KIT = TREE / 'kits/ArmSmmu4K'
INCLUDES = [f'-I{d}' for d in (PORT / 'package', DEVICE / 'package', IRIS / 'include', IRIS / 'shim', LINUX_IRIS, KIT)]
IRIS_INCLUDES = [f'-I{d}' for d in (IRIS / 'include', IRIS / 'src', IRIS / 'shim', LINUX_IRIS, KIT, TREE / '.cache/xnu-work/osfmk')]
C, CXX = 'osfmk/DEVELOPMENT/startup.o.json', 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'


def command(record_name, source, obj, cxx, extra):
    record = json.loads((RECORDS / record_name).read_text().strip().rstrip(','))
    out, suppressions, args, i = [], [], record['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == record['file']:
            out.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                out += ['-o', str(obj)]
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif cxx and value.startswith('-Wno-'):
            suppressions.append(value)
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            out.append(value)
        i += 1
    out += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror']
    if cxx:
        out += ['-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-reorder-init-list',
                '-Wno-deprecated-enum-enum-conversion', '-I', record['directory'], '-I', str(TREE / '.cache/xnu-work/osfmk')]
    return out + list(extra) + suppressions, record['directory']


HBB = ['-DQCOM_IRIS_UBWC_HBB=14']   # ruan: LPDDR4X (ubwc_config_parrot + msm_vidc_ddr_ubwc_config 0xe)
JOBS = [(C, PORT / 'package/RuanVideoCc.c', False, INCLUDES), (C, PORT / 'package/Sm8550AppsSmmu.c', False, INCLUDES),
        (CXX, PORT / 'package/RuanVideo.cpp', True, INCLUDES),
        *[(C, s, False, ['-Os', '-Wno-unused-parameter', *HBB, *IRIS_INCLUDES])
          for s in [*sorted((IRIS / 'src').glob('*.c')), LINUX_IRIS / 'iris_vpu_buffer.c']],
        # B4's decoder device, compiled now so its kernel API use is known to hold on iPadOS 17's XNU (not linked by this port yet)
        (CXX, IRIS / 'kernel/IrisMedia.cpp', True, [*IRIS_INCLUDES, f'-I{IRIS / "kernel"}'])]
failed = 0
with tempfile.TemporaryDirectory() as out:
    for record, source, cxx, extra in JOBS:
        obj = Path(out) / (source.stem + '.o')
        cmd, cwd = command(record, source, obj, cxx, extra)
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        ok = not r.returncode and struct.unpack_from('<4I', obj.read_bytes()) == (0xfeedfacf, 0x100000c, 0, 1)
        print(('PASS ' if ok else 'FAIL ') + source.name)
        if not ok:
            failed += 1
            sys.stdout.write(r.stderr[-6000:])
host = Path(tempfile.mkdtemp()) / 'video_cc_host'
r = subprocess.run(['clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', f'-I{PORT / "package"}', str(PORT / 'check/video_cc_host.c'),
                    str(PORT / 'package/RuanVideoCc.c'), '-o', str(host)], capture_output=True, text=True)
r = r if r.returncode else subprocess.run([str(host)], capture_output=True, text=True)
print(('PASS ' if not r.returncode else 'FAIL ') + 'video_cc_host: ' + (r.stdout + r.stderr).strip()[-2000:])
sys.exit(1 if failed or r.returncode else 0)
