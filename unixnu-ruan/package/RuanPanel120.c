// SPDX-License-Identifier: BSD-2-Clause-Patent
// DRAFT (2026-11-21, not built, not run): switch ruan's panel (n83 NT36532 BOE/CSOT, DSC video mode, C-PHY, DSI0) from 60 to 120 Hz.
// Plan and evidence: macOS/docs/research/120hz/plan.md.
//
// How: the panel's only timing node (live DT :20790) is 60 Hz with a vertical front porch of 2784 lines, and Android reaches 120 Hz
// by "dfps_immediate_porch_mode_vfp": same bit clock, same line time, VFP 2784 -> 26 (vtotal 5516 -> 2758). No DCS command, no
// PHY/PLL change. Here that is two registers: DSI0 VIDEO_MODE_TOTAL (v field) and INTF1 VSYNC_PERIOD_F0, both double-buffered
// (DSI timing DB + TIMING_FLUSH, INTF via CTL0 INTF flush), so they latch on the same frame boundary.
//
// boot-arg ruan-120hz=1 switches; ruan-120hz=2 only reads and checks (no writes). Every value is checked against what UEFI
// programmed (Panel_N83_35_02_0a_wqxga_video_cphy.xml) before anything is written; on any mismatch nothing is written. After the
// write the INTF1 frame counter must read 110-130 Hz, else the old values are written back.
// Call: ruan_panel_set_120hz() at the top of native_legacy_display_late_probe() (before the display registers and reads
// native_legacy_refresh_hz()).

#include <stdint.h>
#ifndef RUAN_PANEL120_SELFTEST
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "QcomRpmh.h"
#endif

// [0] result: 0x120 switched, 0x60 dry run ok, negative = step that refused/failed (see plan.md §6)
// [1] INTF HSYNC_CTL [2] VSYNC_PERIOD_F0 before [3] DSI TOTAL before [4] DSI ACTIVE_V [5] INTF_CONFIG [6] PROG_FETCH_START before
// [7] mHz before [8] mHz after [9] VSYNC_PERIOD_F0 after [10] DSI TOTAL after [11] line at write
// [12] INTF underrun count before [13] after [14] DSI ACK_ERR|TIMEOUT|DLN0_PHY_ERR after (OR) [15] DSI_STATUS after
// [12]/[13] are INTF_UNDERRUN_COUNT raw: Linux reads it as the vsync counter (pixel clocks) of the last underrun and divides by
// htotal (sde_hw_intf_get_underrun_line_count), -1 = none. rbu174u's 102199 / 594 = line 172.05 = VPW 2 + VBP 170: the very first
// active line. [I]
// Perf step (docs/research/120hz/underrun.md): [16] MDP kHz before [17] after [18] PLL0 MODE [19] MDP CFG_RCGR
// [20] VIG0 QOS_CTRL [21] DANGER_LUT [22] SAFE_LUT [23] CREQ_LUT_0 [24] CREQ_LUT_1 [25] CDP_CNTL [26] perf rc (bit 0 BCMs voted,
// bit 1 MDP clock raised, negative = step that failed) [27] MDP INTR_STATUS after the 1 s check [28] underrun pos after the check
// [29] DSI_FIFO_STATUS [30] DSI_DLN0_PHY_ERR [31] DSI_TIMEOUT_STATUS (separately: rbu174u's 0x88888 was the OR of three)
volatile int32_t ruan_panel120_stats[32];

// Hardware (live DT ruan-live.dts :20959 dsi_ctrl reg, sde-intf-off, sde-ctl-off) [V]
#define RP_DSI0  0x0ae94000ULL
#define RP_INTF1 0x0ae36000ULL
#define RP_CTL0  0x0ae16000ULL
// DSI ctrl (downstream dsi_ctrl_reg.h) [V]
#define DSI_CTRL 0x004
#define DSI_STATUS 0x008
#define DSI_ACTIVE_V 0x028
#define DSI_TOTAL 0x02c
#define DSI_ACK_ERR 0x068
#define DSI_DLN0_PHY_ERR 0x0b4
#define DSI_TIMEOUT 0x0c0
#define DSI_TIMING_FLUSH 0x1e4
#define DSI_TIMING_DB_MODE 0x1e8
// INTF (downstream sde_hw_intf.c) [V]
#define INTF_CONFIG 0x004
#define INTF_HSYNC_CTL 0x008
#define INTF_VSYNC_PERIOD_F0 0x00c
#define INTF_DISPLAY_V_END_F0 0x024
#define INTF_FRAME_COUNT 0x0ac
#define INTF_LINE_COUNT 0x0b0
#define INTF_PROG_FETCH_START 0x170
#define INTF_UNDERRUN_COUNT 0x268
// CTL (downstream sde_hw_ctl.c): CTL_FLUSH bit 31 = INTF, CTL_INTF_FLUSH bit 1 = INTF_1 [V]
#define CTL_FLUSH 0x018
#define CTL_INTF_FLUSH 0x110
#define DSI_FIFO_STATUS 0x00c
// Display clock controller (live DT :6650 0xaf00000; dispcc-parrot.c): PLL0 is Lucid EVO (clk-alpha-pll.c: L 0x10, ALPHA 0x14,
// MODE bit 14 latch input, bit 13 latch ack, bit 31 lock). disp_cc_mdss_mdp_clk_src CMD_RCGR 0x80b0, CFG_RCGR 0x80b4, parent map 4
// (src 1 = PLL0), every table entry PLL0 / 3 (hid 5). PLL0 feeds only the MDP RCG (rot uses PLL1). [V]
#define RP_DISPCC 0x0af00000ULL
#define PLL0_MODE 0x000
#define PLL0_L 0x010
#define PLL0_ALPHA 0x014
#define MDP_CMD_RCGR 0x80b0
#define MDP_CFG_RCGR 0x80b4
// MDP top interrupts: sde_hwio.h INTR_STATUS 0x14 / INTR_CLEAR 0x18 at mdp[0].base = sde-off 0x1000; INTF_1 underrun is bit 26
// (sde_hw_interrupts.c). Linux masks the status with its enable bits in software, so the raw bit latches while disabled. [V; raw
// latching I]
#define RP_MDP_TOP 0x0ae01000ULL
#define TOP_INTR_STATUS 0x014
#define TOP_INTR_CLEAR 0x018
#define INTR_INTF1_UNDERRUN (1u << 26)
// VIG0 = sde-sspp-off 0x5000 (= GopFramebuffer's 0xae01000 + 0x4000); QoS registers from sde_hw_sspp.c [V]
#define RP_VIG0 0x0ae05000ULL
#define SSPP_DANGER_LUT 0x60
#define SSPP_SAFE_LUT 0x64
#define SSPP_QOS_CTRL 0x6c
#define SSPP_CREQ_LUT_0 0x74
#define SSPP_CREQ_LUT_1 0x78
#define SSPP_CDP_CNTL 0x134
// Apps RSC DRV 2 (live DT :5113-5121): TCS 0-2 active (RuanTouch 0, RuanWifiWpss 1, RuanWifi 2), 3-4 sleep, 5-6 wake. TCS 5 is
// used as an active one, as rpmh-rsc does with a wake TCS when it has no free active TCS; cmd enable is cleared after. [I]
#define RP_RSC 0x17a20000ULL
#define RP_RSC_TCS 0xd00
#define RP_TCS 5
#define RP_CMD_DB 0x80860000ULL
#define RP_CMD_DB_SIZE 0x20000u

// The panel's 60 Hz mode (DT timing@0 and UEFI xml agree) [V]
#define RP_VACTIVE 2560u
#define RP_VBP 170u
#define RP_VPW 2u
#define RP_VTOTAL60 5516u   // 2560 + 2784 + 170 + 2

struct rp_plan {uint32_t vtotal, vsync_period, dsi_total, prog_fetch;};

// Pure arithmetic (host-tested below): from what is programmed now to the same mode at `hz`, porch-only, as
// dsi_display_dfps_calc_front_porch does: new_vtotal = old_vtotal * 60 / hz. Returns 0 when the input is not the 60 Hz mode
// we expect or the new front porch would be < 1 line.
static int
rp_compute(uint32_t hsync_ctl, uint32_t vsync_period, uint32_t dsi_total, uint32_t dsi_active_v, uint32_t intf_config,
    uint32_t prog_fetch, uint32_t hz, struct rp_plan *out)
{
	uint32_t htotal = hsync_ctl >> 16;
	if (!htotal || vsync_period % htotal || vsync_period / htotal != RP_VTOTAL60) return -1;
	if ((dsi_total >> 16) != RP_VTOTAL60 - 1) return -2;
	if ((dsi_active_v >> 16) != RP_VPW + RP_VBP + RP_VACTIVE) return -3;
	if (!hz || (RP_VTOTAL60 * 60u) % hz) return -4;
	uint32_t vt = RP_VTOTAL60 * 60u / hz;
	if (vt < RP_VPW + RP_VBP + RP_VACTIVE + 1) return -5;
	out->vtotal = vt;
	out->vsync_period = vt * htotal;
	out->dsi_total = ((vt - 1) << 16) | (dsi_total & 0xffffu);
	// Programmable fetch starts N lines before the frame end, inside the front porch. Android caps N at the front porch
	// (sde_encoder_phys_vid programmable_fetch_get_num_lines: actual_vfp_lines = min(v_front_porch, needed)) and programs
	// (vtotal - N) * htotal + 1. UEFI's 60 Hz N is 2783 of a 2784-line porch (rbu172p read 0x18c56b), so at 120 Hz it becomes the
	// whole 26-line porch: keeping N refused the switch (rc -6, rbu172p).
	out->prog_fetch = prog_fetch;
	if (intf_config & (1u << 31)) {
		const uint32_t vfp = vt - (RP_VPW + RP_VBP + RP_VACTIVE);
		if (!prog_fetch || prog_fetch - 1 > RP_VTOTAL60 * htotal) return -6;
		uint32_t lines = (RP_VTOTAL60 * htotal - (prog_fetch - 1)) / htotal;
		if (lines > vfp) lines = vfp;
		out->prog_fetch = (vt - lines) * htotal + 1;
	}
	return 0;
}

// MDP core clock in kHz from PLL0 L/ALPHA (16-bit alpha, 19.2 MHz reference) and the RCG's CFG (src bits 10:8, hid 4:0 = 2*div-1).
// 0 when the RCG is not on TCXO or PLL0.
static uint32_t
rp_mdp_khz(uint32_t l, uint32_t alpha, uint32_t cfg)
{
	const uint32_t src = (cfg >> 8) & 7, hid = cfg & 0x1f;
	uint64_t hz = src == 0 ? 19200000ull : src == 1 ? 19200000ull * (l & 0xffff) + 19200000ull * (alpha & 0xffff) / 65536 : 0;
	if (hid > 1) hz = hz * 2 / (hid + 1);
	return (uint32_t)(hz / 1000);
}

// L and 16-bit alpha for a PLL rate (alpha_pll_round_rate with ALPHA_REG_16BIT_WIDTH, rounded down).
static void
rp_pll_la(uint64_t hz, uint32_t *l, uint32_t *alpha)
{
	*l = (uint32_t)(hz / 19200000u);
	*alpha = (uint32_t)((hz % 19200000u) * 65536u / 19200000u);
}

// BCM_TCS_CMD (commit, valid, x = AB, y = IB) as icc-rpmh bcm_aggregate computes it for a one-node BCM: AB per channel, both scaled
// by the BCM's width / the node's buswidth, then to the BCM's unit (cmd-db aux: u32 unit, u16 width). Saturates at 14 bits.
static uint32_t
rp_bcm(uint32_t unit, uint32_t width, uint32_t buswidth, uint32_t channels, uint32_t ab_kbps, uint32_t ib_kbps)
{
	if (!unit || !buswidth || !channels) return 0;
	uint64_t x = (uint64_t)ab_kbps * width / ((uint64_t)buswidth * channels) * 1000u / unit;
	uint64_t y = (uint64_t)ib_kbps * width / buswidth * 1000u / unit;
	if (x > 0x3fff) x = 0x3fff;
	if (y > 0x3fff) y = 0x3fff;
	return 1u << 30 | 1u << 29 | (uint32_t)x << 14 | (uint32_t)y;
}

// ARC level index for at least `level` from cmd-db aux (u16 levels, zero padded), as qcom_rpmh_level_index: the highest one when
// `level` is above all of them.
static uint32_t
rp_arc_index(const uint8_t *aux, int len, uint32_t level)
{
	uint32_t n = 0;
	while ((int)(2 * n + 1) < len && (n == 0 || (aux[2 * n] | aux[2 * n + 1]))) n++;
	for (uint32_t i = 0; i < n; i++)
		if ((aux[2 * i] | (uint32_t)aux[2 * i + 1] << 8) >= level) return i;
	return n ? n - 1 : 0;
}

#ifdef RUAN_PANEL120_SELFTEST
#include <assert.h>
#include <stdio.h>
int
main(void)
{
	struct rp_plan p;
	// htotal 1660 is the uncompressed figure; the INTF value is the DSC one, the arithmetic does not care.
	assert(rp_compute(1660u << 16 | 10, 1660u * 5516, 5515u << 16 | 1659, 2732u << 16 | 172, 0, 0, 120, &p) == 0);
	assert(p.vtotal == 2758 && p.vsync_period == 1660u * 2758 && p.dsi_total == (2757u << 16 | 1659) && p.prog_fetch == 0);
	assert(p.vtotal - 2732 == 26);                                           // DT dfps VFP at 120 Hz
	assert(rp_compute(1660u << 16, 1660u * 5516, 5515u << 16, 2732u << 16, 1u << 31, 5515u * 1660 + 1, 120, &p) == 0);
	assert(p.prog_fetch == 2757u * 1660 + 1);                                // still one line before the end
	// rbu172p's live values: htotal 594 (INTF HSYNC 0x252000a), PROG_FETCH 0x18c56b = (5516 - 2783) * 594 + 1
	assert(rp_compute(0x252000au, 594u * 5516, 5515u << 16 | 0x251, 2732u << 16 | 0xac, 1u << 31, 0x18c56bu, 120, &p) == 0);
	assert(p.prog_fetch == (2758u - 26) * 594 + 1);                          // capped at the 26-line porch
	assert(rp_compute(1660u << 16, 1660u * 5515, 5515u << 16, 2732u << 16, 0, 0, 120, &p) == -1);   // not UEFI's 60 Hz mode
	assert(rp_compute(1660u << 16, 1660u * 5516, 5515u << 16, 2732u << 16, 0, 0, 60, &p) == 0 && p.vtotal == 5516);
	// MDP clock readout: dispcc-parrot's boot PLL0 config (L 0x1f, alpha 0x4000 = 600 MHz) through hid 5 = 200 MHz
	assert(rp_mdp_khz(0x1f, 0x4000, 1u << 8 | 5) == 200000);
	assert(rp_mdp_khz(95, 0, 1u << 8 | 5) == 608000 && rp_mdp_khz(0, 0, 0) == 19200 && rp_mdp_khz(95, 0, 4u << 8 | 5) == 0);
	uint32_t l, a;
	rp_pll_la(3ull * 506000000, &l, &a);
	assert(l == 79 && a == 0x1000 && rp_mdp_khz(l, a, 1u << 8 | 5) == 506000);
	rp_pll_la(3ull * 380000000, &l, &a);
	assert(l == 59 && a == 0x6000 && rp_mdp_khz(l, a, 1u << 8 | 5) == 380000);
	// BCM: unit 1000000 (hypothetical aux; vote = KB/s * 1000 / unit), width 4 on ebi (buswidth 4, 2 channels): 2 GB/s AB -> x 1000, 3 GB/s IB -> y 3000
	assert(rp_bcm(1000000, 4, 4, 2, 2000000, 3000000) == (1u << 30 | 1u << 29 | 1000u << 14 | 3000u));
	assert((rp_bcm(1, 4, 4, 2, 0, 3000000) & 0x3fff) == 0x3fff && rp_bcm(0, 4, 4, 2, 1, 1) == 0);
	// the scanout's average need at 120 Hz, BGRA linear: 1600 x 2560 x 4 x 120 B/s
	assert(1600ull * 2560 * 4 * 120 / 1000 == 1966080);
	// ARC levels 16 48 64 128 192 256 384, zero padded: NOM (256) is index 5; above all -> the last
	const uint8_t arc[16] = {16, 0, 48, 0, 64, 0, 128, 0, 192, 0, 0, 1, 128, 1, 0, 0};
	assert(rp_arc_index(arc, 16, 256) == 5 && rp_arc_index(arc, 16, 1) == 0 && rp_arc_index(arc, 16, 999) == 6);
	puts("RuanPanel120 selftest ok");
	return 0;
}
#else

static volatile uint32_t *rp_dsi, *rp_intf, *rp_ctl;

static inline uint32_t rd(volatile uint32_t *b, uint32_t o) {return b[o / 4];}
static inline void wr(volatile uint32_t *b, uint32_t o, uint32_t v) {b[o / 4] = v; __asm__ volatile("dsb sy" ::: "memory");}

// INTF1 refresh in mHz from its frame counter over `ms`.
static uint32_t
rp_mhz(uint32_t ms)
{
	uint32_t f0 = rd(rp_intf, INTF_FRAME_COUNT);
	uint64_t t0 = mach_absolute_time(), ns = 0;
	delay_for_interval(ms, NSEC_PER_MSEC);
	uint32_t f1 = rd(rp_intf, INTF_FRAME_COUNT);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	return ns ? (uint32_t)((uint64_t)(f1 - f0) * 1000000000000ull / ns) : 0;
}

// Both writes inside the active area (line 300..2400 of the current frame), far from the frame boundary where both latch.
static int
rp_write(uint32_t dsi_total, uint32_t vsync_period, uint32_t prog_fetch, int prog)
{
	for (int spin = 0; spin < 40000; spin++) {   // <= ~40 ms, more than two 60 Hz frames
		uint32_t line = rd(rp_intf, INTF_LINE_COUNT);
		if (line >= 300 && line <= 2400) {
			boolean_t irq = ml_set_interrupts_enabled(FALSE);
			line = rd(rp_intf, INTF_LINE_COUNT);
			if (line < 300 || line > 2400) {ml_set_interrupts_enabled(irq); continue;}
			wr(rp_dsi, DSI_TIMING_DB_MODE, 1);
			wr(rp_dsi, DSI_TOTAL, dsi_total);
			wr(rp_dsi, DSI_TIMING_FLUSH, 1);
			wr(rp_intf, INTF_VSYNC_PERIOD_F0, vsync_period);
			if (prog) wr(rp_intf, INTF_PROG_FETCH_START, prog_fetch);
			wr(rp_ctl, CTL_INTF_FLUSH, rd(rp_ctl, CTL_INTF_FLUSH) | (1u << 1));
			wr(rp_ctl, CTL_FLUSH, rd(rp_ctl, CTL_FLUSH) | (1u << 31));
			ml_set_interrupts_enabled(irq);
			ruan_panel120_stats[11] = (int32_t)line;
			return 0;
		}
		delay_for_interval(1, NSEC_PER_USEC);
	}
	return -1;
}

// ---- perf step (docs/research/120hz/underrun.md): RPMh BCM votes for the scanout path and, optionally, a faster MDP clock ----
static volatile uint32_t *rp_cc, *rp_top, *rp_vig, *rp_rsc;
static volatile uint8_t *rp_db;
static QcomRscLayout rp_layout;

// A cmd-db entry's RPMh address and aux bytes, read in place (32-bit header reads, byte aux reads, as RuanGpuPower.c does on ruan).
// qcom_cmd_db_find stops BCM aux at a zero u16 (a unit below 65536 has a zero high half), so it is not used for the BCMs.
static int
rp_cmddb(const char *name, uint32_t *addr, uint8_t *aux, unsigned cap)
{
	uint32_t lo = 0, hi = 0;
	for (unsigned i = 0; i < 8 && name[i]; i++) *(i < 4 ? &lo : &hi) |= (uint32_t)(uint8_t)name[i] << (8 * (i & 3));
	const volatile uint32_t *w = (const volatile uint32_t *)rp_db;
	if (w[1] != 0x0c0330dbu) return -1;
	for (unsigned sl = 0; sl < 8; sl++) {
		const uint32_t h0 = w[(8 + 16 * sl) / 4], h1 = w[(12 + 16 * sl) / 4];
		if (!(h0 & 0xffff)) break;
		const uint32_t heads = 144 + (h0 >> 16), data = 144 + (h1 & 0xffff), count = h1 >> 16;
		if (count > 4096 || heads + count * 24 > RP_CMD_DB_SIZE) return -2;
		for (uint32_t e = 0; e < count; e++) {
			const uint32_t at = heads + 24 * e;
			if (w[at / 4] != lo || w[at / 4 + 1] != hi) continue;
			const uint32_t a = w[at / 4 + 5], len = a & 0xffff, off = data + (a >> 16);
			*addr = w[at / 4 + 4];
			if (len > cap || off + len > RP_CMD_DB_SIZE || !*addr) return -3;
			for (unsigned b = 0; b < len; b++) aux[b] = rp_db[off + b];
			for (unsigned b = len; b < cap; b++) aux[b] = 0;
			return (int)len;
		}
	}
	return -4;
}

// One active request (<= 4 commands) on TCS RP_TCS, completion polled (RuanWifiWpss.cpp voteResources).
static int
rp_rpmh(const uint32_t *addr, const uint32_t *val, unsigned n)
{
	QcomRpmhWrite w[24];
	const unsigned c = qcom_rpmh_active_request(&rp_layout, RP_TCS, addr, val, n, w, 24);
	if (!c) return -1;
	wr(rp_rsc, RP_RSC_TCS, rd(rp_rsc, RP_RSC_TCS) | 1u << RP_TCS);   // IRQ_ENABLE: the status bit follows it (RuanTouch.c)
	for (unsigned i = 0; i < c; i++) wr(rp_rsc, RP_RSC_TCS + w[i].offset, w[i].value);
	int done = 0;
	for (int t = 0; t < 1000 && !done; t++) {
		done = (rd(rp_rsc, RP_RSC_TCS + rp_layout.irq_status) >> RP_TCS) & 1;
		if (!done) delay_for_interval(100, NSEC_PER_USEC);
	}
	const unsigned m = qcom_rpmh_finish(&rp_layout, RP_TCS, w, 24);
	for (unsigned i = 0; i < m; i++) wr(rp_rsc, RP_RSC_TCS + w[i].offset, w[i].value);
	return done ? 0 : -2;
}

// Scanout average at 120 Hz, BGRA linear: 1600 x 2560 x 4 x 120 B/s (self-tested above); the IB default (3 GB/s) is above the line
// peak (1600 x 4 B per 1/(120 x 2758) s = 2.12 GB/s) and is one of KGSL's DDR7 levels. Android's DT floors: core IB 2.5 GB/s,
// DRAM IB 1.6 GB/s (qcom,sde-ib-bw-vote). [V DT, I sizing]
#define RP_AB_KBPS 1966080u

static int
rp_perf_up(volatile int32_t *s, int dry)
{
	rp_cc = (volatile uint32_t *)ml_io_map(RP_DISPCC, 0x10000);
	rp_top = (volatile uint32_t *)ml_io_map(RP_MDP_TOP, 0x1000);
	rp_vig = (volatile uint32_t *)ml_io_map(RP_VIG0, 0x1000);
	if (!rp_cc || !rp_top || !rp_vig) return -40;
	const uint32_t cfg = rd(rp_cc, MDP_CFG_RCGR);
	s[16] = (int32_t)rp_mdp_khz(rd(rp_cc, PLL0_L), rd(rp_cc, PLL0_ALPHA), cfg);
	s[18] = (int32_t)rd(rp_cc, PLL0_MODE); s[19] = (int32_t)cfg;
	s[20] = (int32_t)rd(rp_vig, SSPP_QOS_CTRL); s[21] = (int32_t)rd(rp_vig, SSPP_DANGER_LUT); s[22] = (int32_t)rd(rp_vig, SSPP_SAFE_LUT);
	s[23] = (int32_t)rd(rp_vig, SSPP_CREQ_LUT_0); s[24] = (int32_t)rd(rp_vig, SSPP_CREQ_LUT_1); s[25] = (int32_t)rd(rp_vig, SSPP_CDP_CNTL);

	unsigned ib = 3000000, mdp_mhz = 0;   // ruan-120hz-ib=0 skips the BCM votes; ruan-120hz-mdp=380|506 raises the MDP clock
	PE_parse_boot_argn("ruan-120hz-ib", &ib, sizeof(ib));
	PE_parse_boot_argn("ruan-120hz-mdp", &mdp_mhz, sizeof(mdp_mhz));
	if (dry || (!ib && !mdp_mhz)) return 0;
	// shortcut: maps stay for the boot (one-shot at display probe); unmap if this ever runs more than once
	rp_rsc = (volatile uint32_t *)ml_io_map(RP_RSC, 0x10000);
	rp_db = (volatile uint8_t *)ml_io_map(RP_CMD_DB, RP_CMD_DB_SIZE);
	if (!rp_rsc || !rp_db) return -41;
	rp_layout = qcom_rsc_layout(rp_rsc[0]);
	int rc = 0;
	uint8_t aux[32];
	uint32_t addr[4], val[4];
	if (ib) {
		// The MDP -> DDR path's BCMs and the node each one aggregates (interconnect/qcom/parrot.c): MM0 qns_mem_noc_hf (32 B x 2),
		// SH0 qns_llcc (16 x 2), MC0 ebi (4 x 2); ACV ebi, enable_mask 0x8. These are the HLOS (apps) voter's votes: they replace
		// whatever the apps DRV voted before (UEFI) and are kept on a revert (dropping MC0/SH0 could starve the 60 Hz scanout). [V/I]
		static const struct {char name[4]; uint32_t bus, ch;} b[3] = {{"MM0", 32, 2}, {"SH0", 16, 2}, {"MC0", 4, 2}};
		for (unsigned i = 0; i < 3; i++) {
			if (rp_cmddb(b[i].name, &addr[i], aux, sizeof(aux)) < 8) return -42;
			const uint32_t unit = aux[0] | (uint32_t)aux[1] << 8 | (uint32_t)aux[2] << 16 | (uint32_t)aux[3] << 24;
			val[i] = rp_bcm(unit, aux[4] | (uint32_t)aux[5] << 8, b[i].bus, b[i].ch, RP_AB_KBPS, ib);
			if (!val[i]) return -43;
		}
		if (rp_cmddb("ACV", &addr[3], aux, sizeof(aux)) < 0) return -44;
		val[3] = 1u << 30 | 1u << 29 | 0x8u;
		if (rp_rpmh(addr, val, 4)) return -45;
		rc |= 1;
	}
	if (mdp_mhz && (uint32_t)s[16] < mdp_mhz * 1000u) {
		// Only the table rates that keep CX/MX at or below NOMINAL (dispcc-parrot rate_max: 380 LOW_L1, 506 NOM; PLL0 1518 MHz NOM).
		if (mdp_mhz != 380 && mdp_mhz != 506) return -50;
		if (((cfg >> 8) & 7) != 1 || (cfg & 0x1f) != 5) return -51;   // must be PLL0 / 3, as every table entry
		// CX and MX to NOMINAL (RPMh level 256) first. shortcut: the apps DRV holds one vote per ARC, so WpssProbe's later cx/mx
		// "0" drops this one too; give the ARC votes one owner before shipping ruan-120hz-mdp.
		int la = rp_cmddb("cx.lvl", &addr[0], aux, sizeof(aux));
		if (la < 2) return -52;
		val[0] = rp_arc_index(aux, la, 256);
		la = rp_cmddb("mx.lvl", &addr[1], aux, sizeof(aux));
		if (la < 2) return -53;
		val[1] = rp_arc_index(aux, la, 256);
		if (rp_rpmh(addr, val, 2)) return -54;
		// clk-alpha-pll.c alpha_pll_lucid_evo_set_rate: L, ALPHA, then latch input and its ack while the PLL runs.
		uint32_t l, a;
		rp_pll_la(3ull * mdp_mhz * 1000000u, &l, &a);
		const uint32_t mode = rd(rp_cc, PLL0_MODE);
		if (!(mode & (1u << 2))) return -55;   // PLL in reset: not the running scanout clock we expect
		wr(rp_cc, PLL0_L, (rd(rp_cc, PLL0_L) & ~0xffffu) | l);
		wr(rp_cc, PLL0_ALPHA, a);
		wr(rp_cc, PLL0_MODE, mode | (1u << 14));
		delay_for_interval(1, NSEC_PER_USEC);
		const uint32_t ack = rd(rp_cc, PLL0_MODE);
		wr(rp_cc, PLL0_MODE, ack & ~(1u << 14));
		if (!(ack & (1u << 13))) return -56;
		int lock = 0;
		for (int t = 0; t < 200 && !lock; t++) {
			lock = (rd(rp_cc, PLL0_MODE) >> 31) & 1;
			if (!lock) delay_for_interval(5, NSEC_PER_USEC);
		}
		if (!lock) return -57;
		rc |= 2;
	}
	s[17] = (int32_t)rp_mdp_khz(rd(rp_cc, PLL0_L), rd(rp_cc, PLL0_ALPHA), rd(rp_cc, MDP_CFG_RCGR));
	return rc;
}

// 1 s with no INTF1 underrun: the raw top interrupt bit (cleared first) stays clear and the latched position does not move.
static int
rp_underrun_quiet(volatile int32_t *s)
{
	const uint32_t pos = rd(rp_intf, INTF_UNDERRUN_COUNT);
	wr(rp_top, TOP_INTR_CLEAR, INTR_INTF1_UNDERRUN);
	delay_for_interval(1000, NSEC_PER_MSEC);
	const uint32_t st = rd(rp_top, TOP_INTR_STATUS), now = rd(rp_intf, INTF_UNDERRUN_COUNT);
	s[27] = (int32_t)st; s[28] = (int32_t)now;
	return !(st & INTR_INTF1_UNDERRUN) && now == pos;
}

void
ruan_panel_set_120hz(void)
{
	unsigned mode = 0;
	volatile int32_t *s = ruan_panel120_stats;
	if (!PE_parse_boot_argn("ruan-120hz", &mode, sizeof(mode)) || !mode || s[0]) return;
	rp_dsi = (volatile uint32_t *)ml_io_map(RP_DSI0, 0x1000);
	rp_intf = (volatile uint32_t *)ml_io_map(RP_INTF1, 0x1000);
	rp_ctl = (volatile uint32_t *)ml_io_map(RP_CTL0, 0x1000);
	if (!rp_dsi || !rp_intf || !rp_ctl) {s[0] = -10; return;}

	const uint32_t hs = rd(rp_intf, INTF_HSYNC_CTL), vp = rd(rp_intf, INTF_VSYNC_PERIOD_F0);
	const uint32_t tot = rd(rp_dsi, DSI_TOTAL), av = rd(rp_dsi, DSI_ACTIVE_V);
	const uint32_t cfg = rd(rp_intf, INTF_CONFIG), pf = rd(rp_intf, INTF_PROG_FETCH_START);
	s[1] = (int32_t)hs; s[2] = (int32_t)vp; s[3] = (int32_t)tot; s[4] = (int32_t)av; s[5] = (int32_t)cfg; s[6] = (int32_t)pf;
	s[12] = (int32_t)rd(rp_intf, INTF_UNDERRUN_COUNT);
	// The DSI controller must be on in video mode (DSI_CTRL bit 0 enable, bit 1 video mode).
	if ((rd(rp_dsi, DSI_CTRL) & 3u) != 3u) {s[0] = -11; return;}
	const uint32_t before = rp_mhz(250);
	s[7] = (int32_t)before;
	if (before < 55000 || before > 65000) {s[0] = -12; return;}   // not the 60 Hz scanout we expect

	struct rp_plan p;
	int rc = rp_compute(hs, vp, tot, av, cfg, pf, 120, &p);
	if (rc) {s[0] = -20 + rc; return;}
	// The active area must end inside the new frame (DISPLAY_V_END does not move with VFP).
	if (rd(rp_intf, INTF_DISPLAY_V_END_F0) >= p.vsync_period) {s[0] = -13; return;}
	// Read-only part of the perf step also runs for the dry run, so `panel120` shows the MDP clock and VIG0 QoS either way.
	if (mode == 2) {s[26] = rp_perf_up(s, 1); s[0] = 0x60; return;}
	s[26] = rp_perf_up(s, 0);
	if (s[26] < 0) {s[0] = -40; return;}   // a failed vote or clock step: stay at 60 Hz

	const int prog = (cfg >> 31) & 1;
	if (rp_write(p.dsi_total, p.vsync_period, p.prog_fetch, prog)) {s[0] = -30; return;}
	delay_for_interval(100, NSEC_PER_MSEC);
	const uint32_t after = rp_mhz(500);
	s[8] = (int32_t)after; s[9] = (int32_t)rd(rp_intf, INTF_VSYNC_PERIOD_F0); s[10] = (int32_t)rd(rp_dsi, DSI_TOTAL);
	s[13] = (int32_t)rd(rp_intf, INTF_UNDERRUN_COUNT);
	s[14] = (int32_t)(rd(rp_dsi, DSI_ACK_ERR) | rd(rp_dsi, DSI_TIMEOUT) | rd(rp_dsi, DSI_DLN0_PHY_ERR));
	s[15] = (int32_t)rd(rp_dsi, DSI_STATUS);
	const int rate_ok = after >= 110000 && after <= 130000, quiet = rate_ok && rp_underrun_quiet(s);
	s[29] = (int32_t)rd(rp_dsi, DSI_FIFO_STATUS); s[30] = (int32_t)rd(rp_dsi, DSI_DLN0_PHY_ERR); s[31] = (int32_t)rd(rp_dsi, DSI_TIMEOUT);
	unsigned keep = 0;   // ruan-120hz-keep=1: stay at 120 Hz despite underruns (to look at the screen)
	PE_parse_boot_argn("ruan-120hz-keep", &keep, sizeof(keep));
	if (quiet || (rate_ok && keep)) {s[0] = 0x120; return;}
	// Did not take, counter stalled, or the scanout underruns: put UEFI's values back the same way. The votes stay.
	rp_write(tot, vp, pf, prog);
	s[0] = rate_ok ? -32 : -31;
}

// The display's refresh for NativeLegacyDisplay (overrides its weak boot-arg reader): 120 only when the switch was measured,
// otherwise the vsync boot-arg as before. So vsync=120 vsync-fixed=1 can stay on the command line and a refused switch
// still tells iOS the truth.
unsigned
native_legacy_refresh_hz(void)
{
	unsigned hz = 0;
	PE_parse_boot_argn("vsync", &hz, sizeof(hz));
	if (ruan_panel120_stats[0] == 0x120) return 120;
	return hz > 60 ? 60 : hz;
}
#endif
