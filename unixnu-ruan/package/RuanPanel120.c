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
#endif

// [0] result: 0x120 switched, 0x60 dry run ok, negative = step that refused/failed (see plan.md §6)
// [1] INTF HSYNC_CTL [2] VSYNC_PERIOD_F0 before [3] DSI TOTAL before [4] DSI ACTIVE_V [5] INTF_CONFIG [6] PROG_FETCH_START before
// [7] mHz before [8] mHz after [9] VSYNC_PERIOD_F0 after [10] DSI TOTAL after [11] line at write
// [12] INTF underrun count before [13] after [14] DSI ACK_ERR|TIMEOUT|DLN0_PHY_ERR after (OR) [15] DSI_STATUS after
volatile int32_t ruan_panel120_stats[16];

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
	if (mode == 2) {s[0] = 0x60; return;}

	const int prog = (cfg >> 31) & 1;
	if (rp_write(p.dsi_total, p.vsync_period, p.prog_fetch, prog)) {s[0] = -30; return;}
	delay_for_interval(100, NSEC_PER_MSEC);
	const uint32_t after = rp_mhz(500);
	s[8] = (int32_t)after; s[9] = (int32_t)rd(rp_intf, INTF_VSYNC_PERIOD_F0); s[10] = (int32_t)rd(rp_dsi, DSI_TOTAL);
	s[13] = (int32_t)rd(rp_intf, INTF_UNDERRUN_COUNT);
	s[14] = (int32_t)(rd(rp_dsi, DSI_ACK_ERR) | rd(rp_dsi, DSI_TIMEOUT) | rd(rp_dsi, DSI_DLN0_PHY_ERR));
	s[15] = (int32_t)rd(rp_dsi, DSI_STATUS);
	if (after >= 110000 && after <= 130000) {s[0] = 0x120; return;}
	// Did not take (or counter stalled): put UEFI's values back the same way.
	rp_write(tot, vp, pf, prog);
	s[0] = -31;
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
