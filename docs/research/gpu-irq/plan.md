# GPU interrupt (SPI 300) for fence waits on ruan

Goal: GPU fence waiters on ruan should sleep instead of spinning (`RuanAdrenoGlue.cpp` `idle()` = `IODelay(20)`).
Draft: `ruan-gpu-irq.patch`. It changes only `RuanAdrenoGlue.cpp` and is gated by boot-arg `ruan-gpu-irq=1` (default off).
Apply it with `patch -p1 -d macOS/unixnu-ruan < ruan-gpu-irq.patch` (`--dry-run` passes). Not built, not booted.
The new code passed `clang++ -fsyntax-only` against stub declarations only.

## 1. Does SPI 300 reach our EL1 kernel under Gunyah?

**Verified**
- In the live DT (`hardware/private/ruan-live.dts`), `qcom,kgsl-3d0@3d00000` has `interrupts = <0 0x12c 4 0 0x11e 4>`. That is `kgsl_3d0_irq` on SPI 300 (level-high) and `freq_limiter_irq` on SPI 286.
- `qcom,gmu@3d69000` has `hfi` on SPI 304 and `gmu` on SPI 305, both level.
- The interrupt parent is `/soc` `interrupt-parent = <0x01>`, which is the `arm,gic-v3` at `0x17200000` (GICD) and `0x17260000` (GICR, stride 0x20000). The GPU is on the GIC directly, not on the PDC.
- Hardware SPIs on the same GIC already reach our XNU under Gunyah:
  - UFS (`ufshc@1d84000`, SPI 265) is attached with `armcpu_spi_attach` in `ShengUfs-src/NativeUfs.cpp`.
  - The rbu138u heartbeat shows it firing: `[hb] ... ufs=34639/0 irq=34637` (`/root/unixnu-work/logs/rbu138u/info-now.txt`).
  - So Gunyah's vGIC forwards the HLOS VM's physical SPIs, and ArmCpu's distributor, routing and EOI path works on ruan.
- The kgsl node sits in the HLOS VM's DT (Gunyah VM id 3) with the same interrupt parent as UFS. Android's KGSL uses this exact line.

**Inferred (not yet seen on ruan)**
- SPI 300 itself fires to us. It is the same mechanism as SPI 265, and sheng (SM8550, also under Gunyah) attaches INTID 332 the same way (`vg/UniXNU-sheng/package/GpuPowerProbe.cpp` `irqAndTimer`).
- Nobody else has claimed SPI 300. `armcpu_spi_attach` returns `kIOReturnExclusiveAccess` if someone has, and the patch logs that result.
- The GMU HFI and GMU SPIs (304 and 305) should be deliverable the same way. They are not needed here: HFI and OOB stay polled in `RuanGpuPower.c`.

## 2. How the port handles interrupts

- The GIC is ArmCpu's `/root/unixnu-work/UniXNU/drivers/ArmCpu/ArmCpu.cpp`. Distributor init disables every SPI.
- `armcpu_spi_attach(intid, edge, handler, ctx, &readback, &router)` does the following:
  - ICENABLER, then IGROUPR group 1, then priority 0xa0;
  - ICFGR level or edge, then IROUTER set to the boot CPU's affinity;
  - ISENABLER last.
  - It also records the handler for ArmCpu's IRQ dispatch, which does the EOI after the handler returns.
- `armcpu_spi_mask(intid, masked)` is safe in an IRQ handler.
- `smp-gic=1` only prepares secondary-core redistributors. It does not change SPI setup. There is nothing more to do for GICD or GICR.
- Callers on ruan today are UFS (SPI 265, proven firing) and `RuanBacklightSvc.cpp` (SPI 602, delivery not checked).
- For SPI 300: INTID 332, `edge=false` (level), routed to the boot CPU (IROUTER = boot affinity, priority 0xa0, group 1).

## 3. GPU side (A7xx registers, dword offsets from 0x3d00000)

| reg | offset | source |
|---|---|---|
| RBBM_INT_0_STATUS | 0x201 | gen7_reg.h, `RuanA710Regs.h`, Adreno740 `NativeAdrenoIrq.cpp` |
| RBBM_INT_CLEAR_CMD | 0x37 | same |
| RBBM_INT_0_MASK | 0x38 | same |
| CP_INTERRUPT_STATUS | 0x823 | detail for bit 9 HWERROR |

- Status bits:
  - bit 20 CP_CACHE_FLUSH_TS: a ring fence was written;
  - bit 17 RB_DONE_TS;
  - bit 8 CP_SW: a preemption switch;
  - errors 1, 6, 7, 9, 22, 23 (HANGDETECT), 24, 25, 28 (`RUAN_A710_INT_ERRORS`).
- **Verified:** `RuanGpuPower.c` s3_cp already writes `RBBM_INT_0_MASK = RUAN_A710_GEN7_INT_MASK` (bits 1,6,7,8,9,15,17,20,22,23,24,25,28), after a full `INT_CLEAR_CMD`.
- **Verified:** every driver fence is `CP_EVENT_WRITE7` with CACHE_FLUSH_TS and IRQ bit 31 (`NativeAdrenoRing.cpp` `RingFenceIrq`). The GPU side is already armed, and only the CPU side has been missing.

## 4. The driver already has the IRQ wake path (no Adreno740 change needed)

- **Verified** in both upstream and the staged ruan copy (`ufs-variant/.../ruan-adreno/src/drivers/Adreno740/NativeAdrenoPublish.cpp`):
  - `native_adreno_gpu_interrupt(status)` (:250) queues `fencePass` on a thread call when bit 20 or bit 8 is set. It also sets `g.interrupts=true`.
  - From then on, `WaitFence` (:779) sleeps with `IOLockSleepDeadline` until the pass wakes it. It arms before it checks, so a completion between check and sleep is not lost.
  - The hang check runs `fencePass(true)` every 500 ms as a backstop for a lost interrupt.
  - `gpu-fence-wake=0` keeps waiters on the board's `clock.idle` pause instead (:1395).
- So the board only has to:
  1. attach SPI 300;
  2. in the handler, read `INT_0_STATUS` and write it to `INT_CLEAR_CMD` before the EOI, so the level line drops;
  3. call `native_adreno_gpu_interrupt(status)`;
  4. make its own `idle()` sleep until the next IRQ (with a timeout).

  This is exactly what sheng's `gpuIrq` and `ringIdle` do.

## 5. The patch (RuanAdrenoGlue.cpp)

- **`idle()`:**
  - Once the line is attached, and the caller may block (interrupts on, not in interrupt context, preemption level 0), it does `assert_wait_timeout(&irqCount, 1 ms)` and then `thread_block`.
  - Otherwise it uses the old `IODelay(20)`. CP init and the zap path run before the attach, so they still spin.
- **`attachIrq()`, run after a successful `native_adreno_publish`:**
  - Only when `ruan-gpu-irq=1`.
  - Maps the first 4 KiB of `0x3d00000` raw with `ml_io_map`. The handler must not go through `RuanGpuPower.c`'s `rd`/`wr`, because they can call the USB trace.
  - Checks that `INT_0_MASK` has bit 20, and rewrites the gen7 mask if it does not.
  - Calls `armcpu_spi_attach(332, level)` and logs `gpu irq: attach .. gicd .. router .. mask .. irqs .. status ..`.
- **`gpuIrq()`:**
  - Reads status and clears it, then `dsb`.
  - Updates the counters, calls `native_adreno_gpu_interrupt(status)`, then `thread_wakeup(&irqCount)`.
- **Counters:** `ruan_gpu_irq[10]`, described in the code. Together with the driver's `native_adreno_fence_wake[8]`, they are what to read after a boot.

## 6. Risks

- **Interrupt storm.** If the line is asserted but the status reads 0 (something we cannot clear), then after 100000 empty IRQs in a row the handler does three things:
  - masks SPI 332;
  - sets `ruan_gpu_irq[8]`;
  - starts a 1 ms thread call that feeds `native_adreno_gpu_interrupt(1<<20)`. The driver's `g.interrupts` stays true, so without this tick its waiters would sleep until the 500 ms periodic pass.

  Not covered: a status bit that re-asserts forever, for example HANGDETECT on a wedged GPU. The driver's hang recovery is the path for that. Watch `ruan_gpu_irq[2]` against the frame rate.
- **Missed wake.** `idle()` is capped at 1 ms. Driver waiters with `gpu-fence-wake=1` rely on arm-before-check plus the 500 ms periodic pass.
  - A lost IRQ would show as `native_adreno_fence_wake[6]` (periodic wakes) climbing, with a stall of up to 500 ms each time.
  - First boot should therefore use `ruan-gpu-irq=1 gpu-fence-wake=0`: the IRQ only shortens the 1 ms sleep, and the worst case is 1 ms late. Then try `gpu-fence-wake=1`.
- **Status races.** `RuanGpuPower.c`'s blit check (around :2627) also reads and clears `INT_0_STATUS`. With the IRQ on, the handler usually clears it first, so that check can miss error bits. The handler ORs the error bits into `ruan_gpu_irq[9]`. If the blit path matters after publish, make it read that word too.
- **GX power.** The handler reads RBBM registers. That is safe while GX is on, and ruan runs without IFPC or GX collapse at runtime. If GX power-down is added later, the handler must check GX first.
- **IRQ to one core.** IROUTER sends every GPU IRQ to the boot CPU. That is fine at frame rates, and UFS does the same.
- **Latency.** sheng measured that IRQ wakes can be late (asteroids T77 and T77i: the spin saw completions within tens of µs, and mean wait was unchanged). The gain is CPU time, not latency.
  - sheng-gpu-irq=1 cut client fence polls from 573185 to 13707 (asteroids T52).
  - On ruan, compare `[cpu wake]` idle time and the `glue`/`gpu` waits with the flag on and off.

## 7. Test (needs the user's OK and the ROM session's per AGENTS.md; nothing done here)

1. Boot with `ruan-gpu-irq=1 gpu-fence-wake=0`. Expect `gpu irq: attach 0 gicd a003 ...` (group 1, enabled, level, priority 0xa0) and `irqs` > 0.
   - `attach 0` with `irqs 0` after desktop load means Gunyah does not deliver SPI 300 (contradicting the inference in §1). Drop the flag.
2. Check `ruan_gpu_irq[6]` (sleeps) against `[7]` (timeouts). Mostly-timeouts means IRQs are not arriving.
3. Boot with `ruan-gpu-irq=1` alone (gpu-fence-wake defaults to 1). Check `native_adreno_fence_wake` [0] interrupts, [3] woken by IRQ, [5] deadline sleeps and [6] periodic. [6] should stay near 0.
