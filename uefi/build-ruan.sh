#!/bin/bash
# Build ruan's Mu-Silicium UEFI boot image: copies ruanPkg, Binaries/ruan and the ruan resources into
# a Mu-Silicium checkout and runs its build. Output: <checkout>/Mu-ruan.img (an Android boot image for `fastboot boot`).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
mu=${1:-/DATA/tower/Projects/mu-silicium-ruan}  # space-free path: edk2 build breaks on spaces
[ -f "$mu/build_uefi.py" ] || { echo "no Mu-Silicium checkout at $mu"; exit 1; }
[ -d "$here/Binaries/ruan" ] || { echo "Binaries/ruan missing: run tools/make-binaries.py first"; exit 1; }

# edk2's build splits paths on spaces: pass a space-free symlink as $1 to build through it; copies go to the real path
real=$(realpath "$mu")
rsync -a --delete "$here/Platforms/Xiaomi/ruanPkg/" "$real/Platforms/Xiaomi/ruanPkg/"
rsync -a --delete "$here/Binaries/ruan/" "$real/Binaries/ruan/"
install -Dm644 "$here/Resources/Configs/ruan.toml" "$real/Resources/Configs/ruan.toml"
install -Dm644 "$here/Resources/DTBs/ruan.dtb" "$real/Resources/DTBs/ruan.dtb"

cd "$mu"
if command -v uv >/dev/null; then  # this host's python3 -m venv ships without pip
	[ -d .venv ] || uv venv -q .venv
	uv pip install -q -p .venv/bin/python -r pip-requirements.txt
else
	[ -d .venv ] || python3 -m venv .venv
	.venv/bin/pip install -q -r pip-requirements.txt
fi
. .venv/bin/activate
python3 build_uefi.py -d ruan "${@:2}"
ls -l Mu-ruan*.img 2>/dev/null || find . -maxdepth 2 -name "Mu-ruan*" -newer "$here/build-ruan.sh"
