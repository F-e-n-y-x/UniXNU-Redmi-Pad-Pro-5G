GPU governor (DCVS) + profiles for macOS 27.3 on ruan. Prepared 2026-11-26. Not built, not booted, not committed.

Patch: gpu-gov-dcvs-profiles.patch  (replaces ipados-gap/03; do NOT also apply 03)
  cd /root/unixnu-work/UniXNU-ruan-macos && patch -p1 < .../gpu-gov/gpu-gov-dcvs-profiles.patch
  Needs 01/01b/02 (already in the tree). --dry-run clean against the tree of 2026-11-26 19:20.
  Touches package/RuanGpuPower.c, RuanInput.c (ruan_screen_off symbol, as 03), UsbDebugCdc.cpp (USB `gpufreq`).
  ipados-gap/04 and 07 still apply on top (they only need ruan_screen_off).

What changed against 03
  - Default is cpuprof=0 = performance = today's path exactly: one-level tables, {1,1,1} vote, NOM 734 MHz,
    ruan-gpu-corner/-khz still work, no governor thread. (03 turned perf tables + DCVS on by default.)
  - Profile arg = the CPU agent's `cpuprof` (0 performance, 1 balanced, 2 powersave; >2 = 0):
      1: 9-level KGSL tables, boot L6 (734), DCVS floor L3 500 MHz, ceiling L9 940 + tsens cap (85 C -> L6, 95 C -> L3).
      2: same, ceiling L4 600 MHz.
    Overrides keep working: ruan-gpu-perf, ruan-gpu-dcvs, ruan-gpu-max-level, ruan-gpu-level.
    Fixed max with cpuprof=0: ruan-gpu-perf=1 ruan-gpu-level=9 (only after the speed bin is checked, docs/research/opt/gpu-perf.md §4).
  - Vote failure: stop on the FIRST failure (any AdrenoHfi failure poisons the session; 03's "3 failures" were 1 real + 2 Poisoned).
    Vote poll budget 500 -> 5000 x 100 us (~0.5 s) so a slow ack under load is not a failure. dcvs_start refuses when gmu_hfi is poisoned.
    After a stop nothing else is written: the GMU stays at cur or want, OOB GPU_SET stays held, the GPU keeps running.
  - SMP (8 cores): dcvs_start takes dcvs_running with an atomic exchange (USB `gpu N` and glue_late/LegacyDisplayProbe can race).
    Sampling is CPU-agnostic: GMU XOCLK counter + mach_absolute_time (global timer), delay() blocks; nothing per-CPU.
  - Diagnostics array 9 -> 12: [9] cpuprof, [10] boot perf index (0 = one-level tables), [11] failed vote's status.
  Checked, no conflict: GPU IRQ (01) is the RBBM SPI 300, not HFI; hbb_lo (01b) is hw_init; scmCall's hakios_spmi_lock is not
  touched by DCVS (MMIO only, no SCM/SPMI). RuanAdrenoGlue.cpp sends no HFI and does not touch the GMU, so the governor is the
  only runtime HFI sender; ruan_gpu_run (every other sender) joins it first, as in 03.

GPU idle / power collapse: keep it OFF.
  Today the GPU never collapses: RuanGpuPower holds OOB GPU_SET (HOST2GMU bit 30) forever, the A710 has no IFPC
  (gen7_0_0 minus IFPC/ACD/preemption, docs/research/gpu-dev/README.md), the glue's IFPC power-up list is empty, and
  H2F_MSG_PREPARE_SLUMBER (33) is never sent. Slumber would need the KGSL wake path (OOB set/clear around every submit, GX GDSC
  re-on, hw_init + CP init + zap re-run on wake, HFI restart) that this port does not have; any miss = GPU hang with no recovery
  (gpu-gen restart is not wired). No notes record a collapse attempt or a collapse-caused hang (the iPadOS hangs rbu71-78 were
  stale cache lines). Idle power on this port = DCVS floor: balanced/powersave drop to L3 500 MHz within ~64 ms of idle,
  and to L3 with ruan_screen_off once 07 is in. Revisit slumber after DCVS is proven and a GPU restart hook exists.

USB test plan (RAM-only fastboot boot, UFS root unchanged)
  1. cpuprof unset: `gpufreq` -> prof 0 boot L0 dcvs off (0 = governor never started; with mode 16 prof/boot fields may stay 0
     if GX is off). dmesg: no "gpu perf:" lines. Desktop identical to today. This is the regression check.
  2. cpuprof=1 ruan-gpu-max-level=7 (keep 8/9 off until the bin is read): dmesg "gpu perf: L1..L9 vote", "boot vote L6",
     "ruan-gpu: dcvs on, L6, floor 3 ceiling 7, tsens mapped". `gpufreq` idle: L3 500 MHz, busy low, temp 300-600 dC,
     failed 0, no-tsens 0. Drag windows / Mission Control: busy rises, L climbs, changes counts up. Watch the camera for stalls.
  3. cpuprof=2: ceiling L4; idle L3, load caps at L4 600 MHz.
  4. Failure path: if `failed 1 (stat 10)` (Timeout) the governor stopped; the desktop must keep rendering. Report the stat.
  Back out: cpuprof=0 (or ruan-gpu-dcvs=0 to keep the 9-level tables at a fixed level).

Risks
  - tsens TM read at 0xc263000 from EL1 is unproven on macOS (iPadOS note: a map/read abort would SError). Balanced/powersave only.
  - Speed bin not read; L8/L9 may overclock a lower bin. Use ruan-gpu-max-level=7 first.
  - One HFI timeout leaves the level wherever the GMU put it, and a later USB `gpu N`/`gpu off` sees a poisoned session (as today
    after any HFI failure).
  - Busy counter setup (0x23b0e/0x1f841/0x1f840) is first written by this code; if the GMU firmware reprograms it, busy reads wrong.
