#!/bin/bash
# Prepare the Mu-Silicium checkout that UniXNU builds ruan's UEFI from (<work>/uefi-ruan). Run once.
#   uefi/setup-uefi-ruan.sh [dest]          dest defaults to $UNIXNU_WORK/uefi-ruan (UNIXNU_WORK defaults to /root/unixnu-work)
# Needs first: uefi/Binaries/ruan (tools/regen-binaries.sh <your uefi_a.img>) and
#              uefi/Resources/DTBs/ruan.dtb (tools/make-uefi-dtb.sh <your live dtb>). Neither is shipped: both come from your tablet.
# Then:        cd <work>/UniXNU && ./unixnu build firmware --uefi ../uefi-ruan
#              cp ../uefi-ruan/Build/ruanPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd <work>/ramroot/SILICIUM_UEFI-log.fd
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
dest=${1:-${UNIXNU_WORK:-/root/unixnu-work}/uefi-ruan}
case "$dest" in *' '*) echo "the path must not contain spaces (edk2 breaks): $dest"; exit 1;; esac
REV=e51cd877e31c8fbb322032dc4cb6715e83d25ad3   # Mu-Silicium "KEP: Fixes & Improvments", the commit the port is built on
[ -d "$here/Binaries/ruan/QcomPkg" ] || { echo "uefi/Binaries/ruan missing: run tools/regen-binaries.sh <your uefi_a.img> first"; exit 1; }
[ -f "$here/Resources/DTBs/ruan.dtb" ] || { echo "uefi/Resources/DTBs/ruan.dtb missing: run tools/make-uefi-dtb.sh <your live dtb> first"; exit 1; }
if [ ! -d "$dest/.git" ]; then
	git clone https://github.com/Project-Silicium/Mu-Silicium.git "$dest"
	git -C "$dest" checkout -q "$REV"
	git -C "$dest" submodule update --init --recursive
fi
[ "$(git -C "$dest" rev-parse HEAD)" = "$REV" ] || { echo "$dest is not at $REV"; exit 1; }
if git -C "$dest" apply --check "$here/BootShim/ruan-bootshim.patch" 2>/dev/null; then
	git -C "$dest" apply "$here/BootShim/ruan-bootshim.patch"
else
	git -C "$dest" apply -R --check "$here/BootShim/ruan-bootshim.patch" || { echo "BootShim.S is neither stock nor patched"; exit 1; }
fi
mkdir -p "$dest/Platforms/Xiaomi" "$dest/Binaries/ruan"
rsync -a --delete "$here/Platforms/Xiaomi/ruanPkg/" "$dest/Platforms/Xiaomi/ruanPkg/"
rsync -a --delete "$here/Binaries/ruan/" "$dest/Binaries/ruan/"
install -Dm644 "$here/Resources/Configs/ruan.toml" "$dest/Resources/Configs/ruan.toml"
install -Dm644 "$here/Resources/DTBs/ruan.dtb" "$dest/Resources/DTBs/ruan.dtb"
echo "ready: $dest"
