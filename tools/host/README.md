# Host helpers (the build PC)

The scripts used to build, flash and RAM-boot the iPadOS images, as they run on the maintainers' PC. They assume the work
tree at `/root/unixnu-work` (on this PC a symlink to `macOS/host-work/`) (`W=` at the top of each: change it to yours) with `UniXNU` and `UniXNU-ruan` (this repo's
`unixnu-ruan/`) checked out. See [../../docs/INSTALL.md](../../docs/INSTALL.md) for the full flow.

| Script | What |
|---|---|
| `cycle-root.sh TAG` | Rebuild the UFS root (with the overlay below), make the `unixnu` image, build the kernel payload, flash `unixnu`, RAM-boot. `PR=1` starts a fresh Data volume; `KR=1` rebuilds the kernel. Boot args from `args-current.txt`. |
| `build-only-root.sh TAG` | Only the root build and `unixnu-root.img` (no flash). |
| `tail-cycle.sh` | The rest of `cycle-root.sh` (kernel payload, flash, boot) for a root built separately; set `T=` inside. |
| `cycle-args.sh TAG "args"` | Kernel payload with extra boot args on the current root, then RAM-boot (no flash). `UFS_ROOT=` picks the root, `KR=1` rebuilds the kernel. |
| `boot-local.sh TAG` | RAM-boot a built payload (`fastboot boot`), switching the tablet to fastboot over USB or adb first. Never flashes. |
| `oslogx/` | Source of the root-overlay wrapper in the `IOMFB_FDR_Loader` launchd slot: runs Apple's loader (`.real`), the unified-log export and, with the Wi-Fi build, the supplicant launcher (`unixnu-ruan/scripts/RuanWifiLauncher.c`). Build it with the iOS SDK and sign with ldid; the overlay itself holds Apple binaries and is not distributed. |
| `args-current.txt` | The boot args `cycle-args.sh` starts from (the 2026-11-21 set: 60 Hz, Wi-Fi stage 5, no Bluetooth/video surveys). |
| `run-last-good.sh` | How the last stable image (`rbu188u`) was made: kernel payload on the `rbu187` UFS root. |
| `export-core-patches.sh U OUT` | Save a UniXNU checkout's local changes (superproject + submodules, untracked files included) as patches for `apply.sh`. |
