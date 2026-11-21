# 120 Hz: INTF1 underrun and the garbled top of the screen (rbu174u)

2026-11-21. Research + draft code, not built, not run. [V] = read in a source or the live DT, [I] = inferred.

Sources: live DT `ruan-live.dts` (sde node :17587-17700, dispcc :6650, interconnects :7086-7180, rsc :5113/:6089, sde_rscc :21130);
LineageOS `android_kernel_xiaomi_sm7435` (`drivers/clk/qcom/dispcc-parrot.c`, `clk-alpha-pll.c`, `drivers/interconnect/qcom/parrot.c`)
and `android_kernel_xiaomi_sm7435-modules` (`display-drivers/msm/sde/*`, `dsi/*`), branch lineage-24.0.

## 0. What the rbu174u numbers mean

- `underrun -1->102199`: INTF_UNDERRUN_COUNT (INTF+0x268) is not a counter. Linux reads it as the vsync counter value of the last
  underrun, in pixel clocks, and divides by htotal (`sde_hw_intf_get_underrun_line_count`) [V]. -1 = no underrun yet. 102199 / 594 =
  **line 172.05 = VPW 2 + VBP 170: the first active line, pixel ~30** [I]. So the pipe has no data when the active area starts. A
  stuck pipe that catches up over the next lines would explain "top quarter garbled, rest fine" [I].
- So "counter stays flat" is not a valid test: the value stays at 102199 even if every frame underruns at the same pixel. The draft
  uses the raw MDP top interrupt bit instead (INTR_STATUS 0xae01014 bit 26 = INTF_1 underrun, clear 0xae01018; `sde_hwio.h`,
  `sde_hw_interrupts.c`) [V]. Linux masks the status with its enable bits in software, so the raw bit should latch while
  disabled [I].
- `dsi_err 0x88888` is ACK_ERR | TIMEOUT | DLN0_PHY_ERR ORed together. Linux decodes DLN0_PHY_ERR bits 0/4/8/12/16 and TIMEOUT bits
  0/4/8, not bit 3/7/11/15/19 [V]. Unknown which register set them, or whether they were set before the switch. The draft now
  stores DLN0_PHY_ERR, TIMEOUT and DSI_FIFO_STATUS (0x00c, has the MDP->DSI FIFO underflow bits) separately in [29..31].

## 1. What Android sets for 120 Hz

**Porch dfps does not raise the peak rate.** At 60 and at 120 Hz the line time is the same (vtotal 5516 at 60 Hz, 2758 at 120 Hz):
1600 px x 4 B per 3.02 us = **2.12 GB/s peak, and pixel rate 1600 x 2758 x 120 = 529.5 Mpx/s, the same in both modes** [V arithmetic].
Only the average doubles: 1600x2560x4x120 = 1.97 GB/s (0.98 GB/s at 60 Hz). The GPU's share also doubles because it renders
twice as many frames.

- MDP core clock (`disp_cc_mdss_mdp_clk_src`) [V dispcc-parrot.c]: 200 / 325 / 380 / 506 / 608 MHz, all **PLL0 / 3**
  (hid 5), CLK_SET_RATE_PARENT, so the rate is set by moving PLL0 (600/975/1140/1518/1824 MHz). CX corner: 380 LOW_L1, 506 NOM,
  608 HIGH (PLL0 1824 MHz is above NOM's 1800). DT `clock-max-rate` core_clk = 608 MHz [V]. The SDM HAL picks the rate, not the
  kernel (no plane clock calculation in this sde_plane.c) [V]. 529.5 Mpx/s at 1 px/clk needs more than 506, so Android probably runs
  608 at both 60 and 120 Hz [I]. UEFI must already be fast enough for 60 Hz, and 60 Hz scans the same line rate, so **the MDP clock
  probably is not what 120 Hz is short of** [I]. Read it before changing it (section 4).
- DSI byte/pixel clocks: unchanged for porch dfps (one timing node, 688 MHz C-PHY, plan.md) [V].
- Bandwidth [V DT]: paths `qcom,sde-data-bus0` = mmss_noc MASTER_MDP -> gem_noc SLAVE_LLCC, `qcom,sde-ebi-bus` = mc_virt LLCC -> EBI1,
  reg bus. The RSC path (sde_rscc) is MDP_DISP -> EBI1_DISP on the display voter (disp_rsc 0xaf20000, no active TCS). Floors:
  `qcom,sde-ib-bw-vote` <2.5 GB/s core, 0 llcc, 1.6 GB/s dram>, `sde-min-core-ib-kbps` 2.5 GB/s, `sde-min-dram-ib-kbps` 0.8 GB/s,
  `sde-max-bw-high-kbps` 8.5 GB/s (LPDDR5) / 7.3 GB/s (LPDDR4X), 2 DRAM channels. BCMs for that path (icc parrot.c): **MM0**
  (qns_mem_noc_hf, 32 B x 2), MM1 (qnm_mdp, keepalive), **SH0** (qns_llcc, 16 x 2), **MC0** (ebi, 4 x 2), **ACV** (ebi, enable_mask
  0x8; disp voter 0x1).
- QoS/CDP [V DT]: `qcom,sde-qos-refresh-rates` <120 240>, so 60 and 120 Hz use the **same** danger/safe/creq LUT row. CDP setting
  <1 1 1 0>. No `default-ot-rd-limit` (RT reads not OT-limited). So Android changes no QoS for 120 Hz.
- Prog fetch [V sde_encoder_phys_vid.c + catalog]: parrot `min_prefill_lines` 40, scaled to max dfps 120 -> 80 lines; VBP+VPW = 172 >=
  80, so Android uses the "always enabled" case: **2 lines** (delay_prg_fetch_start), start = (vtotal-1)*htotal+1. Our 26 lines
  start earlier than Android's, so prefill length alone is not the cause [I]. Under UEFI's 60 Hz mode the fetch started 2783 lines
  (8.4 ms) early, which hides slow memory at the start of a frame. 120 Hz leaves only about 600 us [I].

## 2. What we have for votes

- GPU DDR votes go through the GMU (HFI bw table built from cmd-db SH0/MC0/ACV, `RuanGpuPower.c` ruan_bcm_data). These are not direct
  RPMh writes, so nothing to reuse except the BCM formula.
- Direct RPMh: apps RSC DRV 2 0x17a20000, TCS block +0xd00; `QcomRpmh.c` builds the writes (`RuanWifi.cpp`/`RuanWifiWpss.cpp`
  apply them). TCS 0 RuanTouch, 1 WpssProbe (cx/mx), 2 RuanWifi. tcs-config gives 3 active, 2 sleep (3-4), 2 wake (5-6). **The
  draft uses TCS 5** (a wake TCS used as an active one, as rpmh-rsc does when it has no free active TCS). It clears cmd_enable after [I].
- `RuanCpuFreq.c` is EPSS (L3/CPU) only; no DDR vote.
- **Conflict [I]:** the apps DRV holds one vote per resource. WpssProbe sends cx.lvl/mx.lvl = 0 at handover, which would also
  drop a CX vote made here for the MDP clock (and any CX vote UEFI left on the apps DRV). The same applies to BCMs: our MC0/SH0
  vote replaces UEFI's apps vote, so a revert keeps it instead of sending 0.

## 3. Ranking the causes

1. **DDR/NoC latency or bandwidth at frame start** (the underrun is at the first active pixel; 2x GPU traffic; DDR clock decided by
   whatever UEFI/GMU vote is current): most likely [I]. Test: `ruan-120hz-ib=` votes.
2. MDP clock below the 529.5 Mpx/s line rate: unlikely if 60 Hz is clean, because the line rate is the same. [16] will show it.
3. VIG0 QoS (danger/safe/creq/CDP): Android uses the same row for 60/120, but UEFI's values may differ from the DT ones. Read
   [20..25] and compare: linear RT danger 0x3ffff / 0x3fffff, safe 0xf800 / 0xe000, creq 0x00112234_45566777 (DT; table index
   mapping not decoded).
4. Prefill: 26 lines is already more than Android's 2. A boot arg to try a larger N is not possible (N cannot exceed the 26-line
   porch).
5. The single-buffer case (`fb-fence-cap-ms`, `dpu-retire-flip`) gives torn frames, not an underrun position. Not the cause [I].

## 4. Draft (`package/RuanPanel120.c`, both copies identical) and how to test

`ruan-120hz=1` now: reads -> `rp_perf_up` -> switch -> rate check -> 1 s underrun check -> keep 120 Hz, or write 60 Hz back (`-32`).
- Always (also the `=2` dry run): [16] MDP kHz from PLL0 L (0xaf00010) / ALPHA (0xaf00014) / CFG_RCGR (0xaf080b4), [18] PLL0 MODE
  (0xaf00000), [19] CFG, [20..25] VIG0 (0xae05000) QOS_CTRL 0x6c, DANGER 0x60, SAFE 0x64, CREQ 0x74/0x78, CDP 0x134.
- `ruan-120hz-ib=<KB/s>` (default 3000000, 0 = off): one TCS-5 request MM0/SH0/MC0 (AB 1.97 GB/s, IB as given) + ACV 0x8.
- `ruan-120hz-mdp=380|506` (off by default): only if below that; CX+MX to NOM (level 256) first, then the Lucid EVO L/ALPHA + latch
  (MODE bit 14, ack bit 13, lock bit 31). 608 is refused (needs CX HIGH).
- `ruan-120hz-keep=1`: stay at 120 Hz even with underruns, to look at the screen.
- USB `panel120` prints a second line: `mdp kHz ... perf <rc> intr <INTR_STATUS> upos <pos> fifo/phy/tmo`. perf: bit 0 BCMs voted,
  bit 1 clock raised; -40..-57 = the step that failed (result -40 then, no switch).
- Self-test: `cc -DRUAN_PANEL120_SELFTEST RuanPanel120.c && ./a.out` (MDP kHz readout, PLL L/alpha for 380/506, BCM x/y packing,
  ARC index, 1.97 GB/s): passes. The kernel part passes `-fsyntax-only` against stubs only; it has not been built with XNU flags.

Suggested order: (a) `ruan-120hz=2`: read [16]/[19]/[20..25] at 60 Hz. (b) `=1 ruan-120hz-ib=0 ruan-120hz-keep=1`: does `intr` bit 26 set
at 120 Hz without votes? (The draft does not check 60 Hz. A baseline needs the same clear-and-wait before the switch.) (c) `=1` with the default IB, then 6074218. (d) only then
`ruan-120hz-mdp=506` if [16] < 529 MHz.
