#!/bin/bash
# The UFS-root variant of ruan as a generated device directory, so the RAM-root board (board.toml) stays the default.
#
#   scripts/make-ufs-board.sh      -> $V/device (V=/root/unixnu-work/ufs-variant): board.toml = ruan's + the edits below,
#                                     everything else symlinked to this repository (incl. .cache: generated firmware headers); drivers/ShengUfs = UniXNU/ShengUfs at the
#                                     core's pin + patches/drivers/ShengUfs-dizi.patch (a device driver beats the core's).
#                                     $V/out = the variant's UniXNU output root (feeds and ipsw shared with the core's out/).
# build-ramroot.sh UFS=1 runs it. Edits (vildangil's dizi, the same SoC under Gunyah; macOS/docs/IPADOS-BRINGUP.md §27):
#   [components] + ShengUfs; [kernel] archives + component:ShengUfs and its original-class bindings; patch_series + ufs.series;
#   [userdata] (partition "unixnu"; LBAs are a 32 GiB placeholder for `unixnu build userdata`'s size check only: ShengUfs
#   finds the partition by its GPT name at boot). Defconfig: configs/ruan_ufs_defconfig (CONFIG_XNU_NATIVE_UFS=y).
set -euo pipefail
W=/root/unixnu-work; U=$W/UniXNU; D=$W/UniXNU-ruan; V=$W/ufs-variant; B=$V/device
rm -rf "$B"; mkdir -p "$B/drivers" "$V/out"
for f in configs dts package patches scripts feeds.toml proprietary-files.txt README.md .cache; do ln -s "$D/$f" "$B/$f"; done
cp "$D/unixnu.lock" "$B/"
ln -sfn "$U/out/feeds" "$V/out/feeds"; ln -sfn "$U/out/ipsw" "$V/out/ipsw"

# dizi's tested base, not the core's pin 2b6638b: from 57f031c on, the verified-root table code (vr_open, verified_root_*) is built
# without NATIVE_VERIFIED_BLOCK and the kernel then needs the VerifiedRoot policy archive (link fails, rbu82u 2026-11-19).
pin=${SHENGUFS_REV:-2f6c7b0}
S=$V/ShengUfs-src
[ -d "$S/.git" ] || git clone -q https://github.com/UniXNU/ShengUfs.git "$S"
git -C "$S" cat-file -e "$pin^{commit}" 2>/dev/null || git -C "$S" fetch -q origin "$pin"
git -C "$S" checkout -q -f "$pin"; git -C "$S" clean -fdq
git -C "$S" apply --3way "$D/patches/drivers/ShengUfs-dizi.patch"
# UniXNU's build-arm-driver.py always compiles NativeUfs with -DNATIVE_VERIFIED_BLOCK=1, which then needs the VerifiedRoot policy
# archive and a signed root (link fails without). ruan's bring-up root is unverified (native-ufs-allow-unverified=1, as dizi): off here.
sed -i '1i // ruan (make-ufs-board.sh): unverified bring-up root, see native-ufs-allow-unverified.\n#undef NATIVE_VERIFIED_BLOCK\n#undef NATIVE_VERIFIED_BLOCK_MULTI_POLICY' "$S/NativeUfs.cpp"
# ruan's display streams are 0x800/0x802 (DT iommus = <&apps_smmu 0x800 0x402>), dizi's 0x1c00/0x1c02 (dpu-scanout, rbu130u).
sed -i 's/(0x1c00U + 2 \* k)/(0x800U + 2 * k)/' "$S/NativeUfs.cpp"
grep -q '(0x800U + 2 \* k)' "$S/NativeUfs.cpp"
ln -s "$S" "$B/drivers/ShengUfs"

python3 - "$D/board.toml" "$B/board.toml" <<'PY'
import os, sys, tomllib
s = open(sys.argv[1]).read()
def edit(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)
edit('"XnuPrelink", "DarwinBoot", "AppleKeyStoreState", "RamBlock"]', '"XnuPrelink", "DarwinBoot", "AppleKeyStoreState", "RamBlock", "ShengUfs"]')
edit('"device:patches/xnu/series"]', '"device:patches/xnu/series", "device:patches/xnu/ufs.series"]')
edit('  "component:RamBlock",\n', '  "component:RamBlock",\n  # UFS variant: the root driver and its bindings to the original IOBlockStorageDevice (scripts/make-ufs-board.sh).\n'
     '  "component:ShengUfs",\n  "build-ufs-original-bindings.py --candidate build/xnu-prelink/ruan-candidate.macho",\n')
edit('defconfig = "ruan_defconfig"', 'defconfig = "ruan_ufs_defconfig"')
edit('"ipados-17.7.11-21H461" = "ruan_defconfig"', '"ipados-17.7.11-21H461" = "ruan_ufs_defconfig"')
s += ('\n# UFS variant: the root partition. MUST VERIFY against ruan\'s real GPT before `unixnu build userdata` is trusted for size;\n'
      '# placeholder = 32 GiB of 4 KiB blocks. ShengUfs finds the partition by name (native-ufs-partition=unixnu).\n'
      f'[userdata]\npartition = "unixnu"\nfirst_lba = {os.environ.get("UFS_FIRST_LBA", "22147072")}\n'
      f'last_lba = {os.environ.get("UFS_LAST_LBA", "30535674")}\nlba_bytes = 4096\n')
if os.environ.get('PERSIST'):   # build-ramroot.sh PERSIST=1: the payload's Data seed is packed (DBRDISK1): /chosen/memory-map/PackedRAMDisk
    edit('[payload.adt]\n', '[payload.adt]\npacked_data_ramdisk = true\n')
if os.environ.get('UFS_ROOT_UUID'):   # build-ramroot.sh UFS_ROOT=: ruan's own root volume in the OS window
    edit('root_match_volume_uuid = "04769D26-2CA0-4825-B443-E2E5F88300F2"', f'root_match_volume_uuid = "{os.environ["UFS_ROOT_UUID"]}"')
    edit('"device:patches/xnu/ufs.series"]', '"device:patches/xnu/ufs-own-root.series"]')
b = tomllib.loads(s)
assert 'ShengUfs' in b['components']['include'] and b['userdata']['partition'] == 'unixnu'
open(sys.argv[2], 'w').write(s)
PY
echo "$B (ShengUfs $pin + dizi patch)"
