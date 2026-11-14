#!/usr/bin/env python3
"""Parse ruan's ramoops region as DarwinBoot's "upload" sends it (fastboot -s unixnu-ramdisk get_staged FILE: the 2 MiB at
0x80D00000), in Linux's pstore/ramoops layout of the stock device tree (reserved-memory ramoops@80D00000):

  0x000000  dmesg-0   256 KiB   Android kernel's last dmesg (kmsg_dump on reboot/panic; printk.always_kmsg_dump=1)
  0x040000  dmesg-1   256 KiB
  0x080000  console   512 KiB   Android's last kernel console, or XNU's log with ruan-ramoops=1 ("[ev]" lines = events)
  0x100000  pmsg      1 MiB     Android logd's pmsg (binary)

Each zone is a persistent_ram_buffer: u32 "DBGC" signature, u32 start (write position), u32 size (bytes used), then the
ring; the text is data[start:size] + data[0:start], as fs/pstore/ram_core.c persistent_ram_save_old reads it (no ECC on ruan).

usage: read-ramoops.py DUMP [--out DIR]     writes DIR/<zone>.txt (pmsg: .bin) and prints a summary; DIR defaults to DUMP.d
       read-ramoops.py --self-test
Exit status 0 when at least one zone carries the signature (the region survived the reset), 1 otherwise.
"""
import argparse
import struct
import sys
from pathlib import Path

SIG = 0x43474244                         # "DBGC" (PERSISTENT_RAM_SIG)
ZONES = (('dmesg-0', 0x000000, 0x40000), ('dmesg-1', 0x040000, 0x40000), ('console', 0x080000, 0x80000),
         ('pmsg', 0x100000, 0x100000))
REGION = 0x200000


def zone_text(blob):
    """The zone's bytes in order, or None without a valid header."""
    sig, start, size = struct.unpack_from('<III', blob, 0)
    data = blob[12:]
    if sig != SIG or size > len(data) or start > len(data):
        return None
    return data[start:size] + data[:start]


def parse(dump):
    if len(dump) != REGION:
        raise SystemExit(f'expected {REGION} bytes (the whole region), got {len(dump)}')
    return {name: zone_text(dump[off:off + length]) for name, off, length in ZONES}


def self_test():
    region = bytearray(REGION)
    struct.pack_into('<III', region, 0x80000, SIG, 3, 0x80000 - 12)      # wrapped console: ...newest at the front
    ring = 0x80000 - 12
    region[0x8000c:0x8000c + ring] = b'x' * ring
    region[0x8000c:0x8000c + 3] = b'END'
    region[0x8000c + 3:0x8000c + 8] = b'START'
    struct.pack_into('<III', region, 0, SIG, 5, 5)                       # unwrapped dmesg-0
    region[12:17] = b'hello'
    zones = parse(bytes(region))
    assert zones['dmesg-0'] == b'hello'
    assert zones['console'].startswith(b'START') and zones['console'].endswith(b'END') and len(zones['console']) == ring
    assert zones['dmesg-1'] is None and zones['pmsg'] is None
    print('read-ramoops: self-test PASS')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump', nargs='?', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.dump:
        ap.error('DUMP is required')
    zones = parse(args.dump.read_bytes())
    out = args.out or args.dump.with_name(args.dump.name + '.d')
    out.mkdir(parents=True, exist_ok=True)
    found = 0
    for name, text in zones.items():
        if text is None:
            print(f'{name:8} no DBGC header (cleared, never written, or not ramoops)')
            continue
        found += 1
        path = out / (name + ('.bin' if name == 'pmsg' else '.txt'))
        path.write_bytes(text)
        lines = text.decode('utf-8', 'replace').splitlines()
        print(f'{name:8} {len(text):7} bytes -> {path}')
        if name != 'pmsg':
            for line in lines[-8:]:
                print(f'    {line[:160]}')
    return 0 if found else 1


if __name__ == '__main__':
    sys.exit(main())
