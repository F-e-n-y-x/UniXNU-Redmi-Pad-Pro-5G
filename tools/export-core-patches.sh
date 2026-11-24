#!/bin/bash
# export-core-patches.sh <UniXNU checkout> <out dir>: save a UniXNU working tree's local changes as patches
# (00-unixnu-core.patch for the superproject, 10-<sub>.patch per changed submodule; untracked files included).
# Re-apply on a clean checkout with the apply.sh next to the patches.
set -euo pipefail
U=${1:?usage: export-core-patches.sh <UniXNU checkout> <out dir>}; O=${2:?out dir}; mkdir -p "$O"
dump() {  # dump <repo> <patch>: tracked changes vs HEAD plus untracked files, submodule pointers left out
  local r=$1 out=$2 new
  mapfile -t new < <(git -C "$r" ls-files --others --exclude-standard -- . ":!*.pyc")
  ((${#new[@]})) && git -C "$r" add -N -- "${new[@]}"
  git -C "$r" diff HEAD --binary --ignore-submodules=all -- . ":!*.pyc" > "$out"
  ((${#new[@]})) && git -C "$r" reset -q -- "${new[@]}"
  [ -s "$out" ] && echo "wrote $out ($(grep -c '^diff --git' "$out") files)" || rm -f "$out"
}
dump "$U" "$O/00-unixnu-core.patch"
git -C "$U" submodule foreach --quiet 'echo $sm_path' | while read -r s; do dump "$U/$s" "$O/10-$(basename "$s").patch"; done
git -C "$U" submodule status | sed 's/^/  /' > "$O/COMMITS.txt"; echo "  superproject $(git -C "$U" rev-parse --short HEAD)" >> "$O/COMMITS.txt"
