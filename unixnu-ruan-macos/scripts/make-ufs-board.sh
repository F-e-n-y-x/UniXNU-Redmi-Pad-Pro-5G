#!/bin/bash
# The UFS-root variant of ruan as a generated device directory, so the RAM-root board (board.toml) stays the default.
#
#   scripts/make-ufs-board.sh      -> $V/device (V=/root/unixnu-work/ufs-variant): board.toml = ruan's + the edits below,
#                                     everything else symlinked to this repository; drivers/ShengUfs = UniXNU/ShengUfs at the
#                                     core's pin + patches/drivers/ShengUfs-dizi.patch (a device driver beats the core's).
#                                     $V/out = the variant's UniXNU output root (feeds and ipsw shared with the core's out/).
# build-ramroot.sh UFS=1 runs it. Edits (vildangil's dizi, the same SoC under Gunyah; macOS/docs/IPADOS-BRINGUP.md §27):
#   [components] + ShengUfs; [kernel] archives + component:ShengUfs and its original-class bindings; patch_series + ufs.series;
#   [userdata] (partition "unixnu"; LBAs are a 32 GiB placeholder for `unixnu build userdata`'s size check only: ShengUfs
#   finds the partition by its GPT name at boot). Defconfig: configs/ruan_ufs_defconfig (CONFIG_XNU_NATIVE_UFS=y).
set -euo pipefail
W=/root/unixnu-work; U=$W/UniXNU; D=$W/UniXNU-ruan; V=$W/ufs-variant; B=$V/device
rm -rf "$B"; mkdir -p "$B/drivers" "$V/out"
for f in configs dts package patches scripts feeds.toml proprietary-files.txt README.md; do ln -s "$D/$f" "$B/$f"; done
cp "$D/unixnu.lock" "$B/"
ln -sfn "$U/out/feeds" "$V/out/feeds"; ln -sfn "$U/out/ipsw" "$V/out/ipsw"

pin=$(python3 -c "import tomllib; print(next(c['revision'] for c in tomllib.load(open('$U/components.toml','rb'))['component'] if c['id']=='ShengUfs'))")
S=$V/ShengUfs-src
[ -d "$S/.git" ] || git clone -q https://github.com/UniXNU/ShengUfs.git "$S"
git -C "$S" cat-file -e "$pin^{commit}" 2>/dev/null || git -C "$S" fetch -q origin "$pin"
git -C "$S" checkout -q -f "$pin"; git -C "$S" clean -fdq
git -C "$S" apply --3way "$D/patches/drivers/ShengUfs-dizi.patch"
ln -s "$S" "$B/drivers/ShengUfs"

python3 - "$D/board.toml" "$B/board.toml" <<'PY'
import sys, tomllib
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
      '[userdata]\npartition = "unixnu"\nfirst_lba = 0x0\nlast_lba = 0x7fffff\nlba_bytes = 4096\n')
b = tomllib.loads(s)
assert 'ShengUfs' in b['components']['include'] and b['userdata']['partition'] == 'unixnu'
open(sys.argv[2], 'w').write(s)
PY
echo "$B (ShengUfs $pin + dizi patch)"
