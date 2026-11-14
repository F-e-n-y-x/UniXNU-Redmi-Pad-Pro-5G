#!/bin/bash
# Build ruan's iPadOS 17.7.11 RAM-root boot images (fastboot boot only; nothing is written to the tablet).
#
#   scripts/build-ramroot.sh mini   root = a 4 MiB APFS (launchd + dyld) inside the payload: the existing XNUP/0xF4000000
#                                   path, no USB. Proves the md0 root mount; dyld then stops without its shared cache.
#   scripts/build-ramroot.sh usb    root = the trimmed OS + SystemOS image (1.5-2 GB), sent over USB with stock
#                                   fastboot once DarwinBoot has placed its window (DarwinBoot UsbRamdisk.h). The payload
#                                   carries only its 32-byte descriptor (size + CRC32), so image and boot image belong
#                                   together: rebuild both, or neither.
#
# Both: kernel args + rd=md0, the IPSW's Static + SystemOS trust caches, owned_memory_size from board.toml.
# Outputs in $OUT (default /root/unixnu-work/ramroot/out): ruan-ramroot-<mode>.img, root-<mode>.apfs, SHA256SUMS.
# Env: REBUILD_KERNEL=1 rebuilds the kernel first; REBUILD_ROOT=1 rebuilds the root image even when it exists.
#      KERNEL_ARGS="a=1 b=2" adds boot-args (e.g. "ram-root-probe=1", see package/RamRootProbe.c); TAG=x names the image
#      ruan-ramroot-<mode>-x.img (and SHA256SUMS-<mode>-x) so variants of one root sit side by side.
#      DATA_CONTAINER=1 (usb): Data as its own container after the root (make-ramroot.py --data-container) instead of a
#      volume in it; adds ram-data-split=<offset> ram-data=1 ram-data-dev=md1 (the offset is root-usb.split in $OUT).
#      RAMBLOCK=1 (with DATA_CONTAINER=1): the Data container is served by NativeRamBlock (a real IOMedia, which APFS needs to
#      mount it) and mounted by its volume UUID: ram-data-uuid=<it> instead of ram-data-dev=md1.
#      WORK=dir: make-ramroot.py's work directory (default $RAMROOT/work-full). FD=file: the UEFI FD to pack.
set -euo pipefail
MODE=${1:?usage: build-ramroot.sh mini|usb}
NAME=$MODE${TAG:+-$TAG}
extra=(); for arg in ${KERNEL_ARGS:-}; do extra+=("--kernel-arg=$arg"); done
case $MODE in mini|usb) ;; *) echo "build-ramroot.sh: mode is mini or usb" >&2; exit 2 ;; esac
W=/root/unixnu-work; U=$W/UniXNU; D=$W/UniXNU-ruan; O=$U/out/ruan/ipados-17.7.11-21H461/ruan
I=$U/out/ipsw/21H461; R=${RAMROOT:-$W/ramroot}; OUT=${OUT:-$R/out}
MKXNUIMG=${MKXNUIMG:-"/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/uefi/mkxnuimg-ruan.py"}
# The default firmware is the one with the receiver's upload/getvar (md1log, 2026-11-14): the plain uefi-ruan build has an older receiver
# that refuses current roots ("size differs from the descriptor", rb6 2026-11-14).
FD=${FD:-$R/SILICIUM_UEFI-log.fd}; WORK=${WORK:-$R/work-full}
data=--data; [ -n "${DATA_CONTAINER:-}" ] && data=--data-container
export PATH=/opt/unixnu-linux/bin:$PATH
mkdir -p "$OUT"

root=$OUT/root-$MODE.apfs
if [ ! -f "$root" ] || [ -n "${REBUILD_ROOT:-}" ]; then
	if [ $MODE = mini ]; then
		python3 $D/scripts/make-ramroot.py --mini --os $I/044-76040-038.dmg --work $R/work-mini --out "$root"
	else
		python3 $D/scripts/make-ramroot.py --full --os $I/044-76040-038.dmg --systemos $I/044-76034-038.dmg \
			--work "$WORK" --out "$root" --descriptor $OUT/root-usb.desc $data
	fi
fi
[ $MODE = usb ] && [ ! -f $OUT/root-usb.desc ] && { echo "build-ramroot.sh: $OUT/root-usb.desc missing: REBUILD_ROOT=1" >&2; exit 1; }
# DarwinBoot places the root inside owned_memory_size (board.toml) beside the kernel and its data: a root that does not fit hangs
# at the UniXNU menu with nothing on screen (rbar2 with 768 MiB Data, 2919 MiB > 2816 MiB, 2026-11-14). 128 MiB stays for the rest.
if [ $MODE = usb ]; then
	window=$(( $(sed -n "s/^owned_memory_size = \"\(0x[0-9A-Fa-f]*\)\"/\1/p" $D/board.toml) ))
	[ $(( $(stat -c %s "$root") + (128 << 20) )) -le $window ] || { echo "build-ramroot.sh: $root ($(( $(stat -c %s "$root") >> 20 )) MiB) + 128 MiB does not fit DarwinBoot's $((window >> 20)) MiB window: lower DATA_SLACK" >&2; exit 1; }
fi
if [ -n "${DATA_CONTAINER:-}" ]; then
	[ -f $OUT/root-usb.split ] || { echo "build-ramroot.sh: $OUT/root-usb.split missing: REBUILD_ROOT=1" >&2; exit 1; }
	data_dev=ram-data-dev=md1
	if [ -n "${RAMBLOCK:-}" ]; then
		# the Data volume's UUID: the APFS volume superblock (APSB) in the Data container, bytes 240..256
		data_dev=ram-data-uuid=$(python3 - "$root" "$(cat $OUT/root-usb.split)" <<'PY'
import sys, uuid
f = open(sys.argv[1], 'rb'); f.seek(int(sys.argv[2])); data = f.read(64 << 20)
found = {str(uuid.UUID(bytes=data[at + 240:at + 256])).upper() for at in range(0, len(data), 4096) if data[at + 32:at + 36] == b'APSB'}
assert len(found) == 1, f'Data container: expected one volume UUID, found {found}'
print(found.pop())
PY
)
	fi
	for arg in ram-data-split=$(cat $OUT/root-usb.split) ram-data=1 $data_dev; do extra+=("--kernel-arg=$arg"); done
fi
python3 $U/scripts/bundle-ipsw-trust.py $OUT/trust.bundle $I/044-76040-038.dmg.trustcache $I/044-76034-038.dmg.trustcache >/dev/null

cd $U
if [ -n "${REBUILD_KERNEL:-}" ] || [ ! -f $O/kernel/kernel.macho ]; then
	# NativeRamBlock binds to the original IOStorageFamily through a previous ruan kernel (build-ram-root-probe.py).
	C=$O/tree/build/xnu-prelink/ruan-candidate
	if [ ! -f $C.macho ]; then mkdir -p $(dirname $C); cp $O/kernel/kernel.macho $C.macho; cp $O/kernel/kernel.json $C.json; fi
	./unixnu build kernel > $OUT/kernel.log 2>&1 || { tail -20 $OUT/kernel.log; exit 1; }
	python3 - $C.json $O/kernel/kernel.json <<'PY'
import json, sys
storage = [[e for e in json.load(open(p))['extents'] if e['module'] == 'com.apple.iokit.IOStorageFamily'] for p in sys.argv[1:]]
assert storage[0] and storage[0] == storage[1], 'IOStorageFamily moved: the NativeRamBlock bindings are stale'
PY
fi
initrd=$root; [ $MODE = usb ] && initrd=$OUT/root-usb.desc
./unixnu build payload --uefi ../uefi-ruan --unique-chip-id "$(cat $W/unique-chip-id.txt)" \
	--data-ramdisk "$initrd" --trust-cache $OUT/trust.bundle --kernel-arg rd=md0 ${extra[@]+"${extra[@]}"} > $OUT/payload-$NAME.log 2>&1 \
	|| { tail -20 $OUT/payload-$NAME.log; exit 1; }

# The payload as a 56 MiB FAT in an "XNUP" container (see macOS/uefi/build-ipados-ruan.sh): ABL took a ~62 MB kernel
# blob, so the FAT stays at 56 MiB and the mini root has to fit next to the kernel inside it.
X=$OUT/xnup-$NAME; rm -rf $X; mkdir -p $X/src
(cd $X/src && 7z x -y $O/payload/payload.img >/dev/null)
truncate -s 56M $X/fat.img
/usr/sbin/mkfs.vfat -F 32 -n RUAN $X/fat.img >/dev/null
MTOOLS_SKIP_CHECK=1 mcopy -s -i $X/fat.img $X/src/EFI ::/
# every file must read back identical from the FAT the tablet will see
mkdir -p $X/check && (cd $X/check && 7z x -y $X/fat.img >/dev/null)
(cd $X/src && find EFI -type f -exec cmp {} $X/check/{} \;)
python3 -c "import struct; d=open('$X/fat.img','rb').read(); open('$X/payload.xnup','wb').write(b'XNUP'+struct.pack('<I',len(d))+d)"
img=$OUT/ruan-ramroot-$NAME.img
cd "$(dirname "$MKXNUIMG")"
MU=$W/uefi-ruan FD=$FD python3 "$MKXNUIMG" -p $X/payload.xnup -o $img | tail -1
rm -rf $X/check
cd $OUT && sha256sum "$(basename $img)" "$(basename $root)" trust.bundle $( [ $MODE = usb ] && echo root-usb.desc ) $( [ -n "${DATA_CONTAINER:-}" ] && echo root-usb.split ) > SHA256SUMS-$NAME
cat SHA256SUMS-$NAME
grep -o '"kernel_args": "[^"]*"' $O/payload/payload.json
