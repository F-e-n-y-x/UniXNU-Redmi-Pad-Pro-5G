// Host test for package/RuanGpuPowerBoard.h (stage S1) and package/RuanA710HwInit.h. cc -Wall -Wextra -Werror -I../package.
// A fake register file stands in for GCC/GPUCC/SMMU: CX "powers on" (hw-ctrl PWR_ON) once SW_COLLAPSE is cleared, and the SMMU
// vote clock's CLK_OFF clears once enabled. Checks the mode gates, the exact write set and the A710 tables.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "RuanGpuPowerBoard.h"
#include "RuanA710HwInit.h"
#include "RuanGmuBoard.h"

static uint32_t gcc[0x1000 / 4], vote[0x1000 / 4], cc[0xa000 / 4], smmu[0x1000 / 4];
static unsigned writes_seen, bad_writes;
static uint32_t *win(unsigned w) {return w == GPB_WIN_GCC_GPU ? gcc : w == GPB_WIN_GCC_VOTE ? vote : w == GPB_WIN_GPUCC ? cc : w == GPB_WIN_SMMU ? smmu : NULL;}
static int rd(void *c, unsigned w, uint32_t off, uint32_t *out) {(void)c; uint32_t *p = win(w); if (!p) return -1; *out = p[off / 4]; return 0;}
static int wr(void *c, unsigned w, uint32_t off, uint32_t v) {
    (void)c; ++writes_seen;
    // allowed: GCC cfg_ahb; GPUCC CX GDSCR, ahb clk, smmu vote clk
    if (!((w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_CFG_AHB) ||
          (w == GPB_WIN_GPUCC && (off == GPB_CC_CX_GDSCR || off == GPB_CC_AHB_CLK || off == GPB_CC_SMMU_VOTE_CLK)))) ++bad_writes;
    win(w)[off / 4] = v;
    if (w == GPB_WIN_GPUCC && off == GPB_CC_CX_GDSCR && !(v & GPB_GDSC_SW_COLLAPSE)) cc[GPB_CC_CX_GDS_HW_CTRL / 4] |= GPB_GDSC_PWR_ON;
    if (w == GPB_WIN_GPUCC && off == GPB_CC_SMMU_VOTE_CLK && (v & GPB_CBCR_ENABLE)) cc[off / 4] &= ~GPB_CBCR_CLK_OFF;
    return 0;
}
static void reset(void) {
    memset(gcc, 0, sizeof gcc); memset(vote, 0, sizeof vote); memset(cc, 0, sizeof cc); memset(smmu, 0, sizeof smmu);
    gcc[GPB_GCC_GPU_CFG_AHB / 4] = GPB_CBCR_CLK_OFF;          // cfg_ahb off at boot
    cc[GPB_CC_CX_GDSCR / 4] = GPB_GDSC_SW_COLLAPSE | (2u << 12);
    cc[GPB_CC_SMMU_VOTE_CLK / 4] = GPB_CBCR_CLK_OFF;
    gcc[GPB_GCC_GPU_SNOC_DVM_GFX / 4] = GPB_CBCR_CLK_OFF | 1;   // as g1 read it: enabled, off
    cc[GPB_CC_CX_GMU_CLK / 4] = 0x80000221; cc[GPB_CC_HUB_CX_INT_CLK / 4] = GPB_CBCR_CLK_OFF;
    smmu[GPB_SMMU_ID0 / 4] = 0x4c017e18; smmu[GPB_SMMU_ID1 / 4] = 0x40000016;
    writes_seen = bad_writes = 0;
}

int main(void) {
    struct gpb_io io = {0, rd, wr, 0, 0};
    const unsigned all = (1u << GPB_WINDOWS) - 1;
    struct gpb_result r;

    reset(); gpb_run(&io, 1, all, &r);   // mode 1: reads only, GPUCC skipped (cfg_ahb off), nothing written
    assert(writes_seen == 0 && r.gcc == GPB_DONE && r.cc == GPB_SKIPPED_PREREQ && r.smmu == GPB_SKIPPED_PREREQ);

    reset(); gpb_run(&io, 2, all, &r);   // mode 2: one write (cfg_ahb), GPUCC read, CX still off so SMMU skipped
    assert(writes_seen == 1 && !bad_writes && r.enabled_ahb && r.cc == GPB_DONE && r.smmu == GPB_SKIPPED_PREREQ && !r.powered_cx);

    reset(); gpb_run(&io, 3, all, &r);   // mode 3 with g1's clocks: CX and the two clocks come up, SMMU skipped (cx_gmu off)
    assert(!bad_writes && r.powered_cx && r.powered_clocks && r.smmu == GPB_SKIPPED_PREREQ && r.smmu_why == 1 && r.id0 == 0);
    reset(); cc[GPB_CC_CX_GMU_CLK / 4] = 1; gpb_run(&io, 3, all, &r); assert(r.smmu_why == 2 && r.smmu == GPB_SKIPPED_PREREQ);
    reset(); cc[GPB_CC_CX_GMU_CLK / 4] = 1; cc[GPB_CC_HUB_CX_INT_CLK / 4] = 1; gpb_run(&io, 3, all, &r); assert(r.smmu_why == 3);
    reset(); cc[GPB_CC_CX_GMU_CLK / 4] = 1; cc[GPB_CC_HUB_CX_INT_CLK / 4] = 1; gcc[GPB_GCC_GPU_SNOC_DVM_GFX / 4] = 1;
    gpb_run(&io, 3, all, &r);           // all six SMMU clocks on: SMMU read; GPU window still skipped (GX off)
    assert(!bad_writes && r.powered_cx && r.smmu == GPB_DONE && r.smmu_why == 0 && r.gpu == GPB_SKIPPED_PREREQ);
    assert((r.cx_init & GPB_GDSC_CLK_DIS_WAIT_MASK) == GPB_GDSC_CLK_DIS_WAIT_VAL && r.num_smr == 0x18 && !r.io_error);

    reset(); cc[GPB_CC_CX_GDSCR / 4] |= GPB_GDSC_HW_CONTROL; gpb_run(&io, 3, all, &r);   // refuse under HW control
    assert(r.cx_refused && !r.powered_cx && writes_seen == 1);

    reset(); gpb_run(&io, 4, all, &r); assert(r.gcc == GPB_SKIPPED_MODE && writes_seen == 0);   // above GPB_MAX_MODE: nothing
    reset(); gpb_run(&io, 0, all, &r); assert(writes_seen == 0);

    // A710 tables: counts and spot values from KGSL adreno-gpulist.h (gen7_3_0) and gen7_reg.h.
    assert(RUAN_A710_HWCG_COUNT == 49 && RUAN_A710_GBIF_COUNT == 5 && RUAN_A710_PROTECT_COUNT == 45);
    assert(ruan_a710_hwcg[0].reg == 0xb0 && ruan_a710_hwcg[0].val == 0x02222222);              // RBBM_CLOCK_CNTL_SP0
    assert(ruan_a710_gbif[4].reg == 0x11 && ruan_a710_gbif[4].val == 0x3);                     // GBIF_CLIENT_QOS_CNTL (A710 differs from A730)
    assert(ruan_a710_protect[0].reg == 0x850 && ruan_a710_protect[0].val == 0x13fc0000);       // 0x0-0x4ff
    assert(ruan_a710_protect[44].reg == 0x850 + 47 && ruan_a710_protect[44].val == 0x8001f8c0); // index 47, 0x1f8c0 noaccess
    for (unsigned i = 0; i < RUAN_A710_PROTECT_COUNT; ++i) assert(ruan_a710_protect[i].reg >= 0x850 && ruan_a710_protect[i].reg < 0x850 + 48);
    // S2/S3 facts: zap segments (b01 1948 + b02 3896, b00 is the header) fit the DT carveout; RCG HID = 2*div - 1.
    assert(1948 + 3896 <= RUAN_ZAP_REGION_BYTES && (RUAN_GMU_RCG_CFG & 0x1f) == 2 && (RUAN_HUB_RCG_CFG & 0x1f) == 3);
    // The DT's GMU window (0x3d68000+0x37000) spans GPUCC (0x3d90000) and CX_MISC (0x3d9e000): S2's write allowlist must go
    // by register, never by window. The GPU window ends where the AO block starts.
    assert(RUAN_GMU_BASE + RUAN_GMU_SIZE > RUAN_GPU_CX_MISC_BASE && RUAN_GMU_BASE + RUAN_GMU_SIZE > GPB_GPUCC_BASE);
    assert(RUAN_GPU_BASE + RUAN_GPU_SIZE <= RUAN_GMU_AO_BASE);
    puts("gpu_power_host: ok");
    return 0;
}
