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

### 20. GPU S2b-2: GMU boot and HFI ready on live hardware (rbu7, 2026-11-15 evening)
- **Page table fix verified:** In `RuanGmuMmu.h` line 19, descriptor attributes changed from `0x643` (EL0 user) to privileged-only `0x603` (`AP[2:1] = 00`). Updated host test `gmu_mmu_host.c` to assert `0x603ull`.
- **Live run on tablet via Pi 5 (`192.168.11.5`):**
  - Staged `rbu7` via 100% RAM-only `fastboot boot` + `fastboot stage root-usb.apfs`.
  - Tablet booted cleanly: SpringBoard (PID 27) and backboardd (PID 53) up and executing in userland.
  - Screen snapshot captured via USB CDC (`ruan-usbdbg.py shot`).
  - Executed `gpu 6`:
    - SMMU context bank 1 configured (`cb 21000`, `smr 84000005`, `s2cr 1`, `cbar 0001f300`, `sctlr 00000027`, `refused 0`).
    - GMU released from reset (`CM3_SYSRESET 0`).
    - PDC and RSC microcode loaded into GMU.
    - Verified register output:
      `booted 1 fwinit 00000100 (5 polls) hfi 1 status 00000001 (0 polls) fsr 00000400 far d644bf087712 fsynr0 00100114 aoirq 00000000 cm3cfg 00004012 GMU up, HFI ready`
    - Status: `gpu m6 ok s6 fw1 hfi1`, return code `rc=0`.
  - **Stage S2 is complete!** The GMU is running firmware from DDR, responding over SMMU stream 5, and the Host-Firmware Interface (HFI) is active.

### 21. GPU S3–S5: GX power, command processor, DMA (rbu8–rbu28, 2026-11-15/16, Antigravity)
Details per build are in README.md checkpoints rbu10→rbu28; this is the porter's summary.
- **S3, `gpu 8`–`gpu 11`:**
  - GX powered via the GMU: OOB GPU_SET (ack interrupt unmasked, 500 ms poll), HFI start, and a GX bandwidth/perf vote. ARC/BCM levels are found from cmd-db, with fallback tables if cmd-db can't be mapped.
  - `gen7_start` writes: GBIF, CP_PROTECT and HWCG.
  - Stream-0 context: **bank 2**, SMR mask 0x400. GPU VA window 0x40000000 + pool offset; the GOP framebuffer (phys 0xb8000000) is at GPU VA 0x50000000.
  - SQE start fix (rbu14): APRIV 0x7f, and don't halt via CP_SQE_CNTL.
  - CP_MEM_WRITE of 0xC0FFEE01 was read back by the CPU.
- **Secure mode:**
  - Writing `RBBM_SECVID_TRUST_CNTL` from the CPU faulted, so it was removed (2167c6d).
  - `CP_SET_SECURE_MODE 0` stalled the CP without a zap shader, so it was removed (rbu17).
  - The zap shader (SCM PAS 13) is **not done**. Non-secure buffers work anyway.
- **S4, `gpu 14`:** CP_MEM_WRITE drew a 100×40 rectangle into the framebuffer; all 4000 pixels were read back.
- **S5, `gpu 15`/`gpu 16`:** a CP_MEMCPY (0x75) copy between buffers (256 dwords) and into the framebuffer (40000 px), both read back. This is command-processor DMA, not the 2D blit engine.
- **Per-frame path (`fb-gpu-stream=1`/`fb-gpu=1`, `native_legacy_gpu_offer`):** maps up to 4 compositor surfaces into stream 0 and does one CP_MEMCPY per frame, with completion on a thread call. Experimental; the default is still the CPU copy (`fb-wc=1`).
- **Mistakes to learn from:**
  - **rbu24 broke the type-7 header**: opcode and count swapped, parity dropped, so the CP halted with `rptr 0`. Fixed in rbu25.
  - The header's count mask was also 0x7fff, which overlaps the parity bit; it is now 0x3fff.
  - Both are now caught by `docs/research/gpu-dev/tests/pm4_host.c`. Build it with `cc -Wall -Wextra -Werror -I../../../../unixnu-ruan/package pm4_host.c`.

### 22. Review of rbu22–rbu28 (Claude, 2026-11-16)
- **Not real yet, despite "verified" in old checkpoints:**
  - Wi-Fi: `RuanWifiProbe.c` only maps one MSA page.
  - Audio: `RuanAudioProbe.c` touches no hardware.
  - Metal/IOAcceleratorES: `NativeAdrenoAccelerator.cpp` and `build-adreno740-accelerator.py` are missing, so it stops at stage 1, and the user-client stub returned success with a NULL handler.
  - Battery %: falls back silently to a hardcoded 3980 mV (= the 73% in screenshots).
- **Per-frame GPU path bugs (handed to Antigravity to fix):**
  - Ring wrap to 0 without NOP padding, so the CP runs stale dwords.
  - The blit returns 0 on timeout; fences report retired = submitted.
  - Cached IOMemoryDescriptors are not retained (GPU DMA from freed pages); `sourceStride` is ignored.
- **Rule going forward:** a subsystem is "verified" only when a register or data readback from that hardware is logged. Printing a status string is not verification.

### 23. Battery, backlight, GPU WIP (rbu29–rbu33, 2026-11-16/17)
- **Battery (tested rbu29):** PM7250B fuel gauge read-only over SPMI observer (QG 0x848: LAST_ADC_V 0xC0, LAST_ADC_I 0xC2); charger state BATTERY_CHARGER_STATUS_1 (CHGR+0x06). Log `battery: vbat 4365 mV from qg …, ibat -507 mA`; iPadOS shows 100% + charging.
- **Backlight = Silergy SY7758 on I2C** (DT `sy7758@2e` on i2c@984000 = QUP0 SE1; panel `bl_ctrl_external`; WLED/LPG disabled). Old DSI-DCS driver was fake.
  - rbu30/31 (tested): GENI FIFO I2C works: id 0x63, bootloader level 500. Brightness regs 0x10/0x11 write+readback OK; DEV_CTL 0x01 switched 0x83→0x85 (register mode, as Linux sy7758_init). USB `backlight N`, `backlight reg R [V]` (decimal). User could not see a light change before the mode switch; after it, not confirmed by eye.
  - iPadOS slider: CoreBrightness (CBDisplayContaineriOS) needs IOObjectConformsTo(service,"AppleARMBacklight").
    - rbu32: defining our own class `AppleARMBacklight` → boot panic "class AppleARMBacklight is a duplicate; kext com.apple.driver.AppleARMPlatform already has a class by that name". Reverted (f1eca33).
    - rbu33: UniXNU-sheng method (copy vtable, slot 7 getMetaClass → Apple's AppleARMBacklight metaclass; publish backlight-control only then) → Apple logo, then black screen, USB never up, no log. Parked as `unixnu-ruan/wip/backlight-impersonate-rbu33-crashes.patch`, not in the tree.
    - After rbu33, a re-boot of known-good rbu31 also failed (fastboot boot accepted, tablet left USB, crash without message). Unknown whether rbu33 was really the cause or the old intermittent early-boot failure / tablet state. Re-test rbu31 first next time.
  - sheng note: publishing `backlight-control` WITHOUT the conformance blocks QuartzCore's libEDR send and stops screen updates — rbu23–31 did that; may explain `copies=3` display stalls.
- **GPU (Antigravity WIP, 7bca7b4):** chunked CP_MEMCPY frame copy, RBBM_INT_0_MASK, wrap/fence/fallback counters. Built into rbu31; 5-minute fb-gpu-stream test never ran. Antigravity's quota ran out (resets ~2026-11-23).
- **Tools:** `/root/unixnu-work/cycle.sh <tag>` packages current kernel with rbu31 args, copies to Pi, reboot-fastboot, RAM boot. Don't `pkill -f` a pattern that appears in your own ssh command line (kills the command).

### Resume here (next session)
0. GPU: **step 4 passes (rbu62b)**: Turnip's Vulkan smoke test works on the A710 (zap via PAS 13 with segments written before init_image; root-cause chain in research/GPU-METAL-PORT.md "Milestone"). Next: boot rbu63 (Metal plugin root, step 5) with `/root/unixnu-work/boot-local.sh rbu63`; pass = 6800..6803 in `gpu`'s TURNIP line. Older notes: "Checkpoint rbu44–rbu60".
   Superseded detail follows: GPU Metal step 4 (2026-11-17): see research/GPU-METAL-PORT.md "Checkpoint rbu44–rbu60". The Turnip smoke test runs on the device but nothing lands because the GPU stays in secure mode (TRUST_CNTL 1); the zap shader is the way out. Boot rbu60a/b/c/d (`/root/unixnu-work/boot-local.sh rbu60x`, tablet on local USB) in that order and read `gpu` + the ramoops replay: they bisect the SCM PAS 13 sequence. Last stable GPU images: rbu54/rbu56 (`gpu-tlbiall=2`, diag=1).
1. Boot known-good rbu31 (`~/ramroot/boot-rbu31.sh` on the Pi, tablet in fastboot). If it fails too, the problem is not the backlight patch: chase the early-boot failure (ramoops replay, `pull-crashes`).
2. Backlight: confirm by eye that `backlight 4095` / `backlight 0` change the light on rbu31. Then retry the slider: apply wip/backlight-impersonate-rbu33-crashes.patch with `backlight-control` publication delayed until after SpringBoard, or compare with UniXNU-sheng / -dby_w09 / -uke / -ishtar / -komodo package/AppleARMBacklight.cpp for what they do differently on iPadOS 17.
3. GPU: run fb-gpu-stream=1 5+ min on rbu31; check gpu_blits, fallbacks + last_fb_reason, wraps, fences in USB `display`.
4. Always look for an existing UniXNU fork implementation first (`gh search code`).

## 24. rbu34–rbu43 (2026-11-17)
- rbu34/35 reset to Android ~20 s after launchd; rbu36 (`ruan-wdt-pet=0`) stable → the Gunyah watchdog pet is now opt-in (`ruan-wdt-pet=1`).
- GPU glue (UniXNU Adreno740 on A710, bank 2): contiguous kernel allocations fail on this kernel (`ctx e00002bd`), so context buffers come from the reserved GPU pool (patch 0003). `ruan-gpu=17` at boot runs from a thread once initproc exists (rbu41 panicked running it before IOKit).
- **rbu43: `gpu 17` ok (CP init, fenced CP_MEM_WRITE scratch 0a710c0d), `gpu 18` ok (`glue s10 published`), tablet stays up.** Args: rbu31 minus `ruan-gpu=16 fb-gpu-stream=1`, plus `ruan-adreno=1 ruan-gpu=17`.
- Next: Metal step 4 (Turnip smoke test with `TU_DEBUG=sysmem`), then AdrenoMetal plugin.

## 25. rbu66 (2026-11-17): iPadOS home screen with the GPU
- rbu66 (zap default, Turnip on A710, AdrenoMetal plugin, sandbox grant `*`): backboardd renders through Metal → Turnip → A710; home screen reached at t≈1154 s (screenshot `testing/rbu66-homescreen-gpu.png`). Display still gets frames by CPU copy (`fb-gpu` off). One widget renders white. Boot: `/root/unixnu-work/boot-local.sh rbu66`.
- Next: direct GPU scanout (fb-gpu), boot time (pipeline cache), widget, GMEM/UBWC, flaky ~1/3 reset after launchd.

## 26. Checkpoint 2026-11-18 (rbu66–rbu80) — Resume here
- Tablet on this PC's USB (not the Pi). Boot: `/root/unixnu-work/boot-local.sh <tag>`; sequential runner `run-series.sh`; USB debug `usb-dev/host/ruan-usbdbg.py --tty /dev/ttyACM0 cmd gpu|display|replay|shot`. Webcam MJPEG: `cam/camserve.py` on :8090.
- **Best build: rbu79** (zap default, Turnip on A710, AdrenoMetal, sandbox grant `*`, 384 MiB Data, wc-clean flush). Lock screen ~16 min, no GPU hang for 15+ min, touch works, but **~0.35 fps**.
- GPU hang root cause (rbu71–rbu78): stale cached alias lines overwriting Turnip's command buffers in DRAM; fixed by `dc civac` of each new BO's kernel alias (patches/adreno/0005, `gpu-wc-clean`, default on).
- fb-gpu (GPU copy into 0xb8000000) hangs the GPU: the GOP splash buffer isn't GPU-writable on ruan → keep `fb-gpu` off; real GPU scanout needs our own DPU scanout buffer.
- Open: (1) fps — backboardd never registers a vsync client (`VSYNC no_client`); rbu80 (booted, not yet read) logs every IOMFB selector backboardd calls (`cmd display` → `IOMFB methods`). (2) ~1/3 boots reset after launchd (unrelated to GPU). (3) boot time ~16 min. (4) one white widget.
- Agent history and details: docs/research/GPU-METAL-PORT.md.
- **rbu80 result** (`cmd display`, ~20 min): `IOMFB methods: 3:1/1(e00002bc) 4:231/0 5:231/0 6:562/230(e00002d5) 8:131/0 12,15,22,27,28,29,33,50,55,57,74,79,84 fail once (mostly e00002c7 unsupported) 68:2/1 88:1/0`. **Selector 72 (vsync registration) is never called.** Selector 6 fails 230 of 562 calls with e00002d5 (kIOReturnTimeout?) — one failure per swap, likely the swap-wait/present path that paces backboardd. Next: decode selector 6 and the once-failing setup selectors (12/22/27/29/33/50/55/57/74/79/84) against sheng's NativeLegacyDisplay; implementing them may be what makes QuartzCore request vsync.
- **Upstream to pick up next session** (UniXNU core is at 7d85ede 2026-11-13; origin is at 429d337 2026-11-17, ~40 commits ahead): a shader-cache registration agent (#304, #302; may cut our ~16 min cold-compile boot), Metal perf runtime R2507 (#292–#298), ArmCpu sleep/resume (#299), build validation fixes (#300–#303). UFS root per unixnu.com/porting/ufs: ShengUfs + `CONFIG_XNU_NATIVE_UFS=y`, `native-ufs-root=1`, `[userdata] first_lba/last_lba/lba_bytes`, `root_match_volume_uuid`. Needs a root partition on UFS, so a flash/repartition decision is the user's (vildangil's dizi does this with a 32 GiB `unixnu` partition).
