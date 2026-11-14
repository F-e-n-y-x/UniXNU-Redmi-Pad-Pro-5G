# iPadOS 17.7.11 (21H461) on Redmi Pad Pro 5G (ruan, SM7435): bring-up log

A record of every blocker, error, cause and fix, as of 2026-11-14, for anyone porting UniXNU to their own device.
The port sources are snapshotted in [`../unixnu-ruan/`](../unixnu-ruan/), and the research plans are in [`research/`](research/).
Everything here was tested with RAM-only `fastboot boot`: nothing was flashed and no slots changed.

## Setup

| Item | Value |
|---|---|
| Device | Xiaomi Redmi Pad Pro 5G "ruan", SM7435 (4× A78 + 4× A55, no PAC), 8 GB, Adreno 710 (gen7-3-0), Gunyah hypervisor |
| Chain | stock ABL `fastboot boot` → Mu-Silicium UEFI (ruanPkg) → UniXNU DarwinBoot → XNU xnu-10063 (iPadOS 21H461) at EL1 |
| Root | trimmed OS + SystemOS APFS image (≈2.5 GB), sent over USB to DarwinBoot's receiver (`fastboot stage` / `continue`), 105–140 s |
| Data | separate APFS container after the root, served by UniXNU **RamBlock** as a real IOMedia, mounted on `/private/var` |
| Display | GOP framebuffer 0xB8000000, 1600×2560, stride 6400, BGRX; NativeLegacyDisplay (IOMobileFramebufferLegacy subclass), CPU copy |
| Host | Linux PC with the tablet on USB (`scripts/zima-ramroot.sh`) |

Image tags (`out-<tag>`): md1 → rb → rb3 → rb4 → rb5 … rb13. Each fix below names the image that proved it.

## Result so far

| Stage | State |
|---|---|
| Kernel, VM, IOKit, BSD, root mount | ✅ |
| dyld shared cache, launchd (normal boot, not restore) | ✅ |
| `/private/var` read-write (RamBlock Data) | ✅ rb |
| Daemons run (no mass SIGABRT) | ✅ rb4 |
| backboardd | ✅ |
| **First iPadOS frame (Apple logo) on the panel** | ✅ rb7 |
| SMP (8 cores) | ✅ rb8s |
| On-screen crash banner after the hand-off | ✅ rb10 |
| **Lock screen (wallpaper, clock, status bar)** | ✅ rb13, about 17 min after `continue` (banner t≈1000 s, copies=21) |
| Touch | ⏳ next (rb14); HakiOS iOS 16.5 touch worked on this tablet |
| Boot speed | ⏳ research (CPU clocks, first-boot caches, boot-page cost) |
| GPU, audio, Wi-Fi, RTC | not started (GPU plan in `research/gpu-adreno710.md`) |

## Blockers and fixes, in order

### 1. APFS: `spaceman_sanity_check:318`, errno 19 on the root
- **Cause:** our mkapfs wrote spaceman `sm_dev[TIER2].addr_offset` (offset 0x80) smaller than `dev0.addr_off + 8n`.
- **Fix:** `make-ramroot.py fix_spaceman()`. Found by disassembling the APFS kext in the kernelcache. Adding slack does **not** help.

### 2. "no dyld cache" / AppleFSCompression `ChunkCompression.cpp:404` "invalid resource map"
- **Cause:** for a decmpfs type-4 resource fork, the first 16 bytes of the map must be zero.
- **Fix:** write Apple's resource-map layout, with the cache in a real directory `/System/Library/Caches/com.apple.dyld` (the cryptex path links to it).
- **Also needed:** `dyld_flags=8`, copied to the commpage, which skips libignition.

### 3. Low memory, watchdogd SIGTRAP panic
- **Cause:** the DarwinBoot window is only 2.75 GiB.
- **Fix:** `memory-outside=1` (patch `ruan-memory-outside.patch`) adopts the DRAM outside the window, for 5561 + 2745 MB. The root was also made lighter (`SKIP_DAEMONS`, `heavy_feature_resource`).
- **Note:** the exit was namespace 2, code 5 = OS_REASON_SIGNAL SIGTRAP, not jetsam.

### 4. backboardd never spawned
- **Cause:** `rd=md0` in `kern.bootargs` puts launchd in the Restore environment.
- **Fix:** `launchd-normal-boot=1` (`ruan-launchd-normal-boot.patch`) shows `rootdev=md0` to userland, so launchd runs a normal boot.

### 5. `mount -P 1` exits 66
- **Fix:** provide `/chosen/boot-manifest-hash` (board.toml).

### 6. Keybag / AppleKeyStore
- **Fix:** an AppleKeyStoreState stand-in with `aks=1 offline-key-events=1`; it reports stage 3, status 0.

### 7. Writable `/private/var`: the long one
| Try | Error | Why |
|---|---|---|
| Data volume in the root container, mount `/dev/md0s2` | ENOENT | md0 is a bare memdev, not an IOMedia, so APFS publishes no per-volume node |
| Mount by role (`VFSIOC_MOUNT_BYROLE`) from PID 1 | 45 "Can only be called from kernel" | APFS checks the caller |
| Same, from a kernel thread | needs a volume group | |
| Split the RAM disk into md0 + md1, mount `/dev/md1` | `nx_is_mount_valid:2306 invalid fsindex: -1`, then error 2 | A bare md device has no volume index. A devfs `md1s1` link only moves the error to `fs iokit node was not found` (EPERM), as the fuxi v30/v31 notes show |
| **UniXNU RamBlock (NativeRamBlock) serves the Data container as an IOBlockStorageDevice** | ✅ `/dev/disk1s1` rw, error 0 | APFS gets a real IOMedia |

- **Implementation:**
  - `ram-data-split=<offset>`: md0 keeps the root and RamBlock serves the rest.
  - `ram-data-uuid=<APFS volume UUID>`: the kernel waits for the volume by UUID and mounts it on `/private/var`.
  - Files: `ruan-ram-data-mount.patch`, `ruan-ram-block-personality.patch`, `scripts/build-ufs-original-bindings.py`, `package/RamBlockNoStore.c`.
  - RamBlock is bound to Apple's IOStorageFamily by address, so `build-ramroot.sh` refuses any kernel where IOStorageFamily moved.

### 8. Data volume full (ENOSPC, 13 free blocks)
- **Fix:** `DATA_SLACK` env in make-ramroot.py; 98304 blocks = 384 MiB works.
- **Watch out:** 768 MiB made the root 2919 MiB. That is over DarwinBoot's 2816 MiB window, and the tablet then **hangs at the UniXNU menu with no message**. `build-ramroot.sh` now refuses any root larger than the window minus 128 MiB.

### 9. Mass SIGABRT of almost every daemon ("Too many corpses being created")
- **Cause:** the root has no union of the OS cryptex. `/usr/lib/libobjc-trampolines.dylib` and 41 other paths exist only under `/private/preboot/Cryptexes/OS`, and libobjc dlopens them.
- **Fix:** `make-ramroot.py cryptex_union()` adds 42 absolute links. amethyst saw the identical failure (`ROOT-BRINGUP.md`).

### 10. Display: rb5/rb6 verdict panic `leaf=345 … swaps=1/0 copy=0xE00002D8`
- **What the code means:** 0xE00002D8 = kIOReturnNotReady (not NotPermitted).
- **Cause:** the boot-progress page's draw lock was held by a preempted console thread. With `cpus=1` and a busy boot, waiting 100 ms and then 2 s still lost.
- **Fix:** `boot_progress_prepare_scanout()` takes the screen at once with a yield flag, and pixel stores stop after the hand-off.
- **Result:** rb7 shows the Apple logo.

### 11. Firmware mix-up: "size differs from the descriptor", `getvar` unsupported
- **Cause:** an image was packed with the plain uefi-ruan FD, whose older USB receiver refuses current roots.
- **Fix:** `build-ramroot.sh` now defaults to `ramroot/SILICIUM_UEFI-log.fd`.

### 12. Auto-reset into fastboot (`ruan-autoreset`, ported from Dizi-Ruan BootReset)
- **Symptom:** the tablet resets exactly 84 s after `continue`, even with the Gunyah watchdog off.
- **Status:** kept out of test images; the cause is likely the SPMI/PMIC access under Gunyah.
- **ramoops:** RAM does not survive this reset (it reads back as 0xFF with bit flips), so logs over USB after a reset don't work.
- **Better long-term route:** a live XNU USB debug driver (j716f `UsbDebugCdc.cpp`, gts7xlwifi `QcomXnuUsbDebug`).

### 13. Diagnostics after the hand-off: crash banner
- **What it is:** a port of fuxi's crash_banner plus BootFaultProbe.c, 9 rows at the bottom of the live screen.
  - Status line: `bp t= copies= exits= LD/DM/BB/SB` plus swap counters `sw a r ret vs i st ar to`.
  - Last crash: PC, LR, ESR, FAR and a backtrace.
  - SIGABRT reason string.
  - Last failed exits.
- **rb9 bug:** the banner vanished. `prepare_scanout` ran on every copy and set the banner target to NULL; fixed in rb10.

### 14. Logo freeze: copies stop at 3, `vs0`, no vsync client (`ar0`)
- **Measured:**
  - rb10: `sw a3 r0 ret3 vs0 i0`.
  - rb11, with the vsync re-check patch `gopfb-vsync-poll.patch`: `st1 ar0 i3145`. The timer runs, but nobody registers for vsync.
- **Conclusion:** not a display-driver stall. On amethyst (same OS), the lock screen came up with CPU-copy presentation and no vsync and **no GPU** (`WORKING-BOOT.md:20-33, 131-140`). Its logo freeze had two causes:
  - missing scene apps;
  - crash-looping daemons.
- **Fix rb12:** keep `/Applications`, which had been dropped entirely. PosterBoard, InputUI, PaperBoard and others back 13 LaunchAngels. `SKIP_APPS` leaves out 11 large apps. The root grew 39 MiB.
- **Result:** rb13 reached the lock screen, confirming both causes.
- **Fix rb13:** disable the crash-loopers (audiomxd, searchd, siriactionsd, maild) plus amethyst's list. `missing_jobs()` also disables the ~40 jobs whose executable isn't in the root (55 entries in `disabled.plist` in all). Added `armcpu-self-ipi=1`: without it, multi-core XNU hangs at the logo in `DebuggerXCallEnter` (amethyst V20–V25).

## GPU: needed?
- **For the lock screen and basic UI: no.** iPadOS 17 falls back to CPU rendering, as amethyst showed.
- **UniXNU's "offline GPU" can't help on hardware:** it forwards Metal over virtio to a host and only exists in VMs.
- **A real Adreno 710 is feasible** (medium risk, about 5–8 weeks). The model is amethyst's Adreno 810 path under Gunyah, which included the zap shader loaded from XNU. Turnip supports FD710. The gaps are the A710 init tables, the parrot GPUCC offsets and a macOS build host for the Metal plugin. See `research/gpu-adreno710.md`; the first read-only probe is in `research/adreno-staged/`.

## Working kernel arguments (rb13)
```
rd=md0 boot-progress=1 debug=0x8 memory-outside=1 launchd-normal-boot=1 ruan-ramoops=1
offline-legacy-reached=1 offline-legacy-verdict=1 cpus=8 smp=1 armcpu-spi-ipi=1 armcpu-self-ipi=1
ram-data-split=<root-usb.split> ram-data=1 ram-data-uuid=<Data volume UUID>
serial=3 sheng-pmgr=2 dyld_flags=8
offline-mobile-graphics-load=1 offline-legacy-register=1 offline-legacy-phase=4 offline-legacy-gop=1
offline-legacy-pm=1 offline-legacy-validate=1 offline-legacy-transfer=1 offline-legacy-cpu-present=1
vsync=60 aks=1 offline-key-events=1
```
No `-v`: it makes DarwinBoot's console text mode, so `isMainDisplay()` is false and backboardd never presents.

## Build and test commands
```
# host (UniXNU-ruan): root + kernel + boot image
DATA_CONTAINER=1 RAMBLOCK=1 DATA_SLACK=98304 REBUILD_ROOT=1 [REBUILD_KERNEL=1] \
  OUT=/root/unixnu-work/ramroot/out-<tag> TAG=<tag> KERNEL_ARGS="<args above minus rd/ram-data/serial…>" \
  bash scripts/build-ramroot.sh usb
# USB PC: copy the img, root-usb.{apfs,desc,split}, trust.bundle, SHA256SUMS-usb-<tag>, zima-ramroot.sh, then
TAG=<tag> bash zima-ramroot.sh usb <dir>     # tablet in stock fastboot
```

## Lessons for other ports
- **Read the boot-progress sticky rows and the banner, not the scrolling console.** Add counters for every pipeline you suspect: they settle questions in one boot.
- **APFS needs IOMedia for every non-root mount.** Use RamBlock (or a real block driver) from the start, not md devices.
- **Check the root size against the DarwinBoot window before sending.** A root that doesn't fit gives no error.
- **Keep the firmware FD pinned.** An older receiver gives misleading errors.
- **When most daemons SIGABRT,** look for files that live only in the cryptex (libobjc-trampolines).
- **A logo freeze after a few frames is usually userspace:** missing `/Applications` scene apps, or daemons that crash-loop. It is rarely the GPU.
- **If the USB host's network drops during `fastboot boot`, the stock fastboot can wedge.** Recover with Power + Volume-down.
