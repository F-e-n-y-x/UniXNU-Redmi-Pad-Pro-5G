#!/bin/bash
# apply.sh <UniXNU checkout>: apply ruan's UniXNU core changes (see README.md). Dry-runs each patch first; stops on a mismatch.
set -euo pipefail
U=${1:?usage: apply.sh <UniXNU checkout>}; P=$(cd "$(dirname "$0")" && pwd)
apply() { local dir=$1 patch=$2; git -C "$dir" apply --check "$P/$patch" && git -C "$dir" apply "$P/$patch" && echo "applied $patch to $dir"; }
apply "$U" 00-unixnu-core.patch
for p in "$P"/10-*.patch; do s=$(basename "$p" .patch); s=${s#10-}; d=$U/drivers/$s; [ -d "$d" ] || d=$U/kits/$s; apply "$d" "10-$s.patch"; done
