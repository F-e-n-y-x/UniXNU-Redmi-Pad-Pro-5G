# ruan panel 60 → 120 Hz under iPadOS (2026-11-21)

Research and drafts only. Nothing was built, flashed or run on the tablet. **[V]** means read from a file or source, **[I]** means inferred.
Draft code: `RuanPanel120.c` in this folder. Its arithmetic self-test passes on the host (`cc -DRUAN_PANEL120_SELFTEST`).

## 1. Answer

**Option (a), and simpler than expected: change only the vertical front porch.** No DCS command, no new PHY/PLL bit clock, no Gunyah
clock call. The panel has a single timing node, 60 Hz with a huge VFP (2784 lines). Android gets every rate in
`qcom,dsi-supported-dfps-list = <120 90 60 50 48 30>` from `qcom,mdss-dsi-pan-fps-update = "dfps_immediate_porch_mode_vfp"`: it keeps
the clock, keeps every other porch, and recomputes only VFP **[V: live DT :20758-20760; dsi_display.c
`dsi_display_dfps_calc_front_porch`]**. At 120 Hz VFP = 26 lines and vtotal = 2758, exactly half of 5516.

## 2. Panel modes (live DT `hardware/private/ruan-live.dts` :20742, `qcom,mdss_dsi_n83_35_02_0a_wqxga_video_cphy`) [V]

Active panel: bootargs `msm_drm.dsi_display0=qcom,mdss_dsi_n83_35_02_0a_wqxga_video_cphy`, "xiaomi n83 35 02 0a cphy nt36532 boe". The
CSOT twin `n83_42_02_0b` (:20850) has identical timings; only its cabc-off command differs. UEFI's
`macOS/uefi/Binaries/ruan/RawFiles/Panel_N83_35_02_0a_wqxga_video_cphy.xml` has the same numbers and `DSIRefreshRate 0x3C0000` (60).

| | 60 Hz (DT timing@0 = UEFI) | 120 Hz (dfps VFP) |
|---|---|---|
| H active / FP / BP / PW | 1600 / 30 / 20 / 10 → htotal 1660 | same |
| V active / FP / BP / PW | 2560 / **2784** / 170 / 2 → vtotal **5516** | 2560 / **26** / 170 / 2 → vtotal **2758** |
| Pixel rate (uncompressed) | 1660×5516×60 = 549.39 MHz | 1660×2758×120 = 549.39 MHz (same) |
| Line time | 3.0215 µs | 3.0215 µs (same) |
| Frame | 16.67 ms | 8.33 ms |
| `qcom,mdss-dsi-panel-clockrate` | 0x29020c00 = 688 000 000 | same (one node, no per-mode clock) |
| Link | C-PHY, 3 trios (lane-0..2), non_burst_sync_event, DSC 1.2, 10 bpc, 8 bpp, slice 800×20, 2 slices/pkt, topology 1/1/1 | same |
| PHY timings | `[00 00 00 00 22 20 07 19 07 02 04 00 00 00]` | same |

(90 Hz: vtotal 3677.3, so not a whole number of lines. Android rounds it. 48: VFP 4163. 30: VFP 8300.)

**Bit clock does not change.** Throughput per line is unchanged: DSC 549.39 M × 8 bpp = 4.395 Gb/s, which is 1.465 Gb/s per trio, which is
~641 Msym/s per trio. Both the DT's 688 MHz and UEFI's timing-derived clock cover it **[V arithmetic, I: UEFI derives the clock from the
timing because the xml has no bit-clock entry]**. Only blank lines are dropped. Do not touch `0xae94400/0xae94900` (PHY/PLL) or disp_cc.

DCS: no `timing-switch-command` and no per-fps panel command beyond the pen-sync pair. `pen-120hz` = `pen-60hz` =
`39 … ff 10 / b2 91 / b3 00`. That is the same B2/B3 the on-command already sends on page 10 (decoded tail of `qcom,mdss-dsi-on-command`:
`ff 10, fb 01, 51 0f ff, 53 2c, 55 00, 3b 03 ac 1a 04 04 00, 90 13, 91 …, 92 10 e0, 9d 01, b2 91, b3 00, 11 (120 ms), 29`). So the panel
is already in the 60/120 pen-sync state. **[V DT; I: the NT36532 TDDI follows the host's VS packets in video mode, as Android's
porch-only switch implies]**

## 3. Register sequence

Bases [V, live DT :20968, :17728 sde offsets]: DSI0 ctrl `0x0ae94000` (dsi-ctrl-hw-v2.6), INTF1 `0x0ae36000`, CTL0 `0x0ae16000`.
Offsets [V, downstream techpack `dsi_ctrl_reg.h`, `sde_hw_intf.c`, `sde_hw_ctl.c` (Xiaomi marble tree)]:

| Reg | Addr | 60 Hz now (expected) | 120 Hz |
|---|---|---|---|
| DSI_VIDEO_MODE_ACTIVE_V | 0xae94028 | end 2732 << 16 \| start 172 | unchanged (check only) |
| DSI_VIDEO_MODE_TOTAL | 0xae9402c | (5515 << 16) \| (htot_dsi−1) | **(2757 << 16)** \| same low half |
| DSI_DSI_TIMING_DB_MODE | 0xae941e8 | ? | 1 (double buffer) |
| DSI_DSI_TIMING_FLUSH | 0xae941e4 | – | 1 |
| INTF_HSYNC_CTL | 0xae36008 | (hsync_period << 16) \| hpw | unchanged |
| INTF_VSYNC_PERIOD_F0 | 0xae3600c | 5516 × hsync_period (pixels, not lines) | **2758 × hsync_period** (= old / 2) |
| INTF_DISPLAY_V_END_F0 | 0xae36024 | (2732 × hsync_period) − 1 + skew | unchanged (it does not depend on VFP) |
| INTF_CONFIG bit 31 / PROG_FETCH_START | 0xae36004 / 0xae36170 | if bit 31 is set: (5516 − N) × hp + 1 | minus 2758 × hp (same N lines before the end) |
| CTL_INTF_FLUSH | 0xae16110 | | \|= BIT(1) (INTF_1) |
| CTL_FLUSH | 0xae16018 | | \|= BIT(31) (INTF) |

Sequence (`rp_write` in the draft), modelled on Android (`dsi_ctrl_async_timing_update`: `set_timing_db(true)`,
`set_video_timing`, which ends with `TIMING_FLUSH=1` [V]; the SDE side `setup_timing_gen` and an INTF flush through CTL [V code, I that
dfps takes this path]):
1. Check that what UEFI programmed matches the table. Otherwise write nothing.
2. Wait until `INTF_LINE_COUNT` (0xae360b0) is in 300..2400. Then, with interrupts off: DSI DB_MODE=1, DSI TOTAL, DSI TIMING_FLUSH=1,
   INTF VSYNC_PERIOD_F0 (+ PROG_FETCH_START), CTL_INTF_FLUSH|=2, CTL_FLUSH|=1<<31. Both blocks latch at the next frame boundary, so
   the current frame still ends at 5516 lines. sde's own comment says the bootloader's INTF is "single buffered" until an INTF
   flush, which is why the flush is needed [V comment, I semantics].
3. Wait 100 ms, then count INTF1 frames for 500 ms. 110-130 Hz means OK. Anything else: write the old values back the same way.

DSC needs nothing: it is per line and the line rate is unchanged. Because only the v-fields are rewritten, the DSC-compressed horizontal
numbers never need to be recomputed.

## 4. DCS over DSI while video mode runs (not needed for 120 Hz; reference)

Proven path: the TPG command FIFO, no DMA buffer and no SMMU: `refs/UniXNU-amethyst/scripts/log/AmethystPanel.cpp` `sendPacket` (DSI0
at the same 0xae94000, ctrl v2.8; same registers in downstream v2.2+). TRIG_CTRL 0x084 = (x & ~(BIT16|0xf)) | 4 (sw trigger). Schedule
it in the VFP: 0x100 = BIT28 | (ACTIVE_V.end + 1). Then CTRL 0x004 |= BIT2 (cmd engine), INT_CTRL 0x110 clear bit 0, TPG_CTRL 0x15c =
BIT1|BIT2|3<<16, the packet words to 0x17c, DMA_CTRL 0x03c |= BIT28 (BIT26 = LP), DMA_LENGTH 0x04c = bytes, SW_TRIGGER 0x090 = 1,
poll INT_CTRL bit 0. Then reset: TPG_CTRL 0, FIFO reset 0x1ec 1→0, schedule 0x100/0x104 = 0. Header word for a long write:
`0xc0000000 | 0x39<<16 | len` (short 0x05/0x15: `0x80000000 | type<<16 | p1<<8 | cmd`). Each DT entry `39 01 00 00 00 00 02 b2 91`
becomes header `0xc0390002` plus payload `0x000091b2`. Note: with 26-line VFP at 120 Hz the window is 78 µs, so long sequences
should be sent before the switch. [V source; I: v2.6 = v2.8 for these registers. AmethystPanel is ruan-untested.]

## 5. Advertising 120 Hz to iOS

- Boot args: `ruan-120hz=1 vsync=120 vsync-fixed=1` (in place of `vsync=60`, `board.toml:116`). GopFramebuffer publishes
  IOMFBDisplayRefresh when hz>60 and `vsync-fixed=1` pins min = max interval to 1/120 s, so CA cannot pick 24 Hz
  (`NativeLegacyDisplay.cpp:582-600`, `gopfb-zzzzz-ruan-pacing.patch`) [V].
- `RuanPanel120.c` overrides the weak `native_legacy_refresh_hz()` (NativeLegacyDisplay.cpp:116). It returns 120 only if the switch was
  measured, otherwise ≤60. A refused switch then never makes the software vsync tick, which drives the 1 s re-kick and pacing, run at
  120 against a 60 Hz panel.
- Integration (not applied):
  - `package/LegacyDisplayProbe.cpp`: `extern "C" void ruan_panel_set_120hz(void);` and call it as the first line of
    `native_legacy_display_late_probe()`. That is before the display registers, which is when it reads the refresh.
  - `scripts/build-ram-root-probe.py` next to :189: `compile_like('osfmk/DEVELOPMENT/startup.o.json', DEVICE / 'package/RuanPanel120.c', OUT / 'RuanPanel120.o')`.
    Copy the file into `package/`.
- The DPU direct-scanout latch (`gopfb-zzzz-ruan-dpu.patch`, 50 ms fallback, CTL flush VIG0 bit) is rate-agnostic. It just latches every 8.3 ms.

## 6. Verify

- USB `dpu` (exists): `intf1 frames a -> b in 100 ms` should read ~12, not ~6.
- New `ruan_panel120_stats[16]` in USB `display` or a new `p120` row. [0]: 0x120 OK, 0x60 dry-run OK, −10 map, −11 DSI not on in video
  mode, −12 not ~60 Hz before, −21..−26 = rp_compute −1..−6 (−21: VSYNC_PERIOD ≠ 5516 lines, −22: DSI TOTAL, −23: ACTIVE_V), −13
  V_END, −30 no write window, −31 did not reach 110-130 Hz and was reverted. [7]/[8] mHz before/after, [12]/[13] underrun count,
  [14] DSI error OR.
- Camera (iphone.jpg :8091 / latest.jpg :8090): the picture must stay stable, and scrolling should look smoother.
- First run `ruan-120hz=2` (reads and checks only) to confirm the expected numbers with zero writes.

### Read-only probe first (proposed `dsi` command for `package/UsbDebugCdc.cpp`, next to `"dpu"`; not applied)
```cpp
} else if (strEq(raw, "dsi")) {
    // Read-only: DSI0 video timing + INTF1 timing engine (docs/research/120hz/plan.md §3). No writes.
    static OSSharedPtr<IOMemoryMap> dsiMap, intfMap;
    static uintptr_t dsi, intf;
    if (!dsi) dsi = mapRangeUncached(0xae94000, 0x1000, dsiMap);
    if (!intf) intf = mapRangeUncached(0xae36000, 0x1000, intfMap);
    if (!dsi || !intf) { s.reply.writeStr("[usb] DSI: map failed\r\n"); } else {
        auto r = [](uintptr_t a) { return *reinterpret_cast<volatile uint32_t *>(a); };
        snprintf(buf, sizeof(buf), "[usb] DSI: hw=%08x ctrl=%08x st=%08x vmctl=%08x ah=%08x av=%08x tot=%08x hs=%08x vs=%08x vpos=%08x "
                 "db=%08x ack=%08x to=%08x phy=%08x clk=%08x\r\n", r(dsi), r(dsi + 4), r(dsi + 8), r(dsi + 0x10), r(dsi + 0x24),
                 r(dsi + 0x28), r(dsi + 0x2c), r(dsi + 0x30), r(dsi + 0x34), r(dsi + 0x38), r(dsi + 0x1e8), r(dsi + 0x68),
                 r(dsi + 0xc0), r(dsi + 0xb4), r(dsi + 0x120));
        s.reply.writeStr(buf);
        snprintf(buf, sizeof(buf), "[usb] INTF1: en0=%08x en6c=%08x cfg=%08x hsync=%08x vper=%08x vpw=%08x vst=%08x vend=%08x "
                 "dhctl=%08x cfg2=%08x fetch=%08x underrun=%08x status=%08x\r\n", r(intf), r(intf + 0x6c), r(intf + 4), r(intf + 8),
                 r(intf + 0xc), r(intf + 0x14), r(intf + 0x1c), r(intf + 0x24), r(intf + 0x3c), r(intf + 0x60), r(intf + 0x170),
                 r(intf + 0x268), r(intf + 0x26c));
        s.reply.writeStr(buf);
        uint32_t f0 = r(intf + 0xac), maxLine = 0;
        for (int i = 0; i < 20000; i++) { uint32_t l = r(intf + 0xb0); if (l > maxLine) maxLine = l; IODelay(50); }   // ~1 s
        snprintf(buf, sizeof(buf), "[usb] INTF1: %u frames in ~1 s, max line %u (vtotal 5516 at 60 Hz, 2758 at 120)\r\n",
                 r(intf + 0xac) - f0, maxLine);
        s.reply.writeStr(buf);
    }
```
Expected at 60 Hz: `tot` upper half 0x158b (5515), `av` 0x0aac00ac, `vper / (hsync>>16)` = 5516, ~60 frames, max line ~5515.
INTF_TIMING_ENGINE_EN is 0x000 in downstream SDE and 0x06c in upstream DPU naming (the sheng probe uses 0x6c), so both are read.

## 7. Risks

- **Black or garbled screen: the only recovery is a reboot** (power button hold / fastboot boot again). Nothing persistent is written,
  so a normal power-on brings back UEFI's 60 Hz. Under the AGENTS.md rules the run itself needs the ROM session's / user's OK.
- Writes go only to DSI0 ctrl, INTF1 and CTL0 registers that HLOS already writes for scanout (VIG0/CTL0 flip works). PHY, PLL, disp_cc
  and SMMU are untouched, so Gunyah should not object [I]. DSI ctrl registers have not been written from XNU on ruan yet (only read via
  ADB/DT) [V].
- Latch semantics [I]: if the INTF registers are not double-buffered on this SDE (parrot, version unknown, read MDP+0), the VSYNC_PERIOD
  write applies at once. It is written mid-active, below the new 2758 total, so the current frame simply ends early: one short frame,
  no wrap. If DSI DB mode misbehaves, DSI and INTF disagree for one frame and may raise a DSI error. Self-check, then revert.
- Panel side [I]: Android uses exactly VFP 26 at 120 Hz on this panel, but Xiaomi's closed `mi_dsi` code might send extra commands at
  fps changes (not in the DT). If the picture rolls or shows a band, send the `pen-120hz` triple through §4 and re-test.
- Touch (NT36532 TDDI, `RuanTouch.c`): the touch scan rate is tied to the display frame on TDDI. Touch may report faster or change
  timing. Check `ruan_touch_events` and the touch feel.
- Power and heat: twice the scanout bandwidth and twice the composition. The GPU/CPU governors' thermal caps already apply.
- The frame counter at 0xac must be enabled (`INTF_FRAME_LINE_COUNT_EN` 0xa8 = 3). The existing `dpu` probe already sees it move [V,
  display-fps-plan / pacing patch "INTF1 counts 60 frames/s"].
