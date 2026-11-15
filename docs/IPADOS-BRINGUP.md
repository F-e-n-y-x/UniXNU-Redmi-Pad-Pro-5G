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
| **Touch (Novatek NT36532, HakiOS driver + j716f NativeHidInput)** | ✅ rb14f: lock screen → home screen, Settings opens from a tap |
| Home screen icons | ⚠️ white placeholders: iconservicesagent was in DISABLED_JOBS (fix in rb15) |
| UI speed | ⚠️ about 1 fps (CPU render + uncached 16 MiB copy per frame); write-combined mapping, damage-only copy next |
| Buttons (PMK8350 PON + PMIC GPIO, HakiOS driver) | ✅ rb18: btn events, power press handled |
| **Live USB debug (j716f UsbDebugCdc on ruan's DWC3, arena DMA at 0x80D00000)** | ✅ rb18g1: screenshots, kernel log and status over USB (`host/ruan-usbdbg.py`) |
| Frame copy (write-combined GOP, `fb-wc=1`) | ✅ 13–18 ms per frame, from about 330 ms |
| GPU S1 g1 (read-only GPUCC/GDSC survey) | ✅ gcc ok, cc ok, w0 e0; CX/GX collapsed (0x0022f001) |
| Boot speed | ✅ rb14f: lock screen at about 540 s, from about 1000 s (console redraw throttle, ruan-cpufreq=2, fault-scan cap) |
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

### 15. USB debug link
- **rb16:** `usb: dmaDesc alloc/prepare failed`. A contiguous allocation searches only DarwinBoot's 2.75 GiB window, which the RAM root nearly fills (`prep=0xe00002bd`, kIOReturnNoMemory).
- **rb17:** falls back to a 64 KiB arena in the unused dmesg-0 part of the ramoops region (0x80D00000).
- **rb18g1:** enumerates as 1209:0001. On the host: `ruan-usbdbg.py cmd status | shot | log` (use one tty client at a time).
- **USB transfers to DarwinBoot's receiver dropping data** ("short download", desynced replies): this was the cable. Re-plugging fixed it.

### 16. Findings from the first USB log (rb18g1)
- **68 s between `exec launchd` and launchd's "hello":** the largest remaining boot cost. It is likely code-signature checking of the dyld cache.
- **Spotlight and searchd crash:** NSInvalidArgumentException (nil AssetProperties key).
- **PridePosterExtension killed** (ns 7, code 3). Widgets stay white.
- **Settings → Accessibility crashes when opened.** Reduce Motion and Reduce Transparency are preset in the Data seed instead (com.apple.Accessibility).

### 17. Remote test loop and GPU S1 (rbu3b, 2026-11-14 evening)
- **USB commands work:** `dmesg` (full msgbuf), `ls`, `cat`/`get`, `gpu <mode>` (traced, run after boot) and `reboot-fastboot`.
- **reboot-fastboot works:** it probes the PMIC reboot-reason cell (SDAM 0x48), marks it 00→04 and calls PSCI SYSTEM_RESET. The tablet is back in fastboot about 2 s later. So the fully remote loop is: boot via zima, then test over USB, then reboot-fastboot. The old 84 s autoreset bug was not this write.
- **GPU S1 g3, run over USB after boot:** CX GDSC on (0022f001→f8228800, first try, not refused), the GPU clocks on, the GPU SMMU readable (scr0 00210406, id0 4c017e18, id1 40000016, 24 SMRs, none valid). Run at boot instead, the same sequence reset the tablet: S1/S2 stay post-boot USB commands.
- **ruan-cpufreq confirmed:** d0 1804→1958 MHz, d1 1900→2400 MHz.
- **No swap** ("Failed to open swap file"): iPadOS kills apps instead of paging.
- **Settings → Accessibility:** Preferences SIGSEGV with PC=0, LR 0x1a040eac4 (a call through NULL; a missing stripped piece). Spotlight crash-loops every ~20 s (nil AssetProperties). CollectionsPoster/PridePosterExtension get killed (ns 7, code 3). Symbolization in progress.

### 18. GPU S2 progress (rbu4–rbu5c, run remotely over USB)
- **S2a, `gpu 4`:** GPLL0 GPU votes 0060257f→0061a57f; gmu_clk_src 200 MHz (cfg 0x602) and hub_clk_src 150 MHz (0x603) from GPLL0_DIV; branches gmu/cxo/hub/aon running (rcgfail 0). The GMU window is readable: CM3_SYSRESET 1, cm3cfg 0x4052, rpmh 0xf, the AO counter ticking.
- **S2b part 1, `gpu 5`:** `gmu_gen70000.bin` (57272 B, sha256 b2d3f0e2…) is shipped in the root at /usr/standalone/firmware/ruan. 15 blocks, 11 preallocations; ITCM/DTCM loaded and verified (135 words, 0 bad), with the GMU still in reset. The main code (56492 B at GMU VA 0x4000) runs from DDR through the GPU SMMU, stream 5, so the GMU boot (`gpu 6`) needs an S1 context bank and page tables.
- **Bugs fixed along the way:**
  - a hand-typed digest array: the expected hash is now a hex string checked by a host test;
  - the USB host tool's EAGAIN and garbled commands: blocking writes and draining input.
- **Intermittent early boot failure:** about 1 in 5 boots freezes at bsd_autoconf or resets into Android before USB comes up. The same image boots fine on retry. Not yet understood. Recovery is remote (`adb reboot bootloader`).
- **The Settings → Accessibility crash cannot be fixed by files:** CoreImage's GL engine is inside the dyld shared cache and calls a GL dispatch table with no backend. It needs a Metal device (GPU) or a code shim. Spotlight was dropped from the root, which ends its AssetProperties crash loop.

### 19. GPU S2b-2: first GMU boot attempt (rbu6–rbu6d, 2026-11-14 night)
- **rbu6:** "no usable GMU pool". The 512 KiB contiguous allocation fails because contiguous allocations only search DarwinBoot's 2.75 GiB window.
- **rbu6b:** a **reserved 16 MiB GPU pool** at phys 0x8B8B00000 (the last 16 MiB of HLOS bank 0x880000000–0x8B9B00000, kept out of memory-outside adoption via board.toml `owned_memory_outside_reserved`; checked against the kernel's kept list before use). Pool OK.
- **rbu6b panic:** `Kernel data abort … esr 0x96000010` (synchronous external abort) writing GPU-SMMU GR1 **CBAR(1) = 0** in the teardown path.
  - The trace showed every stage-1 write sticking except CB1 TCR2 (wrote 0x2, read 0x62: hardware-set bits), which tripped an over-strict readback.
  - Under Gunyah, HLOS may program CBAR TYPE 1 (S1) but a write of 0 (TYPE 0, S2) is rejected with an SEA.
  - Fix: no GR1 "disable" writes anywhere; TCR2 compared as a superset.
- **DT facts** (`kgsl-smmu@3da0000`): `qcom,num-context-banks-override = 6`, `qcom,num-smr-override = 6`, `qcom,skip-init`. Banks 0–5 hold power-on garbage with SCTLR.M off; all CBARs and SMRs are 0. The GMU is SID 5 (mask 0x400).
- **`gpu 7`:** a read-only survey of banks 0–5 and SMR/S2CR 0–5.
- **rbu6d `gpu 6`:** stage 4 reached. The SMMU context was accepted (SMR 84000005, S2CR 1, CBAR 0001f300, SCTLR 27, refused 0) and the GMU was released from reset. It **then faulted on its first instruction fetch**: FSR 0x408 (permission fault, AArch64 format), FAR 0x4000 (GMU code start), FSYNR0 0x00100063 (instruction fetch). The tablet stayed up.
  - **Cause:** the GMU page descriptors use AP[1]=1 (EL0-accessible, 0x643). Privileged execution of EL0-writable memory is not permitted, and the GMU fetches privileged.
  - **Next fix:** map the GMU regions privileged-only (descriptor 0x603, like KGSL's IOMMU_PRIV). The code region may also be read-only (AP[2]=1).

### Resume here (next session)
1. `RuanGpuPower.c` / `gmu_mmu_host.c`: GMU page descriptor 0x643 → 0x603 (AP[1]=0). ✅ **Applied and verified in host test.** Next: Rebuild the kernel only (rbu5 root via hard links). Run `gpu 6`; expect `booted 1 fwinit 00000100 … hfi 1`.
2. If it still does not boot: compare with KGSL's gen7 GMU start (the DDR bandwidth vote, the AOP ACD message, GMU IRQs).
3. Then S3: SQE microcode, zap via SCM PAS 13, a first ringbuffer and fence (the 15.5 MiB left in the GPU pool).
4. Open issue: about 1 in 5 boots freezes early (bsd_autoconf) or resets into Android before USB comes up. On retry the same image boots.
5. Lab procedure:
   - zima `~/usbdbg/ruan-usbdbg.py`: `shot | log | dmesg | ls | get | gpu N | reboot-fastboot`. Use one tty client at a time.
   - If the receiver replies get out of step ("size differs"), use `fastboot -s unixnu-ramdisk reboot`, then `adb reboot bootloader`, for a fresh connection.
   - Never `pkill -f` a pattern contained in your own ssh command.
