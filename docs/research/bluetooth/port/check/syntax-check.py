#!/usr/bin/env python3
"""Syntax check of the Bluetooth port with the exact flags of ruan's last XNU build (its compile records), -fsyntax-only: nothing is
built or linked. Recipes as ../../../wifi/port/check/syntax-check.py: C with kern_exec.o flags (as build-qca-bluetooth.py), C++ with
IOPlatformExpert.cpo flags (as build-ram-root-probe.py builds RuanWifi.cpp).

usage: syntax-check.py [TREE] [QCA_BLUETOOTH]
  TREE           the kernel build tree (default /root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/tree)
  QCA_BLUETOOTH  a scripts/fetch-qca-bluetooth.sh output (default /tmp/bt-ios17/QcaBluetooth)
"""
import json
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
DEVICE = Path(__file__).resolve().parents[5] / 'unixnu-ruan'       # read only: SmmuBypass.h, FatVolume.h
TREE = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/root/unixnu-work/UniXNU/out/ruan/ipados-17.7.11-21H461/ruan/tree')
QCA = Path(sys.argv[2]) if len(sys.argv) > 2 else Path('/tmp/bt-ios17/QcaBluetooth')
RECORDS = TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
INCLUDES = ['-I', str(PORT / 'package'), '-I', str(DEVICE / 'package'),
            *[f'-I{QCA / d}' for d in ('include', 'src', 'shim', 'out/linux')]]


def command(record_name, source, cxx):
    record = json.loads((RECORDS / record_name).read_text().strip().rstrip(','))
    out, suppressions, args, i = [], [], record['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == record['file']:
            out.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif cxx and value.startswith('-Wno-'):
            suppressions.append(value)
        elif not value.startswith('-W') and value not in ('-MD', '-MP', '-c'):
            out.append(value)
        i += 1
    out += ['-fsyntax-only', '-mgeneral-regs-only', '-DQBT_SOC_TYPE=QCA_WCN6750']
    if cxx:
        out += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-I', record['directory'],
                '-I', str(TREE / '.cache/xnu-work/osfmk')]
    else:
        out += ['-Wno-sign-compare']   # build-qca-bluetooth.py's flags (asteroids'): QcaBluetooth is not -Wextra clean
    return out + INCLUDES + suppressions, record['directory']


C, CXX = 'bsd/DEVELOPMENT/kern_exec.o.json', 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
JOBS = [(CXX, PORT / 'package/RuanBluetooth.cpp', True), (C, PORT / 'package/RuanGeniUart.c', False),
        (C, PORT / 'package/BluetoothFirmware.c', False),
        *[(C, QCA / f, False) for f in ('src/qbt_core.c', 'src/qbt_linux.c', 'src/qbt_geni.c', 'src/qbt_transport.c', 'out/linux/btqca.c')]]
failed = 0
for record, source, cxx in JOBS:
    cmd, cwd = command(record, source, cxx)
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    print(('PASS ' if not r.returncode else 'FAIL ') + source.name)
    if r.returncode:
        failed += 1
        sys.stdout.write(r.stderr[-6000:])
sys.exit(1 if failed else 0)
