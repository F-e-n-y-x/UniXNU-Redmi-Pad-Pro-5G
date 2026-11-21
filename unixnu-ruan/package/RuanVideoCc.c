/* SPDX-License-Identifier: GPL-2.0-only */
#include "RuanVideoCc.h"

/* gcc-parrot: video AHB and XO are kept on at GCC probe; AXI0 is HALT_SKIP with hardware clock gating (bit 1), and probe sets bits 20
 * and 21 (ignore the PMU clock-disable signal). Offsets from RUAN_GCC_VIDEO_BASE (0x142000). */
enum {GccVideoAhbCbcr = 0x004, GccVideoAxi0Cbcr = 0x00c, GccVideoXoCbcr = 0x018};
/* videocc-parrot */
enum {
    Pll0 = 0x0000, IrisRcg = 0x4000,
    MvscGdscr = 0x5004, MvscCoreCbcr = 0x501c,              /* DT video_cc_mvsc_gdsc @0xaaf5004 = iris-ctl-supply; "core_clk" */
    Mvs0Gdscr = 0x6004, Mvs0CoreCbcr = 0x6018,              /* DT video_cc_mvs0_gdsc reg 0xaaf6004 = vcodec-supply; "vcodec_clk" */
    IrisAhbCbcr = 0x7004, XoCbcr = 0x9018,
    CtlAxiCbcr = 0xb004, Mvs0AxiCbcr = 0xb00c, VenusAhbCbcr = 0xb01c,   /* video_ctl_axi0_clk, video_mvs0_axi_clk, iface_clk */
};
/* clk-alpha-pll, Lucid EVO layout (no TEST_CTL_U2 written: videocc-parrot's config has none) */
enum {PllMode = 0x00, PllOpmode = 0x04, PllLVal = 0x10, PllAlpha = 0x14, PllUserCtl = 0x18, PllUserCtlU = 0x1c, PllConfigCtl = 0x20,
      PllConfigCtlU = 0x24, PllConfigCtlU1 = 0x28};
#define PLL_OUTCTRL (1u << 0)
#define PLL_RESET_N (1u << 2)
#define PLL_LOCK_DET (1u << 31)
#define PLL_STANDBY 0u
#define PLL_RUN 1u
#define PLL_OUT_MASK 7u
#define LUCID_EVO_ENABLE_VOTE_RUN (1u << 25)
#define LUCID_EVO_PLL_L_VAL_MASK 0xffffu
#define TRION_PLL_CAL_VAL 0x44u
#define LUCID_EVO_PLL_CAL_L_VAL_SHIFT 16
#define LUCID_OLE_PLL_PROCESS_CAL_L_VAL_SHIFT 24
#define RCG_CMD_UPDATE (1u << 0)
#define RCG_CMD_ROOT_EN (1u << 1)
#define RCG_CMD_ROOT_OFF (1u << 31)
#define RCG_CFG 0x4u
#define RCG_CFG_SRC_SEL_SHIFT 8
#define RCG_CFG_MASK (0x1fu | (7u << 8) | (3u << 12) | (1u << 20))
#define RCG_SRC_PLL0_EVEN 2u                 /* video_cc_parent_map_0: P_VIDEO_PLL0_OUT_EVEN */
#define CBCR_ENABLE (1u << 0)
#define CBCR_HWCG (1u << 1)
#define CBCR_CLK_OFF (1u << 31)
/* downstream gdsc-regulator without qcom,poll-cfg-gdscr: the GDSCR's own PWR_ON bit */
#define GDSC_SW_COLLAPSE (1u << 0)
#define GDSC_HW_CONTROL (1u << 1)
#define GDSC_RETAIN_FF_ENABLE (1u << 11)    /* qcom,retain-regs */
#define GDSC_PWR_ON (1u << 31)
#define GDSC_POLL_US 2000u
#define XO_HZ 19200000u

/* ftbl_video_cc_iris_clk_src: every rate is PLL0's even output (post divider 1) through the root's divider, and the rail level is the
 * clk's vdd rate_max corner (VDD_LOWER = LOW_SVS 64 ... VDD_HIGH = TURBO 384, RPMh levels). */
static const struct {uint32_t hz, div, level;} rates[] = {
    {133333333u, 3, 64}, {240000000u, 2, 128}, {335000000u, 2, 192}, {424000000u, 2, 256}, {460000000u, 2, 384},
};
static int rate_index(uint32_t hz)
{
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; ++i)
        if (rates[i].hz == hz) return (int)i;
    return -1;
}
bool ruan_video_cc_rate_supported(uint32_t hz) {return rate_index(hz) >= 0;}
uint32_t ruan_video_cc_rail_level(uint32_t hz) {const int i = rate_index(hz); return i < 0 ? 0 : rates[i].level;}

static uint32_t rd(const struct ruan_video_cc_io *io, enum ruan_video_cc_window w, uint32_t o) {return io->read(io->context, w, o);}
static void wr(const struct ruan_video_cc_io *io, enum ruan_video_cc_window w, uint32_t o, uint32_t v) {io->write(io->context, w, o, v);}
static void update(const struct ruan_video_cc_io *io, enum ruan_video_cc_window w, uint32_t o, uint32_t mask, uint32_t value)
{
    wr(io, w, o, (rd(io, w, o) & ~mask) | (value & mask));
}
static bool fail(struct ruan_video_cc_state *s, enum ruan_video_cc_step step, uint32_t value)
{
    s->failed_step = (uint32_t)step;
    s->failed_value = value;
    return false;
}

void ruan_video_cc_survey(const struct ruan_video_cc_io *io, struct ruan_video_cc_survey *v)
{
    v->gcc_ahb = rd(io, RuanVideoGcc, GccVideoAhbCbcr);
    v->gcc_axi0 = rd(io, RuanVideoGcc, GccVideoAxi0Cbcr);
    v->gcc_xo = rd(io, RuanVideoGcc, GccVideoXoCbcr);
    v->pll_mode = rd(io, RuanVideoCc, Pll0 + PllMode);
    v->pll_l = rd(io, RuanVideoCc, Pll0 + PllLVal);
    v->pll_user = rd(io, RuanVideoCc, Pll0 + PllUserCtl);
    v->rcg_cmd = rd(io, RuanVideoCc, IrisRcg);
    v->rcg_cfg = rd(io, RuanVideoCc, IrisRcg + RCG_CFG);
    v->mvsc_gdscr = rd(io, RuanVideoCc, MvscGdscr);
    v->mvs0_gdscr = rd(io, RuanVideoCc, Mvs0Gdscr);
    v->ctl_axi = rd(io, RuanVideoCc, CtlAxiCbcr);
    v->mvs0_axi = rd(io, RuanVideoCc, Mvs0AxiCbcr);
    v->mvsc_core = rd(io, RuanVideoCc, MvscCoreCbcr);
    v->mvs0_core = rd(io, RuanVideoCc, Mvs0CoreCbcr);
    v->venus_ahb = rd(io, RuanVideoCc, VenusAhbCbcr);
    v->xo = rd(io, RuanVideoCc, XoCbcr);
}

bool ruan_video_cc_vpu_accessible(const struct ruan_video_cc_survey *v)
{
    const uint32_t clocks = v->gcc_ahb | v->ctl_axi | v->mvsc_core | v->venus_ahb;
    return (v->mvsc_gdscr & GDSC_PWR_ON) && !(clocks & CBCR_CLK_OFF);
}

/* clk_lucid_evo_pll_configure with the rate's L and 16-bit alpha; the other values are video_pll0_config. Left in standby. */
static void pll_configure(const struct ruan_video_cc_io *io, uint32_t vco_hz)
{
    const uint64_t l = vco_hz / XO_HZ, rest = vco_hz % XO_HZ;
    const uint32_t lval = (uint32_t)l | TRION_PLL_CAL_VAL << LUCID_EVO_PLL_CAL_L_VAL_SHIFT |
                          TRION_PLL_CAL_VAL << LUCID_OLE_PLL_PROCESS_CAL_L_VAL_SHIFT;
    const struct {uint32_t offset, value;} writes[] = {
        {PllLVal, lval}, {PllAlpha, (uint32_t)((rest << 16) / XO_HZ)}, {PllConfigCtl, 0x20485699}, {PllConfigCtlU, 0x00182261},
        {PllConfigCtlU1, 0x32aa299c}, {PllUserCtl, 0x00000001 | PLL_OUT_MASK}, {PllUserCtlU, 0x00000805},
    };
    for (unsigned i = 0; i < sizeof writes / sizeof writes[0]; ++i) wr(io, RuanVideoCc, Pll0 + writes[i].offset, writes[i].value);
    update(io, RuanVideoCc, Pll0 + PllMode, PLL_OUTCTRL, 0);
    wr(io, RuanVideoCc, Pll0 + PllOpmode, PLL_STANDBY);
    update(io, RuanVideoCc, Pll0 + PllMode, PLL_RESET_N, PLL_RESET_N);
}
static bool pll_enabled(const struct ruan_video_cc_io *io)
{
    return (rd(io, RuanVideoCc, Pll0 + PllOpmode) & PLL_RUN) && (rd(io, RuanVideoCc, Pll0 + PllMode) & PLL_OUTCTRL);
}
/* alpha_pll_lucid_evo_enable (not in FSM mode) */
static bool pll_enable(const struct ruan_video_cc_io *io)
{
    if (rd(io, RuanVideoCc, Pll0 + PllUserCtl) & LUCID_EVO_ENABLE_VOTE_RUN) return false;
    if (pll_enabled(io)) return true;
    update(io, RuanVideoCc, Pll0 + PllMode, PLL_RESET_N, PLL_RESET_N);
    wr(io, RuanVideoCc, Pll0 + PllOpmode, PLL_RUN);
    bool locked = false;
    for (unsigned i = 0; i < 1500 && !locked; ++i) {
        locked = (rd(io, RuanVideoCc, Pll0 + PllMode) & PLL_LOCK_DET) != 0;
        if (!locked) io->delay_us(io->context, 1);
    }
    if (!locked) return false;
    update(io, RuanVideoCc, Pll0 + PllUserCtl, PLL_OUT_MASK, PLL_OUT_MASK);
    update(io, RuanVideoCc, Pll0 + PllMode, PLL_OUTCTRL, PLL_OUTCTRL);
    return true;
}
static void pll_disable(const struct ruan_video_cc_io *io)
{
    if (rd(io, RuanVideoCc, Pll0 + PllUserCtl) & LUCID_EVO_ENABLE_VOTE_RUN) return;
    update(io, RuanVideoCc, Pll0 + PllMode, PLL_OUTCTRL, 0);
    io->delay_us(io->context, 1);
    wr(io, RuanVideoCc, Pll0 + PllOpmode, PLL_STANDBY);
}

static bool rcg_wait(const struct ruan_video_cc_io *io, uint32_t mask, uint32_t value)
{
    for (unsigned i = 0; i < 500; ++i) {
        if ((rd(io, RuanVideoCc, IrisRcg) & mask) == value) return true;
        io->delay_us(io->context, 1);
    }
    return (rd(io, RuanVideoCc, IrisRcg) & mask) == value;
}
/* The configuration written with the root forced on, then latched (clk_rcg2 update_config). hid = 2 * divider - 1. */
static bool rcg_set(const struct ruan_video_cc_io *io, uint32_t cfg)
{
    update(io, RuanVideoCc, IrisRcg, RCG_CMD_ROOT_EN, RCG_CMD_ROOT_EN);
    bool ok = rcg_wait(io, RCG_CMD_ROOT_OFF, 0);
    if (ok) {
        update(io, RuanVideoCc, IrisRcg + RCG_CFG, RCG_CFG_MASK, cfg);
        update(io, RuanVideoCc, IrisRcg, RCG_CMD_UPDATE, RCG_CMD_UPDATE);
        ok = rcg_wait(io, RCG_CMD_UPDATE, 0);
    }
    update(io, RuanVideoCc, IrisRcg, RCG_CMD_ROOT_EN, 0);
    return ok;
}

/* clk_branch2: skip for BRANCH_HALT_SKIP, and in hardware clock gating mode (clk_branch_in_hwcg_mode). */
static bool branch(const struct ruan_video_cc_io *io, enum ruan_video_cc_window w, uint32_t cbcr, bool on, bool skip)
{
    update(io, w, cbcr, CBCR_ENABLE, on ? CBCR_ENABLE : 0);
    if (skip || (rd(io, w, cbcr) & CBCR_HWCG)) return true;
    for (unsigned i = 0; i < 200; ++i) {
        if (((rd(io, w, cbcr) & CBCR_CLK_OFF) == 0) == on) return true;
        io->delay_us(io->context, 1);
    }
    return false;
}

static bool gdsc_poll(const struct ruan_video_cc_io *io, uint32_t gdscr, bool on)
{
    for (unsigned i = 0; i < GDSC_POLL_US; ++i) {
        if (((rd(io, RuanVideoCc, gdscr) & GDSC_PWR_ON) != 0) == on) return true;
        io->delay_us(io->context, 1);
    }
    return ((rd(io, RuanVideoCc, gdscr) & GDSC_PWR_ON) != 0) == on;
}
static bool gdsc_on(const struct ruan_video_cc_io *io, uint32_t gdscr)
{
    update(io, RuanVideoCc, gdscr, GDSC_SW_COLLAPSE | GDSC_HW_CONTROL, 0);
    if (!gdsc_poll(io, gdscr, true)) return false;
    io->delay_us(io->context, 1);
    update(io, RuanVideoCc, gdscr, GDSC_RETAIN_FF_ENABLE, GDSC_RETAIN_FF_ENABLE);
    return true;
}
static void gdsc_off(const struct ruan_video_cc_io *io, uint32_t gdscr)
{
    update(io, RuanVideoCc, gdscr, GDSC_HW_CONTROL, 0);
    update(io, RuanVideoCc, gdscr, GDSC_SW_COLLAPSE, GDSC_SW_COLLAPSE);
    (void)gdsc_poll(io, gdscr, false);
}

bool ruan_video_cc_configure(const struct ruan_video_cc_io *io, struct ruan_video_cc_state *s, uint32_t hz)
{
    const int r = rate_index(hz);
    if (!io || !s) return false;
    if (r < 0) return fail(s, RuanVideoStepRate, hz);
    if (s->configured) return s->hz == hz;
    s->failed_step = RuanVideoStepNone;
    if (pll_enabled(io)) return fail(s, RuanVideoStepPllBusy, rd(io, RuanVideoCc, Pll0 + PllMode));
    update(io, RuanVideoGcc, GccVideoAhbCbcr, CBCR_ENABLE, CBCR_ENABLE);
    update(io, RuanVideoGcc, GccVideoXoCbcr, CBCR_ENABLE, CBCR_ENABLE);
    update(io, RuanVideoGcc, GccVideoAxi0Cbcr, CBCR_HWCG | 3u << 20, CBCR_HWCG | 3u << 20);
    (void)branch(io, RuanVideoGcc, GccVideoAxi0Cbcr, true, true);
    update(io, RuanVideoCc, XoCbcr, CBCR_ENABLE, CBCR_ENABLE);
    pll_configure(io, hz * rates[r].div);
    s->hz = hz;
    s->configured = true;
    return true;
}

bool ruan_video_cc_controller_on(const struct ruan_video_cc_io *io, struct ruan_video_cc_state *s)
{
    if (!io || !s || !s->configured) return false;
    if (s->controller_on) return true;
    const int r = rate_index(s->hz);
    if (!gdsc_on(io, MvscGdscr)) return fail(s, RuanVideoStepMvscGdsc, rd(io, RuanVideoCc, MvscGdscr));
    if (!branch(io, RuanVideoCc, CtlAxiCbcr, true, false)) return fail(s, RuanVideoStepCtlAxi, rd(io, RuanVideoCc, CtlAxiCbcr));
    if (!pll_enable(io)) return fail(s, RuanVideoStepPllLock, rd(io, RuanVideoCc, Pll0 + PllMode));
    if (!rcg_set(io, RCG_SRC_PLL0_EVEN << RCG_CFG_SRC_SEL_SHIFT | (2 * rates[r].div - 1)))
        return fail(s, RuanVideoStepRcg, rd(io, RuanVideoCc, IrisRcg));
    if (!branch(io, RuanVideoCc, MvscCoreCbcr, true, false)) return fail(s, RuanVideoStepCtrlClock, rd(io, RuanVideoCc, MvscCoreCbcr));
    if (!branch(io, RuanVideoCc, VenusAhbCbcr, true, false)) return fail(s, RuanVideoStepAhbClock, rd(io, RuanVideoCc, VenusAhbCbcr));
    s->controller_on = true;
    return true;
}

bool ruan_video_cc_hardware_on(const struct ruan_video_cc_io *io, struct ruan_video_cc_state *s)
{
    if (!io || !s || !s->controller_on) return false;
    if (s->hardware_on) return true;
    if (!gdsc_on(io, Mvs0Gdscr)) return fail(s, RuanVideoStepMvs0Gdsc, rd(io, RuanVideoCc, Mvs0Gdscr));
    if (!branch(io, RuanVideoCc, Mvs0AxiCbcr, true, false)) return fail(s, RuanVideoStepMvs0Axi, rd(io, RuanVideoCc, Mvs0AxiCbcr));
    /* BRANCH_HALT_VOTED with hwcg: the status follows the hardware once the domain is handed over */
    if (!branch(io, RuanVideoCc, Mvs0CoreCbcr, true, false)) return fail(s, RuanVideoStepHwClock, rd(io, RuanVideoCc, Mvs0CoreCbcr));
    s->hardware_on = true;
    return true;
}

bool ruan_video_cc_hardware_mode(const struct ruan_video_cc_io *io, struct ruan_video_cc_state *s, bool hardware)
{
    if (!io || !s || !s->hardware_on) return false;
    update(io, RuanVideoCc, Mvs0Gdscr, GDSC_HW_CONTROL, hardware ? GDSC_HW_CONTROL : 0);
    io->delay_us(io->context, 1);
    if (!hardware && !gdsc_poll(io, Mvs0Gdscr, true)) return fail(s, RuanVideoStepHwMode, rd(io, RuanVideoCc, Mvs0Gdscr));
    return true;
}

/* __power_off_iris2_hardware's clock half: vcodec clock, vcodec domain, then video_mvs0_axi (one pipe). */
void ruan_video_cc_hardware_off(const struct ruan_video_cc_io *io, struct ruan_video_cc_state *s)
{
    if (!io || !s || !s->hardware_on) return;
    (void)ruan_video_cc_hardware_mode(io, s, false);
    (void)branch(io, RuanVideoCc, Mvs0CoreCbcr, false, true);
    gdsc_off(io, Mvs0Gdscr);
    (void)branch(io, RuanVideoCc, Mvs0AxiCbcr, false, false);
    s->hardware_on = false;
}

/* __power_off_iris2_controller's clock half: iface, core, video_ctl_axi0, the root parked on XO, PLL0 standby, then iris-ctl. */
void ruan_video_cc_controller_off(const struct ruan_video_cc_io *io, struct ruan_video_cc_state *s)
{
    if (!io || !s || !s->controller_on || s->hardware_on) return;
    (void)branch(io, RuanVideoCc, VenusAhbCbcr, false, false);
    (void)branch(io, RuanVideoCc, MvscCoreCbcr, false, false);
    (void)branch(io, RuanVideoCc, CtlAxiCbcr, false, false);
    (void)rcg_set(io, 0);
    pll_disable(io);
    gdsc_off(io, MvscGdscr);
    s->controller_on = false;
}
