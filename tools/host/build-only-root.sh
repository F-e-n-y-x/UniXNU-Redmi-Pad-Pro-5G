#!/bin/bash
# cycle-root.sh TAG: rebuild the UFS root with oslogx/overlay-dir, make the unixnu image, build TAGu, flash unixnu, boot + watch.
set -e
T=${1:?tag}; W=/root/unixnu-work; cd $W/UniXNU-ruan
mkdir -p $W/tr/out-${T}root
RUAN_FULL_ROOT=1 RUAN_WALLPAPER_OVERLAY=$W/wallpaper-overlay OVERLAY=$W/oslogx/overlay-dir DATA_OVERLAY=$W/data-overlay DATA_CONTAINER=1 REBUILD_KERNEL= REBUILD_ROOT=1 TAG=${T}root OUT=$W/tr/out-${T}root KERNEL_ARGS="boot-progress=1" bash scripts/build-ramroot.sh usb > $W/build-${T}root.log 2>&1 || true
cd $W/tr/out-${T}root; python3 $W/UniXNU-ruan/scripts/make-ufs-root.py root-usb.apfs $(cat root-usb.split) $W/UniXNU/out/ipsw/21H461/044-76034-038.dmg unixnu-root.img > mkufs.log 2>&1
