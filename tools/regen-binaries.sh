#!/bin/bash
# Regenerate macOS/uefi/Binaries/ruan from the tablet's stock uefi_a.img:
#   extract the FV, fetch garnet's reference INFs and patched drivers (Project-Silicium/Device-Binaries),
#   rebuild the tree with make-binaries.py, then port the garnet driver patches with port-patches.py.
# Work files live in macOS/build/binaries-work (gitignored). Needs: gh, curl, uv.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
stock=${STOCK_UEFI:-$here/../stock/backup/uefi_a.img}
w=$here/build/binaries-work
dest=$here/uefi/Binaries/ruan
R=Project-Silicium/Device-Binaries
mkdir -p "$w"

[ -x "$w/venv/bin/python" ] || { uv venv -q "$w/venv"; uv pip install -q -p "$w/venv/bin/python" uefi_firmware; }

# 1. FV from the stock image (it starts 40 bytes before the first _FVH signature)
python3 - "$stock" "$w/uefi.fv" <<'E'
import sys
b = open(sys.argv[1], "rb").read()
i = b.find(b"_FVH") - 40
assert i >= 0, "no firmware volume in image"
open(sys.argv[2], "wb").write(b[i:])
E
rm -rf "$w/uefi.fv_output"
"$w/venv/bin/uefi-firmware-parser" -e -O "$w/uefi.fv" >/dev/null 2>&1 || true
[ -d "$w/uefi.fv_output" ] || { echo "FV extraction failed"; exit 1; }

# 2. garnet reference: INFs + RawFiles names (empty placeholders) + the drivers garnet patched
if [ ! -d "$w/ref" ]; then
	gh api "repos/$R/git/trees/HEAD?recursive=1" --jq '.tree[] | select(.type=="blob") | .path' | grep '^garnet/' > "$w/ref.list"
	while read -r p; do
		rel=${p#garnet/}
		case "$rel" in
		*.inf) mkdir -p "$w/ref/$(dirname "$rel")"; curl -fsSL "https://raw.githubusercontent.com/$R/HEAD/$p" -o "$w/ref/$rel" ;;
		RawFiles/*) mkdir -p "$w/ref/RawFiles"; : > "$w/ref/$rel" ;;
		*.efi) mkdir -p "$w/garnet-efi/$(dirname "$rel")"; curl -fsSL "https://raw.githubusercontent.com/$R/HEAD/$p" -o "$w/garnet-efi/$rel" ;;
		esac
	done < "$w/ref.list"
fi

# 3. ruan tree
rm -rf "$dest"
python3 "$here/tools/make-binaries.py" "$w/uefi.fv_output" "$w/ref" "$dest"

# 4. patches garnet applied; DisplayDxe needs hand work (README.md), so it stays unpatched
for d in ButtonsDxe UFSDxe UsbMsdDxe; do
	g=$w/garnet-efi/QcomPkg/Drivers/$d
	python3 "$here/tools/port-patches.py" "$g/$d.efi" "$g/$d.patched.efi" \
		"$dest/QcomPkg/Drivers/$d/$d.efi" "$dest/QcomPkg/Drivers/$d/$d.patched.efi"
done
sed -i 's/PE32|DisplayDxe.patched.efi|\*/PE32|DisplayDxe.efi|*/' "$dest/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf"

# 5. every file an INF names must exist
miss=0
while read -r f; do
	d=$(dirname "$f")
	for ref in $(grep -oE '\|[A-Za-z0-9_.]+\.(efi|depex|te)\|' "$f" | tr -d '|'); do
		[ -e "$d/$ref" ] || { echo "MISSING ${f#"$dest"/} -> $ref"; miss=1; }
	done
done < <(find "$dest" -name '*.inf')
exit $miss
