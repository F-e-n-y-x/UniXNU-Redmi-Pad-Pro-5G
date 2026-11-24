#!/bin/bash
# mount-host-work.sh: run once after each PC start, before any build. Safe to re-run.
# 1. host-work/apple-trees.ext4 -> host-work/apple-trees: the extracted iPadOS trees, on ext4 because NTFS can't store Apple's
#    "*"/":" file names (ramroot/work-* link into it).
# 2. host-work -> /root/unixnu-work as a recursive bind mount (not a symlink): XNU's makefiles break on the space in
#    "Redmi Pad Pro 5G", and the build resolves symlinks, so it must see a real path without spaces.
H="$(cd "$(dirname "$0")/.." && pwd)/host-work"
if ! mountpoint -q "$H/apple-trees"; then
	/sbin/e2fsck -p "$H/apple-trees.ext4" && mount -o loop "$H/apple-trees.ext4" "$H/apple-trees" || exit 1
fi
if ! mountpoint -q /root/unixnu-work; then
	[ -L /root/unixnu-work ] && rm /root/unixnu-work
	mkdir -p /root/unixnu-work && mount --rbind "$H" /root/unixnu-work || exit 1
fi
ls /root/unixnu-work/ramroot/work-full/root >/dev/null && echo "host-work ready at /root/unixnu-work"
