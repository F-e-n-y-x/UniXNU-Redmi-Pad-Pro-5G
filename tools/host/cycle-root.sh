#!/bin/bash
# cycle-root.sh TAG: rebuild the UFS root with oslogx/overlay-dir, make the unixnu image, build TAGu, flash unixnu, boot + watch.
set -e
T=$1; W=/root/unixnu-work; cd $W/UniXNU-ruan
mkdir -p $W/tr/out-${T}root
RUAN_FULL_ROOT=1 RUAN_WALLPAPER_OVERLAY=$W/wallpaper-overlay OVERLAY=$W/oslogx/overlay-dir DATA_OVERLAY=$W/data-overlay DATA_CONTAINER=1 REBUILD_KERNEL= REBUILD_ROOT=1 TAG=${T}root OUT=$W/tr/out-${T}root KERNEL_ARGS="boot-progress=1" bash scripts/build-ramroot.sh usb > $W/build-${T}root.log 2>&1 || true
cd $W/tr/out-${T}root; python3 $W/UniXNU-ruan/scripts/make-ufs-root.py root-usb.apfs $(cat root-usb.split) $W/UniXNU/out/ipsw/21H461/044-76034-038.dmg unixnu-root.img > mkufs.log 2>&1
cd $W; A=$(cat $W/args-current.txt)
RAMROOT=$W/ramroot WORK=$W/ramroot/work-full UFS=1 PERSIST=1 PERSIST_RESET=${PR:-} REBUILD_SEED=${PR:-} UFS_ROOT=$W/tr/out-${T}root REBUILD_KERNEL=${KR:-} REBUILD_ROOT=1 TAG=${T}u OUT=$W/tr/out-${T}u KERNEL_ARGS="$A" "/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/unixnu-ruan/scripts/build-ramroot.sh" usb > build-${T}u.log 2>&1
ln -sfn $W/tr/out-${T}u ramroot/out-${T}u
ps -eo pid,args | awk '/watch-long.sh rbu1/ && !/awk/ {print $1}' | xargs -r kill || true; sleep 2
ps -eo pid,args | awk '/ruan-usbdbg/ && !/awk/ {print $1}' | xargs -r kill || true; sleep 2
timeout 60 python3 usb-dev/host/ruan-usbdbg.py --tty /dev/ttyACM0 reboot-fastboot > /dev/null 2>&1 || true
until [ -n "$(fastboot devices)" ]; do sleep 2; done
fastboot flash unixnu tr/out-${T}root/unixnu-root.img > flash-unixnu-${T}.log 2>&1
./boot-local.sh ${T}u > boot-local-${T}u.log 2>&1
logs/watch-long.sh ${T}u
