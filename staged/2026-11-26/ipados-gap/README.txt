iPadOS -> macOS gap patches (2026-11-26). Not built, not run. Each was checked with patch(1) against a copy of
UniXNU-ruan-macos taken at about 16:10. 01-04 and 07 were applied in order to that copy. 05 and 08 were dry-run against
the xnu-12377 feed (ufs-variant-macos/out/feeds/xnu-3199e8bc).

Apply from /root/unixnu-work/UniXNU-ruan-macos with patch -p1, in this order: 01 02 03 04 07.
  01-macos-gpu-irq-default.patch     RuanAdrenoGlue.cpp: GPU SPI 300 interrupt, so fence waits sleep instead of spinning in 20 us steps.
                                     On by default (ruan-gpu-irq=0 turns it off). If the interrupt storms, it masks the line and falls back to a 1 ms tick.
  02-macos-gpu-clock-nom.patch       RuanGpuPower.c: the GX corner is NOM, 734 MHz, as iPadOS since rbu124u (it was LOW_SVS, 295 MHz).
                                     ruan-gpu-corner and ruan-gpu-khz override it.
  03-macos-gpu-perf-dcvs.patch       RuanGpuPower.c and RuanInput.c: the 9-level KGSL tables, the GPU governor and the tsens thermal cap.
                                     Default on (ruan-gpu-perf=0 and ruan-gpu-dcvs=0 turn them off). Needs 02.
  04-macos-cpufreq-governor.patch    RuanCpuFreq.c: mode 3 (governor, L3 vote, big-core thermal cap, touch boost) and a start-once guard.
                                     The build script switches the default from 2 to 3. Keeps the macOS rule of no vote for a cluster that is down.
                                     Needs 03 (ruan_screen_off). Host self-test passes:
                                     cc -DRUAN_CPUFREQ_HOST -DRUAN_CPUFREQ_SELFTEST RuanCpuFreq.c && ./a.out
  07-macos-backlight-off-at-zero.patch  RuanBacklight.cpp: level 0 turns the backlight off and sets ruan_screen_off, so the
                                     governors drop to their floor. Needs 03.
Apply from /root/unixnu-work/UniXNU-macos (core) with patch -p1:
  06-adreno-0006-sandbox-grant-gpu-service.patch  The iPadOS patch, unchanged. Copy it to UniXNU-ruan-macos/patches/adreno/
                                     (build-ruan-adreno.py applies patches/adreno/*.patch to the driver copy).
xnu-12377 (add to patches/xnu-12377/ufs.series):
  05-xnu12377-ruan-default-bootargs.patch  Built-in default boot-args. The command line wins. The list holds only arguments the
                                     kernel reads. Once applied, remove those arguments from board.toml kernel_args (about 400 bytes).
  08-xnu12377-ruan-slow-wait-log.patch     The "ruan-slow:" logger, which found the iPadOS 5 s libEDR stall. Use it for the
                                     coreanimation GpuHang. Only the sleh.c prologue hunk was changed.
