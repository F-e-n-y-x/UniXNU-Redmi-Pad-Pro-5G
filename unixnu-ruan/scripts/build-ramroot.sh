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
#      DATA_OVERLAY=dir (with REBUILD_ROOT=1 DATA_CONTAINER=1): files for the Data volume, mirroring private/var (warm Metal/Turnip
#      caches pulled from a booted tablet). DATA_SLACK (make-ramroot.py) sets the Data container's free blocks.
#      OVERLAY=dir (usb, with REBUILD_ROOT=1): build-turnip-smoke.py's output; its overlay/ joins the root (make-ramroot.py
#      --overlay) and its cdhashes go to $OUT/root-usb.cdhash, which every later payload of this root appends to trust.bundle.
#      UFS=1 (usb): the UFS-root variant (IPADOS-BRINGUP.md §27). Root = the flashed "unixnu" partition (ShengUfs, dizi's patches),
#      built from the board scripts/make-ufs-board.sh generates, in its own output root (/root/unixnu-work/ufs-variant/out);
#      the USB image is only the RAM Data container ($OUT/data-usb.apfs, scripts/make-ufs-data.py, DATA_SLACK default 1 GiB);
#      no rd=md0. UFS_IMAGE=1 also builds the partition image (unixnu build userdata, ~8.7 GiB) and links it as $OUT/unixnu-root.img.
#      UFS_ROOT=<out dir of a RAM root> (e.g. out-rbu74): the partition image carries THAT root (GPU stack included) as its OS window
#      (scripts/make-ufs-root.py), root_match_volume_uuid becomes its volume, and its root-usb.cdhash joins trust.bundle.
#      UFS_FIRST_LBA/UFS_LAST_LBA: the unixnu partition's real LBAs for board.toml [userdata] (not used by the boot image itself).
#      PERSIST=1 (with UFS=1): Data on UFS (docs/research/opt/data-on-ufs.md Plan A). No USB image: a packed 16 GiB Data seed rides in
#      the payload (board packed_data_ramdisk = true) and NativeRamBlock keeps its changed pages in ShengUfs's store window at the end
#      of "unixnu" (ram-data=1 ram-data-uuid= data-persist=2 data-fua=1). The seed is the store's identity, so it is built once, at
#      PERSIST_SEED (default /root/unixnu-work/ufs-variant/data-seed.apfs), and only rebuilt with REBUILD_SEED=1 (REBUILD_ROOT leaves
#      it alone). PERSIST_RESET=1 (or 2: also Setup) adds data-reset=<it>: the first boot of a seed only, never after.
set -euo pipefail
MODE=${1:?usage: build-ramroot.sh mini|usb}
NAME=$MODE${TAG:+-$TAG}
extra=(); for arg in ${KERNEL_ARGS:-}; do extra+=("--kernel-arg=$arg"); done
case $MODE in mini|usb) ;; *) echo "build-ramroot.sh: mode is mini or usb" >&2; exit 2 ;; esac
[ -z "${PERSIST:-}" ] || [ -n "${UFS:-}" ] || { echo "build-ramroot.sh: PERSIST=1 needs UFS=1" >&2; exit 2; }
W=${UNIXNU_WORK:-/root/unixnu-work}; U=$W/UniXNU; D=$W/UniXNU-ruan; O=$U/out/ruan/ipados-17.7.11-21H461/ruan
I=$U/out/ipsw/21H461; R=${RAMROOT:-$W/ramroot}; OUT=${OUT:-$R/out}
MKXNUIMG=${MKXNUIMG:-"$(cd "$(dirname "$(realpath "$0")")/../.." && pwd)/uefi/mkxnuimg-ruan.py"}   # the repo's uefi/
# The default firmware is the one with the receiver's upload/getvar (md1log, 2026-11-14): the plain uefi-ruan build has an older receiver
# that refuses current roots ("size differs from the descriptor", rb6 2026-11-14).
FD=${FD:-$R/SILICIUM_UEFI-log.fd}; WORK=${WORK:-$R/work-full}
[ -f "$FD" ] || { echo "build-ramroot.sh: UEFI FD $FD missing: build it (docs/INSTALL.md step 2a) and copy it there" >&2; exit 2; }
[ -f "$MKXNUIMG" ] || { echo "build-ramroot.sh: $MKXNUIMG missing: make $W/UniXNU-ruan a symlink to the repo's unixnu-ruan/, or set MKXNUIMG" >&2; exit 2; }
data=--data; [ -n "${DATA_CONTAINER:-}" ] && data=--data-container
export PATH=/opt/unixnu-linux/bin:$PATH
mkdir -p "$OUT"

base=$OUT/root-$MODE; rd=(--kernel-arg rd=md0)
if [ -n "${UFS:-}" ]; then
	[ $MODE = usb ] || { echo "build-ramroot.sh: UFS=1 needs usb" >&2; exit 2; }
	if [ -n "${PERSIST:-}" ]; then DATA_CONTAINER= RAMBLOCK=; base=$OUT/data-seed; else DATA_CONTAINER=1 RAMBLOCK=1; base=$OUT/data-usb; fi; rd=()
	REBUILD_KERNEL=${REBUILD_KERNEL-1}   # its out/.../kernel starts as a copy of the RAM kernel (below); REBUILD_KERNEL= skips
	if [ -n "${UFS_ROOT:-}" ]; then
		# the root container's volume UUID (first APSB), for root_match_volume_uuid
		export UFS_ROOT_UUID=$(python3 - "$UFS_ROOT/root-usb.apfs" <<'PY'
import sys, uuid
f = open(sys.argv[1], 'rb')
for at in range(0, 1 << 34, 1 << 20):
    f.seek(at); d = f.read(1 << 20)
    hit = next((i for i in range(0, len(d), 4096) if d[i + 32:i + 36] == b'APSB'), None)
    if hit is not None: print(str(uuid.UUID(bytes=d[hit + 240:hit + 256])).upper()); break
PY
)
		[ -n "$UFS_ROOT_UUID" ]; ln -f $UFS_ROOT/root-usb.cdhash $OUT/root-usb.cdhash
	fi
	# the board is made again when PERSIST changes (packed_data_ramdisk, make-ufs-board.sh)
	packed=0; [ -n "${PERSIST:-}" ] && packed=1
	V=$W/ufs-variant; [ -f $V/device/board.toml ] && [ -z "${REBUILD_KERNEL:-}" ] && [ "$(grep -c '^packed_data_ramdisk = true' $V/device/board.toml)" = $packed ] || $D/scripts/make-ufs-board.sh
	export UNIXNU_OUT=$V/out UNIXNU_DEVICE=$V/device
	O=$V/out/ruan/ipados-17.7.11-21H461/ruan_ufs
	# build-gop-legacy-display.py and build-ufs-original-bindings.py take a previous ruan kernel as their reference candidate.
	if [ ! -f $O/kernel/kernel.macho ]; then mkdir -p $O/kernel; cp $U/out/ruan/ipados-17.7.11-21H461/ruan/kernel/kernel.{macho,json} $O/kernel/; fi
	[ -f $O/.config ] || (cd $U && ./unixnu defconfig ruan_ufs_defconfig >/dev/null)
	# ShengUfs as dizi runs it (ruan's live DT: ufshc@1d84000, SPI 265, apps-smmu stream 0x20). pmgr=2 is payload.py's
	# check_native_root spelling; ruan's PMGR stand-in reads sheng-pmgr=2 (already in board.toml).
	# Keep all GPU and debug arguments (e.g. tu-debug for Turnip sysmem mode on the custom UFS root).
	for arg in native-ufs-root=1 native-ufs-partition=unixnu native-ufs-stream=0x20 native-ufs-smmu-reuse=1 native-ufs-zero-copy=0 \
		native-ufs-allow-unverified=1 pmgr=2; do extra+=("--kernel-arg=$arg"); done
fi
root=$base.apfs
if [ -n "${PERSIST:-}" ]; then
	seed=${PERSIST_SEED:-$V/data-seed.apfs}
	if [ ! -f "$seed" ] || [ -n "${REBUILD_SEED:-}" ]; then
		# a new seed does not match the store any more: without the reset the store is kept unused and Data writes wait (NativeRamBlock)
		[ -n "${PERSIST_RESET:-}" ] || { echo "build-ramroot.sh: a new Data seed starts Data over: add PERSIST_RESET=1" >&2; exit 1; }
		python3 $D/scripts/make-ufs-data.py "$WORK" "$seed"
	fi
	cp -f "$seed" "$root"; cp -f "${seed%.apfs}.uuid" $base.uuid
	for arg in ram-data=1 ram-data-uuid=$(cat $base.uuid) data-persist=2 data-fua=1 ${PERSIST_RESET:+data-reset=$PERSIST_RESET}; do
		extra+=("--kernel-arg=$arg"); done
elif [ -n "${UFS:-}" ]; then
	if [ ! -f "$root" ] || [ -n "${REBUILD_ROOT:-}" ]; then python3 $D/scripts/make-ufs-data.py "$WORK" "$root"; fi
elif [ ! -f "$root" ] || [ -n "${REBUILD_ROOT:-}" ]; then
	if [ $MODE = mini ]; then
		python3 $D/scripts/make-ramroot.py --mini --os $I/044-76040-038.dmg --work $R/work-mini --out "$root"
	else
		python3 $D/scripts/make-ramroot.py --full --os $I/044-76040-038.dmg --systemos $I/044-76034-038.dmg \
			--work "$WORK" --out "$root" --descriptor $base.desc $data ${OVERLAY:+--overlay "$OVERLAY/overlay"} ${DATA_OVERLAY:+--data-overlay "$DATA_OVERLAY"}
		rm -f $OUT/root-usb.cdhash; [ -n "${OVERLAY:-}" ] && cp "$OVERLAY/cdhashes" $OUT/root-usb.cdhash
	fi
fi
[ $MODE = usb ] && [ -z "${PERSIST:-}" ] && [ ! -f $base.desc ] && { echo "build-ramroot.sh: $base.desc missing: REBUILD_ROOT=1" >&2; exit 1; }
# DarwinBoot places the root inside owned_memory_size (board.toml) beside the kernel and its data: a root that does not fit hangs
# at the UniXNU menu with nothing on screen (rbar2 with 768 MiB Data, 2919 MiB > 2816 MiB, 2026-11-14). 128 MiB stays for the rest.
if [ $MODE = usb ]; then
	window=$(( $(sed -n "s/^owned_memory_size = \"\(0x[0-9A-Fa-f]*\)\"/\1/p" $D/board.toml) ))
	[ $(( $(stat -c %s "$root") + (128 << 20) )) -le $window ] || { echo "build-ramroot.sh: $root ($(( $(stat -c %s "$root") >> 20 )) MiB) + 128 MiB does not fit DarwinBoot's $((window >> 20)) MiB window: lower DATA_SLACK" >&2; exit 1; }
fi
if [ -n "${DATA_CONTAINER:-}" ]; then
	[ -f $base.split ] || { echo "build-ramroot.sh: $base.split missing: REBUILD_ROOT=1" >&2; exit 1; }
	data_dev=ram-data-dev=md1
	if [ -n "${RAMBLOCK:-}" ]; then
		# the Data volume's UUID: the APFS volume superblock (APSB) in the Data container, bytes 240..256
		data_dev=ram-data-uuid=$(python3 - "$root" "$(cat $base.split)" <<'PY'
import sys, uuid
f = open(sys.argv[1], 'rb'); f.seek(int(sys.argv[2])); data = f.read(64 << 20)
found = {str(uuid.UUID(bytes=data[at + 240:at + 256])).upper() for at in range(0, len(data), 4096) if data[at + 32:at + 36] == b'APSB'}
assert len(found) == 1, f'Data container: expected one volume UUID, found {found}'
print(found.pop())
PY
)
	fi
	for arg in ram-data-split=$(cat $base.split) ram-data=1 $data_dev; do extra+=("--kernel-arg=$arg"); done
fi
rm -f $OUT/trust.bundle   # a fresh file: cycle.sh's out-<tag> directories share their roots' files as hard links
python3 $U/scripts/bundle-ipsw-trust.py $OUT/trust.bundle $I/044-76040-038.dmg.trustcache $I/044-76034-038.dmg.trustcache >/dev/null
# The root's own binaries (OVERLAY above), as one more module.
[ -f $OUT/root-usb.cdhash ] && python3 $U/scripts/trust-append-module.py $OUT/trust.bundle $OUT/trust.bundle $(cat $OUT/root-usb.cdhash)

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
initrd=$root; [ $MODE = usb ] && [ -z "${PERSIST:-}" ] && initrd=$base.desc
# With the root's own module the bundle is no longer one of the IPSW's two (board allow-list): pass it as the owner's bundle.
trust_arg=--trust-cache; [ -f $OUT/root-usb.cdhash ] && trust_arg=--owner-trust-cache
./unixnu build payload --uefi ../uefi-ruan --unique-chip-id "$(cat $W/unique-chip-id.txt)" \
	--data-ramdisk "$initrd" $trust_arg $OUT/trust.bundle ${rd[@]+"${rd[@]}"} ${extra[@]+"${extra[@]}"} > $OUT/payload-$NAME.log 2>&1 \
	|| { tail -20 $OUT/payload-$NAME.log; exit 1; }

# The payload as a 56 MiB FAT in an "XNUP" container (uefi/mkxnuimg-ruan.py): ABL took a ~62 MB kernel
# blob, so the FAT stays at 56 MiB and the mini root has to fit next to the kernel inside it.
X=$OUT/xnup-$NAME; rm -rf $X; mkdir -p $X/src
(cd $X/src && 7z x -y $O/payload/payload.img >/dev/null)
truncate -s 58M $X/fat.img   # rbu169p: Wi-Fi made the kernel 54 MB; 58 MiB (60.8 MB) stays under ABL's ~62 MB
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
cd $OUT && sha256sum "$(basename $img)" "$(basename $root)" trust.bundle $( [ $MODE = usb ] && [ -z "${PERSIST:-}" ] && basename $base.desc ) $( [ -n "${DATA_CONTAINER:-}" ] && basename $base.split ) > SHA256SUMS-$NAME
cat SHA256SUMS-$NAME
grep -o '"kernel_args": "[^"]*"' $O/payload/payload.json
if [ -n "${UFS_IMAGE:-}" ] && [ -n "${UFS_ROOT:-}" ]; then
	python3 $D/scripts/make-ufs-root.py $UFS_ROOT/root-usb.apfs "$(cat $UFS_ROOT/root-usb.split)" $I/044-76034-038.dmg $OUT/unixnu-root.img
elif [ -n "${UFS_IMAGE:-}" ]; then
	(cd $U && ./unixnu build userdata > $OUT/userdata.log 2>&1) || { tail -20 $OUT/userdata.log; exit 1; }
	ln -f $O/userdata/*.img $OUT/unixnu-root.img 2>/dev/null || ln -sf "$(ls $O/userdata/*.img)" $OUT/unixnu-root.img
	ls -la $OUT/unixnu-root.img
fi
