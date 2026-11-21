# GPU governor + thermal caps (GPU and CPU)

Patches (apply in `macOS/unixnu-ruan/package` with `patch -p1`, both `--dry-run` clean on 2026-11-21, also together):

- `ruan-gpu-dcvs.patch` → `RuanGpuPower.c`. Boot-args `ruan-gpu-dcvs=1` (needs `ruan-gpu-perf=1`), `ruan-gpu-max-level=N` (default 9).
- `ruan-cpu-thermal.patch` → `RuanCpuFreq.c`. No new boot-arg: it is part of `ruan-cpufreq=3`.

Nothing was built, flashed or run on the device. A host test of the pure parts passed (GPU `dcvs_want`/`dcvs_cap`/`tsens_dc` copied into a scratch test, and the CPU file's own `-DRUAN_CPUFREQ_SELFTEST`, which now also checks `ruan_tsens_dc`). The GPU block was syntax-checked with clang against stubs, not against the XNU headers.

Tags: **[V]** = checked against source or the live DT, **[I]** = inferred, needs a device check.

## Registers

| What | Address / register | Source |
|---|---|---|
| tsens0 TM | 0x0c263000 (DT `thermal-sensor@c263000` reg[0], phandle 0x168); SROT 0x0c222000 (reg[1], not used) | [V] dts:17113 |
| TM_Sn_STATUS | TM + 0xa0 + 4·n: bits 11:0 signed deci-C, bit 21 valid; up to 3 reads for valid | [V] tsens-v2.c via sheng `ThermalProbe.h` |
| GPU sensors | gpuss-0 = sensor 11, gpuss-1 = sensor 12 (tsens0). DT trip `tj_cfg` 95 C, 115 C / 125 C engine/reset | [V] dts:10284-10330 |
| Big-core sensors | cpu-1-0..cpu-1-7 = sensors 3..10 (tsens0); cpu4..7 emerg trips sit on these zones | [V] dts:9964-10245, 27810 |
| GPU busy count | `GPU_GMU_AO_GPU_CX_BUSY_MASK` 0x23b0e = 0xff000000; `GMU_CX_GMU_POWER_COUNTER_SELECT_0` 0x1f841 low byte = 0x20; `..._ENABLE` 0x1f840 = 1; `XOCLK_0_L/H` 0x1f844/0x1f845 (19.2 MHz ticks while busy; read hi, lo, hi) | [V] KGSL `adreno_gen7.c:476-480`, `gen7_reg.h` |
| GPU DCVS vote | HFI msg 30 `{1, perf idx, bus idx}`, bus idx = the level's `qcom,bus-freq-ddr7` | [V] gpu-perf.md §5 |

GMU dword offsets go through the existing `GMU_REG()` into `va[W_GMU]`, both inside the 0x37000 window [V: arithmetic]. The governor writes them directly (as `s3_gx_on` does for OOB), not through `wr()`, whose allow-list would refuse them.

## GPU governor (`ruan-gpu-dcvs=1`)

- Starts at the end of `ruan_gpu_run` for modes 16/17/18 (the only ones that keep `va[]` mapped), when GX is on and the perf tables went in (`perf_level`, set from `ruan-gpu-level`).
- Thread (priority 80, as `glue_late`), every `delay(16000)`: busy ‰ = busy ticks × 52.08 ns / elapsed. `need = cur kHz × busy / 0.6`; want = lowest level ≥ need in [3, ceiling]. Up at once; down one level after 4 windows in a row that want less. Each change = msg 30.
- Thermal ceiling: hottest of sensors 11/12. Over 85 C → L6 (734), over 95 C → L3 (500); each lifts 5 C lower (80 / 90 C). No valid reading → L6. A ceiling below the current level drops at once.
- 3 failed votes in a row stop the governor (an HFI timeout poisons the session anyway).
- **Locking:** no HFI lock existed. Instead `ruan_gpu_run` calls `dcvs_stop_join()` first (before `trace_fn` is set, before any HFI send, unmap or `gpu off`) and waits up to 2 s for the thread to exit (one vote is at most 500 × 100 µs). It restarts the governor at the end, at the last voted level (`ruan_gpu_dcvs[1]`). So the governor never runs alongside another HFI sender or a window unmap. The Adreno glue (17/18) does not use HFI [V: grep].
- Diagnostics: `volatile int32_t ruan_gpu_dcvs[9]` = running, level, thermal ceiling, changes, failed votes, busy ‰ (1 s average), GPU deci-C (updated about once a second), seconds, windows with no valid tsens. `kprintf` on start, ceiling changes and stop.

## CPU thermal cap (`ruan-cpufreq=3`)

Each 16 ms window reads sensors 3..10. Over 90 C → domain 1 (A78) is held at its middle level `(n-1)/2` (with the usual LUT, about 1.5–1.8 GHz) [I], at once, during a touch boost too. It lifts below 85 C. No valid reading → no cap. `ruan_cpufreq_temp_dc` (hottest, deci-C) and `ruan_cpufreq_capped` (windows capped) are global for diagnostics; `printf` on cap on/off. The L3 follows the capped clock through the existing `frac`.

## Inferred / unchecked

- **tsens TM readable from EL1 under Gunyah [I].** Android's tsens driver reads the same block from the HLOS VM, so it is very likely mapped. If not, the first read aborts (SError/SEA panic). To test: boot with one patch at a time, GPU first. A `tm` map failure is logged ("tsens MAP FAILED") and only then disables the cap.
- Sensor numbering = the DT `thermal-sensors` cells, assuming the tsens hardware channel = DT cell, as Linux does [V for Linux; I for ruan's controller enable mask, since SROT 0x4 is not checked]. An unused sensor reads valid=0 and is skipped.
- Busy counter: KGSL programs it in `gen7_start`. Nothing in this tree programmed it before [V: grep]. Whether the GMU firmware reprograms SELECT_0 itself is not known [I].
- mach_absolute_time runs at 19.2 MHz on QC, but the code converts through `absolutetime_to_nanoseconds`, so it does not matter.
- `gpu off` and a later re-power keep the old `gmu_hfi_started`, so no tables or boot vote are resent (an existing quirk); the governor would then resume at its last level.
- Thresholds 85/95 C (GPU) and 90/85 C (CPU) are the requested values, below the DT's 95 C `tj_cfg` and 115 C engine trips.

## First boot checklist

1. Boot-args `ruan-gpu-perf=1 ruan-gpu-level=6 ruan-gpu-dcvs=1 ruan-gpu-max-level=7` (keep 8/9 off until the speed bin is read, gpu-perf.md §4).
2. Look for `ruan-gpu: dcvs on, L6, floor 3 ceiling 7, tsens mapped`. No line → the mode is not 16/17/18, or perf tables fell back (`gpu perf:` lines).
3. Read `ruan_gpu_dcvs[]` (USB link / debugger): [6] should be 300–600 deci-C at idle and change under load; [8] should stay 0. [5] should rise when rendering; [1] should follow it; [4] = 0.
4. If [5] stays 0 under load, the busy counter is not counting: check 0x1f840/0x1f841/0x23b0e readback.
5. Thermal: a sustained GPU load should show the ceiling change line at 85 C.
6. CPU: `ruan-cpufreq=3` alone; read `ruan_cpufreq_temp_dc` (30–60 C idle = 300–600). Under a long CPU load, check for `d1 thermal cap on`.
