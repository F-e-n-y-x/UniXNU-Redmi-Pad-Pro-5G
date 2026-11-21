# UniXNU documentation site: notes for ruan (2026-11-18)

Source: the site's VitePress sources in `UniXNU/docs` at origin `429d337` (2026-11-17), read with
`git show origin/HEAD:docs/...` (same content as https://unixnu.com). /zh/ skipped. Our core is at `7d85ede`.
Pages read: all of porting/, guide/, drivers/, reference/, xnu/, concepts/, plus device pages sheng, dizi,
cupid, asteroids, zizhan, aston, droidvm, garnet. Device pages are generated from each repo's `porting.toml`.

## 1. ~0.35 fps / vsync (selector 72 never called, selector 6 e00002d5)
- https://unixnu.com/porting/display (step 2 "Adapt the XNU display backend", "Read the display pipes")
- https://unixnu.com/drivers/gop-framebuffer, https://unixnu.com/devices/sheng#display,
  https://unixnu.com/drivers/community-unixnu-nativemobiledisplay
- No page names IOMFB selectors. The docs say: the GOP scanout has no vblank interrupt, so `vsync=<Hz>`
  supplies a work-loop timer (we already run `vsync=60`; sheng uses `vsync=120`). Our `VSYNC no_client`
  means the timer has no registered client, so the problem is upstream of the timer.
- The swap contract the docs require (display.md step 2.4 and the NativeGOPWire paragraph): track the pending frame
  by `NGPSwap.sequence`, return `NGPComplete` with that sequence after the copy, keep the source frame and
  IOSurface until completion, and *complete the error path on timeout or failure*. GopFramebuffer: a fence
  query that is busy retries after 200 us; a 250 ms wait logs a slow frame and keeps waiting; the
  previous frame stays until completion or cancel.
- Try on ruan: our selector 6 returns e00002d5 (timeout) on ~every 2nd call. That matches a swap that is
  never retired in time. Check our CPU-present path against that contract: retire each swap with its
  own sequence, and never let a swap-wait time out while the copy is still running. Hypothesis (not in
  the docs): backboardd/QuartzCore only registers for vsync (sel 72) once swaps complete normally, so
  fix sel 6 first, then look at sel 72 again.
- sheng (the reference board, iPadOS 27) reaches 68–73 fps average with this same GopFramebuffer +
  timer vsync (devices/sheng, `metal-flow=0` vs `1` runs). So the timer approach can work; nothing in the
  docs needs a hardware vblank.
- Diagnostics to copy: sheng `LegacyDisplayProbe.cpp` @8a85350 has a ring that timestamps "VSync timer arms,
  sends, idle stops, late ticks, starts and refresh-rate reports"; `log=1 log-display=1`.
  Frame-latency capture: `log=1 gpu-trace=1 gpu-perf=1 diag-export=240` (https://unixnu.com/porting/gpu-diagnostics),
  segment "Display update: validation, fence wait, flip, latch, retirement and blanking".
- Also check the default surface: on 24A446 the adapter creates a BGRA IOSurface through IOSurfaceRoot,
  and `create_default_fb_surface()` returns void ("its residual return register is not an object pointer").
- `vmm-present` in the ADT makes MobileGestalt answer IsVirtualDevice. On macOS that switched QuartzCore to
  half-float frames (`ca-fb-8bit=1`, `fb-half=2`), and on iPadOS 27 it switched CoreBrightness to VMBLControl. If our ADT
  publishes `vmm-present`, it might change backboardd's display/vsync path too. Worth a check.

## 2. GPU scanout (GPU can't write the GOP splash at 0xb8000000)
- https://unixnu.com/porting/display#dpu-pipes, https://unixnu.com/porting/ufs (findings), devices/sheng#display
- `dpu=2`: DisplayTimingProbe reads VIG0/1 source, size, format, fetch, CTL0, the tear checks of both
  interfaces and the display streams in the apps SMMU. `dpu=3` also rewrites each pipe's address and
  times the CTL flush latch (sheng: 5.4 ms, no start needed, the panel self-refreshes).
- `dpu-scanout=2` (experimental, off by default) points the VIG pipes at the compositor's IOSurfaces
  through a stage-1 window on the display context bank. That window is owned by ShengUfs (the driver that took
  over the apps SMMU). TCR2 SEP (bits 17:15) must be 7, otherwise addresses with bit 31 set fault at level 0.
  The flip waits for the GPU fence (newest + completed, exported by Adreno). Without that wait the panel
  flickers (808 → 2 content changes per run). Needs an 8-bit BGRA framebuffer. Log row: `[direct scanout]`.
  Any failure falls back to the GPU copy.
- On ruan: our LegacyDisplayProbe declines (no NativeUfs DPU window). Steps: (a) port `dpu=2` reads for the
  SM7435 DPU (single DSI, so probably one VIG pipe) to learn where firmware left the pipes; (b) the
  display bank's SMMU context. Under Gunyah, dizi found GR1/context-bank writes take an external abort
  (`native-ufs-smmu-reuse=1` keeps firmware's mapping), and sheng found that a translating context for the audio streams
  resets the board. So we may only be able to reuse a context bank the firmware already set up, not program a new one.
  Read the display stream's existing SMR/S2CR/CB state before trying `dpu-scanout`.

## 3. ~1/3 silent resets right after launchd
- https://unixnu.com/porting/uefi-firmware (SHENG_BOOT_GUARD), devices/sheng#firmware, devices/zizhan,
  devices/asteroids, https://unixnu.com/guide/debugging, https://unixnu.com/porting/log-capture
- Gunyah watchdog: sheng firmware (`SHENG_BOOT_GUARD=1`) arms the 30000 ms Gunyah watchdog in SEC, pings
  every 2 s in DXE (180 s budget), and hands the *armed* timer to XNU. The kernel takes it over at
  EARLY_BOOT with `uefi-watchdog=1` and pings it from a servicing thread. Ours is `ruan-wdt-pet=1` (opt-in, off).
  Try: confirm whether stock uefi_a / XBL leaves a bark armed. A reset ~20–30 s into a busy launchd
  with pet off matches a 30 s watchdog.
- Interrupt-masking timeouts under Gunyah (zizhan, following sheng): `ml-timeout-lock-panic-scale=40
  interrupt-masked-debug-mode=1` ("ordinary pauses can exceed the DEVELOPMENT 500 us threshold").
- IPIs: Gunyah does not deliver SGIs, so use `armcpu-spi-ipi=1` (we already do).
- Evidence for a reset with no panic: sheng `storage-probe=1 log=1` records BSD init, bootstrap calls and PID 1
  exec stages into spare UFS log blocks (survives the reset). Without UFS: `disable-watchdogd=1`,
  `disable-jobs=<list>` to bisect launch jobs, `boot-ui-focus=launchd`. Panic screen: `patch-xnu-entry-probe.py
  <kernel> --panic-screen --no-reset`.

## 4. ~16 min boot (cold shader compiles)
- https://unixnu.com/porting/gpu#shader-warmup, https://unixnu.com/drivers/adreno-metal (Cache and priority, Shader priming)
- The "shader-cache registration agent" (#304, 630b85a) is **macOS only**: it installs `/Applications/Shader
  Cache.app` and a login agent that registers it with LaunchServices. It does not compile anything and does not
  help iPadOS boot time.
- What does help, and works with RAM-only Data: a **pipeline pack**.
  1. Boot an image built with `make-gpu-addon.py --cache-export`; the plugin writes its cache dir to the board log.
  2. Read it out, then `scripts/make-m2vpack.py OUT EXPORT_DIR...` (later dirs win).
  3. Ship with `make-gpu-addon.py --pipeline-pack OUT` (+ `--prime-list build/prime-list.txt`, embedded as
     `__DATA,__primelist` / `addons/NativeAdrenoMetal.primelist`). The plugin imports the pack into the cache
     directory before the runtime opens it, so a fresh RAM Data still starts warm.
- Cache identity = translator build + Turnip `LC_UUID`; rebuilding Turnip invalidates everything, but a plugin-only
  rebuild keeps it. Priming starts 90 s after the render server opens the device. Progress files: `prime-done`, `prime-fn`,
  `prime-current`, `prime-bad`, `prime.log`. iPadOS counters: `PR`, `PRb`, `PRc`.
- Cache path: `Library/Caches/org.unixnu.metal` per UID, or `NATIVE_METAL_SHARED_CACHE_DIR`. backboardd's
  is under `/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/`.
- On ruan we have no USB-readable cache export yet. Use our USB debug CDC to pull the cache dir, or tar it to
  the log region, then build the pack on the host.

## 5. UFS root / persistent Data
- https://unixnu.com/porting/ufs, https://unixnu.com/devices/dizi#storage (**dizi = Redmi Pad Pro, SM7435,
  vildangil/UniXNU-Dizi-Ruan, same SoC under Gunyah**), https://unixnu.com/concepts/userdata, https://unixnu.com/guide/userdata
- dizi results (2026-11-16/17), the closest match to ruan: ShengUfs on UFS host `0x1d84000`, SPI 265, SMMU stream `0x20`
  (`native-ufs-stream`); **`native-ufs-smmu-reuse=1`** (Gunyah aborts GR1/context-bank writes; keep the firmware's
  stream→bank-2); **`native-ufs-zero-copy=0`** (DMA into caller pages powered the tablet off);
  `native-ufs-allow-unverified=1` + `dizi-ufs-unverified-root.patch`; SystemOS union via `dizi-systemos-union.patch`
  (XNU_OWNER_CACHE_MOUNT, CACHE_STARTUP, UNION_SYSTEMOS); placeholder `boot-manifest-hash` in /chosen
  (otherwise mount(8) exits 66); Data from a NativeRamBlock seed (`dizi-data-volume.patch`, `data-uuid=`).
  Dual boot: userdata split with sgdisk (from OrangeFox) into `unixnu` (32 GiB) + userdata; `native-ufs-partition=unixnu`.
  SpringBoard started from the UFS root on 2026-11-17. Their Data is still RAM (128 MB).
  Copy `patches/drivers/ShengUfs-dizi.patch` @8b7d7c4 first.
- Core config: `CONFIG_XNU_NATIVE_UFS=y`, `native-ufs-root=1`, `[components] include "ShengUfs"`,
  `[kernel] archives "component:ShengUfs"`, `[userdata] first_lba/last_lba/lba_bytes`,
  `[payload.adt] root_match_volume_uuid`. iOS 17 adds `native-source-storage=1`.
- Persistent Data: `data-persist=1` (prove the store window read-only) → `data-persist=2` (writable); `data-reset=1|2`;
  `log-reserve=<MiB>` (default 1024, at the end of userdata); `data-fua=1`. Writes use FUA. `seed-data-grow.py --gib N` needs
  macOS hdiutil. Above 2 GiB, only nonzero seed pages are kept in RAM.
- UFS findings worth reusing: put the request list on a WC page (32-byte entries share a cache line; the same class of bug
  as our GPU alias fix); enable UTRCE only and find completions from the doorbell bits; poll 200 us then sleep;
  `native-ufs-wait=1`/`native-ufs-spin-us`; `ufs-bench=1`.

- **2026-11-19:** ported as the UFS=1 build variant (rbu82u, built, not booted/flashed): IPADOS-BRINGUP.md §27, with the
  install procedure (needs the user's OK, erases Android userdata).

## 6. Metal performance (R2507), GMEM/UBWC on A7xx
- https://unixnu.com/porting/gpu#render-mode, #scheduling, #memory-path; https://unixnu.com/drivers/adreno-metal; drivers/adreno740
- R2507 (#292–#298): AdrenoMetal submodule `73d582f` → `8c7f950` (dev `46e7cc96` + `0063-main-optimizations...patch`):
  compute command reuse, constant views, tiling-cost and region copies, staged row copies, scheduler/rebind.
  The docs only say "integration"; the details are in `reports/fork-coverage-20261117.md`. Just bump the submodule. It is A740-tuned, so check for A710 regressions.
- Render mode: the default is `TU_DEBUG=gmem,nocb` (all passes in GMEM, no concurrent binning); override with `tu-debug=`;
  `tum=a` alternates the modes every 8 s for measurement. Patches `turnip-gmem-memory-side-swap.patch` and
  `turnip-gmem-mutable-swap.patch` fix the BGRA byte order in GMEM loads/stores. Check that we carry them.
- `metal-flow=<bits>` (1 lowering worker, 2 submit from commit, 4 early warm; iPadOS default 7). sheng saw backboardd
  stall with `metal-flow=1` once. If we see stalls, try `metal-flow=0`.
- Single-sample default (`NATIVE_METAL_SINGLE_SAMPLE`); `gpu-preempt=2` = 2 rings (needs GMEM save on preempt; for A710
  start with `gpu-preempt=0`).
- Memory path rules (to keep per-frame cost low): BOs are WC `IOBufferMemoryDescriptor`; batched maps; unmap ≤512 pages
  uses TLBIVAL per page, else ASID invalidate; no TLBI on submit. Rows `[vm map]`, `[vm unmap]`, `[bo memory]`.
- GPU governor: idle only after 80 ms without work (the old 16 ms rule kept boosting to the top level).
- UBWC: no doc beyond the trace fields (pass records list "UBWC and tiling"). There is no A7xx UBWC guidance.
- Other GPUs: Adreno830 community driver (Shad0w23333, SM8750, from Adreno740 `5c8c145`) runs the iPadOS 17.7.11
  desktop. It is the only non-A740 port with results, so it is the closest model for an A710 fork.
  garnet (SM7435 too) lists "Adreno 710 revision, firmware, clocks, mappings, and driver ABI" as pending only.

## Other notable
- SM7435 devices: dizi (above) and garnet (not bootable). Both are worth watching for ruan-specific patches.
- Gunyah notes across devices: cupid: Gunyah traps EL1 physical-timer access (CNTHCTL_EL2.EL1PCEN clear) →
  `timers-avoid-trapped-physical-timer.patch`; EL1 PAC key writes trap. aston: Gunyah traps WFI, so idle cores stay
  shallow unless the guest asks PSCI for power-down states (kalama: rail-pc 0x40000004, l3-off 0x41000044, llcc-off 0x4100c344).
  This matters for battery and heat. DroidVM: UniXNU as a crosvm guest on Gunyah.
- ArmCpu sleep/resume landed (#299, `9087339`).
- Backlight/iPadOS 17: `brightness` + `FadePeriod` (16.16 s, ≤5 s); verification row `L` with `st=4`, `xerr=0`.
- Debug: `debug_terminal.py` giulia protocol (screen.start, reboot.fastboot) is a model for our USB CDC tool;
  `repack-sheng-boot.py --kernel-arg/--remove-arg` repacks args without a rebuild.
- Boot args go through a 1000-byte limit (DarwinBoot 1024-byte buffer). Check that our long line stays under it.
