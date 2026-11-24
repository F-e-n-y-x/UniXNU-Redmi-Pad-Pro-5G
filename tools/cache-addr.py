#!/usr/bin/env python3
"""cache-addr.py MAP SLIDE ADDR [ADDR ...]: name the library and segment of runtime dyld-shared-cache addresses.

MAP is the cache's .map file (host-work/macos-extract/26C5035r__Mac17,11/dyld_shared_cache_arm64e.map), SLIDE the shared-region
slide the kernel logged (`ruan-sr: slide 0x…`, ruan-srtrace=1), ADDR runtime addresses (e.g. a ruan-ubrk / ruan-uabort pc or lr).
Prints: address, unslid address, library, segment and the offset into that segment.
"""
import re
import sys


def load(path):
    segments, image = [], None
    for line in open(path, errors='replace'):
        if line.startswith('/'):
            image = line.strip()
        m = re.match(r'\s+(\S+)\s+0x([0-9A-Fa-f]+)\s+->\s+0x([0-9A-Fa-f]+)', line)
        if image and m:
            segments.append((int(m.group(2), 16), int(m.group(3), 16), image, m.group(1)))
    return sorted(segments)


def lookup(segments, addr):
    for start, end, image, seg in segments:
        if start <= addr < end:
            return image, seg, addr - start
    return None


def self_test():
    segs = [(0x1000, 0x2000, '/usr/lib/a.dylib', '__TEXT')]
    assert lookup(segs, 0x1800) == ('/usr/lib/a.dylib', '__TEXT', 0x800)
    assert lookup(segs, 0x2000) is None


if __name__ == '__main__':
    self_test()
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    segs, slide = load(sys.argv[1]), int(sys.argv[2], 0)
    for a in sys.argv[3:]:
        addr = int(a, 0)
        hit = lookup(segs, addr - slide)
        where = f'{hit[0]} {hit[1]}+{hit[2]:#x}' if hit else 'not in the cache map'
        print(f'{addr:#x} (unslid {addr - slide:#x}): {where}')
