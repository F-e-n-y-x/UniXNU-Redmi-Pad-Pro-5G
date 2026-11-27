#!/bin/bash
# Builds the one native tool make-ramroot.py needs: xnu-deps/host/bin/unzbm (LZBITMAP decompressor). Run once.
set -euo pipefail
cd "$(dirname "$0")/xnu-deps/host"
mkdir -p bin
cc -O2 -o bin/unzbm src/unzbm.c src/libzbitmap.c
echo "built $(pwd)/bin/unzbm"
