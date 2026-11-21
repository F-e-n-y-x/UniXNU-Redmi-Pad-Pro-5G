/* SPDX-License-Identifier: GPL-2.0-only */
/* ruan (SM7435 "parrot") video clock controller (videocc at 0xaaf0000) and the GCC video branches the iris2 VPU needs: the Lucid EVO
 * PLL0, the iris clock root, the MVSC (iris-ctl) and MVS0 (vcodec) power domains and the branch clocks. The shape and sequences are
 * UniXNU-sm8550-common kits/Sm8550Platform/Sm8550VideoCc.c (GPL-2.0-only, 2dc650c); the offsets and values are downstream
 * drivers/clk/qcom/videocc-parrot.c, gcc-parrot.c and msm_vidc_iris2.c (GPL-2.0), checked against ruan's live DT. NOT BUILT, NOT BOOTED.
 * Register access goes through callbacks, so a host test runs the same code against a register model. */
#ifndef RUAN_VIDEO_CC_H
#define RUAN_VIDEO_CC_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RUAN_VIDEO_CC_BASE 0x0aaf0000ULL   /* clock-controller "qcom,parrot-videocc", reg <0xaaf0000 0x10000> */
#define RUAN_VIDEO_CC_SIZE 0x00010000ULL
#define RUAN_GCC_VIDEO_BASE 0x00142000ULL  /* GCC 0x100000 + 0x42000: the video branches' page (gcc-parrot 0x42004/0x4200c/0x42018) */
#define RUAN_GCC_VIDEO_SIZE 0x00001000ULL

enum ruan_video_cc_window {RuanVideoGcc = 0, RuanVideoCc = 1};   /* offsets from RUAN_GCC_VIDEO_BASE / RUAN_VIDEO_CC_BASE */
struct ruan_video_cc_io {
    void *context;
    uint32_t (*read)(void *context, enum ruan_video_cc_window window, uint32_t offset);
    void (*write)(void *context, enum ruan_video_cc_window window, uint32_t offset, uint32_t value);  /* completes the store */
    void (*delay_us)(void *context, unsigned us);
};

enum ruan_video_cc_step {
    RuanVideoStepNone = 0, RuanVideoStepPllBusy, RuanVideoStepPllLock, RuanVideoStepRcg, RuanVideoStepMvscGdsc, RuanVideoStepCtlAxi,
    RuanVideoStepCtrlClock, RuanVideoStepAhbClock, RuanVideoStepMvs0Gdsc, RuanVideoStepMvs0Axi, RuanVideoStepHwClock,
    RuanVideoStepHwMode, RuanVideoStepRate,
};
struct ruan_video_cc_state {
    bool configured, controller_on, hardware_on;
    uint32_t hz;            /* the iris root rate */
    uint32_t failed_step;   /* enum ruan_video_cc_step */
    uint32_t failed_value;  /* the register at the failure */
};
/* Read-only snapshot (ruan-video=1): nothing is written. */
struct ruan_video_cc_survey {
    uint32_t gcc_ahb, gcc_axi0, gcc_xo;                       /* GCC CBCRs */
    uint32_t pll_mode, pll_l, pll_user, rcg_cmd, rcg_cfg;     /* PLL0, video_cc_iris_clk_src */
    uint32_t mvsc_gdscr, mvs0_gdscr;                          /* GDSCRs: bit 31 PWR_ON, bit 0 SW_COLLAPSE, bit 1 HW_CONTROL */
    uint32_t ctl_axi, mvs0_axi, mvsc_core, mvs0_core, venus_ahb, xo;   /* videocc CBCRs: bit 31 CLK_OFF */
};

/* The iris root rates of ftbl_video_cc_iris_clk_src (Hz) and the RPMh level (cx.lvl and mx.lvl) each needs (its vdd rate_max). */
bool ruan_video_cc_rate_supported(uint32_t hz);
uint32_t ruan_video_cc_rail_level(uint32_t hz);
void ruan_video_cc_survey(const struct ruan_video_cc_io *, struct ruan_video_cc_survey *);
/* True when the VPU's register space may be read: the MVSC domain is on and the controller's AXI, core and AHB clocks run. */
bool ruan_video_cc_vpu_accessible(const struct ruan_video_cc_survey *);

/* videocc probe: GCC video AHB/XO and videocc XO on, the GCC video AXI0 enabled under hardware clock gating, PLL0 configured for hz
 * (standby). Refuses (step PllBusy) a PLL0 that already runs: someone else owns the block. */
bool ruan_video_cc_configure(const struct ruan_video_cc_io *, struct ruan_video_cc_state *, uint32_t hz);
/* __power_on_iris2_controller: iris-ctl (MVSC) on, video_ctl_axi0, PLL0 + iris root at the rate, core (MVSC core), iface (venus AHB). */
bool ruan_video_cc_controller_on(const struct ruan_video_cc_io *, struct ruan_video_cc_state *);
/* __power_on_iris2_hardware: vcodec (MVS0) on, video_mvs0_axi (one pipe), vcodec clock (MVS0 core). */
bool ruan_video_cc_hardware_on(const struct ruan_video_cc_io *, struct ruan_video_cc_state *);
/* MVS0 under hardware (true) or software (false) control (qcom,support-hw-trigger). */
bool ruan_video_cc_hardware_mode(const struct ruan_video_cc_io *, struct ruan_video_cc_state *, bool hardware);
void ruan_video_cc_hardware_off(const struct ruan_video_cc_io *, struct ruan_video_cc_state *);
void ruan_video_cc_controller_off(const struct ruan_video_cc_io *, struct ruan_video_cc_state *);
#ifdef __cplusplus
}
#endif
#endif
