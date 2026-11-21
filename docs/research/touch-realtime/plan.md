# Real-time touch on ruan: hardware interrupt, report rate, latency

Patch: `ruan-touch-realtime.patch` (in this folder). It changes `package/RuanTouch.c` and `package/hakios.novatek_ruan.h`. To apply it, run `cd macOS/unixnu-ruan/package && patch -p1 < …/ruan-touch-realtime.patch`. On 2026-11-21 it passed `--dry-run` against the working tree. It has not been built, flashed or booted.

Tags:
- **[V]** = checked in source, the live DT, or code that already runs.
- **[I]** = inferred; a boot must confirm it.

Sources:
- Xiaomi's own ruan kernel, `MiCode/Xiaomi_Kernel_OpenSource` branch **`ruan-u-oss`**. Files read: `drivers/pinctrl/qcom/pinctrl-parrot.{c,h}`, `pinctrl-msm.{c,h}`, `irqchip/qcom-pdc.c`, `input/touchscreen/nt36532_spi/nt36xxx.{c,h}`, `nt36xxx_mem_map.h` and `input/touchscreen/xiaomi/xiaomi_touch.h`.
- The live DT.
- UniXNU `drivers/ArmCpu/ArmCpu.cpp`.
- UniXNU-sheng `package/TouchProbe.cpp`.

New boot-args. Defaults are 0, so behaviour without them is unchanged.

| boot-arg | effect |
|---|---|
| `touch_irq=1` | Arms the GIC interrupt (TLMM summary) and sleeps on it. Until it attaches (up to 30 attempts, 1 s apart), the existing tick runs. Also sets pin 54. |
| `touch_active=1` | Sends Xiaomi's Touch_Active_MODE command (0x7A 0x01) after probe and after every firmware reload. |
| `touch_burst_us=N` (100–999) | Fallback only, with `touch_fast=1` and no IRQ. Ticks every N µs for 20 ms after a read, and at the normal 1 ms otherwise. |

Suggested test line: `ruan_touch=1 touch_fast=1 touch_irq=1`. Add `touch_active=1` separately afterwards, one change at a time.

---

## (a) A real interrupt for GPIO 54

### Facts

| Item | Value | Tag |
|---|---|---|
| Touch INT pin | GPIO 54 (`novatek,irq-gpio = <&tlmm 0x36 0x2001>`) | V |
| Trigger | **Rising edge.** The DT cell says 2 (falling), but the vendor driver ignores it: `nt36xxx.h` `INT_TRIGGER_TYPE IRQ_TYPE_EDGE_RISING`, `request_threaded_irq(..., int_trigger_type \| IRQF_ONESHOT)`. | V |
| TLMM | DT reg 0x0f000000. The pins sit at `REG_BASE 0x100000`, so GPIO n is at 0x0f100000 + 0x1000·n: CFG +0, IO +4, INTR_CFG +8, INTR_STATUS +0xc. INTR_TARGET is the same register as INTR_CFG. | V (pinctrl-parrot.h) |
| INTR_CFG bits (parrot PINGROUP) | enable 0, polarity 1, detection 3:2 (width 2: 1 rising, 2 falling, 3 both), raw_status 4, target 7:5 with kpss (apps) = 3. INTR_STATUS bit 0 is cleared by writing 0 (no `intr_ack_high`). | V |
| GPIO 54 WAKEUP_INT_EN | 0x0f100000 + 0x8D010, bit 6. Used only by `msm_gpio_mpm_wake_set`, not by the PDC path. | V |
| TLMM summary IRQ | `interrupts = <0 0xd0 4>`: SPI 208, level high, **INTID 240** | V |
| PDC map | `parrot_pdc_map { 54, 94 }`. The DT's `qcom,pdc-ranges` gives PDC pin 94 → **SPI 609** (range `0x5e 0x261 0x1f`), INTID 641. | V |
| What Linux does on ruan | The PDC is the TLMM `wakeup-parent`, so every mapped GPIO, 54 included, goes into `skip_wake_irqs`. For those pins Linux never touches the TLMM INTR_CFG and handles the IRQ at PDC SPI 609. The summary line serves only unmapped pins. | V |
| XNU interrupt API on this port | ArmCpu (in ruan's `board.toml`) exports `armcpu_spi_attach(intid, edge, fn, ctx, …)`, `armcpu_spi_share(intid, fn, ctx)` (level, multiple handlers) and `armcpu_spi_mask`. These program GICD group 1, priority 0xa0, ICFGR and IROUTER directly. RuanBacklightSvc.cpp already attaches SPI 602 this way. | V (code) |
| Gunyah | HLOS sees a GICv3 at 0x17200000; Gunyah emulates the distributor. SPI 208 and SPI 609 are both in the HLOS DT, so both belong to the HLOS VM. Device SPIs already work on ruan (UFS SPI 265; the `[hb]` lines count UFS interrupts). Pin 54's TLMM registers are writable from EL1: the latch already works. | V for the device SPIs and the TLMM writes; I that SPI 208 in particular is delivered |

### Chosen path: TLMM summary (SPI 208)

This is the path sheng uses for its NT36532 touch (`TouchProbe.cpp armTouchIrq`, SM8550 under Gunyah, GPIO 25):
1. Write INTR_CFG = target 3, raw 1, rising, polarity 1.
2. Clear INTR_STATUS.
3. Call `armcpu_spi_share(240, handler)`.
4. Set the enable bit last.

The handler:
1. Checks INTR_STATUS bit 0. If clear, the interrupt is another pin's: count it as spurious and return.
2. Writes 0 to INTR_STATUS, which drops the level summary.
3. Stamps `mach_absolute_time`.
4. Bumps a counter.
5. Calls `thread_wakeup_prim`.

The touch thread:
- Sleeps with `assert_wait_deadline_with_leeway(SYS_CRITICAL)` on the counter. A `clear_wait` closes the race between checking the counter and the assert.
- Reads once per interrupt.
- Wakes on its own after 20 ms with a finger down (to catch a lost lift) or after 80 ms idle.

With no finger and no reports, the thread costs nothing: the 1 kHz tick goes away.

Why not the PDC: it needs three extra register sets, and one of them is in the APSS/GIC block, where Gunyah might trap it. The summary path needs only the TLMM writes that already work. Use the PDC only if SPI 208 never fires.

**PDC alternative [V addresses, I access under Gunyah]:**
- PDC `IRQ_i_CFG[94]` = 0x0b220000 + 0x110 + 4·94 = **0x0b220288**, value 0b110 (rising).
- `IRQ_ENABLE_BANK[2]` = **0x0b220018**, bit 30.
- `apss-shared-spi-cfg` 0x174000f0 + 4·(609/32) = **0x1740013c**, bit 1 cleared (edge). The DT has no `qcom,scm-spi-cfg`, so Linux writes it as plain MMIO.
- Then `armcpu_spi_attach(641, true, …)`.
- Leave the TLMM INTR_CFG alone, as Linux does.

Risks [I]:
- SPI 208 might be withheld from HLOS while Linux isn't using it. The patch logs `touch_irq: attach result` and, if nothing arrives, the 80 ms safety read keeps touch alive. Check the `touch_irq: irqs, spurious` line.
- The firmware might pulse INT on every scan frame, even with no finger. That shows up as a high irq count while idle, with harmless empty reads.

### Fallback: tighter latch poll (no IRQ)

`touch_fast=1` already polls the INTR_STATUS latch with one uncached MMIO read per 1 ms SYS_CRITICAL tick. `touch_burst_us=250` cuts the tick to 250 µs for 20 ms after any read. Reports arrive in trains while a finger is down, so this means about 4 k wakeups/s only while touching and the normal 1 ms otherwise. The first-touch delay stays ≤ 1 ms; tracking delay drops to ≤ 250 µs.

---

## (b) Report rate / game mode

- **[V] Ruan's vendor driver has no report-rate command.** `xiaomi_touch.h` defines `Touch_Report_Rate = 9`, but `nvt_set_cur_value` has no case for it, and `ruan-live.dts` has no report-rate property. The `xiaomi_touch` node only has `get_panel_info`.
- **[V] Game mode itself sends nothing.** `Touch_Game_Mode` only gates whether the other modes reach the chip, through `nvt_xiaomi_write_reg(hi, lo)`. That function writes `[0x50 | 0x80, hi, lo]` after `nvt_set_page(EVENT_BUF | 0x50)` (EVENT_BUF 0x125800, the NT36532 single/cascade map).

Command bytes at EVENT_BUF + 0x50:

| Mode | Bytes | Values |
|---|---|---|
| Touch_Active_MODE | `0x7A, 0/1` | |
| Tolerance (jitter) | `0x70, 0..4` | |
| UP_THRESHOLD | `0x71, 0..4` | |
| Edge filter | `0x72, 0..3` | |
| Orientation | `ER_*, 0` | |
| USB plug | `0x53` / `0x51` | |
| Pen | `0x7C`, `0x7D` | ack polled at 0x50 = 0x00 |

- **touch_rate=480 is therefore not implemented for the NT36532.** The patch leaves the existing Goodix-only `touch_rate` alone. The 480 Hz figure belongs to other Xiaomi parts.
- **[I] The rate is fixed by the TDDI scan, which follows the panel refresh.** The panel's DFPS list is 144/120/60/48. Measure the actual rate with a moving finger: count valid packets per second (the `polls, valid packets` log, or the irq count / s in `touch_irq` mode). If the panel runs at 60 Hz under the port (`vsync=60`), the touch rate is probably reduced too. Running the panel at 120/144 is the real "rate" lever.
- **What the patch does instead:** `touch_active=1` sends `0x7A 0x01` with the existing `nvt_write(NVT_EVENT_BUF + NVT_EV_HOST_CMD, …)`. This is the same page the point reads use, so the cached `page_set` stays valid. It is re-sent after a 0xFD firmware recovery. [I] the effect is to keep the chip in active scan (no idle/doze scan), which should cut first-touch latency. Compare first-tap latency on the camera with and without it. The vendor sleeps 35 ms before the write; here probe has already slept 50 ms.

---

## (c) Latency instrumentation

In the patch, `touch_irq` mode only:
- Every 1024 interrupt reads, it logs `touch_irq: irq->hid us mean, max`. That is the time from the interrupt handler's `mach_absolute_time` to the return of `chip_poll()`.
- `chip_poll()` covers the SPI read and `hakios_touch_post`. That calls the NativeHidInput sink, which does a synchronous `runAction` → `handleReport` on the input work loop. So the number covers interrupt → report handed to IOHID.
- A second line logs `irqs, spurious`. Divide irqs by elapsed time to get the report rate.

Not in the patch (proposal):
1. Put two more stamps into `ruan_input_row`: `native_touch_seq` change time and `handleReport` return time. The touch thread → workloop gate hop is then separate from the SPI read time (≈ 65 B at 6 MHz ≈ 0.1 ms + page set).
2. For report → pixels: the live cam (latest.jpg :8090) at 240 fps, with a tap on a high-contrast target. Compare `touch_fast=1` against `touch_irq=1` against `touch_irq=1 touch_active=1`.
3. Expected [I]: first touch drops from ≤ 1 ms + read (tick) to ≈ 50–100 µs + read (IRQ). Tracking becomes one report period plus about 0.2 ms.

## Not done / risks

- Nothing has been compiled. `clear_wait` and `thread_wakeup_prim` are declared `extern` by hand, with XNU's signatures. `armcpu_spi_share` is `weak_import`, as in RuanBacklightSvc.cpp. If ArmCpu lacks it, the log shows `attach result ffffffff` and the code stays on the tick.
- The handler runs in primary IRQ context. It only touches TLMM MMIO, which is mapped before arming.
