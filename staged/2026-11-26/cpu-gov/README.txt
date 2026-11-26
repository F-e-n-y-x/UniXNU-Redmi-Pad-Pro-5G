CPU profiles + idle-down for macOS 27.3 on ruan (prepared 2026-11-26, NOT built, NOT booted)

Patches
  01-macos-cpufreq-profiles.patch   UniXNU-ruan-macos, patch -p1. RuanCpuFreq.c + UsbDebugCdc.cpp. Supersedes
                                    ipados-gap/04 (do not apply both). No build-script change: default stays ruan-cpufreq=2.
                                    Applies before or after gpu-gov/gpu-gov-dcvs-profiles.patch (checked both orders).
                                    Host self-test: cc -DRUAN_CPUFREQ_HOST -DRUAN_CPUFREQ_SELFTEST package/RuanCpuFreq.c && ./a.out
  02-xnu12377-ruan-idle-wfi.patch   xnu-12377 osfmk/arm64/cpu.c. Add to patches/xnu-12377/ufs.series right after
                                    ruan-smp-sigp.patch (it is made against arm64e-subtype-grade + smp-sigp). Default off.

Profiles: boot-arg cpuprof=0|1|2 (same arg/numbering as the GPU patch; >2 = 0), runtime USB `cpuprof N`.
Live value: volatile uint32_t ruan_perf_profile (RuanCpuFreq.c), for the GPU side to read (weak) if it wants runtime switching.
  0 performance (default) - byte-for-byte today's behaviour: top non-turbo vote once (d0 1958, d1 2400), no L3 write, no
    thermal cap. The thread then blocks with no timeout (zero wakeups) until USB changes the profile.
  1 balanced - 16 ms windows, per domain the busiest CPU's busy share from recount_processor_usage idle time (accounts the
    idle spin as idle; idle time is brought up to date for a CPU still idling). Level >= 1.25 x load x clock, top at 90 %,
    up at once, down after 4 windows. Floor d0 >= 1000 MHz (cpu0 takes USB/UFS interrupts), d1 lowest. Touch boost 1 s.
    L3 (17d90000 +0x90) follows the faster domain, as iPadOS.
  2 powersave - same governor, d0 600..1400 MHz, d1 <= 1500 MHz, no touch boost.
  1/2: big-core thermal cap at 90 C, release < 85 C after >= 1 s, on a 4-window average (the iPadOS log shows the
  unfiltered cap flipping every 16 ms between 83 and 93 C).
8-core changes vs 04: CPU -> domain from topology MPIDR Aff1 (not cpu number / 4); processors re-read every window (04
  cached cpu_to_processor once, before the secondaries may exist -> d1 seen idle forever); CPUs not RUNNING/IDLE/DISPATCHING
  count idle and get a fresh baseline; first window only takes baselines (no drop to floor on no data); one start only
  (BootProgress.c and RuanMacBootProgress.c both call ruan_cpufreq_start -> two governors would fight).
Low Power Mode: no kernel signal in xnu-12377 (powerd posts the notify key com.apple.system.lowpowermode, consumed by
  Apple's CLPC kext, which we do not have). Would need a sysctl + a userland agent; left out.

Idle (finding): with smp=1 smp-gic=1 the secondaries never take an SGI (logged `cpu` rows: i=0 everywhere, idle n=0), so
  SIGPdisabled stays set and cpu_idle refuses WFI: idle cores spin at their domain clock. ruan-sigp-recover=1 (clears it ->
  real IPIs) hard-reset cpus=2/8. 02 lets an idle core WFI anyway, bounded by its idle timer (ruan-idle-wfi-us=N), keeping
  SIGPdisabled set so senders behave exactly as today; wake latency <= N. PSCI CPU_SUSPEND: not proposed (unproven; resume
  needs IPIs). Frequency of an idle domain: profiles 1/2 drop it to the floor because spin counts as idle.

Boot-arg budget: default needs no arg. cpuprof=1 costs 10 bytes, ruan-idle-wfi-us=1000 costs 22 (987 + 32 = 1019 > 1000):
  test one at a time, or put them in ruan-default-bootargs.patch's list (both are kernel-read), or drop args that list absorbs.

Test plan (USB)
  1. Default boot: `cpufreq` -> "mode 2 prof 0 d0 1958 d1 2400(~2054 under LMh) l3 row -1": must equal today.
  2. `cpuprof 1` at the desktop idle: within 0.1 s busy ~0-50/0-50, d0 ~1000+ MHz, d1 lowest, l3 row low. Drag a window /
     touch: d1 jumps to top (boost 1). `cpuprof 0`: back to 1958/2400. Check USB, UFS (ls/cat), touch stay fine.
  3. `cpuprof 2`: d0 <= 1400, d1 <= 1500 under load. Watch temp/capped.
  4. cpuprof=1 on the boot line: boot time vs prof 0, no hang at display start.
  5. ruan-idle-wfi-us=1000 (prof 0 first): `cpu` IDLE wfi c0..c7 rising, `sigp ... refused` stops rising; UI/touch latency;
     then together with cpuprof=1. Battery drain 10 min each (`battery`).
Not checked: kernel compile (no recipe locally; only the host self-test ran), real LUT values between bottom and top.
