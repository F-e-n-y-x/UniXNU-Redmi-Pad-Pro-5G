# The 5 s frame cadence: which build started it (2026-11-21)

Read-only analysis of the build outputs, args and device logs in `/root/unixnu-work`. Nothing was built or booted.
[V] = verified in logs or source. [I] = inferred.

## Bottom line

1. **The 5 s cadence did not start at rbu145p.** Every build with a `display` capture from rbu114u to rbu145p shows it.
   SUBMIT-MS has a median of 4989-5051 ms (one build: 10016). Between any two captures the rate never goes above
   **0.20 frames/s**, so no interaction ever produced a burst. That includes rbu127u (the Control Center screenshot) and
   rbu143u ("fast comparatively": 18 frames in about 700 s). [V]
2. **The last build with real frame bursts is rbu84** (RAM root, 2026-11-19). Its SUBMIT-MS is `55 47 48 62 4999 9899 44
   71 …`: bursts at about 20 fps during interaction, then 5 s idle gaps. rbu83c ran at a steady 39-55 ms (1855 frames).
   The 5 s-only pattern appears in the first capture after that, rbu114u. rbu85u-rbu113u have no SUBMIT-MS capture. [V]
3. **Strongest candidate: rbu110u dropped `armcpu-spi-ipi=1 armcpu-self-ipi=1`.** It switched to `smp-gic=1`, and no
   later build brings them back (they are not in `ruan-default-bootargs.patch` either). ArmCpu.cpp:36 says
   `armcpu-spi-ipi=1: IPIs are software-pended SPIs … (ICC_SGI1R_EL1 generates nothing on this board)`, and
   `research/unixnu-site-notes.md:66` says "Gunyah does not deliver SGIs, so use armcpu-spi-ipi=1". Without them,
   cross-core wakeups (IPIs) are sent as SGIs that never arrive. [V for the arg change and the code comment]
   A woken render thread, or a timer armed on another core, then waits until that core takes some other interrupt. That
   fits gaps that are exact multiples of 5 s whatever the display, GPU clock, GPU IRQ or pacing settings, and the odd
   negative-time messages (`took -10199 us`, `Spent too much time (18446744073709)s`). [I]

## Per-build table (rbu127u-rbu155p)

"copies" and "accepted" are from the last `display` capture. `copies=0` means direct scanout. `copies=accepted` with
`fallbacks=accepted` means every frame went through the CPU copy.

| build | SUBMIT-MS median / <1 s intervals | max fps between captures | copies / accepted | arg changes vs previous | root / Data / kernel |
|---|---|---|---|---|---|
| 127u | 5010 / 1 of 29 | 0.16 | 47/47 (copy) | baseline (vsync=120 vsync-fixed=1 fb-wc=1, no dpu-scanout) | rbu127root flashed, RAM Data |
| 128u | 5003 / 0 of 10 | – | 10/10 | – | same |
| 129u | 5012 / 0 of 12 | – | 12/12 | – | same |
| 130u | 5008 / 0 of 39 | 0.17 | 0/51 (direct) | +dpu-scanout=2, −vsync-fixed | same |
| 131u | 10016 / 0 of 39 | – | 0/28 | – | same |
| 132u | 5010 / 11 of 86 | 0.16 | 0/154 | – | same |
| 133u | 5005 / 1 of 11 | – | 0/13 | vsync=60 vsync-fixed=1, cpufreq=3, touch_fast, −gpu-corner/khz | same |
| 134u | 5009 / 0 of 85 | 0.20 | 0/133 | – | rbu134root |
| 135u-136u | 5005-5009 / ≤1 | – | 0/23-25 | – | rbu135root, rbu136root |
| 137u | 5005 / 0 of 20 | 0.11 | 0/21 | +dpu-retire-flip, +ruan-gpu-perf | rbu137root |
| 138u | 5004 / 2 of 20 | 0.10 | 0/23 | +ruan-gpu-dcvs, −debug=0x8, −disable-watchdogd | same |
| 139u | no device log | – | – | – | rbu139root (trimmed + wallpaper fixes) |
| 140p | 5009 / 0 of 23 | 0.17 | 22/22 (copy again) | +gpu-irq, armcpu-edge, owner-dataprotect, touch_irq, gpu-fence-wake=0, data-persist=2, data-reset; −fb-wc, −memlimit-floor, −wifi/audio | persistent Data, seed 1 (Reduce Motion off) |
| 141p | 5006 / 0 of 33 | 0.16 | 31/31 | −data-reset | Data from 140p |
| 142p | 5012 / 7 of 50 | 0.15 | 46/46 | +launchd_disable_all_timeouts | Data from 140p |
| 143u | 5003 / 1 of 17 | (one capture) | 18/18 | – (TU_DEBUG gmem from a data file) | rbu143root (full stock root) flashed |
| 144p | 4989 / 0 of 14 | (one capture) | 15/15 | +tu-debug=sysmem, −gpu-fence-wake=0 | same |
| 145p | 5042 / 0 of 4 | (one capture) | 5/5 at 93 s uptime | +fb-wc, +data-reset, −tu-debug | seed 2 (liveactivitiesd, biomed, biomesyncd re-enabled), new kernel (DPU diagnostics) |
| 146p | no capture on disk | – | – | board args moved into `ruan-default-bootargs.patch` (armcpu-edge was missing from it) | Data from 145p |
| 147p-148p | no capture | – | – | +armcpu-edge | – |
| 149p | only `dm.txt` | – | – | +ruan-120hz=2 | backboardd SIGBUS in libvulkan_freedreno at 170 s, respawned (see below) |
| 150p | no capture | – | – | +gpu-fence-wake=0, −120hz | – |
| 151p | no capture | – | – | +ruan-gpu-dcvs=0 ruan-gpu-irq=0 | – |
| 152p | no capture | – | – | +gpu-prime=0 gpu-prime-system=0, dcvs/irq back on | – |
| 153p | no capture | – | – | +dpu-scanout=0 | – |
| 154p | no capture | – | – | +ruan-gpu-perf=0 ruan-gpu-dcvs=0 | – |
| 155p | no capture | – | – | vsync=120 vsync-fixed=0 dpu-retire-flip=0 | – |

The parent session has the rbu146p-rbu155p SUBMIT-MS numbers. They are not saved under `/root/unixnu-work/logs`.

Earlier builds (all [V]):

| build | SUBMIT-MS | IPI args |
|---|---|---|
| rbu81 | 15 s pattern (old pacing) | spi-ipi + self-ipi |
| rbu83c | median 52 ms, 116 of 122 intervals under 1 s, 1855 frames | spi-ipi + self-ipi |
| rbu84 | 50 ms bursts plus 5 s idle gaps | spi-ipi + self-ipi |
| rbu85u-rbu109u | no SUBMIT-MS capture | spi-ipi + self-ipi (94u/95u also had smp-gic=1) |
| **rbu110u** | no capture | **spi-ipi and self-ipi dropped, smp-gic=1 only** (also anim_max=1, tu-debug=sysmem) |
| rbu114u-rbu126u | median about 5008, ≤2 short intervals, ≤0.20 fps | smp-gic only (+noidle=1 from 114u) |

## What is *not* the cause (verified)

- **The CollectionsPoster crash loop** (opt/pacing.md). It explains the idle 5 s redraws on rbu130u, but rbu142p-rbu145p
  have one poster PID and no poster corpses, and the cadence stays the same.
- **Display mode, pacing, retire-flip, scanout, GPU clock/DCVS, GPU IRQ, fence-wake, tu-debug, fb-wc.** Each was changed
  in rbu127u-rbu145p with no effect on the cadence. PACING keeps `slot345=0 timeouts=0 retire_refused=0`, and SWAPS shows
  validate→flip→retire within 5-40 ms, so the display side is not the 5 s.
- **The rbu145p seed (liveactivitiesd, biomed, biomesyncd).** The cadence was already there with the old seed (140p-144p)
  and before persistent Data existed (114u-138u). The rbu149p dmesg has no biomed or liveactivitiesd exits and no
  timeout/wait lines (only `ifnet_attach: Waiting…`).
- **The sandbox denial `backboardd deny iokit-open-user-client RuanAdrenoDeviceUserClient`.** It is present in every build
  from 127u on, the same as before.

## Process exits and crashes around the slow builds

- rbu149p: backboardd (pid 48) took a SIGBUS in `libvulkan_freedreno` ← NativeAdrenoMetal at 170 s. It was respawned as
  pid 210, and RBS killed SpringBoard, PosterBoard and the other extensions ("backboardd respawn"). Just before that
  (162 s) came "backboardd caught waking the CPU 45121 times over ~106 s (424/s)". rbu144p has the same backboardd SIGBUS
  at 140 s (tu-debug=sysmem). [V]
- rbu149p: passd crash-loops (FinanceKit `swift_unexpectedError`, 11 reports, about every 20 s). This is new with the
  data-reset 145p seed. Wasteful, but not a 5 s source. [V]
- SpringBoard: FBS `processIdentity` asserts (EXC_BREAKPOINT in FrontBoardServices), already noted for the trimmed root. [V]
- No JetsamEvent reports and no memorystatus kills in any of the captured dmesg. [V]
- backboardd's CPU-wake rate went from about 210/s (127u-138u) to 420-440/s (140p+). [V] The likely reason is
  touch_irq/gpu-irq and armcpu-edge. [I]

## Ranked causes

1. **IPIs lost since rbu110u** (`armcpu-spi-ipi=1 armcpu-self-ipi=1` gone). Evidence: the arg change is the documented
   requirement under Gunyah, and the 5 s-only pattern starts at the first capture after it. Nothing in display or GPU
   settings moves the cadence, which points to a scheduling-layer quantum. [V arg/timing, I mechanism]
   Not checked: whether `smp-gic=1` makes SGIs work after all. `armcpu_core_sgi[]` and `armcpu_ipi_sent` exist in
   ArmCpu.cpp, but no USB command prints them.
2. **noidle=1 (rbu114u+) combined with 1.** Idle cores that never take an interrupt make lost wakeups wait longer. [I]
3. **The UFS root / rbu85u-113u userland changes** (trust.bundle enrollment of NativeAdrenoMetal at rbu113u, so the GPU
   path runs everywhere). These could add a 5 s timeout inside the render server instead. [I] The only 5 s constant
   found in the GPU drivers is the BO contiguous-alloc backoff (NativeAdrenoMemory.cpp:236), which does not block frames. [V]
4. The rbu145p seed and DPU-diagnostics kernel: ruled out as the start (see above). They may still cost a little more
   per frame. [I]

## Cheapest A/B boots (boot-args only, no data reset)

| boot | add to the command line | expect if cause 1 is right |
|---|---|---|
| A | `armcpu-spi-ipi=1 armcpu-self-ipi=1` (keep smp-gic=1) | SUBMIT-MS drops to 16-50 ms while touching, as on rbu84 |
| B | `cpus=1` (or `smp=0`) | no cross-core IPIs are needed, so the 5 s quantum disappears (slower, but no 5 s steps) |
| C | A + `noidle=0` | only if A helps partly |

Measure each with `display` taken within about 10 s of a swipe (the ring holds about 21 submits), plus the accepted delta
over 60 s of continuous touching. If A works, add `armcpu-spi-ipi=1 armcpu-self-ipi=1` to `ruan_default_boot_args`.
Only kernel-side readers see the default list, which is fine here because ArmCpu parses them with PE_parse_boot_argn.
Mind the 1000-byte command line.
