#!/bin/bash
# Rebuild ruan's UniXNU iPadOS 17.7.11 boot image (RAM only): kernel and payload with UniXNU, then the payload repacked
# as a 56 MiB FAT in an "XNUP" container that the BootShim copies to 0xF4000000 (ABL's header-v4 initrd puts the
# vendor ramdisk in front of a boot-image ramdisk; UniXNURamDiskDxe here falls back to that container).
set -euo pipefail
W=/root/unixnu-work; U=$W/UniXNU; O=$U/out/ruan/ipados-17.7.11-21H461/ruan
export PATH=/opt/unixnu-linux/bin:$PATH
cd $U
./unixnu build kernel > $W/k.log 2>&1 || { tail -20 $W/k.log; exit 1; }
./unixnu build payload --uefi ../uefi-ruan --unique-chip-id "$(cat $W/unique-chip-id.txt)" > $W/p.log 2>&1 || { tail -20 $W/p.log; exit 1; }
X=$W/xnup; rm -rf $X; mkdir -p $X/src
(cd $X/src && 7z x -y $O/payload/payload.img >/dev/null)
truncate -s 56M $X/fat.img
/usr/sbin/mkfs.vfat -F 32 -n RUAN $X/fat.img >/dev/null
MTOOLS_SKIP_CHECK=1 mcopy -s -i $X/fat.img $X/src/EFI ::/
python3 -c "import struct,sys; d=open('$X/fat.img','rb').read(); open('$X/payload.xnup','wb').write(b'XNUP'+struct.pack('<I',len(d))+d)"
cd "$(dirname "$0")"
MU=$W/uefi-ruan FD=$W/uefi-ruan/Build/ruanPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd python3 mkxnuimg-ruan.py -p $X/payload.xnup -o $W/ipados-xnup.img | tail -1
