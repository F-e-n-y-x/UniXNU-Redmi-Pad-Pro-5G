#!/bin/bash
set -e; T=rbu177; W=/root/unixnu-work; cd $W
A=$(cat $W/args-current.txt)
RAMROOT=$W/ramroot WORK=$W/ramroot/work-full UFS=1 PERSIST=1 PERSIST_RESET=${PR:-} REBUILD_SEED=${PR:-} UFS_ROOT=$W/tr/out-${T}root REBUILD_KERNEL=${KR:-} REBUILD_ROOT=1 TAG=${T}u OUT=$W/tr/out-${T}u KERNEL_ARGS="$A" "/DATA/tower/Projects/Redmi Pad Pro 5G/macOS/unixnu-ruan/scripts/build-ramroot.sh" usb > build-${T}u.log 2>&1
ln -sfn $W/tr/out-${T}u ramroot/out-${T}u
ps -eo pid,args | awk '/watch-long.sh rbu1/ && !/awk/ {print $1}' | xargs -r kill || true; sleep 2
ps -eo pid,args | awk '/ruan-usbdbg/ && !/awk/ {print $1}' | xargs -r kill || true; sleep 2
timeout 60 python3 usb-dev/host/ruan-usbdbg.py --tty /dev/ttyACM0 reboot-fastboot > /dev/null 2>&1 || true
until [ -n "$(fastboot devices)" ]; do sleep 2; done
fastboot flash unixnu tr/out-${T}root/unixnu-root.img > flash-unixnu-${T}.log 2>&1
./boot-local.sh ${T}u > boot-local-${T}u.log 2>&1
logs/watch-long.sh ${T}u
