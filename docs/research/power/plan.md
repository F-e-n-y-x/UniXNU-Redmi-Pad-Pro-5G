# Power saving and sleep (ruan, iPadOS 17.7.11 on UniXNU)

Status 2026-11-21: code written and compile-checked, **not run on the tablet**.

## Findings

| # | Finding | How we know |
|---|---|---|
| F1 | `noidle=1` (built-in default) is **not** CPU idle. It is IOPMrootDomain's `gNoIdleFlag` (`iokit/Kernel/IOPMrootDomain.cpp:1595`), which only stops the *system* idle-sleep timer. | verified, source |
| F2 | Cores spin in idle because `cpu_idle()` (`osfmk/arm64/cpu.c:501`) refuses WFI while `cpu_signal & SIGPdisabled`. That flag is cleared only when a core takes its first IPI, and ruan has no IPIs. `wfi` and `idle_enable` are both 1. | verified, source |
| F3 | XNU already has a per-core **idle timer** (`idle_timer_notify` / `idle_timer_deadline`, `cpu_idle`, `timer_intr` in `arm_timer.c`, `timer_resync_deadlines`). A core in WFI with this timer armed wakes without an IPI. `timer_intr` calls the callback without a NULL check, so one must be installed. | verified, source |
| F4 | Governor load (`RuanCpuFreq.c`, `recount_processor_usage` idle time) already counts the idle thread's spin as idle: `processor_idle()` calls `recount_processor_idle()` before its loop (`sched_prim.c:6614`). **So the spin does not explain 2.4 GHz with the screen off.** | verified, source |
| F5 | The 2.4 GHz big cluster with the display off therefore comes from a real busy CPU in domain 1 (≥90 % of a 16 ms window) or from the touch boost. Candidates: touch reports while the backlight is 0 (Novatek keeps scanning), and backboardd/CoreAnimation still running on the software vsync (`vsync-fixed=1`, gopfb ticks keep going while the backlight is 0). The new `busy`/`boost` fields in USB `cpufreq` will tell which. | inferred |
| F6 | `delay()` in the GPU DCVS thread blocks rather than spins (`ml_delay_should_spin` is FALSE while `cpu_idle_latency` is 0). | verified, source |
| F7 | UniXNU ArmCpu has a platform-sleep path (`platform-sleep=1`, `ArmCpuSleep.h`). It is used by UniXNU-sheng (`patches/xnu-12377/sheng-platform-sleep.patch`, `package/ShengSleep.cpp`, macOS xnu-12377). On wake, the boot core and each secondary core need **an IPI**: `processor_cpu_reinit` waits for `SIGPdisabled` to clear, and ArmCpu sends a self-SGI. ruan's SGIs do nothing, and SPI IPIs hang at display start. So deep sleep is blocked on IPIs. | verified (source); the ruan IPI facts come from earlier tests |

## Implemented (default off unless stated)

1. **Timer-bounded WFI idle**, `patches/xnu/ruan-idle-wfi.patch` (added to `patches/xnu/series`, both boards). The boot-arg is `ruan-idle-wfi-us=N` (TUNABLE_WRITEABLE, default 0 = today's spin).
   - With N > 0, a core whose interrupt handler is installed may WFI even though SIGPdisabled is set.
   - Its idle timer is capped at N µs. If ArmCpu has not registered an idle timer callback, a no-op one is installed.
   - On wake, `processor_idle` re-checks the core's state and run queue, so scheduler latency is at most N.
   - `ruan_idle_wfi_count[cpu]` counts WFIs per CPU.
   - `ruan_idle_wfi_sleep_us`, when non-zero, replaces N. RuanInput.c sets it while asleep.
   - Not made the default: it is untested on Gunyah. Try `ruan-idle-wfi-us=1000` on the command line first.
2. **Shared display state**, `package/RuanInput.c`: `ruan_screen_set(off)`, `ruan_screen_off`, `ruan_sleeping`, `ruan_screen_changes`. `RuanBacklightSvc.cpp` calls it whenever it writes a level, with `target == 0`.
3. **Screen-off caps** (always on):
   - `RuanCpuFreq.c` caps both domains at level index 1 (second-lowest) and turns off the touch boost. Asleep, the cap is index 0. The L3 follows the domains down. A drop to the cap happens at once.
   - `RuanGpuPower.c`'s DCVS ceiling becomes `DCVS_FLOOR` (level 3, 500 MHz, the governor's own floor). Levels 1-2 are left alone because they are untested under DCVS.
4. **Suspend-to-idle stage 1**, `ruan-sleep=1` (default 0). It is active while the screen is off.
   - Idle cores WFI with a bound of `ruan-sleep-wfi-us` (default 10000), even when `ruan-idle-wfi-us=0`.
   - The CPUs run at their lowest levels and the GPU at its floor.
   - The touch thread reads only on its interrupt (no 8/80 ms heartbeat reads). Without the IRQ path it does no reads and sleeps 100 ms.
   - The buttons are polled every 50 ms instead of 16 ms. Two looks still catch a press of about 100 ms. Coordinator suggested 100 ms, which would miss taps.
   - Wake works through the normal path: the power key reaches SpringBoard, the backlight goes up, and `ruan_screen_set(0)` restores everything.
5. **USB**:
   - `cpufreq` gains `busy b0/b1 boost screen-off sleep`.
   - `cpu` gains a first row, `IDLE: wfi|spin us= sleep-us= wfi c0/../c7 | screen-off sleep changes`.

Compile-checked (verified):
- The patched `cpu.c` with its real compile record (ufs-variant tree, rc 0).
- RuanInput, RuanTouch, RuanButtons, RuanCpuFreq, RuanGpuPower and RuanBacklightSvc with `build-ram-root-probe.py`'s flags (`-Werror`).
- UsbDebugCdc with `build-gop-legacy-display.py`'s flags.
- The RuanCpuFreq host self-test.
- `git apply --check` of the patch on the pristine `cpu.c`.

Not linked or booted.

## Test order

1. Default boot with no new args. Check `cpufreq` with the screen on and off: `screen-off 1` and `d1` at its second-lowest level. Note `busy`/`boost` before the cap takes effect (F5).
2. `ruan-idle-wfi-us=1000`: boot to the home screen, then check `cpu` IDLE wfi counts rising, UI latency and touch. Watch for a hang at display start, where the IPI attempts hung before.
3. `ruan-sleep=1`: press power, check `cpu`/`cpufreq` (`sleep 1`, `sleep-us=10000`), then press power again and check that the screen comes back. Compare the battery drain over 10 min (`battery`).

## Deeper platform sleep (stage 2, not implemented)

Needs, in order:
- **IPIs or an IPI-free resume.** `processor_cpu_reinit` and the sleep path wait for SIGPdisabled to clear. The options are to find why `armcpu-spi-ipi` hangs at display start, or to patch the resume wait for ruan the same way `ruan-idle-wfi.patch` lifts the idle gate.
- **Port `sheng-platform-sleep.patch` to xnu-10063.** It installs `arm64_platform_sleep_hook` in `ml_arm_sleep` and lifts IOPMrootDomain sleep-disabled. Then remove `noidle=1` and change `make-ramroot.py` SEED_PREFERENCES `SBDisableIdleSleep`/`SBDontDimOrLockOnAC` so SpringBoard asks for sleep.
- **A ruan `armcpu_sleep_board`**, modelled on ShengSleep.cpp:
  - **Wake sources:** the PMK8350 PON KPDPWR interrupt through the SPMI arbiter (PMIC_ARB irq) and the PDC, which needs PON interrupt enable writes. RuanButtons is read-only today, so this means a new and careful write path. Also USB/charger (PM7250B USBIN).
  - **Fallback:** a `tick` every 1 s that reads PON_RT_STS and wakes on a press.
- **Quiesce/resume:**
  - DPU/DSI panel off/on (NT36532 + DSI cmds, gopfb scanout restart).
  - GMU power collapse over HFI, then a cold GX restart.
  - The Novatek sleep command (0x11 to the host-cmd event), with a reset and firmware check on resume.
  - UFS hibern8.
  - WPSS: keep it, with Wi-Fi as a wake source later.
  - USB debug port off, and the governors parked.
