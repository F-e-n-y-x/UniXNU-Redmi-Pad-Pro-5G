#!/bin/bash
# The UFS-root variant of the macOS board, as the iPadOS one (unixnu-ruan/scripts/make-ufs-board.sh): a generated device
# directory $V/device (V=/root/unixnu-work/ufs-variant-macos) whose board.toml = UniXNU-ruan-macos's + the edits below, everything
# else symlinked. ShengUfs is the iPadOS variant's checkout (pin 2f6c7b0 + ShengUfs-dizi.patch + ruan's stream/verified edits),
# shared because the chip is the same; run the iPadOS make-ufs-board.sh once first if $W/ufs-variant/ShengUfs-src is missing.
#
# Edits: [components] + ShengUfs; the macOS kernel archives + component:ShengUfs (as sheng's macOS override); the macOS patch_series + xnu-12377/ufs.series
# (ruan-ufs-unverified-root.patch); the UFS defconfig (CONFIG_XNU_NATIVE_UFS=y); [userdata] for the "unixnu" partition; with
# UFS_ROOT_UUID the macOS root_match_volume_uuid. The UFS kernel args come from build-macos-ramroot.sh KERNEL_ARGS.
set -euo pipefail
W=/root/unixnu-work; D=$W/UniXNU-ruan-macos; V=$W/ufs-variant-macos; B=$V/device; S=$W/ufs-variant/ShengUfs-src
[ -f "$S/NativeUfs.cpp" ] && grep -q '(0x800U + 2 \* k)' "$S/NativeUfs.cpp" || { echo "make-macos-ufs-board.sh: no patched $S" >&2; exit 1; }
rm -rf "$B"; mkdir -p "$B/drivers" "$V/out"
for f in configs dts package patches scripts tools feeds.toml proprietary-files.txt README.md .cache; do [ -e "$D/$f" ] && ln -s "$D/$f" "$B/$f"; done
cp "$D/unixnu.lock" "$B/"
ln -sfn "$W/UniXNU-macos/out/feeds" "$V/out/feeds"; ln -sfn "$W/UniXNU-macos/out/ipsw" "$V/out/ipsw"
ln -s "$S" "$B/drivers/ShengUfs"

python3 - "$D/board.toml" "$B/board.toml" <<'PY'
import os, sys, tomllib
s = open(sys.argv[1]).read()
def edit(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)
edit('"XnuPrelink", "DarwinBoot", "AppleKeyStoreState", "RamBlock"]', '"XnuPrelink", "DarwinBoot", "AppleKeyStoreState", "RamBlock", "ShengUfs"]')
edit('"device:patches/xnu-12377/series"]', '"device:patches/xnu-12377/series", "device:patches/xnu-12377/ufs.series"]')
edit('defconfig = "ruan_macos_arm64_defconfig"', 'defconfig = "ruan_macos_arm64_ufs_defconfig"')
edit('"macos-27.3-26C5035r" = "ruan_macos_arm64_defconfig"', '"macos-27.3-26C5035r" = "ruan_macos_arm64_ufs_defconfig"')
# the macOS archives list: the UFS root driver and its bindings to the original IOBlockStorageDevice, after RamBlock
head, tail = s.split('[os_overrides."macos-27.3-26C5035r".kernel]', 1)
assert tail.count('  "component:RamBlock",\n') == 1
# no build-ufs-original-bindings.py: it is pinned to the iPadOS 17 kernelcache; sheng's macOS override uses the component alone
# build-ruan-mac-ufs.py: the source IOBlockStorageDevice (macOS ABI) and late registration instead of address bindings
tail = tail.replace('  "component:RamBlock",\n', '  "component:RamBlock",\n  "component:ShengUfs",\n  "build-ruan-mac-ufs.py",\n', 1)
s = head + '[os_overrides."macos-27.3-26C5035r".kernel]' + tail
s += ('\n# UFS variant: the root partition (ShengUfs finds it by name, native-ufs-partition=unixnu). ruan GPT 2026-11-19: sda22.\n'
      '[userdata]\npartition = "unixnu"\nfirst_lba = 22147072\nlast_lba = 30535674\nlba_bytes = 4096\n')
if os.environ.get('UFS_ROOT_UUID'):
    edit('[os_overrides."macos-27.3-26C5035r".payload.adt]\nchip_id = "6050"',
         f'[os_overrides."macos-27.3-26C5035r".payload.adt]\nchip_id = "6050"\nroot_match_volume_uuid = "{os.environ["UFS_ROOT_UUID"]}"')
    # kern.bootuuid / kern.apfsprebootuuid name the booted volume; launchd's mount phases (apfs_boot_util), logd and dyld look it
    # up ("lookup_boot_device: Failed to find an AppleAPFSVolume matching 84581E99…", the profile's OS image, full-OS boot 2).
    s += (f'\n# The booted volume is our UFS root (make-macos-ufs-board.sh, UFS_ROOT_UUID)\n[adt.chosen.string]\n'
          f'"boot-uuid" = "{os.environ["UFS_ROOT_UUID"]}"\n"apfs-preboot-uuid" = "{os.environ["UFS_ROOT_UUID"]}"\n')
# kern.maxvnodes: without it XNU sized the vnode table at 1024 on ruan; full macOS filled it ("vnode: table is full") and
# memorystatus kept killing daemons for vnodes (clocksyncd, amfid, logd_helper …, eth boot 3). 8 GiB / 64 KiB + 1024, as XNU's rule.
s += '\n[adt.defaults.u32]\n"kern.maxvnodes" = 132096\n'
# persistent_data: the Data volume is a RAM disk, but /chosen/ephemeral-storage must NOT be published: usermanagerd then never creates
# its persona manager (main: os_variant_uses_ephemeral_storage skips it) and aborts on "_gPersonaManager != nil" at the first
# login, so no user session starts (Dock, Finder; black screen). devicetree.py adds it for every data RAM disk unless
# persistent_data is set. Rebuilding persona.kb each boot is fine (DroidVM log-2026-10:383, TODO.md:111).
s = s.replace('[os_overrides."macos-27.3-26C5035r".payload.adt]\nchip_id = "6050"',
              '[os_overrides."macos-27.3-26C5035r".payload.adt]\nchip_id = "6050"\npersistent_data = true', 1)
b = tomllib.loads(s)
assert b['adt']['defaults']['u32']['kern.maxvnodes'] == 132096
assert b['os_overrides']['macos-27.3-26C5035r']['payload']['adt']['persistent_data'] is True
assert 'ephemeral-storage' not in b['adt']['chosen'].get('u32', {})
assert b['adt']['chosen']['asmb']['u64']['lp-sip0'] & 2   # board's 0x867: CSR_ALLOW_UNRESTRICTED_FS for /Library/GPUBundles
assert 'ShengUfs' in b['components']['include'] and b['userdata']['partition'] == 'unixnu'
assert 'component:ShengUfs' in b['os_overrides']['macos-27.3-26C5035r']['kernel']['archives']
open(sys.argv[2], 'w').write(s)
PY
echo "$B (ShengUfs $(git -C "$S" rev-parse --short HEAD) + ruan edits)"
