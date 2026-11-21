# CPU frequency (EPSS + L3) and touch latency on ruan

Patches next to this file (nothing built, flashed or tested on the device):

- `ruan-cpufreq-l3.patch`: `package/RuanCpuFreq.c`, new mode `ruan-cpufreq=3`. Optional `ruan-cpufreq-boost-ms=<ms>` (default 1000, 0 turns the boost off).
- `ruan-touch-latency.patch`: `package/RuanTouch.c` + `package/hakios.novatek_ruan.h`, new boot-arg `touch_fast=1`.

Apply from `macOS/unixnu-ruan/package` with `patch -p1 < …`. Both patches apply cleanly (`--dry-run`) to the working tree as of 2026-11-21, which already includes the uncommitted `touch_any_down()` change.

Tags: **[V]** = checked against the live DT, source or logs. **[I]** = inferred, needs a boot to confirm.

## 1. Register map (live DT `hardware/private/ruan-live.dts`)

| Block | DT node | Address | Notes |
|---|---|---|---|
| CPU domain 0 (cpus 0-3, A55) | `qcom,cpufreq-hw` `qcom,cpufreq-hw-epss`, freq-domain0 | 0x17d91000 (0x1000) | [V] The cpus' `qcom,freq-domain = <&cpufreq 0 4>`. |
| CPU domain 1 (cpus 4-7, A78) | freq-domain1 | 0x17d92000 (0x1000) | [V] `<&cpufreq 1 4>`. The node has `qcom,skip-enable-check` and dcvsh IRQs SPI 30/31. |
| Domain registers | qcom-cpufreq-hw epss data | state 0x20, dcvs_ctrl 0xb0, LUT 0x100 (4-byte rows), perf_state 0x320 (+4·cpu) | [V] Mode 2 works: IPADOS-BRINGUP.md shows "d0 1804→1958, d1 1900→2400 MHz", so EL1 can write these under Gunyah. |
| **L3** | **not `qcom,epss-l3`**. The node is the vendor `/soc/qcom,dcvs/l3` (`qcom,dcvs-hw`, `qcom,dcvs-hw-type = 2`) | `l3-base` 0x17d90000 (0x4000), `l3tbl-base` 0x17d90100 (0xa0 = 40 rows) | [V] |
| L3 vote | slow path `l3/sp`, `qcom,dcvs-path-type = 0`, `qcom,shared-offset = 0x90` | **0x17d90090** | [V] Same offset as sheng's `EPSS_REG_L3_VOTE`. There is no fast path (no `qcom,per-cpu-offset`). [I] The L3 LUT uses the same row format as the CPU LUT (sheng/osm-l3), and the write takes effect without an enable check. |
| TLMM | `pinctrl@f000000` `qcom,parrot-pinctrl`, `wakeup-parent` = PDC, reserved gpios 2-3 | GPIO n block = 0x0f100000 + 0x1000·n: CFG +0, IO +4, INTR_CFG +8, INTR_STATUS +0xc | [V] for base and CFG/IO (SPI pins work). [I] For the INTR_CFG bits (enable 0, polarity 1, detect 3:2, raw-status 4, target from 5 up), from pinctrl-msm. The parrot layout is not in any tree here. |
| **Touch INT** | `spi@a8c000/touch@0`: `novatek,irq-gpio = <&tlmm 0x36 0x2001>`, `interrupts = <0x36 2>` | **GPIO 54** | [V] 0x36 is hex. |

## 2. What RuanCpuFreq.c does today (mode 2)

[V] One thread maps each domain and waits up to 30 s for `state != 0`. It then reads the LUT and writes the highest non-turbo row into perf_state for 1 or 4 CPUs (depending on dcvs_ctrl bit 0), logs, and exits. The CPUs stay pinned at 1958/2400 MHz forever. There is no L3 vote, so the L3 stays at whatever firmware or UEFI left, and with no Linux memlat it is probably low [I]. There is no load tracking and no thermal handling beyond the EPSS firmware's dcvsh clamp.

## 3. Mode 3 design (in the patch)

- First runs mode 2 (both domains at the top), then a 16 ms loop in the same thread.
- **Load:** for each CPU, `1 − Δidle/Δt` from `recount_processor_usage(&cpu_to_processor(c)->pr_recount, …)`. This is the same as sheng's `sheng_cpu_idle_mach` helper. RuanCpuFreq.c is compiled with the osfmk flags (`startup.o.json`), so it calls the helper directly and needs no XNU patch [I: assumes MACH_KERNEL_PRIVATE in those flags]. A domain's load is its busiest CPU. CPU c belongs to domain c/4 [I: XNU cpu order = DT order].
- **Level:** the lowest LUT level ≥ 1.25 × load × current MHz (schedutil's margin). At ≥ 90 % busy it jumps to the top. It goes up at once, and down only after 4 windows in a row ask for less.
- **Touch boost:** when `ruan_touch_events` (RuanInput.c) changes, both domains go to the top for `boost_ms`.
- **L3:** the lowest L3 level ≥ (faster domain's current MHz / its top) × L3 top, with the same up/down hysteresis. It starts at the top. It is written to 0x17d90090.
- The pure parts (`ruan_cpufreq_levels`, `ruan_cpufreq_want`) have a host self-check, which passes:
  `cc -DRUAN_CPUFREQ_HOST -DRUAN_CPUFREQ_SELFTEST RuanCpuFreq.c && ./a.out`

Left out on purpose: a frame floor (sheng's GPU-submit signal; ruan has no GPU submit counter yet), throttle counters, and a cap boot-arg.

## 4. Why the touch edge latch "misses most reports"

[V] **The code watches the wrong pin.** `NVT_PIN_INT` is 36, copied from the DT cell `0x36`, which is decimal **54**. Both `interrupts = <0x36 2>` and `irq-gpio` say 54, and `gpio36` appears nowhere in the DT. Every `nvt_edge_seen()` hit came from whatever GPIO 36 does, not from the chip. Touch only works because of the fallback reads:

- the 80 ms idle read, which gives **up to 80 ms first-touch latency**;
- `touch_any_down()`, which reads every tick while a finger is down.

The latch code itself looks correct [I]:
- It sets raw-status (bit 4), so INTR_STATUS latches with delivery off.
- It sets both-edge detection (3 in bits 3:2), and polarity bit 1 as Linux does for edges.
- It clears by writing 0. The read→clear race window is a few ns, which does not matter.
- It keeps bits ≥ 5 (target/wakeup) as found.

GPIO 54 also shows up in camera `cci_i2c_scl2` pinctrl states. Those states are unused while the touch node owns the pin, but a boot must confirm it [I].

## 5. Low first-touch latency

- Use the right pin and keep the latch. Polling INTR_STATUS costs one uncached MMIO read (well under 1 µs) per tick, so a 1 ms tick is cheap. A raw latch catches pulses far shorter than any poll period, so polling the level is not needed. The DT's IRQ type is falling edge; both edges are latched anyway.
- `touch_fast=1` does the following:
  - sets pin 54;
  - polls every 1 ms (`touch_poll_us` still overrides);
  - does a blind read while a finger is down only when nothing was read in the last 5 ms (one 240 Hz report period). That prevents duplicate 1 kHz reads when INT works, and falls back to about 200 Hz if it doesn't;
  - logs `novatek: touch_fast: 256 edges; pin, reads` once. If that line never appears, pin 54 is wrong too.
- Expected: first touch ≤ 1 ms + one SPI read, down from ≤ 80 ms [I].
- A real GIC interrupt (TLMM summary SPI 208, or the PDC) would remove the tick entirely. That needs an XNU interrupt hookup; not done.

## 6. Precise wakeup instead of `sleep_us`

[V, from XNU source in /root/unixnu-work/scratch/xnu-test] `sleep_us` → `delay_for_interval` → `assert_wait_deadline` with urgency SYS_NORMAL. `timer_call_slop` then adds `min(interval>>3, 1 ms)` for a kernel-priority thread, plus the user-idle-level scaling (`tcoal kt shift 3`). A 2 ms sleep therefore wakes after ≥ 2.25 ms, and the read time adds drift on top.

`TIMEOUT_URGENCY_LEEWAY` with leeway 0 does **not** help, because it can only raise the slop. **`TIMEOUT_URGENCY_SYS_CRITICAL`** gives zero slop.

The patch's `touch_tick()` uses an absolute deadline that advances by the period each tick, and skips ahead if it falls behind:
`assert_wait_deadline_with_leeway(ev, THREAD_UNINT, TIMEOUT_URGENCY_SYS_CRITICAL, deadline, 0); thread_block(...)`

The thread priority (kernel default, about 81) is left alone. If RT render threads starve it, start the thread with `kernel_thread_start_priority(…, BASEPRI_PREEMPT)`.

## Test plan (for the ROM/port session, one change at a time)

1. `touch_fast=1` alone: look for the `256 edges` log line, then check first-tap latency on the camera.
2. `ruan-cpufreq=3`: look for the `governor … l3 N levels` line, then check scroll smoothness and idle temperature/battery against `=2`.
