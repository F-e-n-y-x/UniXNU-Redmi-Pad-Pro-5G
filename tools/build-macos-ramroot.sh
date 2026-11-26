#!/bin/bash
# Build ruan's macOS 27.3 (26C5035r) RAM-boot set: fastboot boot only, nothing is written to the tablet.
#
#   1. XNU 12377 as a plain arm64 kernel (no PAC codegen, HAS_APPLE_PAC/HAS_NATIVE_PAC unset; board.toml
#      [os_overrides."macos-27.3-26C5035r"], configs/ruan_macos_arm64_defconfig) prelinked into Apple's kernel collection.
#   2. kc-nopac.py: pacstrip every fileset entry, de-authenticate the root chained fixups, reseal the FIPS HMACs,
#      then check that the collection is clean; DarwinBoot's own ParseKernel (test-kernel-parser.py) must pass.
#   3. The DarwinBoot payload (FAT, `unixnu payload`) with the same kernel arguments, Data descriptor and trust bundle
#      as the first set, in an "XNUP" container.
#   4. The boot image: the reference image's BootShim + UEFI FD + DTB + ramdisk, with the new payload in the gzip blob
#      (same layout mkxnuimg-ruan.py writes; the shim and FD are reused so no shared UEFI tree is rebuilt).
#   5. Install into $EXTRACT (macos-extract/): the boot image and SHA256SUMS-macos. root-usb.apfs and data-usb.* are not
#      rebuilt: the payload carries root-usb.desc (size + CRC32), so they stay valid as long as they are unchanged.
#
# Env: SKIP_KERNEL=1 reuses out/.../ruan_macos_arm64/kernel/kernel.macho; OUT (default /root/unixnu-work/macos-build/out);
#      EXTRACT (default <worktree>/macos-extract); REF_IMG (reference boot image for shim/FD/DTB/ramdisk); NO_INSTALL=1.
set -euo pipefail
W=/root/unixnu-work; U=$W/UniXNU-macos; D=$W/UniXNU-ruan-macos
O=$U/out/ruan/macos-27.3-26C5035r/ruan_macos_arm64; DEFCONFIG=ruan_macos_arm64_defconfig
# UFS=1: the UFS-root variant (make-macos-ufs-board.sh): ShengUfs, root on the "unixnu" partition. UFS_ROOT_UUID names its volume.
if [ -n "${UFS:-}" ]; then
	"$(cd "$(dirname "$0")" && pwd)/make-macos-ufs-board.sh"
	V=$W/ufs-variant-macos; D=$V/device; DEFCONFIG=ruan_macos_arm64_ufs_defconfig
	export UNIXNU_OUT=$V/out UNIXNU_DEVICE=$D
	O=$V/out/ruan/macos-27.3-26C5035r/ruan_macos_arm64_ufs; RD=   # root = the UFS partition, not md0; Data still rides in root-usb.apfs
	# build-gop-legacy-display-27.py and build-ufs-original-bindings.py take a previous ruan kernel as their reference candidate
	if [ ! -f $O/kernel/kernel.macho ]; then mkdir -p $O/kernel; cp $U/out/ruan/macos-27.3-26C5035r/ruan_macos_arm64/kernel/kernel.{macho,json} $O/kernel/; fi
	export UNIXNU_PMGR_CHECKED=1   # see UniXNU-macos scripts/unixnu/payload.py check_native_root
	KERNEL_ARGS="native-ufs-root=1 native-ufs-partition=unixnu native-ufs-stream=0x20 native-ufs-smmu-reuse=1 native-ufs-zero-copy=0 native-ufs-allow-unverified=1 pmgr=2 ruan-mac-data=${MAC_DATA:-1} ruan-mac-data-uuid=$(cat ${DATA_UUID_FILE:-/root/unixnu-work/macos-build/data-full/macos-data.uuid}) ${KERNEL_ARGS:-}"
fi
HERE=$(cd "$(dirname "$0")" && pwd)
EXTRACT=${EXTRACT:-$W/macos-extract}
OUT=${OUT:-$W/macos-build/out}
REF_IMG=${REF_IMG:-$W/macos-build/backup-arm64e-20261121/ruan-ramroot-usb-macos.img}
IMG=ruan-ramroot-usb-macos.img
# The first set's arguments (its payload.json), extras first as unixnu payload orders them; the board adds the rest.
ARGS="${RD-rd=md0} cpus=1 serial=3 sheng-pmgr=2 dyld_flags=8 offline-legacy-register=1 offline-legacy-gop=1
      ${KERNEL_ARGS:-}"   # no ram-data*: iPadOS-only args the macOS kernel ignores (the command line limit is 1000 bytes)
mkdir -p "$OUT"
export PATH=/opt/unixnu-linux/bin:$PATH

cd "$U"
if [ -z "${SKIP_KERNEL:-}" ]; then
	echo "== kernel (plain arm64) + prelink"
	./unixnu defconfig $DEFCONFIG -D "$D" --os macos-27 >/dev/null
	touch "$OUT/.kernel-start"
	./unixnu build kernel -D "$D" --os macos-27 > "$OUT/kernel.log" 2>&1 || { tail -20 "$OUT/kernel.log"; exit 1; }
	# unixnu can return 0 after make failed (2026-11-26: a link error, then the 07:11 kernel was packed): require a new kernel
	[ "$O/kernel/kernel.macho" -nt "$OUT/.kernel-start" ] && ! grep -aq "linker command failed\|\*\*\* \[.*Error" "$OUT/kernel.log" \
		|| { grep -a -B3 -A3 "Undefined symbols\|error:" "$OUT/kernel.log" | grep -av "was built for newer" | tail -20; echo "kernel build failed"; exit 1; }
fi
cp "$O/kernel/kernel.macho" "$OUT/kernel.macho"

echo "== kc-nopac + DarwinBoot parser"
python3 "$HERE/kc-nopac.py" "$OUT/kernel.macho"
python3 "$U/drivers/DarwinBoot/Test/test-kernel-parser.py" "$OUT/kernel.macho"

echo "== payload"
extra=(); for arg in $ARGS; do extra+=("--kernel-arg=$arg"); done
./unixnu payload -D "$D" --os macos-27 --kernel "$OUT/kernel.macho" --unique-chip-id "$(cat $W/unique-chip-id.txt)" \
	--data-ramdisk "$EXTRACT/root-usb.desc" --owner-trust-cache "$EXTRACT/trust.bundle" "${extra[@]}" > "$OUT/payload.log" 2>&1 \
	|| { tail -20 "$OUT/payload.log"; exit 1; }
# every payload file must read back identical from the FAT the tablet will see
rm -rf "$OUT/fat-check"; mkdir -p "$OUT/fat-check"; (cd "$OUT/fat-check" && 7z x -y "$O/payload/payload.img" >/dev/null)
cmp "$OUT/fat-check/EFI/UniXNU/Payloads/kernel.macho" "$OUT/kernel.macho"
cmp "$OUT/fat-check/EFI/UniXNU/Payloads/scratch-data.img" "$EXTRACT/root-usb.desc"
cmp "$OUT/fat-check/EFI/UniXNU/Payloads/kernel.macho.trustcache" "$EXTRACT/trust.bundle"
cp "$OUT/fat-check/EFI/UniXNU/unixnu.toml" "$OUT/unixnu.toml"; rm -rf "$OUT/fat-check"
cp "$O/payload/payload.json" "$OUT/payload.json"

echo "== XNUP + boot image"
python3 - "$O/payload/payload.img" "$REF_IMG" "$OUT/payload.xnup" "$OUT/$IMG" <<'PY'
import gzip, struct, sys, zlib
fat, ref, xnup_path, out = sys.argv[1:]
fat = open(fat, 'rb').read()
xnup = b'XNUP' + struct.pack('<I', len(fat)) + fat
assert len(xnup) <= 0x10000000 - 0x1000, 'payload exceeds DarwinBoot/BootShim window (PAYLOAD_MAX 0x10000000)'
open(xnup_path, 'wb').write(xnup)
img = open(ref, 'rb').read()
assert img[:8] == b'ANDROID!' and struct.unpack_from('<I', img, 40)[0] == 4, 'reference is not a boot image v4'
kernel_size, ramdisk_size = struct.unpack_from('<II', img, 8)
page = lambda n: (n + 4095) // 4096 * 4096
z = zlib.decompressobj(31); blob = z.decompress(img[4096:4096 + kernel_size]) + z.flush(); dtb = z.unused_data
at = blob.find(b'XNUP')
assert at > 0 and dtb[:4] == bytes.fromhex('d00dfeed'), 'reference layout: BootShim + FD + XNUP, DTB after the gzip stream'
blob = blob[:at] + xnup
blob += b'\0' * (-len(blob) % 16)
kernel = gzip.compress(blob, 6, mtime=0) + dtb
rest = img[4096 + page(kernel_size):]          # ramdisk and boot signature sections, unchanged
assert len(rest) == page(ramdisk_size) + page(struct.unpack_from('<I', img, 1580)[0])
header = bytearray(img[:4096]); struct.pack_into('<I', header, 8, len(kernel))
open(out, 'wb').write(bytes(header) + kernel + b'\0' * (page(len(kernel)) - len(kernel)) + rest)
print(f'{out}: {page(len(kernel)) + len(rest) + 4096} bytes; kernel blob {len(kernel)} (uncompressed {len(blob)}, shim+FD {at}, XNUP {len(xnup)})')
PY

cd "$OUT"
sha256sum "$IMG" payload.xnup kernel.macho > SHA256SUMS-out
if [ -z "${NO_INSTALL:-}" ]; then
	echo "== install into $EXTRACT"
	cp "$IMG" "$EXTRACT/$IMG"
	(cd "$EXTRACT" && sha256sum "$IMG" root-usb.apfs root-usb.desc root-usb.split data-usb.apfs data-usb.desc trust.bundle > SHA256SUMS-macos \
		&& sha256sum -c SHA256SUMS-macos)
fi
grep -o '"kernel_args": "[^"]*"' "$OUT/payload.json"
