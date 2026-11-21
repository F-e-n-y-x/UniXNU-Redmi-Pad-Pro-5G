# 120 Hz: DSI errors, Android's dfps and the prog-fetch line (rbu174u / rbu176u)

2026-11-21. Research + code, not built, not run. [V] = read in a source, the live DT or a device log; [I] = inferred.
Sources: LineageOS `android_kernel_xiaomi_sm7435-modules` lineage-24.0 (`display-drivers/msm/dsi/dsi_ctrl_hw_cmn.c`, `dsi_ctrl_reg.h`,
`dsi_ctrl.c`, `dsi_display.c`, `sde/sde_encoder_phys_vid.c`, `sde_hw_intf.c`, `sde_hw_catalog.c`, `sde_encoder.c`), mainline
`drivers/gpu/drm/msm/registers/display/dsi.xml` (its offsets are the downstream ones minus 4), live DT `ruan-live.dts` :20742.

## 1. The DSI errors

rbu176u read after the 1 s check, before the revert: `fifo 0x1ccc1219 phy 0x88888 tmo 0`, `st 0x8`. All are sticky. They had never
been read before the switch, so they may date from UEFI.

| Register | Bits | Meaning |
|---|---|---|
| DSI_STATUS 0x008 = 0x8 | 3 | VIDEO_MODE_ENGINE_BUSY. This is normal [V dsi.xml STATUS0] |
| DLN0_PHY_ERR 0x0b4 = 0x88888 | 3, 7, 11, 15, 19 | Linux and dsi.xml only name bits 0/4/8/12/16 (ESC, SYNC_ESC, CONTROL, LP0/LP1 contention) for lane 0 [V]. The +3 pattern fits the same five errors on **data lane 3**, which C-PHY does not use (DT enables only lane-0/1/2-state) [I]. Not a link fault |
| TIMEOUT 0x0c0 = 0 | none | No HS/LP/BTA timeout [V] |
| ACK_ERR 0x068 | inside 0x88888 | The OR equals phy, so ACK_ERR adds no new bits [V arithmetic] |
| FIFO_STATUS 0x00c = 0x1ccc1219 | 0, 3 | **VIDEO_MDP_FIFO_OVERFLOW + UNDERFLOW**: the MDP→DSI pixel FIFO ran dry, which matches the INTF1 underrun [V bits] |
| | 18/19, 22/23, 26/27 | HS FIFO overflow+underflow on lanes 0-2 (Linux reports them as errors) [V] |
| | 28 | DLN3_HS_FIFO_EMPTY: lane 3 is idle, which fits the lane-3 reading of PHY_ERR [V bit, I meaning] |
| | 9, 12 | DMA write watermark and DLN0 LP FIFO empty, both status only. Bit 4 is undocumented |

**Verdict [I]:** this is not a C-PHY lane error and not DSC. The DSI side shows the MDP starving it (the same event as the INTF underrun).
The code now saves FIFO_STATUS/PHY_ERR before the switch ([32]/[33], `fifo0`/`phy0`) and clears them by writing the value back, as
mainline `dsi_fifo_status` does. After that, `fifo`/`phy` show only what the switch caused.

## 2. Android's 120 Hz on n83 [V DT + source]

- One timing node at 60 Hz. `dfps_immediate_porch_mode_vfp`, list `<120 90 60 50 48 30>`, `panel-clockrate` 688 MHz, no
  `timing-switch-command`, no per-mode DSC. `pen-120hz` = `pen-60hz` (`ff 10 / b2 91 / b3 00`, already sent by on-command). Xiaomi's
  `sde_encoder.c` sends fps-tied DCS only for M16T/N16/M80 panels, not n83. So 120 Hz is porch-only. No DCS, no new byte clock, same DSC
  slices and RC.
- DSI: `dsi_display_set_mode` → `set_timing_db(true)` → `dsi_ctrl_async_timing_update` → `set_video_timing` (rewrites the same
  ACTIVE_H/V, TOTAL, HSYNC, VSYNC, VPOS, then TIMING_FLUSH). That is what we do.
- INTF: `setup_timing_engine` + `programmable_fetch_config`. Video mode has no TE (`mdp-trigger none`).

## 3. What was wrong: the prog-fetch line

`programmable_fetch_config` programs `(vtotal - N) * htotal + 1` **plus one htotal when `delay_prg_fetch_start`** is set, "to start
from the second line after the active region". **Parrot sets it** (`sde_hw_catalog.c` IS_PARROT_TARGET) [V]. UEFI follows this rule
at 60 Hz: 0x18c56b = line 2733 = N 2783 of a 2784-line porch [V].

rbu174u/176u capped N at the whole 26-line porch: `(2758 - 26) * 594 + 1` = **line 2732, the first line after the active area**. The
rule forbids exactly that line, and every frame then underran at the first active line [V value; I cause]. The bandwidth vote did not
help (rbu176u), which fits a timing cause rather than a bandwidth one.

With the rule applied (N ≤ vfp − 1 = 25), the 120 Hz value is **0x18c56b, UEFI's own register value, unchanged**. Android's own choice
is N = 1 (line 2757: prefill is VPW+VBP, 172 lines, since `min_prefill_lines` 40 × 2 = 80 ≤ 172).

Not the cause [V/I]: byte clock (unchanged), DSC params (independent of VFP), TE (video mode), flush order (same as Android: DSI DB +
flush, then INTF + CTL flush, both latch at the frame boundary; vtotal 5516 = 2 × 2758 keeps them in phase even if one latches early).

## 4. Code (`package/RuanPanel120.c`, `UsbDebugCdc.cpp`; UniXNU copies identical)

- `rp_compute(..., fetch_lines, ...)`: caps N at vfp − 1, which gives 0x18c56b. `ruan-120hz-fetch=N` (1..25) overrides it, and
  `=1` gives Android's line 2757. An N outside that range gives result −27.
- Stats grow to 36: [32] FIFO_STATUS before, [33] DLN0_PHY_ERR before, [34] fetch lines used, [35] DSI TIMING_DB_MODE before. USB
  `panel120` line 2 adds `fifo0 phy0 fetch db`.
- Self-test (`cc -DRUAN_PANEL120_SELFTEST`) passes: 120 Hz → 0x18c56b, `fetch=1` → 2757 × 594 + 1, `fetch=26` → −7, 60 Hz → UEFI's
  value. The kernel path passes `-fsyntax-only` against stubs only.

## 5. Test order

1. `ruan-120hz=1` (default IB vote): expect `fetch 25`, `prog 0x18c56b`, result 0x120, no `intr` bit 26, `fifo` without bits 0/3.
2. If it still underruns: `ruan-120hz-fetch=1` (Android's line), then `ruan-120hz-ib=0` to see whether the vote is needed at all.
3. If `phy0` is already 0x88888 before the switch, the lane-3 reading is confirmed.
