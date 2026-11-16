// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stage S1 of the ruan Adreno 710 bring-up (gpu-dev/README.md): ruan-gpu=<mode> runs RuanGpuPowerBoard.h's gated sequence,
//   1  read GCC's GPU branches, then GPUCC (only if gcc_gpu_cfg_ahb_clk is enabled), never write
//   2  also enable gcc_gpu_cfg_ahb_clk if it is off (the one write gcc-parrot's probe makes), then read GPUCC
//   3  also power the CX GDSC (downstream gdsc-regulator sequence), enable gpu_cc_ahb_clk and gpu_cc_hlos1_vote_gpu_smmu_clk,
//      then read the GPU SMMU's global space 0 and every SMR/S2CR
// Drop-in replacement for rb15g's RuanGpuSurvey.c: same entry (ruan_gpu_survey, called from boot_progress_start) and the same
// banner fields (ruan_gpu_row, ruan_gpu_short). ruan-gpu-survey=1 alone now means ruan-gpu=1 (gated: GPUCC is not read while
// its AHB clock is off, the SError risk S0 had). Writes are allowed only to the four registers listed in allowed(); any other
// write is refused here as well, whatever the board header asks for. Every window is announced before its first access, so a
// bus hang leaves its name as the last boot-page line.
// ruan_gpu_run(mode, trace, ctx) is the same sequence for the USB debug link's `gpu <mode>` command (UsbDebugCdc.cpp): trace()
// gets one line BEFORE every register access (and the value after each read), and must return only once the host has the line;
// a non-zero return skips that access (counted as an I/O error), so the last line zima holds names the access that hung or reset.
#include <stdint.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
#include "RuanGpuPowerBoard.h"
#include "RuanGmuBoard.h"
#include <pexpert/device_tree.h>
#include "RuanGmuMmu.h"
#include "AdrenoHfi.h"   // UniXNU kits/AdrenoFirmware: the modern 16 KiB HFI queue table
#include "RuanSha256.h"   // self-contained: libkern SHA256_* gave rbu5 a wrong digest for correct bytes
#include "RuanA710Regs.h"
#include "RuanA710HwInit.h"
#include "RuanA710Pm4.h"
#include "RuanBuiltinFw.h"

extern void boot_progress_stage(const char *s);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern void delay(int usec);

#define RUAN_GPU_OFF 99u   // the USB link's `gpu off`
char ruan_gpu_row[128], ruan_gpu_short[32];
struct gpb_result ruan_gpu_result;

// Stage S2a adds two read-only windows: the GMU block (GMU core and AO registers; it also spans GPUCC and CX_MISC, so writes
// are allowed by register, never by window) and the GMU's RSCC. Both are mapped only by mode 4, after the GMU clocks run.
// Stage S2b (gpu 6) adds the whole GPU SMMU (its stream-5 context bank), CX_MISC (TCM retention) and the GMU PDC. The GPU
// window grows to 64 KiB for GBIF_HALT (CX side, 0xf114); RBBM is still read only once GX is on.
enum {W_GMU = GPB_WINDOWS, W_RSCC, W_SMMUF, W_CXMISC, W_PDC, NWIN};
static const uint64_t phys[NWIN] = {GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE, RUAN_GMU_BASE,
                                    RUAN_GMU_RSCC_BASE, GPB_SMMU_BASE, RUAN_GPU_CX_MISC_BASE, RUAN_GMU_PDC_BASE};
static const uint64_t len[NWIN] = {0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, RUAN_GPU_SIZE, RUAN_GMU_SIZE, RUAN_GMU_RSCC_SIZE, 0x40000,
                                   RUAN_GPU_CX_MISC_SIZE, RUAN_GMU_PDC_SIZE};
static const char *const name[NWIN] = {"GCC GPU page 0x181000", "GCC vote page 0x162000", "GPUCC 0x3d90000",
                                       "GPU SMMU GR0 0x3da0000", "GPU 0x3d00000", "GMU 0x3d68000", "GMU RSCC 0x3d50000",
                                       "GPU SMMU 0x3da0000 (whole)", "CX_MISC 0x3d9e000", "GMU PDC 0xb270000"};
static vm_offset_t va[NWIN];

// S2a (mode 4) and `off`: GMU clock registers in GPUCC (gpucc-parrot.c) and the GCC GPLL0 vote.
#define CC_GMU_RCG RUAN_CC_GMU_RCGR      // 0x9318 CMD, +4 CFG
#define CC_HUB_RCG RUAN_CC_HUB_RCGR      // 0x93ec
#define RCG_UPDATE (1u << 0)
#define RCG_ROOT_EN (1u << 1)
#define RCG_ROOT_OFF (1u << 31)
#define RCG_CFG_MASK 0x371fu              // clk-rcg2.c: SRC_DIV 0-4, SRC_SEL 8-10, MODE 12-13
// KGSL gen7_reg.h GEN7_GMU_CM3_ITCM_START 0x1b400 / DTCM_START 0x1c400 (dwords from the GPU base): GMU window + 0x5000 /
// + 0x9000, 16 KiB each (adreno_gen7_gmu.c gen7_gmu_vma: ITCM at GMU VA 0, DTCM at VA 0x10004000).
#define TCM_ITCM_OFF ((uint32_t)(0x1b400u * 4u - (uint32_t)(RUAN_GMU_BASE - RUAN_GPU_BASE)))
#define TCM_DTCM_OFF ((uint32_t)(0x1c400u * 4u - (uint32_t)(RUAN_GMU_BASE - RUAN_GPU_BASE)))
#define TCM_BYTES 0x4000u
#define GMU_VA_ITCM 0x0u
#define GMU_VA_DTCM 0x10004000u
#define GMU_VA_CACHE_END 0x01000000u        // GMU_CACHE: 16 KiB .. 16 MiB, DDR behind the GPU SMMU (SID 5)
#define GMU_VA_NONCACHED 0x60000000u        // GMU_NONCACHED_KERNEL: 512 MiB window, DDR behind the SMMU
static const uint8_t *fw_data;
static size_t fw_size;
static const uint8_t *sqe_fw_data;
static size_t sqe_fw_size;
// UsbDebugGpu.h hands over the bytes of /usr/standalone/firmware/ruan/gmu_gen70000.bin before `gpu 5` (it keeps the buffer).
void ruan_gpu_set_firmware(const void *data, size_t size)
{
	fw_data = (const uint8_t *)data, fw_size = size;
}
void ruan_gpu_set_sqe_firmware(const void *data, size_t size)
{
	sqe_fw_data = (const uint8_t *)data, sqe_fw_size = size;
}
static const uint32_t s2a_branch[4] = {RUAN_CC_CX_GMU_CLK, RUAN_CC_CXO_CLK, RUAN_CC_HUB_CX_INT_CLK, RUAN_CC_HUB_AON_CLK};

// gpu 6: the GMU registers KGSL's first boot writes (gen7_reg.h dwords from the GPU base), the RSCC/PDC microcode registers
// (gen7_load_rsc_ucode / gen7_load_pdc_ucode, dwords from their own bases), and the SMMU registers of one context bank.
#define GMU_REG(dw) ((uint32_t)((dw) * 4u - (uint32_t)(RUAN_GMU_BASE - RUAN_GPU_BASE)))
static const uint32_t gmu_writable[] = {0x1f800, 0x1f801, 0x1f81c, 0x1f82d, 0x1f400, 0x1f401, 0x1f40f, 0x1f984, 0x1f985, 0x1f986,
                                        0x1f9cd, 0x1f9ce, 0x1f9cf, 0x1f8f0, 0x1f8f1, 0x23b11,
                                        RUAN_GMU_GMU2HOST_INTR_CLR, RUAN_GMU_GMU2HOST_INTR_MASK, RUAN_GMU_HOST2GMU_INTR_SET,
                                        0x1f8c3, 0x23b04};
static const uint32_t rscc_writable[] = {0x4, 0x8, 0x9, 0xa, 0xb, 0xd, 0xe, 0xf, 0x10, 0x11, 0x12, 0x100, 0x180, 0x181, 0x182, 0x183, 0x184};
// The DT limits HLOS to the first 6 context banks and 6 SMRs (kgsl-smmu qcom,num-context-banks-override / num-smr-override); the
// others belong to the secure world or the hypervisor and are never touched. Bank 0 stays for the GPU, as adreno-smmu assigns
// it; gpu 6 takes the lowest bank 1..5 and slot 0..5 that the survey (gpu 7) proves free.
#define SMMU_HLOS_BANKS 6u
#define SMMU_HLOS_SMRS 6u
static unsigned gmu_bank = ~0u, gmu_slot = ~0u;
#define GMU_SMMU_BANK gmu_bank
#define GPB_SMMU_S2CR_FAULT_RUAN 0x00020000u   // S2CR TYPE fault: what every unused slot read on ruan (g3)
#define GMU_SMMU_SLOT gmu_slot
static uint32_t gmu_cb_off;              // the bank's page in the SMMU window, from ID1 (0 until computed)

/* Stream-0 (GPU user context) on SMMU Bank 2, Slot 1 */
#define GPU_STREAM0_BANK 2u
#define GPU_STREAM0_SLOT 1u
static uint32_t gpu_stream0_cb_off = 0x22000u;

static int gpu_s3_writable(uint32_t off)
{
	if (off == RUAN_GBIF_HALT * 4u || off == RUAN_RBBM_GBIF_HALT * 4u) return 1;
	if (off == RUAN_RBBM_INT_CLEAR_CMD * 4u || off == RUAN_RBBM_INT_0_MASK * 4u) return 1;
	if (off == RUAN_CP_PROTECT_CNTL * 4u || off == RUAN_CP_APRIV_CNTL * 4u || off == RUAN_CP_BV_APRIV_CNTL * 4u || off == RUAN_CP_AHB_CNTL * 4u) return 1;
	if (off >= RUAN_CP_RB_BASE * 4u && off <= RUAN_CP_SQE_CNTL * 4u) return 1;
	if (off == RUAN_CP_BV_RB_RPTR_ADDR_LO * 4u || off == RUAN_CP_BV_RB_RPTR_ADDR_HI * 4u) return 1;
	if (off == RUAN_CP_SQE_INSTR_BASE_LO * 4u || off == RUAN_CP_SQE_INSTR_BASE_HI * 4u) return 1;
	if (off == RUAN_RBBM_PERFCTR_CNTL * 4u || off == RUAN_RBBM_PERFCTR_GPU_BUSY_MASKED * 4u || off == RUAN_RBBM_INTERFACE_HANG_INT_CNTL * 4u) return 1;
	if (off >= 0x00e80u * 4u && off <= 0x00e88u * 4u) return 1;   /* UCHE */
	if (off == 0x00e3au * 4u) return 1;   /* UCHE_GBIF_GX_CONFIG */
	for (unsigned i = 0; i < RUAN_A710_HWCG_COUNT; i++) if (off == ruan_a710_hwcg[i].reg * 4u) return 1;
	for (unsigned i = 0; i < RUAN_A710_GBIF_COUNT; i++) if (off == ruan_a710_gbif[i].reg * 4u) return 1;
	for (unsigned i = 0; i < RUAN_A710_PROTECT_COUNT; i++) if (off == ruan_a710_protect[i].reg * 4u) return 1;
	return 0;
}

static int allowed(unsigned w, uint32_t off)
{
	if (w == GPB_WIN_GPU) return gpu_s3_writable(off);
	if (w == W_CXMISC) return off == 0x39u * 4u;                 // GPU_CX_MISC_TCM_RET_CNTL
	if (w == W_PDC) return off == 0x1140u * 4u || off == 0x1148u * 4u;
	if (w == W_RSCC) {
		for (unsigned i = 0; i < sizeof rscc_writable / sizeof *rscc_writable; i++) if (off == rscc_writable[i] * 4u) return 1;
		return 0;
	}
	if (w == W_SMMUF) {
		if (gmu_bank < SMMU_HLOS_BANKS && gmu_slot < SMMU_HLOS_SMRS) {
			if (off == GPB_SMMU_SMR(GMU_SMMU_SLOT) || off == GPB_SMMU_S2CR(GMU_SMMU_SLOT)) return 1;
			if (off == 0x1000u + 4u * GMU_SMMU_BANK || off == 0x1800u + 4u * GMU_SMMU_BANK) return 1;   // GR1 CBAR, CBA2R
			if (gmu_cb_off && off >= gmu_cb_off && off < gmu_cb_off + 0x1000u) return 1;
		}
		if (GPU_STREAM0_BANK < SMMU_HLOS_BANKS && GPU_STREAM0_SLOT < SMMU_HLOS_SMRS) {
			if (off == GPB_SMMU_SMR(GPU_STREAM0_SLOT) || off == GPB_SMMU_S2CR(GPU_STREAM0_SLOT)) return 1;
			if (off == 0x1000u + 4u * GPU_STREAM0_BANK || off == 0x1800u + 4u * GPU_STREAM0_BANK) return 1;
			if (gpu_stream0_cb_off && off >= gpu_stream0_cb_off && off < gpu_stream0_cb_off + 0x1000u) return 1;
		}
		return 0;
	}
	if (w == W_GMU) {
		for (unsigned i = 0; i < sizeof gmu_writable / sizeof *gmu_writable; i++) if (off == GMU_REG(gmu_writable[i])) return 1;
	}
	if (w == GPB_WIN_GCC_GPU) return off == GPB_GCC_GPU_CFG_AHB;
	if (w == GPB_WIN_GCC_VOTE) return off == GPB_GCC_PLL_VOTE;
	if (w == W_GMU) return off >= TCM_ITCM_OFF && off < TCM_DTCM_OFF + TCM_BYTES;   // S2b: ITCM and DTCM only (GMU in reset)
	if (w != GPB_WIN_GPUCC) return 0;
	if (off == GPB_CC_CX_GDSCR || off == GPB_CC_AHB_CLK || off == GPB_CC_SMMU_VOTE_CLK) return 1;
	if (off == CC_GMU_RCG || off == CC_GMU_RCG + 4 || off == CC_HUB_RCG || off == CC_HUB_RCG + 4) return 1;
	for (unsigned i = 0; i < 4; i++) if (off == s2a_branch[i]) return 1;
	return 0;
}
static int (*trace_fn)(void *, const char *);
static void *trace_ctx;
static const char *const wshort[NWIN] = {"GCC+0x81000", "GCC+0x62000", "GPUCC", "SMMU", "GPU", "GMU", "RSCC", "SMMUF", "CXMISC", "PDC"};
static const char *regname(unsigned w, uint32_t off)
{
	if (w == GPB_WIN_GCC_GPU) return off == GPB_GCC_GPU_CFG_AHB ? "gpu_cfg_ahb" : off == GPB_GCC_GPU_MEMNOC_GFX ? "gpu_memnoc_gfx" :
	    off == GPB_GCC_GPU_SNOC_DVM_GFX ? "gpu_snoc_dvm_gfx" : off == GPB_GCC_DDRSS_GPU_AXI ? "ddrss_gpu_axi" : "";
	if (w == GPB_WIN_GCC_VOTE) return "gpll0_vote";
	if (w == GPB_WIN_GPUCC) return off == GPB_CC_CX_GDSCR ? "cx_gdscr" : off == GPB_CC_CX_GDS_HW_CTRL ? "cx_gds_hw_ctrl" :
	    off == GPB_CC_GX_GDSCR ? "gx_gdscr" : off == GPB_CC_AHB_CLK ? "gpu_cc_ahb" : off == GPB_CC_SMMU_VOTE_CLK ? "hlos1_vote_gpu_smmu" :
	    off == GPB_CC_CX_GMU_CLK ? "cx_gmu" : off == GPB_CC_HUB_CX_INT_CLK ? "hub_cx_int" : off == RUAN_CC_CXO_CLK ? "cxo" :
	    off == RUAN_CC_HUB_AON_CLK ? "hub_aon" : off == CC_GMU_RCG ? "gmu_rcg_cmd" : off == CC_GMU_RCG + 4 ? "gmu_rcg_cfg" :
	    off == CC_HUB_RCG ? "hub_rcg_cmd" : off == CC_HUB_RCG + 4 ? "hub_rcg_cfg" : "";
	if (w == W_GMU) return "gmu";
	if (w == W_RSCC) return "rscc";
	if (w == W_SMMUF) return off < 0x1000u ? "smmu_gr0" : off < 0x2000u ? "smmu_gr1" : "smmu_cb";
	if (w == W_CXMISC) return "cx_misc";
	if (w == W_PDC) return "pdc";
	return w == GPB_WIN_SMMU ? "smmu_gr0" : "rbbm";
}
static int say(const char *line)
{
	return trace_fn ? trace_fn(trace_ctx, line) : 0;
}
static int rd(void *c, unsigned w, uint32_t off, uint32_t *out)
{
	(void)c;
	if (w >= NWIN || !va[w] || off + 4 > len[w]) return -1;
	char line[96];
	if (trace_fn) {
		snprintf(line, sizeof(line), "rd %s+0x%x %s", wshort[w], off, regname(w, off));
		if (say(line)) return -1;
	}
	*out = *(volatile uint32_t *)(va[w] + off);
	if (trace_fn) {
		snprintf(line, sizeof(line), "   = %08x", *out);
		(void)say(line);
	}
	return 0;
}
static int wr(void *c, unsigned w, uint32_t off, uint32_t v)
{
	(void)c;
	if (w >= NWIN || !va[w] || !allowed(w, off)) return -1;
	char line[96];
	if (trace_fn) {
		snprintf(line, sizeof(line), "WR %s+0x%x %s <- %08x", wshort[w], off, regname(w, off), v);
		if (say(line)) return -1;
	}
	*(volatile uint32_t *)(va[w] + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
	(void)say("   written");
	return 0;
}
static void stage(void *c, unsigned w)
{
	(void)c;
	char line[96];
	snprintf(line, sizeof(line), "gpu: touching %s", w == GPB_STAGE_CX_GDSC ? "CX GDSC (power-up writes)" : w < NWIN ? name[w] : "?");
	boot_progress_stage(line);
	(void)say(line);
}
static void delay_us(void *c, unsigned us)
{
	(void)c;
	delay((int)us);
}

static void result_line(const char *line)
{
	boot_progress_stage(line);
	(void)say(line);
}


// What this code itself turned on, so `off` (mode 99) reverts only that and leaves what the firmware left running.
static struct {
	unsigned cx, smmu_clock, vote_bits, branch_mask, gmu_cfg_valid, hub_cfg_valid, gmu_ctx, gmu_started;
	uint32_t gmu_cfg_old, hub_cfg_old;
} owned;
struct ruan_gpu_s2a {
	uint32_t vote_before, vote_after;
	uint32_t gmu_cmd, gmu_cfg_before, gmu_cfg_after, hub_cmd, hub_cfg_before, hub_cfg_after;
	uint32_t branch_before[4], branch_after[4];
	unsigned rcg_fail, branch_fail, gmu_read;
	uint32_t sysreset, boot_config, fw_init, cm3_cfg, hfi_status, rpmh_state, ao_irq, cx_busy, rscc_ack, ao_count, rsc_status0, seq_busy;
	int io_error;
} ruan_gpu_s2a_result;

static int poll_bits(unsigned w, uint32_t off, uint32_t mask, uint32_t want, unsigned tries, uint32_t *last)
{
	for (unsigned i = 0; i < tries; i++) {
		if (rd(NULL, w, off, last)) return -1;
		if ((*last & mask) == want) return 0;
		delay(1);
	}
	return 1;
}

// One RCG to `cfg` (SRC_SEL/SRC_DIV/MODE only), as clk_rcg2_shared_ops does on a root that may be off: force ROOT_EN, wait
// for the root, write CFG, CMD_UPDATE, wait for it to clear, then drop the forced ROOT_EN again.
static int rcg_set(uint32_t cmd_off, uint32_t cfg, uint32_t *cmd_out, uint32_t *before, uint32_t *after, unsigned *valid, uint32_t *old)
{
	uint32_t cmd = 0, v = 0, last = 0;
	if (rd(NULL, GPB_WIN_GPUCC, cmd_off, &cmd) || rd(NULL, GPB_WIN_GPUCC, cmd_off + 4, &v)) return -1;
	*cmd_out = cmd, *before = *after = v;
	if ((v & RCG_CFG_MASK) == cfg) return 0;   // already there
	if (!*valid) *old = v, *valid = 1;
	if (wr(NULL, GPB_WIN_GPUCC, cmd_off, cmd | RCG_ROOT_EN)) return -1;
	if (poll_bits(GPB_WIN_GPUCC, cmd_off, RCG_ROOT_OFF, 0, 500, &last)) return 2;
	if (wr(NULL, GPB_WIN_GPUCC, cmd_off + 4, (v & ~RCG_CFG_MASK) | cfg)) return -1;
	if (wr(NULL, GPB_WIN_GPUCC, cmd_off, cmd | RCG_ROOT_EN | RCG_UPDATE)) return -1;
	int rc = poll_bits(GPB_WIN_GPUCC, cmd_off, RCG_UPDATE, 0, 500, &last) ? 3 : 0;
	if (!(cmd & RCG_ROOT_EN)) (void)wr(NULL, GPB_WIN_GPUCC, cmd_off, last & ~(RCG_ROOT_EN | RCG_UPDATE));
	(void)rd(NULL, GPB_WIN_GPUCC, cmd_off + 4, after);
	return rc;
}

// Stage S2a: the GMU clocks KGSL's gen7_gmu_enable_clks sets (gmu_clk 200 MHz, hub_clk 150 MHz, both GPLL0_OUT_MAIN_DIV), the
// GCC GPLL0 GPU votes they need, the GMU node's GPUCC branches, then a read-only look at the GMU core, AO and RSCC registers
// (only when cx_gmu, cxo and hub_cx_int run). Nothing in the GMU or RSCC is written; the GMU stays in reset.
static void s2a(const struct gpb_result *r, struct ruan_gpu_s2a *k)
{
	__builtin_memset(k, 0, sizeof *k);
	const int cx = gpb_gdsc_on(r->cx_gdscr) || gpb_gdsc_on(r->cx_hwctrl);
	if (!cx || r->cc != GPB_DONE) {
		result_line("gpu s2a: CX is not on (run gpu 3 first or check its result), nothing done");
		k->io_error = 1;
		return;
	}
	result_line("gpu s2a: GPLL0 GPU votes");
	const uint32_t want = GPB_GCC_GPLL0_GPU | GPB_GCC_GPLL0_DIV_GPU;
	if (rd(NULL, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &k->vote_before)) {k->io_error = 1; return;}
	k->vote_after = k->vote_before;
	if ((k->vote_before & want) != want) {
		owned.vote_bits |= want & ~k->vote_before;
		if (wr(NULL, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, k->vote_before | want)) {k->io_error = 1; return;}
		delay(GPB_BRANCH_HALT_DELAY_US);   // BRANCH_HALT_DELAY: no status bit
		(void)rd(NULL, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &k->vote_after);
	}
	result_line("gpu s2a: gmu_clk_src 200 MHz (GPLL0_DIV / 1.5), hub_clk_src 150 MHz (GPLL0_DIV / 2)");
	int rc = rcg_set(CC_GMU_RCG, RUAN_GMU_RCG_CFG, &k->gmu_cmd, &k->gmu_cfg_before, &k->gmu_cfg_after, &owned.gmu_cfg_valid, &owned.gmu_cfg_old);
	if (rc) k->rcg_fail = 0x10u | (unsigned)(rc & 0xf);
	rc = rcg_set(CC_HUB_RCG, RUAN_HUB_RCG_CFG, &k->hub_cmd, &k->hub_cfg_before, &k->hub_cfg_after, &owned.hub_cfg_valid, &owned.hub_cfg_old);
	if (rc) k->rcg_fail |= 0x20u | (unsigned)(rc & 0xf);
	if (k->rcg_fail) {
		result_line("gpu s2a: an RCG did not take its rate, branches not enabled");
		return;
	}
	result_line("gpu s2a: GMU branches cx_gmu cxo hub_cx_int hub_aon");
	for (unsigned i = 0; i < 4; i++) {
		uint32_t v = 0;
		if (rd(NULL, GPB_WIN_GPUCC, s2a_branch[i], &v)) {k->io_error = 1; return;}
		k->branch_before[i] = k->branch_after[i] = v;
		// cx_gmu also gets gpucc-parrot's probe workaround (bits 4-7: AXI/GMU sleep staggering).
		const uint32_t set = GPB_CBCR_ENABLE | (i == 0 ? 0xf0u : 0u);
		if ((v & set) != set) {
			if (!(v & GPB_CBCR_ENABLE)) owned.branch_mask |= 1u << i;
			if (wr(NULL, GPB_WIN_GPUCC, s2a_branch[i], v | set)) {k->io_error = 1; return;}
		}
		if (poll_bits(GPB_WIN_GPUCC, s2a_branch[i], GPB_CBCR_CLK_OFF, 0, 500, &k->branch_after[i])) k->branch_fail |= 1u << i;
	}
	if (k->branch_fail & 7u) {   // hub_aon is not needed for the GMU registers
		result_line("gpu s2a: a GMU clock did not start, GMU registers not read");
		return;
	}
	result_line("gpu s2a: reading GMU core/AO and RSCC state (read only; the GMU stays in reset)");
	va[W_GMU] = ml_io_map_unmappable((vm_offset_t)phys[W_GMU], (vm_size_t)len[W_GMU], 7);
	va[W_RSCC] = ml_io_map_unmappable((vm_offset_t)phys[W_RSCC], (vm_size_t)len[W_RSCC], 7);
	if (!va[W_GMU] || !va[W_RSCC]) {result_line("gpu s2a: GMU/RSCC map failed"); k->io_error = 1; return;}
	// KGSL gen7_reg.h: GMU registers are dword offsets from the GPU base 0x3d00000; RSCC ones from the rscc base.
#define GMU_OFF(dw) ((uint32_t)((dw) * 4u - (uint32_t)(RUAN_GMU_BASE - RUAN_GPU_BASE)))
	const struct {uint32_t off; uint32_t *out;} g[] = {
		{GMU_OFF(0x1f800), &k->sysreset}, {GMU_OFF(0x1f801), &k->boot_config}, {GMU_OFF(0x1f81c), &k->fw_init},
		{GMU_OFF(0x1f82d), &k->cm3_cfg}, {GMU_OFF(0x1f980), &k->hfi_status}, {GMU_OFF(0x1f8ec), &k->rpmh_state},
		{GMU_OFF(0x23b05), &k->ao_irq}, {GMU_OFF(0x23b0c), &k->cx_busy}, {GMU_OFF(0x23b08), &k->rscc_ack},
		{GMU_OFF(0x1f888), &k->ao_count}};
#undef GMU_OFF
	for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) if (rd(NULL, W_GMU, g[i].off, g[i].out)) {k->io_error = 1; return;}
	if (rd(NULL, W_RSCC, 0x4 * 4, &k->rsc_status0) || rd(NULL, W_RSCC, 0x101 * 4, &k->seq_busy)) {k->io_error = 1; return;}
	k->gmu_read = 1;
}


static int need(unsigned w);
static void smmu_ctx_off(void);

// `gpu off`: undo, in reverse, only what this code turned on: GMU branches, RCG rates, GPLL0 votes, the SMMU vote clock, CX.
static void gpu_off(void)
{
	uint32_t v = 0;
	if (owned.gmu_started && !need(W_GMU)) (void)wr(NULL, W_GMU, GMU_REG(0x1f800), 1);   // CM3_SYSRESET: the GMU back in reset
	owned.gmu_started = 0;
	if (owned.gmu_ctx) smmu_ctx_off();
	owned.gmu_ctx = 0;
	for (int i = 3; i >= 0; i--) {
		if (!(owned.branch_mask & (1u << i)) || rd(NULL, GPB_WIN_GPUCC, s2a_branch[i], &v)) continue;
		(void)wr(NULL, GPB_WIN_GPUCC, s2a_branch[i], v & ~GPB_CBCR_ENABLE);
	}
	owned.branch_mask = 0;
	if (owned.gmu_cfg_valid && !rd(NULL, GPB_WIN_GPUCC, CC_GMU_RCG + 4, &v)) {
		uint32_t c = 0, last = 0;
		(void)wr(NULL, GPB_WIN_GPUCC, CC_GMU_RCG + 4, (v & ~RCG_CFG_MASK) | (owned.gmu_cfg_old & RCG_CFG_MASK));
		if (!rd(NULL, GPB_WIN_GPUCC, CC_GMU_RCG, &c) && !wr(NULL, GPB_WIN_GPUCC, CC_GMU_RCG, c | RCG_UPDATE))
			(void)poll_bits(GPB_WIN_GPUCC, CC_GMU_RCG, RCG_UPDATE, 0, 500, &last);
	}
	if (owned.hub_cfg_valid && !rd(NULL, GPB_WIN_GPUCC, CC_HUB_RCG + 4, &v)) {
		uint32_t c = 0, last = 0;
		(void)wr(NULL, GPB_WIN_GPUCC, CC_HUB_RCG + 4, (v & ~RCG_CFG_MASK) | (owned.hub_cfg_old & RCG_CFG_MASK));
		if (!rd(NULL, GPB_WIN_GPUCC, CC_HUB_RCG, &c) && !wr(NULL, GPB_WIN_GPUCC, CC_HUB_RCG, c | RCG_UPDATE))
			(void)poll_bits(GPB_WIN_GPUCC, CC_HUB_RCG, RCG_UPDATE, 0, 500, &last);
	}
	owned.gmu_cfg_valid = owned.hub_cfg_valid = 0;
	if (owned.vote_bits && !rd(NULL, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &v)) (void)wr(NULL, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, v & ~owned.vote_bits);
	owned.vote_bits = 0;
	if (owned.smmu_clock && !rd(NULL, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &v)) (void)wr(NULL, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, v & ~GPB_CBCR_ENABLE);
	owned.smmu_clock = 0;
	if (owned.cx && !rd(NULL, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &v)) (void)wr(NULL, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, v | GPB_GDSC_SW_COLLAPSE);   // gdsc disable
	owned.cx = 0;
	char line[160];
	uint32_t cxr = 0, hw = 0;
	(void)rd(NULL, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &cxr);
	(void)rd(NULL, GPB_WIN_GPUCC, GPB_CC_CX_GDS_HW_CTRL, &hw);
	snprintf(line, sizeof(line), "gpu off: done, cx %08x hw %08x", cxr, hw);
	result_line(line);
	snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu off cx%s", gpb_gdsc_on(hw) ? "on" : "off");
}

struct ruan_gpu_s2b {
	unsigned fw_ok, blocks, tcm_blocks, ddr_blocks, prealloc, tcm_words, tcm_bad;
	uint32_t ddr_bytes, ddr_first, sysreset;
	char why[64];
} ruan_gpu_s2b_result;

// Stage S2b, step 1 (`gpu 5`): check the GMU firmware (size, SHA-256), walk its blocks as gen7_gmu_load_fw does, report what
// needs TCM and what needs SMMU-backed DDR, then write the ITCM/DTCM blocks through the GMU window and read them back. Only
// while CM3_SYSRESET reads 1 (the GMU held in reset); nothing else in the GMU, RSCC or SMMU is written, and the GMU is not
// started. The DDR blocks (the code at GMU VA 0x4000) need the SID-5 SMMU context: the next stage.
static void s2b_tcm(struct ruan_gpu_s2b *b)
{
	__builtin_memset(b, 0, sizeof *b);
	uint8_t digest[32];
	if (!fw_data) {
		fw_data = ruan_builtin_gmu_fw;
		fw_size = sizeof(ruan_builtin_gmu_fw);
	}
	if (!fw_data || fw_size != RUAN_GMU_FW_BYTES) {
		snprintf(b->why, sizeof b->why, "firmware %s (size %lu)", fw_data ? "size differs" : "not loaded", (unsigned long)fw_size);
		return;
	}
	ruan_sha256(fw_data, fw_size, digest);   // for the message below
	if (!ruan_gmu_fw_ok(fw_data, fw_size)) {
		// say what was hashed: the digest, the byte count and the first 16 bytes (a decmpfs header or zeros shows here)
		char line[200];
		int n = snprintf(line, sizeof line, "gpu s2b fw read %lu bytes, sha256 ", (unsigned long)fw_size);
		for (unsigned i = 0; i < 32 && n > 0 && (size_t)n < sizeof line; i++) n += snprintf(line + n, sizeof line - (size_t)n, "%02x", digest[i]);
		n += snprintf(line + n, sizeof line - (size_t)n, " head");
		for (unsigned i = 0; i < 16 && n > 0 && (size_t)n < sizeof line; i++) n += snprintf(line + n, sizeof line - (size_t)n, " %02x", fw_data[i]);
		result_line(line);
		strlcpy(b->why, "firmware sha256 differs", sizeof b->why);
		return;
	}
	b->fw_ok = 1;
	// pass 1: validate every block against the gen7 GMU VA map before any write
	for (size_t o = 0; o + 16 <= fw_size;) {
		uint32_t h[4];
		memcpy(h, fw_data + o, 16);
		o += 16;
		b->blocks++;
		if (h[1] > fw_size - o) {strlcpy(b->why, "block runs past the end", sizeof b->why); return;}
		if (!h[1]) {b->prealloc++; continue;}
		const int itcm = h[0] < TCM_BYTES && h[0] + h[1] <= TCM_BYTES;
		const int dtcm = h[0] >= GMU_VA_DTCM && h[0] + h[1] <= GMU_VA_DTCM + TCM_BYTES;
		if ((itcm || dtcm) && !(h[1] & 3)) b->tcm_blocks++;
		else if ((h[0] >= TCM_BYTES && h[0] + h[1] <= GMU_VA_CACHE_END) || (h[0] >= GMU_VA_NONCACHED)) {
			if (!b->ddr_blocks) b->ddr_first = h[0];
			b->ddr_blocks++, b->ddr_bytes += h[1];
		} else {snprintf(b->why, sizeof b->why, "block %08x+%x outside the gen7 GMU map", h[0], h[1]); return;}
		o += h[1];
	}
	if (rd(NULL, W_GMU, (uint32_t)(0x1f800u * 4u - (uint32_t)(RUAN_GMU_BASE - RUAN_GPU_BASE)), &b->sysreset) || b->sysreset != 1) {
		if (b->sysreset == 0) {
			b->fw_ok = 1;
			b->why[0] = 0;
			return;
		}
		strlcpy(b->why, "GMU not held in reset (CM3_SYSRESET != 1), TCM not written", sizeof b->why);
		return;
	}
	// pass 2: TCM write, then read back
	for (int pass = 0; pass < 2; pass++) {
		for (size_t o = 0; o + 16 <= fw_size;) {
			uint32_t h[4];
			memcpy(h, fw_data + o, 16);
			o += 16;
			const int itcm = h[1] && h[0] < TCM_BYTES && h[0] + h[1] <= TCM_BYTES;
			const int dtcm = h[1] && h[0] >= GMU_VA_DTCM && h[0] + h[1] <= GMU_VA_DTCM + TCM_BYTES;
			if ((itcm || dtcm) && !(h[1] & 3)) {
				const uint32_t base = itcm ? TCM_ITCM_OFF + h[0] : TCM_DTCM_OFF + (h[0] - GMU_VA_DTCM);
				for (uint32_t i = 0; i < h[1]; i += 4) {
					uint32_t want, got = 0;
					memcpy(&want, fw_data + o + i, 4);
					if (!pass) {
						if (wr(NULL, W_GMU, base + i, want)) {strlcpy(b->why, "TCM write refused or not sent", sizeof b->why); return;}
						b->tcm_words++;
					} else if (rd(NULL, W_GMU, base + i, &got) || got != want) {
						b->tcm_bad++;
					}
				}
			}
			o += h[1];
		}
	}
	if (b->tcm_bad) snprintf(b->why, sizeof b->why, "%u TCM words read back wrong", b->tcm_bad);
}

// ---- stage S2b `gpu 6`: GMU context on the GPU SMMU, DDR regions, cold boot to FW_INIT and HFI init --------------------
static uint8_t *pool_cpu;
static uint64_t pool_pa;
static size_t pool_bytes;
// The reserved GPU pool (RuanGmuBoard.h RUAN_GPU_POOL_*), mapped once, write-combined (Normal NC: no cache maintenance needed
// for the SMMU walker or the GMU) and kept for good. Refused unless the range is in /chosen/darwinboot-memory-outside-kept,
// i.e. DarwinBoot passed board.toml's reservation and memory-outside did not hand it to the VM.
static int gpu_pool_reserved(void)
{
	DTEntry chosen;
	const uint8_t *kept = NULL;
	unsigned int kept_size = 0;
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "darwinboot-memory-outside-kept", (void const **)&kept, &kept_size) != kSuccess) return 0;
	for (unsigned int o = 0; o + 16 <= kept_size; o += 16) {
		uint64_t b, n;
		memcpy(&b, kept + o, 8);
		memcpy(&n, kept + o + 8, 8);
		if (b <= RUAN_GPU_POOL_BASE && b + n >= RUAN_GPU_POOL_BASE + RUAN_GPU_POOL_BYTES) return 1;
	}
	return 0;
}
static int gpu_pool(void)
{
	if (pool_cpu) return 0;
	if (!gpu_pool_reserved()) return -1;
	pool_cpu = (uint8_t *)ml_io_map_unmappable((vm_offset_t)RUAN_GPU_POOL_BASE, (vm_size_t)RUAN_GPU_POOL_BYTES, 6);   // VM_WIMG_WCOMB
	if (!pool_cpu) return -2;
	pool_pa = RUAN_GPU_POOL_BASE, pool_bytes = RUAN_GPU_POOL_BYTES;
	return 0;
}
static int need(unsigned w)
{
	if (!va[w]) va[w] = ml_io_map_unmappable((vm_offset_t)phys[w], (vm_size_t)len[w], 7);
	return va[w] ? 0 : -1;
}
static void clean_pool_range(uintptr_t start, size_t size)
{
	uint64_t ctr = 0;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	const uintptr_t line = (uintptr_t)4u << ((ctr >> 16) & 15u);
	__asm__ volatile("dsb sy" ::: "memory");
	for (uintptr_t a = start & ~(line - 1); a < start + size; a += line)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}
static void clean_pool(void)   // dc civac over the active pool (up to 3 MiB)
{
	if (pool_cpu) clean_pool_range((uintptr_t)pool_cpu, 0x300000u);
}
static int gmu_hfi_notify(void *c)
{
	(void)c;
	__asm__ volatile("dsb sy" ::: "memory");
	int rc = wr(NULL, W_GMU, GMU_REG(RUAN_GMU_HOST2GMU_INTR_SET), 1u);
	__asm__ volatile("dsb sy" ::: "memory");
	return rc;
}
static int hfi_wait_once(void *c)
{
	(void)c;
	delay(100);
	return 0;
}
static AdrenoHfi gmu_hfi;

struct ruan_gpu_s2c {
	unsigned regions, pt_used, smmu_refused, booted, hfi_ready, fw_polls, hfi_polls, stage;
	uint32_t id1, cb_off, smr_before, s2cr_before, cbar_before, sctlr_before;
	uint32_t smr_after, s2cr_after, cbar_after, sctlr_after, fwinit, hfi_status, fsr, fsynr0, ao_irq, cm3_cfg;
	uint64_t far, ttbr0;
	char why[72];
} ruan_gpu_s2c_result;

#define CB(reg) (gmu_cb_off + (reg))
enum {CB_SCTLR = 0x0, CB_TCR2 = 0x10, CB_TTBR0_LO = 0x20, CB_TTBR0_HI = 0x24, CB_TCR = 0x30, CB_MAIR0 = 0x38, CB_MAIR1 = 0x3c,
      CB_FSR = 0x58, CB_FAR_LO = 0x60, CB_FAR_HI = 0x64, CB_FSYNR0 = 0x68, CB_TLBIALL = 0x618, CB_TLBSYNC = 0x7f0, CB_TLBSTATUS = 0x7f4};

static uint32_t smmu_bad_off, smmu_bad_want, smmu_bad_got;   // the first SMMU write that did not read back
// superset: the readback must hold every written bit but may add hardware-set ones (CB TCR2: rbu6b wrote 0x2, read 0x62).
static int smmu_wr_check_sup(uint32_t off, uint32_t v, uint32_t *readback, int superset)
{
	if (wr(NULL, W_SMMUF, off, v) || rd(NULL, W_SMMUF, off, readback)) return -1;
	if (*readback == v || (superset && (*readback & v) == v)) return 0;
	if (!smmu_bad_off) smmu_bad_off = off, smmu_bad_want = v, smmu_bad_got = *readback;
	return 1;
}
static int smmu_wr_check(uint32_t off, uint32_t v, uint32_t *readback) {return smmu_wr_check_sup(off, v, readback, 0);}
static void smmu_tlb_sync(void)
{
	uint32_t st = 0;
	(void)wr(NULL, W_SMMUF, CB(CB_TLBIALL), 0);
	(void)wr(NULL, W_SMMUF, CB(CB_TLBSYNC), 0);
	(void)poll_bits(W_SMMUF, CB(CB_TLBSTATUS), 1u, 0u, 1000, &st);
}
// Turn the stream-5 context off again: SMR invalid, S2CR back to fault, the bank disabled. Used by `gpu off` and on refusal.
static void smmu_ctx_off(void)
{
	if (need(W_SMMUF) || !gmu_cb_off) return;
	(void)wr(NULL, W_SMMUF, GPB_SMMU_SMR(GMU_SMMU_SLOT), 0);
	(void)wr(NULL, W_SMMUF, GPB_SMMU_S2CR(GMU_SMMU_SLOT), GPB_SMMU_S2CR_FAULT_RUAN);
	(void)wr(NULL, W_SMMUF, CB(CB_SCTLR), 0);
	smmu_tlb_sync();
	// GR1 (CBAR/CBA2R) is left as it is: rbu6b froze on writing CBAR 0 here. With the SMR invalid the bank is unreachable.
}


// gpu 7 (read only) and gpu 6's bank choice: GR1 CBAR/CBA2R and each HLOS bank's SCTLR/TTBR0/TCR, plus SMR/S2CR 0..5.
struct ruan_smmu_survey {
	uint32_t id0, id1, cb_page, cbar[SMMU_HLOS_BANKS], cba2r[SMMU_HLOS_BANKS], sctlr[SMMU_HLOS_BANKS], ttbr_lo[SMMU_HLOS_BANKS],
	    ttbr_hi[SMMU_HLOS_BANKS], tcr[SMMU_HLOS_BANKS], smr[SMMU_HLOS_SMRS], s2cr[SMMU_HLOS_SMRS];
	unsigned free_bank, free_slot, read;
} ruan_smmu_survey_result;
static uint32_t cb_offset(uint32_t id1, unsigned bank)
{
	const uint32_t page = (id1 & (1u << 31)) ? 0x10000u : 0x1000u;
	return page * ((2u << ((id1 >> 28) & 7u)) + bank);
}
static void smmu_survey(struct ruan_smmu_survey *v)
{
	__builtin_memset(v, 0, sizeof *v);
	v->free_bank = v->free_slot = ~0u;
	if (need(W_SMMUF) || rd(NULL, W_SMMUF, GPB_SMMU_ID0, &v->id0) || rd(NULL, W_SMMUF, GPB_SMMU_ID1, &v->id1)) return;
	const unsigned banks = (v->id1 & 0xffu) < SMMU_HLOS_BANKS ? (v->id1 & 0xffu) : SMMU_HLOS_BANKS;
	for (unsigned i = 0; i < SMMU_HLOS_SMRS; i++) {
		if (rd(NULL, W_SMMUF, GPB_SMMU_SMR(i), &v->smr[i]) || rd(NULL, W_SMMUF, GPB_SMMU_S2CR(i), &v->s2cr[i])) return;
	}
	for (unsigned n = 0; n < banks; n++) {
		const uint32_t cb = cb_offset(v->id1, n);
		if (cb + 0x1000u > 0x40000u) break;
		if (rd(NULL, W_SMMUF, 0x1000u + 4u * n, &v->cbar[n]) || rd(NULL, W_SMMUF, 0x1800u + 4u * n, &v->cba2r[n]) ||
		    rd(NULL, W_SMMUF, cb + 0x0, &v->sctlr[n]) || rd(NULL, W_SMMUF, cb + 0x20, &v->ttbr_lo[n]) ||
		    rd(NULL, W_SMMUF, cb + 0x24, &v->ttbr_hi[n]) || rd(NULL, W_SMMUF, cb + 0x30, &v->tcr[n])) return;
	}
	v->read = 1;
	for (unsigned n = 1; n < banks && v->free_bank == ~0u; n++) {
		// rbu6c gpu 7: every HLOS bank read CBAR 0 with power-on garbage in TTBR0/TCR/SCTLR (M off); only M and routing count
		int used = v->sctlr[n] & 1u;
		for (unsigned i = 0; i < SMMU_HLOS_SMRS; i++)
			if ((v->smr[i] & GPB_SMMU_SMR_VALID) && ((v->s2cr[i] >> 16) & 3u) == 0 && (v->s2cr[i] & 0xffu) == n) used = 1;
		if (!used) v->free_bank = n;
	}
	for (unsigned i = 0; i < SMMU_HLOS_SMRS && v->free_slot == ~0u; i++)
		if (!(v->smr[i] & GPB_SMMU_SMR_VALID) && v->s2cr[i] == GPB_SMMU_S2CR_FAULT_RUAN) v->free_slot = i;
}
static void smmu_survey_lines(const struct ruan_smmu_survey *v)
{
	char line[200];
	snprintf(line, sizeof line, "gpu smmu id0 %08x id1 %08x read %u free bank %d slot %d", v->id0, v->id1, v->read,
	    (int)v->free_bank, (int)v->free_slot);
	result_line(line);
	for (unsigned n = 0; n < SMMU_HLOS_BANKS; n++) {
		snprintf(line, sizeof line, "gpu smmu cb%u @%05x cbar %08x cba2r %08x sctlr %08x ttbr0 %08x%08x tcr %08x | smr%u %08x s2cr %08x", n,
		    cb_offset(v->id1, n), v->cbar[n], v->cba2r[n], v->sctlr[n], v->ttbr_hi[n], v->ttbr_lo[n], v->tcr[n], n, v->smr[n], v->s2cr[n]);
		result_line(line);
	}
}

static void s2c(struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (c->hfi_ready) return;
	uint32_t hfi_st = 0;
	if (va[W_GMU] && !rd(NULL, W_GMU, GMU_REG(0x1f980), &hfi_st) && (hfi_st & 1u)) {
		c->booted = 1;
		c->hfi_ready = 1;
		c->hfi_status = hfi_st;
		c->why[0] = 0;
		return;
	}
	__builtin_memset(c, 0, sizeof *c);
	if (!fw_data) {
		fw_data = ruan_builtin_gmu_fw;
		fw_size = sizeof(ruan_builtin_gmu_fw);
	}
	struct ruan_pt pt;
	struct ruan_gmu_region reg[RUAN_GMU_MAX_REGIONS];
	if (!b->fw_ok || b->tcm_bad || (b->sysreset != 1 && b->sysreset != 0)) {strlcpy(c->why, "gpu 5 part did not pass (fw/TCM/reset)", sizeof c->why); return;}
	const int pr = gpu_pool();
	if (pr) {strlcpy(c->why, pr == -1 ? "GPU pool not reserved (board.toml / kept list)" : "GPU pool map failed", sizeof c->why); return;}
	if (ruan_pt_init(&pt, pool_cpu, pool_pa, RUAN_GMU_POOL_BYTES)) {strlcpy(c->why, "no usable GMU pool", sizeof c->why); return;}
	c->stage = 1;
	result_line("gpu s2c: page tables and GMU regions on the pool");
	const int n = ruan_gmu_layout(&pt, fw_data, fw_size, reg);
	if (n < 0) {strlcpy(c->why, "layout did not fit the pool", sizeof c->why); return;}
	c->regions = (unsigned)n;
	// the firmware's DDR blocks (the code at GMU VA 0x4000) into their regions
	for (size_t o = 0; o + 16 <= fw_size;) {
		uint32_t h[4];
		memcpy(h, fw_data + o, 16);
		o += 16;
		if (h[1] && !ruan_gmu_va_in_tcm(h[0])) {
			uint8_t *dst = ruan_gmu_cpu(&pt, reg, c->regions, h[0], h[1]);
			if (!dst) {snprintf(c->why, sizeof c->why, "no region for fw block %08x+%x", h[0], h[1]); return;}
			memcpy(dst, fw_data + o, h[1]);
		}
		o += h[1];
	}
	uint8_t *hfi = ruan_gmu_cpu(&pt, reg, c->regions, RUAN_GMU_HFI_VA, RUAN_GMU_HFI_BYTES);
	if (!hfi || AdrenoHfiInitializeStopped(&gmu_hfi, (volatile uint32_t *)(void *)hfi, RUAN_GMU_HFI_BYTES, RUAN_GMU_HFI_VA, NULL,
	    gmu_hfi_notify) != AdrenoHfiSuccess) {strlcpy(c->why, "HFI queue table init failed", sizeof c->why); return;}
	c->pt_used = pt.used;
	clean_pool();

	c->stage = 2;
	result_line("gpu s2c: stream-5 context bank on the GPU SMMU");
	if (need(W_SMMUF)) {strlcpy(c->why, "SMMU map failed", sizeof c->why); return;}
	struct ruan_smmu_survey *v = &ruan_smmu_survey_result;
	smmu_survey(v);
	smmu_survey_lines(v);
	c->id1 = v->id1;
	if (!v->read || v->free_bank == ~0u || v->free_slot == ~0u) {strlcpy(c->why, "no free HLOS bank/slot (gpu 7 lines)", sizeof c->why); return;}
	gmu_bank = v->free_bank, gmu_slot = v->free_slot;
	c->cb_off = cb_offset(v->id1, gmu_bank);
	gmu_cb_off = c->cb_off;
	(void)rd(NULL, W_SMMUF, GPB_SMMU_SMR(GMU_SMMU_SLOT), &c->smr_before);
	(void)rd(NULL, W_SMMUF, GPB_SMMU_S2CR(GMU_SMMU_SLOT), &c->s2cr_before);
	(void)rd(NULL, W_SMMUF, 0x1000u + 4u * GMU_SMMU_BANK, &c->cbar_before);
	(void)rd(NULL, W_SMMUF, CB(CB_SCTLR), &c->sctlr_before);
	if ((c->smr_before & GPB_SMMU_SMR_VALID) || c->s2cr_before != GPB_SMMU_S2CR_FAULT_RUAN || (c->sctlr_before & 1u)) {
		strlcpy(c->why, "slot 2 / bank 1 already in use, not touched", sizeof c->why);
		return;
	}
	c->ttbr0 = pt.pa | (1ull << 48);   // ASID 1
	smmu_bad_off = 0;
	int bad = 0;
	uint32_t rb = 0;
	// arm-smmu order: CBA2R, CBAR, TCR2, TTBR0, TCR, MAIR, then SCTLR; every write read back (a hypervisor that owns the SMMU
	// would drop them: reported as refused, nothing forced)
	bad |= smmu_wr_check(0x1800u + 4u * GMU_SMMU_BANK, 1u, &rb);                                  // CBA2R.VA64
	bad |= smmu_wr_check(0x1000u + 4u * GMU_SMMU_BANK, (1u << 16) | (3u << 8) | (0xfu << 12), &c->cbar_after);   // S1, NSH, WB
	bad |= smmu_wr_check_sup(CB(CB_TCR2), RUAN_SMMU_TCR2, &rb, 1);
	bad |= smmu_wr_check(CB(CB_TTBR0_LO), (uint32_t)c->ttbr0, &rb);
	bad |= smmu_wr_check(CB(CB_TTBR0_HI), (uint32_t)(c->ttbr0 >> 32), &rb);
	bad |= smmu_wr_check(CB(CB_TCR), RUAN_SMMU_TCR, &rb);
	bad |= smmu_wr_check(CB(CB_MAIR0), RUAN_SMMU_MAIR0, &rb);
	bad |= smmu_wr_check(CB(CB_MAIR1), 0u, &rb);
	(void)wr(NULL, W_SMMUF, CB(CB_FSR), 0xffffffffu);                                             // clear stale faults
	bad |= smmu_wr_check(CB(CB_SCTLR), (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5), &c->sctlr_after);   // M TRE AFE CFRE, no CFIE
	smmu_tlb_sync();
	bad |= smmu_wr_check(GPB_SMMU_S2CR(GMU_SMMU_SLOT), GMU_SMMU_BANK, &c->s2cr_after);            // translate via bank 1
	bad |= smmu_wr_check(GPB_SMMU_SMR(GMU_SMMU_SLOT), GPB_SMMU_SMR_VALID | (RUAN_SMMU_SID_GMU_MASK << 16) | RUAN_SMMU_SID_GMU,
	    &c->smr_after);
	owned.gmu_ctx = 1;
	if (bad) {
		c->smmu_refused = 1;
		if (bad < 0) strlcpy(c->why, "SMMU write failed", sizeof c->why);
		else snprintf(c->why, sizeof c->why, "SMMU +%x wrote %08x read %08x: refused?", smmu_bad_off, smmu_bad_want, smmu_bad_got);
		smmu_ctx_off();
		owned.gmu_ctx = 0;
		return;
	}

	c->stage = 3;
	result_line("gpu s2c: GMU register config (KGSL gen7_gmu_register_config / device_start)");
	if (need(W_CXMISC) || need(W_PDC) || need(W_RSCC)) {strlcpy(c->why, "CX_MISC/PDC/RSCC map failed", sizeof c->why); return;}
	const struct {unsigned w; uint32_t off, v;} cfg[] = {
	    {W_GMU, GMU_REG(0x1f8f1), 1}, {W_GMU, GMU_REG(0x1f8f0), 1},            // FALNEXT_INTF, FAL_INTF: vote veto for FAL10
	    {W_CXMISC, 0x39u * 4u, 1},                                            // TCM retention
	    {W_GMU, GMU_REG(0x1f81c), 0}, {W_GMU, GMU_REG(0x1f801), 2},           // FW_INIT_RESULT 0, BOOT_CONFIG 2
	    {W_GMU, GMU_REG(0x1f985), RUAN_GMU_HFI_VA}, {W_GMU, GMU_REG(0x1f984), 1},   // HFI_QTBL_ADDR, HFI_QTBL_INFO
	    {W_GMU, GMU_REG(0x23b11), (1u << 31) | (0x32u << 18) | 0x8a0u},       // AHB_FENCE_RANGE_0
	    {W_GMU, GMU_REG(0x1f82d), 0x4052},                                    // CM3_CFG at its reset value (no NMI)
	    {GPB_WIN_GPU, 0x3c45u * 4u, 0},                                       // GBIF_HALT clear
	    {W_GMU, GMU_REG(0x1f9ce), 0}, {W_GMU, GMU_REG(0x1f9cf), RUAN_A710_CHIPID_GMU},   // GENERAL_9 log wptr, GENERAL_10 chip id
	    {W_GMU, GMU_REG(0x1f9cd), (RUAN_GMU_LOG_VA & 0xfffff000u) | (RUAN_GMU_LOG_BYTES / 4096u - 1u)},   // GENERAL_8 log
	    {W_GMU, GMU_REG(0x1f40f), 1}, {W_GMU, GMU_REG(0x1f400), 1}, {W_GMU, GMU_REG(0x1f401), 1}};     // SYS_BUS/ICACHE/DCACHE
	for (unsigned i = 0; i < sizeof cfg / sizeof *cfg; i++)
		if (wr(NULL, cfg[i].w, cfg[i].off, cfg[i].v)) {snprintf(c->why, sizeof c->why, "config write %u refused", i); return;}

	c->stage = 4;
	result_line("gpu s2c: CM3_SYSRESET 0 (the GMU starts), poll FW_INIT_RESULT bit 8");
	if (wr(NULL, W_GMU, GMU_REG(0x1f800), 0)) {strlcpy(c->why, "SYSRESET write refused", sizeof c->why); return;}
	owned.gmu_started = 1;
	for (c->fw_polls = 0; c->fw_polls < 1000; c->fw_polls++) {   // KGSL: 100 ms
		if (rd(NULL, W_GMU, GMU_REG(0x1f81c), &c->fwinit)) break;
		if ((c->fwinit & 0x1ffu) == 0x100u) {c->booted = 1; break;}
		delay(100);
	}
	if (!c->booted) strlcpy(c->why, "GMU did not report FW_INIT bit 8", sizeof c->why);
	else {
		c->stage = 5;
		result_line("gpu s2c: PDC and RSC microcode (gen7_load_pdc_ucode / gen7_load_rsc_ucode)");
		const struct {unsigned w; uint32_t dw, v;} rsc[] = {
		    {W_PDC, 0x1148, 0}, {W_PDC, 0x1140, 0x80000001u},
		    {W_RSCC, 0x4, 1u << 24}, {W_RSCC, 0xb, 1}, {W_RSCC, 0xe, 0}, {W_RSCC, 0xd, 0}, {W_RSCC, 0x10, 0}, {W_RSCC, 0xf, 0},
		    {W_RSCC, 0x12, 0x80000000u}, {W_RSCC, 0x11, 0}, {W_RSCC, 0x100, 0}, {W_RSCC, 0x8, 0x4520}, {W_RSCC, 0x9, 0x4510},
		    {W_RSCC, 0xa, 0x4514}, {W_RSCC, 0x180, 0xeaaae5a0u}, {W_RSCC, 0x181, 0xe1a1ebabu}, {W_RSCC, 0x182, 0xa2e0a581u},
		    {W_RSCC, 0x183, 0xecac82e2u}, {W_RSCC, 0x184, 0x0020edadu}};
		for (unsigned i = 0; i < sizeof rsc / sizeof *rsc; i++)
			if (wr(NULL, rsc[i].w, rsc[i].dw * 4u, rsc[i].v)) {snprintf(c->why, sizeof c->why, "PDC/RSC write %u refused", i); break;}
		if (!c->why[0]) {
			c->stage = 6;
			result_line("gpu s2c: HFI_CTRL_INIT 1, poll HFI_CTRL_STATUS bit 0");
			if (!wr(NULL, W_GMU, GMU_REG(0x1f986), 1)) {
				for (c->hfi_polls = 0; c->hfi_polls < 1000; c->hfi_polls++) {
					if (rd(NULL, W_GMU, GMU_REG(0x1f980), &c->hfi_status)) break;
					if (c->hfi_status & 1u) {c->hfi_ready = 1; break;}
					delay(100);
				}
			}
			if (!c->hfi_ready) strlcpy(c->why, "HFI_CTRL_STATUS bit 0 never set", sizeof c->why);
		}
	}
	// what the GMU and the SMMU say afterwards, pass or fail
	(void)rd(NULL, W_SMMUF, CB(CB_FSR), &c->fsr);
	(void)rd(NULL, W_SMMUF, CB(CB_FSYNR0), &c->fsynr0);
	uint32_t lo = 0, hi = 0;
	(void)rd(NULL, W_SMMUF, CB(CB_FAR_LO), &lo);
	(void)rd(NULL, W_SMMUF, CB(CB_FAR_HI), &hi);
	c->far = (uint64_t)hi << 32 | lo;
	(void)rd(NULL, W_GMU, GMU_REG(0x23b05), &c->ao_irq);
	(void)rd(NULL, W_GMU, GMU_REG(0x1f82d), &c->cm3_cfg);
}

/* =========================================================================
 * Stage S3: Adreno 710 GX Power-Up, HW Init, Stream-0 SMMU, SQE Load, CP, Ring
 * ========================================================================= */

#define RUAN_S3_POOL_STREAM0_PT_OFF   0x100000u  /* 256 KiB */
#define RUAN_S3_POOL_STREAM0_PT_SIZE  0x040000u  /* 256 KiB */
#define RUAN_S3_POOL_SQE_OFF          0x090000u  /* 64 KiB */
#define RUAN_S3_POOL_RB_OFF           0x0A0000u  /* 32 KiB */
#define RUAN_S3_POOL_MEMPTRS_OFF      0x0A8000u  /* 4 KiB */
#define RUAN_S3_POOL_SCRATCH_OFF      0x0A9000u  /* 4 KiB */
#define RUAN_S3_POOL_PWRUP_OFF        0x0AA000u  /* 4 KiB */
#define RUAN_S3_POOL_SCM_META_OFF     0x0B0000u  /* 64 KiB (CPU only) */
#define RUAN_S3_POOL_USER_OFF         0x140000u  /* ~14.75 MiB */

#define RUAN_S3_GPU_VA_BASE           0x40000000u
#define RUAN_S3_GPU_VA_SQE            (RUAN_S3_GPU_VA_BASE + RUAN_S3_POOL_SQE_OFF)      /* 0x40090000 */
#define RUAN_S3_GPU_VA_RB             (RUAN_S3_GPU_VA_BASE + RUAN_S3_POOL_RB_OFF)       /* 0x400A0000 */
#define RUAN_S3_GPU_VA_MEMPTRS        (RUAN_S3_GPU_VA_BASE + RUAN_S3_POOL_MEMPTRS_OFF)  /* 0x400A8000 */
#define RUAN_S3_GPU_VA_SCRATCH        (RUAN_S3_GPU_VA_BASE + RUAN_S3_POOL_SCRATCH_OFF)  /* 0x400A9000 */
#define RUAN_S3_GPU_VA_PWRUP          (RUAN_S3_GPU_VA_BASE + RUAN_S3_POOL_PWRUP_OFF)    /* 0x400AA000 */
#define RUAN_S3_GPU_VA_USER           (RUAN_S3_GPU_VA_BASE + RUAN_S3_POOL_USER_OFF)     /* 0x40140000 */
#define RUAN_S3_GPU_VA_FB             0x50000000u
#define RUAN_S3_GPU_VA_SRC            0x60000000u
#define RUAN_FB_PHYS                  0xb8000000ull
#define RUAN_FB_SIZE                  0x01000000u   /* 16 MiB GOP Framebuffer */

struct ruan_gpu_s3_gx {
	uint32_t gx_before, gx_after;
	uint32_t oob_info, oob_polls;
	uint32_t vote_status, fw_error;
	uint32_t gx_status_before, gx_status_after;
	uint32_t rbbm_status, rbbm_int_0, rbbm_gbif_halt;
	int gx_on;
	char why[64];
};
static struct ruan_gpu_s3_gx ruan_gpu_s3_gx_result;

/* Resource discovery via AOP command-db at 0x80860000 (128 KiB) */
#define RUAN_CMD_DB_PHYS 0x80860000ULL
#define RUAN_CMD_DB_SIZE 0x20000u
#define RUAN_CMD_DB_MAGIC 0x0c0330dbu

struct ruan_cmddb_res {
	uint32_t address;
	uint32_t length;
	uint8_t aux[32];
	int found;
};

static inline int ruan_res_match(uint32_t lo, uint32_t hi, const char *name)
{
	uint32_t elo = 0, ehi = 0;
	for (unsigned i = 0; i < 8 && name[i]; i++) {
		if (i < 4) elo |= (uint32_t)(uint8_t)name[i] << (8 * i);
		else ehi |= (uint32_t)(uint8_t)name[i] << (8 * (i - 4));
	}
	return (lo == elo && hi == ehi);
}

static inline uint16_t ruan_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int ruan_arc_vote(const struct ruan_cmddb_res *pri, const struct ruan_cmddb_res *sec, uint32_t lvl, uint32_t *vote)
{
	if (!pri->found || !pri->length || (pri->length & 1)) return 0;
	uint32_t p = 0, s = 0;
	for (p = 0; p < pri->length / 2; p++) {
		if (ruan_le16(pri->aux + 2 * p) >= lvl) break;
	}
	if (p >= pri->length / 2) p = pri->length / 2 - 1;
	if (sec->found && sec->length >= 2) {
		for (s = 0; s < sec->length / 2; s++) {
			if (ruan_le16(sec->aux + 2 * s) >= lvl) break;
		}
		if (s >= sec->length / 2) s = sec->length / 2 - 1;
	}
	*vote = ((uint32_t)ruan_le16(pri->aux + 2 * p) << 16) | (s << 8) | p;
	return 1;
}

static void ruan_build_hfi_tables(uint32_t *perf, uint32_t *bw)
{
	memset(perf, 0, 58 * sizeof(uint32_t));
	memset(bw, 0, 159 * sizeof(uint32_t));

	/* Default fallback votes if cmd-db is unmapped */
	uint32_t gpu_vote = 0x00380101u;
	uint32_t gmu_vote = 0x00380101u;
	uint32_t cn0_addr = 0x50000u;
	uint32_t sh0_addr = 0x50004u;
	uint32_t mc0_addr = 0x50008u;
	uint32_t acv_addr = 0x5000cu;

	vm_offset_t cdb_va = ml_io_map_unmappable((vm_offset_t)RUAN_CMD_DB_PHYS, (vm_size_t)RUAN_CMD_DB_SIZE, 7);
	if (cdb_va) {
		uint32_t magic = *(volatile uint32_t *)(cdb_va + 4);
		if (magic == RUAN_CMD_DB_MAGIC) {
			result_line("gpu s3_gx: cmd-db magic 0x0c0330db OK at 0x80860000");
			static const char *const res_names[10] = {
				"gfx.lvl", "mx.lvl", "cx.lvl", "SH0", "MC0", "ACV", "SH2", "xo.lvl", "SN15", "SN3"
			};
			struct ruan_cmddb_res res[10];
			memset(res, 0, sizeof(res));

			for (unsigned slave = 0; slave < 8; slave++) {
				uint32_t h0 = *(volatile uint32_t *)(cdb_va + 8 + 16 * slave);
				uint32_t h1 = *(volatile uint32_t *)(cdb_va + 8 + 16 * slave + 4);
				if (!(h0 & 0xffff)) break;
				uint32_t rec_off = 144 + (h0 >> 16);
				uint32_t aux_off = 144 + (h1 & 0xffff);
				uint32_t cnt = h1 >> 16;
				if (cnt > 4096 || rec_off > RUAN_CMD_DB_SIZE) break;

				for (uint32_t i = 0; i < cnt; i++) {
					uint32_t r0 = *(volatile uint32_t *)(cdb_va + rec_off + i * 24);
					uint32_t r1 = *(volatile uint32_t *)(cdb_va + rec_off + i * 24 + 4);
					uint32_t r_addr = *(volatile uint32_t *)(cdb_va + rec_off + i * 24 + 16);
					uint32_t r_aux = *(volatile uint32_t *)(cdb_va + rec_off + i * 24 + 20);

					for (unsigned n = 0; n < 10; n++) {
						if (!res[n].found && ruan_res_match(r0, r1, res_names[n])) {
							res[n].found = 1;
							res[n].address = r_addr;
							uint32_t len = r_aux & 0xffff;
							uint32_t off = aux_off + (r_aux >> 16);
							res[n].length = len > 32 ? 32 : len;
							for (unsigned b = 0; b < res[n].length; b++) {
								res[n].aux[b] = *(volatile uint8_t *)(cdb_va + off + b);
							}
						}
					}
				}
			}

			if (res[0].found && res[1].found) (void)ruan_arc_vote(&res[0], &res[1], 64, &gpu_vote);
			if (res[2].found && res[1].found) (void)ruan_arc_vote(&res[2], &res[1], 48, &gmu_vote);
			if (res[6].found) cn0_addr = res[6].address;
			if (res[3].found) sh0_addr = res[3].address;
			if (res[4].found) mc0_addr = res[4].address;
			if (res[5].found) acv_addr = res[5].address;

			char msg[96];
			snprintf(msg, sizeof msg, "gpu s3_gx: cmd-db ARC gpu=%08x gmu=%08x sh0=%05x mc0=%05x acv=%05x",
				gpu_vote, gmu_vote, sh0_addr, mc0_addr, acv_addr);
			result_line(msg);
		} else {
			result_line("gpu s3_gx: cmd-db magic mismatch, using fallback tables");
		}
		ml_io_unmap(cdb_va, (vm_size_t)RUAN_CMD_DB_SIZE);
	} else {
		result_line("gpu s3_gx: cmd-db map failed, using fallback tables");
	}

	/* 1. Build DCVS / Performance Table (58 words) */
	perf[0] = 2;              /* gpu_level_num */
	perf[1] = 2;              /* gmu_level_num */
	perf[2] = 0;              /* Level 0: vote off */
	perf[3] = 0xffffffffu;    /* Level 0: cx_vote disabled */
	perf[4] = 0;              /* Level 0: 0 kHz */
	perf[5] = gpu_vote;       /* Level 1: active vote */
	perf[6] = 0xffffffffu;    /* Level 1: cx_vote disabled */
	perf[7] = 295000;         /* Level 1: 295 MHz (lowest active on A710) */
	perf[50] = 0;             /* GMU Level 0: vote off */
	perf[51] = 0;             /* GMU Level 0: 0 kHz */
	perf[52] = gmu_vote;      /* GMU Level 1: active vote */
	perf[53] = 200000;        /* GMU Level 1: 200 MHz */

	/* 2. Build Bandwidth Table (159 words) */
	bw[0] = 2;                /* bandwidth_count (off, active) */
	bw[1] = 1;                /* cnoc_count */
	bw[2] = 3;                /* ddr_count */
	bw[3] = 1;                /* cnoc_wait_mask */
	bw[4] = (1u << 2);        /* ddr_wait_mask (commit on ACV) */
	bw[5] = cn0_addr;         /* cnoc_addresses[0] */
	bw[11] = 0x40000000u;     /* cnoc_data[0][0] off */
	bw[17] = 0x60000001u;     /* cnoc_data[1][0] active */
	bw[23] = sh0_addr;        /* ddr_addresses[0] */
	bw[24] = mc0_addr;        /* ddr_addresses[1] */
	bw[25] = acv_addr;        /* ddr_addresses[2] */
	/* Row 0: off */
	bw[31] = 0; bw[32] = 0; bw[33] = 0x40000000u;
	/* Row 1: active */
	bw[39] = 0x60000001u;     /* SH0 active */
	bw[40] = 0x60000001u;     /* MC0 active */
	bw[41] = 0x60000008u;     /* ACV active (ACV_APPS = BIT(3)) */
}

static void s3_gx_on(struct ruan_gpu_s3_gx *gx, struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (gx->gx_on) return;
	if (need(W_GMU) || need(GPB_WIN_GPU) || need(GPB_WIN_GPUCC)) {
		strlcpy(gx->why, "GMU/GPU/GPUCC window map failed", sizeof gx->why);
		return;
	}
	uint32_t gdsc = 0, pwr_st = 0;
	(void)rd(NULL, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &gdsc);
	(void)rd(NULL, W_GMU, GMU_REG(0x1f8d0), &pwr_st);
	if ((gdsc & (1u << 31)) && !(pwr_st & 3u)) {
		result_line("gpu s3_gx: GX already powered on and stable");
		gx->gx_on = 1;
		gx->gx_before = gdsc;
		gx->gx_after = gdsc;
		gx->gx_status_after = pwr_st;
		(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_STATUS * 4u, &gx->rbbm_status);
		(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_INT_0_STATUS * 4u, &gx->rbbm_int_0);
		(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_GBIF_HALT * 4u, &gx->rbbm_gbif_halt);
		return;
	}
	__builtin_memset(gx, 0, sizeof *gx);
	uint32_t hfi_stat = 0;
	(void)rd(NULL, W_GMU, GMU_REG(RUAN_GMU_HFI_CTRL_STATUS), &hfi_stat);
	if (hfi_stat & 1u) c->hfi_ready = 1;
	if (!c->hfi_ready) {
		s2c(c, b);
		if (!c->hfi_ready) {
			snprintf(gx->why, sizeof gx->why, "GMU/HFI not ready: %s", c->why[0] ? c->why : "failed");
			return;
		}
	}

	static int gmu_hfi_started = 0;
	if (!gmu_hfi_started) {
		uint32_t perf_table[58];
		uint32_t bw_table[159];
		ruan_build_hfi_tables(perf_table, bw_table);

		result_line("gpu s3_gx: send HFI Msg 4 (H2F_MSG_PERF_TBL, 58 words)");
		uint32_t out[16] = {0};
		size_t count = 0;
		uint32_t fw_err = 0;
		AdrenoHfiStatus stat = AdrenoHfiExchange(&gmu_hfi, 4, perf_table, 58, 500, hfi_wait_once, out, 16, &count, &fw_err);
		if (stat != AdrenoHfiSuccess) {
			snprintf(gx->why, sizeof gx->why, "HFI Msg 4 PERF_TBL failed: stat %d err %u", stat, fw_err);
			return;
		}

		result_line("gpu s3_gx: send HFI Msg 3 (H2F_MSG_BW_TBL, 159 words)");
		stat = AdrenoHfiExchange(&gmu_hfi, 3, bw_table, 159, 500, hfi_wait_once, out, 16, &count, &fw_err);
		if (stat != AdrenoHfiSuccess) {
			snprintf(gx->why, sizeof gx->why, "HFI Msg 3 BW_TBL failed: stat %d err %u", stat, fw_err);
			return;
		}

		result_line("gpu s3_gx: send HFI Msg 14 (H2F_MSG_CORE_FW_START, 1 word)");
		const uint32_t core_start = 0;
		stat = AdrenoHfiExchange(&gmu_hfi, 14, &core_start, 1, 500, hfi_wait_once, out, 16, &count, &fw_err);
		if (stat != AdrenoHfiSuccess) {
			snprintf(gx->why, sizeof gx->why, "HFI Msg 14 CORE_FW_START failed: stat %d err %u", stat, fw_err);
			return;
		}

		result_line("gpu s3_gx: send HFI Msg 30 (H2F_MSG_GX_BW_PERF_VOTE, 3 words)");
		const uint32_t vote[3] = {1, 1, 1};
		stat = AdrenoHfiExchange(&gmu_hfi, 30, vote, 3, 500, hfi_wait_once, out, 16, &count, &fw_err);
		if (stat != AdrenoHfiSuccess) {
			snprintf(gx->why, sizeof gx->why, "HFI Msg 30 PERF_VOTE failed: stat %d err %u", stat, fw_err);
			return;
		}
		gmu_hfi_started = 1;
	}

	result_line("gpu s3_gx: clear/unmask GMU interrupts");
	if (wr(NULL, W_GMU, GMU_REG(RUAN_GMU_GMU2HOST_INTR_CLR), ~0u)) {
		strlcpy(gx->why, "GMU2HOST_INTR_CLR write refused", sizeof gx->why);
		return;
	}
	(void)wr(NULL, W_GMU, GMU_REG(0x23b04u), ~0u); /* GEN7_GMU_AO_HOST_INTERRUPT_CLR */
	if (wr(NULL, W_GMU, GMU_REG(RUAN_GMU_GMU2HOST_INTR_MASK), ~(1u << 23))) {
		strlcpy(gx->why, "GMU2HOST_INTR_MASK write refused", sizeof gx->why);
		return;
	}
	(void)rd(NULL, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &gx->gx_before);
	(void)rd(NULL, W_GMU, GMU_REG(0x1f8d0), &gx->gx_status_before);

	/* Assert OOB GPU SET (bit 30) */
	const uint32_t req = (1u << 30);
	const uint32_t ack_mask = (1u << 31);
	result_line("gpu s3_gx: send OOB GPU SET (bit 30), poll GMU2HOST_INTR_INFO bit 31");
	if (wr(NULL, W_GMU, GMU_REG(RUAN_GMU_HOST2GMU_INTR_SET), req)) {
		strlcpy(gx->why, "HOST2GMU_INTR_SET write refused", sizeof gx->why);
		return;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	/* Fast raw polling loop without CDC tracing overhead */
	for (gx->oob_polls = 1; gx->oob_polls <= 50000; gx->oob_polls++) {
		gx->oob_info = *(volatile uint32_t *)(va[W_GMU] + GMU_REG(RUAN_GMU_GMU2HOST_INTR_INFO));
		if (gx->oob_info & ack_mask) break;
		delay(10);
	}
	if (!(gx->oob_info & ack_mask)) {
		snprintf(gx->why, sizeof gx->why, "OOB GPU SET timed out (polls %u, info %08x)", gx->oob_polls, gx->oob_info);
		return;
	}

	/* Clear ACK bit 31 on GMU2HOST (keep bit 30 held in HOST2GMU) */
	*(volatile uint32_t *)(va[W_GMU] + GMU_REG(RUAN_GMU_GMU2HOST_INTR_CLR)) = ack_mask;
	__asm__ volatile("dsb sy" ::: "memory");

	result_line("gpu s3_gx: send HFI Msg 10 (H2F_MSG_START, 0 words)");
	uint32_t out[16] = {0};
	size_t count = 0;
	uint32_t fw_err = 0;
	AdrenoHfiStatus stat = AdrenoHfiExchange(&gmu_hfi, 10, NULL, 0, 500, hfi_wait_once, out, 16, &count, &fw_err);
	if (stat != AdrenoHfiSuccess) {
		snprintf(gx->why, sizeof gx->why, "HFI Msg 10 START failed: stat %d err %u", stat, fw_err);
		return;
	}

	result_line("gpu s3_gx: enable SPTPRAC (0x1f8c3 = 1)");
	*(volatile uint32_t *)(va[W_GMU] + GMU_REG(0x1f8c3)) = 1;
	__asm__ volatile("dsb sy" ::: "memory");

	uint32_t pwr_status = 0;
	for (unsigned p = 0; p < 50000; p++) {
		pwr_status = *(volatile uint32_t *)(va[W_GMU] + GMU_REG(0x1f8d0));
		if (!(pwr_status & 3u)) break;
		delay(10);
	}
	gx->gx_status_after = pwr_status;
	if (pwr_status & 3u) {
		snprintf(gx->why, sizeof gx->why, "SPTPRAC power on timed out (status %08x)", pwr_status);
		return;
	}

	/* Wait for AHB fence to drop */
	for (unsigned p = 0; p < 50000; p++) {
		if (*(volatile uint32_t *)(va[W_GMU] + GMU_REG(0x23b10)) == 0) break;
		delay(10);
	}

	(void)rd(NULL, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &gx->gx_after);
	gx->gx_on = 1;
	result_line("gpu s3_gx: GX powered on! Reading RBBM_STATUS from GPU block");
	(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_STATUS * 4u, &gx->rbbm_status);
	(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_INT_0_STATUS * 4u, &gx->rbbm_int_0);
	(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_GBIF_HALT * 4u, &gx->rbbm_gbif_halt);
}

struct ruan_gpu_s3_hw {
	unsigned gbif_writes, protect_writes, hwcg_writes;
	uint32_t smr_after, s2cr_after, cbar_after, sctlr_after, ttbr0_lo;
	uint32_t rbbm_status;
	int ok;
	char why[64];
};
static struct ruan_gpu_s3_hw ruan_gpu_s3_hw_result;
static struct ruan_pt stream0_pt;

static void s3_hw_init(struct ruan_gpu_s3_hw *h, struct ruan_gpu_s3_gx *gx, struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (h->ok) return;
	__builtin_memset(h, 0, sizeof *h);
	if (!gx->gx_on) {
		s3_gx_on(gx, c, b);
		if (!gx->gx_on) {
			snprintf(h->why, sizeof h->why, "GX not on: %s", gx->why[0] ? gx->why : "failed");
			return;
		}
	}
	if (!pool_cpu && gpu_pool()) {
		strlcpy(h->why, "gpu_pool failed", sizeof h->why);
		return;
	}
	if (need(W_SMMUF)) {
		strlcpy(h->why, "SMMUF map failed", sizeof h->why);
		return;
	}
	result_line("gpu s3_hw: GBIF register configuration");
	for (unsigned i = 0; i < RUAN_A710_GBIF_COUNT; i++) {
		if (wr(NULL, GPB_WIN_GPU, ruan_a710_gbif[i].reg * 4u, ruan_a710_gbif[i].val)) {
			snprintf(h->why, sizeof h->why, "GBIF write %u (0x%05x) refused", i, ruan_a710_gbif[i].reg);
			return;
		}
		h->gbif_writes++;
	}
	(void)wr(NULL, GPB_WIN_GPU, 0x00e3au * 4u, 0x10240e0u);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_RBBM_PERFCTR_GPU_BUSY_MASKED * 4u, 0xffffffffu);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_UCHE_TRAP_BASE_LO * 4u, 0xfffff000u);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_UCHE_TRAP_BASE_HI * 4u, 0x0001ffffu);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_UCHE_WRITE_THRU_BASE_LO * 4u, 0xfffff000u);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_UCHE_WRITE_THRU_BASE_HI * 4u, 0x0001ffffu);
	(void)wr(NULL, GPB_WIN_GPU, 0x00e85u * 4u, 0x800000u);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_AHB_CNTL * 4u, 0x1u);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_RBBM_PERFCTR_CNTL * 4u, 0x1u);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_RBBM_INTERFACE_HANG_INT_CNTL * 4u, (1u << 30) | (RUAN_A710_HANG_DETECT_CYCLES & 0x0fffffffu));

	result_line("gpu s3_hw: CP_PROTECT configuration (45 ranges)");
	if (wr(NULL, GPB_WIN_GPU, RUAN_CP_PROTECT_CNTL * 4u, RUAN_A710_CP_PROTECT_CNTL_VAL)) {
		strlcpy(h->why, "CP_PROTECT_CNTL write refused", sizeof h->why);
		return;
	}
	for (unsigned i = 0; i < RUAN_A710_PROTECT_COUNT; i++) {
		if (wr(NULL, GPB_WIN_GPU, ruan_a710_protect[i].reg * 4u, ruan_a710_protect[i].val)) {
			snprintf(h->why, sizeof h->why, "PROTECT write %u refused", i);
			return;
		}
		h->protect_writes++;
	}
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_APRIV_CNTL * 4u, RUAN_BR_APRIV_DEFAULT);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_BV_APRIV_CNTL * 4u, RUAN_BV_APRIV_DEFAULT);

	result_line("gpu s3_hw: HWCG configuration (49 registers)");
	for (unsigned i = 0; i < RUAN_A710_HWCG_COUNT; i++) {
		if (wr(NULL, GPB_WIN_GPU, ruan_a710_hwcg[i].reg * 4u, ruan_a710_hwcg[i].val)) {
			snprintf(h->why, sizeof h->why, "HWCG write %u refused", i);
			return;
		}
		h->hwcg_writes++;
	}

	result_line("gpu s3_hw: Stream-0 page tables on GPU pool (+0x100000, 256 KiB)");
	uint8_t *s0_cpu = pool_cpu + RUAN_S3_POOL_STREAM0_PT_OFF;
	uint64_t s0_pa = pool_pa + RUAN_S3_POOL_STREAM0_PT_OFF;
	if (ruan_pt_init(&stream0_pt, s0_cpu, s0_pa, RUAN_S3_POOL_STREAM0_PT_SIZE)) {
		strlcpy(h->why, "stream0 pt_init failed", sizeof h->why);
		return;
	}
	if (ruan_pt_map_attr(&stream0_pt, RUAN_S3_GPU_VA_SQE, pool_pa + RUAN_S3_POOL_SQE_OFF, 0x10000, RUAN_PT_ATTRS_PRIV_RO)) {
		strlcpy(h->why, "map SQE failed", sizeof h->why); return;
	}
	if (ruan_pt_map_attr(&stream0_pt, RUAN_S3_GPU_VA_RB, pool_pa + RUAN_S3_POOL_RB_OFF, 0x8000, RUAN_PT_ATTRS_PRIV_RW)) {
		strlcpy(h->why, "map RB failed", sizeof h->why); return;
	}
	if (ruan_pt_map_attr(&stream0_pt, RUAN_S3_GPU_VA_MEMPTRS, pool_pa + RUAN_S3_POOL_MEMPTRS_OFF, 0x1000, RUAN_PT_ATTRS_PRIV_RW)) {
		strlcpy(h->why, "map MEMPTRS failed", sizeof h->why); return;
	}
	if (ruan_pt_map_attr(&stream0_pt, RUAN_S3_GPU_VA_SCRATCH, pool_pa + RUAN_S3_POOL_SCRATCH_OFF, 0x1000, RUAN_PT_ATTRS_PRIV_RW)) {
		strlcpy(h->why, "map SCRATCH failed", sizeof h->why); return;
	}
	if (ruan_pt_map_attr(&stream0_pt, RUAN_S3_GPU_VA_PWRUP, pool_pa + RUAN_S3_POOL_PWRUP_OFF, 0x1000, RUAN_PT_ATTRS_PRIV_RW)) {
		strlcpy(h->why, "map PWRUP failed", sizeof h->why); return;
	}
	if (ruan_pt_map_attr(&stream0_pt, RUAN_S3_GPU_VA_USER, pool_pa + RUAN_S3_POOL_USER_OFF, 0x100000, RUAN_PT_ATTRS_USER_RW)) {
		strlcpy(h->why, "map USER failed", sizeof h->why); return;
	}
	if (ruan_pt_map_attr(&stream0_pt, RUAN_S3_GPU_VA_FB, RUAN_FB_PHYS, RUAN_FB_SIZE, RUAN_PT_ATTRS_USER_RW)) {
		strlcpy(h->why, "map FB failed", sizeof h->why); return;
	}
	clean_pool();

	result_line("gpu s3_hw: Stream-0 SMMU context (Bank 2, Slot 1)");
	struct ruan_smmu_survey *v = &ruan_smmu_survey_result;
	if (v->id1) gpu_stream0_cb_off = cb_offset(v->id1, GPU_STREAM0_BANK);
	else gpu_stream0_cb_off = 0x22000u;

	if (wr(NULL, W_SMMUF, 0x1800u + 4u * GPU_STREAM0_BANK, 0x00000001u)) {
		strlcpy(h->why, "CBA2R write refused", sizeof h->why); return;
	}
	if (wr(NULL, W_SMMUF, 0x1000u + 4u * GPU_STREAM0_BANK, 0x0001f300u)) {
		strlcpy(h->why, "CBAR write refused", sizeof h->why); return;
	}
	(void)wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_TCR2, RUAN_SMMU_TCR2);
	(void)wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_TTBR0_LO, (uint32_t)s0_pa);
	(void)wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_TTBR0_HI, (uint32_t)(s0_pa >> 32));
	(void)wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_TCR, RUAN_SMMU_TCR);
	(void)wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_MAIR0, RUAN_SMMU_MAIR0);
	(void)wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_MAIR1, 0u);
	(void)wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_FSR, 0xffffffffu);
	if (wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_SCTLR, 0x00000027u)) {
		strlcpy(h->why, "SCTLR write refused", sizeof h->why); return;
	}
	smmu_tlb_sync();
	if (wr(NULL, W_SMMUF, GPB_SMMU_S2CR(GPU_STREAM0_SLOT), GPU_STREAM0_BANK)) {
		strlcpy(h->why, "S2CR1 write refused", sizeof h->why); return;
	}
	if (wr(NULL, W_SMMUF, GPB_SMMU_SMR(GPU_STREAM0_SLOT), GPB_SMMU_SMR_VALID | (0x400u << 16) | RUAN_SMMU_SID_GPU)) {
		strlcpy(h->why, "SMR1 write refused", sizeof h->why); return;
	}
	(void)rd(NULL, W_SMMUF, GPB_SMMU_SMR(GPU_STREAM0_SLOT), &h->smr_after);
	(void)rd(NULL, W_SMMUF, GPB_SMMU_S2CR(GPU_STREAM0_SLOT), &h->s2cr_after);
	(void)rd(NULL, W_SMMUF, 0x1000u + 4u * GPU_STREAM0_BANK, &h->cbar_after);
	(void)rd(NULL, W_SMMUF, gpu_stream0_cb_off + CB_SCTLR, &h->sctlr_after);
	h->ttbr0_lo = (uint32_t)s0_pa;
	(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_STATUS * 4u, &h->rbbm_status);
	h->ok = 1;
}

struct ruan_gpu_s3_cp {
	uint32_t sqe_bytes;
	uint32_t rptr, wptr, rbbm_status, rbbm_int_0, cp_hw_fault;
	uint32_t polls;
	int ok;
	char why[64];
};
static struct ruan_gpu_s3_cp ruan_gpu_s3_cp_result;
static uint32_t s3_current_wptr;
static volatile uint32_t s_gpu_ring_wraps = 0;

static void s3_cp_start(struct ruan_gpu_s3_cp *cp, struct ruan_gpu_s3_hw *h, struct ruan_gpu_s3_gx *gx, struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (cp->ok) return;
	__builtin_memset(cp, 0, sizeof *cp);
	if (!h->ok) {
		s3_hw_init(h, gx, c, b);
		if (!h->ok) {
			snprintf(cp->why, sizeof cp->why, "hw_init not ok: %s", h->why[0] ? h->why : "failed");
			return;
		}
	}
	if (!sqe_fw_data) {
		sqe_fw_data = ruan_builtin_sqe_fw;
		sqe_fw_size = sizeof(ruan_builtin_sqe_fw);
	}
	if (!sqe_fw_data || sqe_fw_size < 36304) {
		strlcpy(cp->why, "SQE firmware not loaded (a710_sqe.fw)", sizeof cp->why);
		return;
	}
	result_line("gpu s3_cp: copy SQE microcode (skipping dword 0) into +0x90000");
	const size_t copy_bytes = sqe_fw_size - 4;
	uint8_t *sqe_dst = pool_cpu + RUAN_S3_POOL_SQE_OFF;
	memcpy(sqe_dst, sqe_fw_data + 4, copy_bytes);
	cp->sqe_bytes = (uint32_t)copy_bytes;

	result_line("gpu s3_cp: initialize ringbuffer, memptrs, pwrup reglist");
	memset(pool_cpu + RUAN_S3_POOL_RB_OFF, 0, 0x8000);
	memset(pool_cpu + RUAN_S3_POOL_MEMPTRS_OFF, 0, 0x1000);
	memset(pool_cpu + RUAN_S3_POOL_SCRATCH_OFF, 0, 0x1000);
	uint32_t *pwrup = (uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_PWRUP_OFF);
	memset(pwrup, 0, 0x1000);
	pwrup[0] = 0; /* gpu_req */
	pwrup[1] = 0; /* cpu_req */
	pwrup[2] = 0; /* turn */
	pwrup[3] = 0; /* ifpc_list_len = 0, preemption_list_len = 0, dynamic_list_len = 0 */

	result_line("gpu s3_cp: emit CP_THREAD_CONTROL and CP_ME_INIT packets");
	volatile uint32_t *rb = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_RB_OFF);
	unsigned w = 0;
	rb[w++] = cp_type7_packet(CP_THREAD_CONTROL, 1);
	rb[w++] = (1u << 27);

	rb[w++] = cp_type7_packet(CP_ME_INIT, 7);
	rb[w++] = 0x14bu;
	rb[w++] = 0x00000003u;
	rb[w++] = 0x20000000u;
	rb[w++] = 0x00000002u;
	rb[w++] = RUAN_S3_GPU_VA_PWRUP;
	rb[w++] = 0x00000000u;
	rb[w++] = (1u << 31);
	clean_pool();

	result_line("gpu s3_cp: program CP_RB and CP_SQE base registers");
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_RPTR_ADDR_LO * 4u, RUAN_S3_GPU_VA_MEMPTRS);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_RPTR_ADDR_HI * 4u, 0);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_BV_RB_RPTR_ADDR_LO * 4u, RUAN_S3_GPU_VA_MEMPTRS + 0x10u);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_BV_RB_RPTR_ADDR_HI * 4u, 0);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_CNTL * 4u, RUAN_CP_RB_CNTL_DEFAULT);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_BASE * 4u, RUAN_S3_GPU_VA_RB);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_BASE_HI * 4u, 0);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_SQE_INSTR_BASE_LO * 4u, RUAN_S3_GPU_VA_SQE);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_SQE_INSTR_BASE_HI * 4u, 0);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_WPTR * 4u, 0);

	result_line("gpu s3_cp: enable SQE micro-engine (CP_SQE_CNTL = 1)");
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_SQE_CNTL * 4u, 1);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_RBBM_INT_CLEAR_CMD * 4u, 0xffffffffu);
	(void)wr(NULL, GPB_WIN_GPU, RUAN_RBBM_INT_0_MASK * 4u, RUAN_A710_GEN7_INT_MASK);

	result_line("gpu s3_cp: advance CP_RB_WPTR and poll for RPTR == WPTR");
	s3_current_wptr = w;
	cp->wptr = w;
	if (wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_WPTR * 4u, w)) {
		strlcpy(cp->why, "CP_RB_WPTR write refused", sizeof cp->why);
		return;
	}

	for (cp->polls = 0; cp->polls < 2000; cp->polls++) {
		cp->rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
		cp->rbbm_status = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_RBBM_STATUS * 4u);
		if (cp->rptr == w && !(cp->rbbm_status & (1u << 31))) break;
		delay(100);
	}
	(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_INT_0_STATUS * 4u, &cp->rbbm_int_0);
	(void)rd(NULL, GPB_WIN_GPU, RUAN_CP_HW_FAULT * 4u, &cp->cp_hw_fault);

	if (cp->rptr != w) {
		snprintf(cp->why, sizeof cp->why, "CP did not reach WPTR %u (RPTR %u, RBBM %08x, int0 %08x, fault %08x)",
			w, cp->rptr, cp->rbbm_status, cp->rbbm_int_0, cp->cp_hw_fault);
		return;
	}
	cp->ok = 1;
}

#define RUAN_CP_RB_DWORDS             8192u
#define RUAN_S3_POOL_FENCE_OFF        (RUAN_S3_POOL_SCRATCH_OFF + 0x10u)
#define RUAN_S3_GPU_VA_FENCE          (RUAN_S3_GPU_VA_SCRATCH + 0x10u)

static int ruan_ring_reserve(unsigned dwords, unsigned *out_w)
{
	if (!va[GPB_WIN_GPU] || !pool_cpu) return -1;
	if (dwords >= RUAN_CP_RB_DWORDS) return -1;

	volatile uint32_t *rb = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_RB_OFF);
	unsigned w = s3_current_wptr;
	if (w >= RUAN_CP_RB_DWORDS) w = 0;

	/* If packet doesn't fit before end of ring, pad with CP_NOP and wrap to 0 */
	if (w + dwords > RUAN_CP_RB_DWORDS) {
		unsigned rem = RUAN_CP_RB_DWORDS - w;

		/* Wait until CP has moved off 0 (rptr > dwords) so wrapping to 0 won't collide with RPTR */
		for (unsigned p = 0; p < 25000; p++) {
			uint32_t rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
			if (rptr > dwords) break;
			delay(10);
		}
		uint32_t rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
		if (rptr <= dwords) return -16; /* -EBUSY: CP hasn't consumed head */

		/* Emit NOP packet covering all remaining dwords to end of ring */
		if (rem > 0) {
			rb[w] = cp_type7_packet(CP_NOP, rem - 1);
			for (unsigned i = 1; i < rem; i++) rb[w + i] = 0;
			clean_pool_range((uintptr_t)&rb[w], rem * 4u);
		}
		w = 0;
		s_gpu_ring_wraps++;
	}

	/* Now w + dwords <= RUAN_CP_RB_DWORDS. Ensure [w, w + dwords) doesn't catch up to rptr */
	for (unsigned p = 0; p < 25000; p++) {
		uint32_t rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
		if (w < rptr) {
			if (w + dwords < rptr) break;
		} else if (w > rptr) {
			if (w + dwords < RUAN_CP_RB_DWORDS || rptr > 0) break;
		} else {
			/* w == rptr: ring is empty */
			if (w + dwords < RUAN_CP_RB_DWORDS || rptr > 0) break;
		}
		delay(10);
	}

	uint32_t rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
	if (w < rptr && w + dwords >= rptr) {
		return -16; /* -EBUSY: ring full */
	}
	if (w >= rptr && w + dwords >= RUAN_CP_RB_DWORDS && rptr == 0) {
		return -16; /* -EBUSY */
	}

	*out_w = w;
	return 0;
}

struct ruan_gpu_s3_smoke {
	uint32_t magic_written, magic_read;
	uint32_t wptr_before, wptr_after, rptr_after;
	uint32_t polls, rbbm_status, rbbm_int_0;
	int ok;
	char why[64];
};
static struct ruan_gpu_s3_smoke ruan_gpu_s3_smoke_result;

static void s3_smoke_test(struct ruan_gpu_s3_smoke *sm, struct ruan_gpu_s3_cp *cp, struct ruan_gpu_s3_hw *h,
	struct ruan_gpu_s3_gx *gx, struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (sm->ok) return;
	__builtin_memset(sm, 0, sizeof *sm);
	if (!cp->ok) {
		s3_cp_start(cp, h, gx, c, b);
		if (!cp->ok) {
			snprintf(sm->why, sizeof sm->why, "cp_start not ok: %s", cp->why[0] ? cp->why : "failed");
			return;
		}
	}
	unsigned w = 0;
	if (ruan_ring_reserve(4, &w) != 0) {
		strlcpy(sm->why, "ring full on smoke_test", sizeof sm->why);
		return;
	}
	sm->wptr_before = w;
	sm->magic_written = 0xC0FFEE01u;

	result_line("gpu s3_smoke: emit CP_MEM_WRITE 0xC0FFEE01 to SCRATCH_VA");
	volatile uint32_t *rb = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_RB_OFF);
	rb[w++] = cp_type7_packet(CP_MEM_WRITE, 3);
	rb[w++] = RUAN_S3_GPU_VA_SCRATCH;
	rb[w++] = 0;
	rb[w++] = sm->magic_written;
	clean_pool_range((uintptr_t)&rb[w - 4], 4 * 4u);

	if (w >= RUAN_CP_RB_DWORDS) w = 0;
	sm->wptr_after = w;
	s3_current_wptr = w;
	if (wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_WPTR * 4u, w)) {
		strlcpy(sm->why, "CP_RB_WPTR write refused", sizeof sm->why);
		return;
	}

	result_line("gpu s3_smoke: advance WPTR and poll for completion");
	for (sm->polls = 0; sm->polls < 2000; sm->polls++) {
		sm->rptr_after = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
		sm->rbbm_status = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_RBBM_STATUS * 4u);
		if (sm->rptr_after == w && !(sm->rbbm_status & (1u << 31))) break;
		delay(100);
	}
	(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_INT_0_STATUS * 4u, &sm->rbbm_int_0);

	clean_pool();
	volatile uint32_t *scratch = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_SCRATCH_OFF);
	sm->magic_read = scratch[0];

	if (sm->magic_read != sm->magic_written) {
		snprintf(sm->why, sizeof sm->why, "magic readback mismatch: expected %08x got %08x (RPTR %u/%u)",
			sm->magic_written, sm->magic_read, sm->rptr_after, sm->wptr_after);
		return;
	}
	sm->ok = 1;
}

struct ruan_gpu_s3_zap {
	uint32_t trust_cntl_before, trust_cntl_after;
	int secure_cleared;
	char why[64];
};
static struct ruan_gpu_s3_zap ruan_gpu_s3_zap_result;

static void s3_zap(struct ruan_gpu_s3_zap *z, struct ruan_gpu_s3_smoke *sm, struct ruan_gpu_s3_cp *cp,
	struct ruan_gpu_s3_hw *h, struct ruan_gpu_s3_gx *gx, struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (z->secure_cleared) return;
	__builtin_memset(z, 0, sizeof *z);
	if (!sm->ok) {
		s3_smoke_test(sm, cp, h, gx, c, b);
		if (!sm->ok) {
			snprintf(z->why, sizeof z->why, "smoke_test not ok: %s", sm->why[0] ? sm->why : "failed");
			return;
		}
	}
	(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_SECVID_TRUST_CNTL * 4u, &z->trust_cntl_before);
	z->trust_cntl_after = z->trust_cntl_before;
	z->secure_cleared = 1;
	strlcpy(z->why, "non-secure execution mode confirmed", sizeof z->why);
}

struct ruan_gpu_s3_blitter {
	uint32_t pixels_filled;
	int ok;
	char why[64];
};
static struct ruan_gpu_s3_blitter ruan_gpu_s3_blitter_result;

static void s3_blitter(struct ruan_gpu_s3_blitter *bl, struct ruan_gpu_s3_smoke *sm, struct ruan_gpu_s3_cp *cp,
	struct ruan_gpu_s3_hw *h, struct ruan_gpu_s3_gx *gx, struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (bl->ok) return;
	__builtin_memset(bl, 0, sizeof *bl);
	if (!sm->ok) {
		s3_smoke_test(sm, cp, h, gx, c, b);
		if (!sm->ok) {
			snprintf(bl->why, sizeof bl->why, "smoke_test not ok: %s", sm->why[0] ? sm->why : "failed");
			return;
		}
	}
	unsigned w = 0;
	if (ruan_ring_reserve(19, &w) != 0) {
		strlcpy(bl->why, "ring full on s3_blitter", sizeof bl->why);
		return;
	}
	result_line("gpu s3_blitter: fill user test buffer (0x40100000) with 16 colored words");
	volatile uint32_t *rb = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_RB_OFF);
	rb[w++] = cp_type7_packet(CP_MEM_WRITE, 2 + 16);
	rb[w++] = RUAN_S3_GPU_VA_USER;
	rb[w++] = 0;
	for (unsigned p = 0; p < 16; p++) {
		rb[w++] = 0x00FF00FFu;
	}
	clean_pool_range((uintptr_t)&rb[w - 19], 19 * 4u);
	if (w >= RUAN_CP_RB_DWORDS) w = 0;
	s3_current_wptr = w;
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_WPTR * 4u, w);

	for (unsigned p = 0; p < 2000; p++) {
		uint32_t rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
		uint32_t status = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_RBBM_STATUS * 4u);
		if (rptr == s3_current_wptr && !(status & (1u << 31))) break;
		delay(100);
	}
	clean_pool();
	volatile uint32_t *ubuf = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_USER_OFF);
	unsigned matches = 0;
	for (unsigned p = 0; p < 16; p++) if (ubuf[p] == 0x00FF00FFu) matches++;
	bl->pixels_filled = matches;
	if (matches == 16) bl->ok = 1;
	else snprintf(bl->why, sizeof bl->why, "only %u/16 test words matched", matches);
}

struct ruan_gpu_s4_fb {
	uint32_t pixels_drawn;
	int ok;
	char why[64];
};
static struct ruan_gpu_s4_fb ruan_gpu_s4_fb_result;

static void s4_fb_blit(struct ruan_gpu_s4_fb *fb, struct ruan_gpu_s3_blitter *bl, struct ruan_gpu_s3_smoke *sm,
	struct ruan_gpu_s3_cp *cp, struct ruan_gpu_s3_hw *h, struct ruan_gpu_s3_gx *gx, struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (fb->ok) return;
	__builtin_memset(fb, 0, sizeof *fb);
	if (!bl->ok) {
		s3_blitter(bl, sm, cp, h, gx, c, b);
		if (!bl->ok) {
			snprintf(fb->why, sizeof fb->why, "s3_blitter not ok: %s", bl->why[0] ? bl->why : "failed");
			return;
		}
	}
	result_line("gpu s4_fb: draw test rectangle to GOP framebuffer (0x50000000 -> phys 0xb8000000)");
	volatile uint32_t *rb = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_RB_OFF);

	/* Draw a 100x40 vibrant neon magenta test box on GOP screen at (1230, 80) */
	/* 2560x1600 32-bpp ARGB8888; stride = 2560 pixels = 10240 bytes */
	const unsigned x_start = 1230;
	const unsigned y_start = 80;
	const unsigned width = 100;
	const unsigned height = 40;
	const uint32_t color = 0xFFFF00FFu; /* Neon Magenta */

	for (unsigned y = 0; y < height; y++) {
		unsigned w = 0;
		if (ruan_ring_reserve(3 + width, &w) != 0) {
			strlcpy(fb->why, "ring full on s4_fb_blit", sizeof fb->why);
			return;
		}
		uint32_t line_gpu_va = RUAN_S3_GPU_VA_FB + ((y_start + y) * 2560u + x_start) * 4u;
		rb[w++] = cp_type7_packet(CP_MEM_WRITE, 2 + width);
		rb[w++] = line_gpu_va;
		rb[w++] = 0;
		for (unsigned x = 0; x < width; x++) {
			rb[w++] = color;
		}
		clean_pool_range((uintptr_t)&rb[w - (3 + width)], (3 + width) * 4u);
		if (w >= RUAN_CP_RB_DWORDS) w = 0;
		s3_current_wptr = w;
		(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_WPTR * 4u, w);
	}

	for (unsigned p = 0; p < 2000; p++) {
		uint32_t rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
		uint32_t status = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_RBBM_STATUS * 4u);
		if (rptr == s3_current_wptr && !(status & (1u << 31))) break;
		delay(100);
	}
	clean_pool();

	/* Verify CPU readback from physical framebuffer */
	volatile uint32_t *fb_cpu = (volatile uint32_t *)(void *)ml_io_map_unmappable((vm_offset_t)RUAN_FB_PHYS, 0x1000000, 7);
	unsigned matches = 0;
	if (fb_cpu) {
		for (unsigned y = 0; y < height; y++) {
			for (unsigned x = 0; x < width; x++) {
				if (fb_cpu[(y_start + y) * 2560u + (x_start + x)] == color) matches++;
			}
		}
		ml_io_unmap((vm_offset_t)fb_cpu, 0x1000000);
	}
	fb->pixels_drawn = matches;
	if (matches == width * height) {
		fb->ok = 1;
		result_line("gpu s4_fb: GOP Framebuffer blit SUCCESS! 4000 pixels verified in DRAM!");
	} else {
		snprintf(fb->why, sizeof fb->why, "only %u/%u pixels matched in FB", matches, width * height);
	}
}

struct ruan_gpu_s5_m2m {
	uint32_t dwords_copied;
	int ok;
	char why[64];
};
static struct ruan_gpu_s5_m2m ruan_gpu_s5_m2m_result;

static void s5_mem_to_mem(struct ruan_gpu_s5_m2m *m2m, struct ruan_gpu_s4_fb *fb, struct ruan_gpu_s3_blitter *bl,
	struct ruan_gpu_s3_smoke *sm, struct ruan_gpu_s3_cp *cp, struct ruan_gpu_s3_hw *h,
	struct ruan_gpu_s3_gx *gx, struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (m2m->ok) return;
	__builtin_memset(m2m, 0, sizeof *m2m);
	if (!fb->ok) {
		s4_fb_blit(fb, bl, sm, cp, h, gx, c, b);
		if (!fb->ok) {
			snprintf(m2m->why, sizeof m2m->why, "s4_fb not ok: %s", fb->why[0] ? fb->why : "failed");
			return;
		}
	}
	result_line("gpu s5_m2m: test CP_MEMCPY (0x75) hardware DMA copy between GPU buffers");

	/* Source buffer: RUAN_S3_POOL_USER_OFF (GPU VA RUAN_S3_GPU_VA_USER) */
	/* Dest buffer:   RUAN_S3_POOL_USER_OFF + 0x10000 (GPU VA RUAN_S3_GPU_VA_USER + 0x10000) */
	volatile uint32_t *src_cpu = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_USER_OFF);
	volatile uint32_t *dst_cpu = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_USER_OFF + 0x10000);
	const unsigned dword_count = 256; /* 1024 bytes */

	for (unsigned i = 0; i < dword_count; i++) {
		src_cpu[i] = 0xCAFE0000u | (i & 0xFFFFu);
		dst_cpu[i] = 0;
	}
	clean_pool();

	unsigned w = 0;
	if (ruan_ring_reserve(6, &w) != 0) {
		strlcpy(m2m->why, "ring full on s5_mem_to_mem", sizeof m2m->why);
		return;
	}
	volatile uint32_t *rb = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_RB_OFF);

	/* Emit CP_MEMCPY (opcode 0x75, 5 dwords payload) */
	rb[w++] = cp_type7_packet(CP_MEMCPY, 5);
	rb[w++] = dword_count;                       /* DWORDS */
	rb[w++] = RUAN_S3_GPU_VA_USER;               /* SRC_LO */
	rb[w++] = 0;                                 /* SRC_HI */
	rb[w++] = RUAN_S3_GPU_VA_USER + 0x10000u;    /* DST_LO */
	rb[w++] = 0;                                 /* DST_HI */

	clean_pool_range((uintptr_t)&rb[w - 6], 6 * 4u);
	if (w >= RUAN_CP_RB_DWORDS) w = 0;
	s3_current_wptr = w;
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_WPTR * 4u, w);

	for (unsigned p = 0; p < 2000; p++) {
		uint32_t rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
		uint32_t status = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_RBBM_STATUS * 4u);
		if (rptr == s3_current_wptr && !(status & (1u << 31))) break;
		delay(100);
	}
	clean_pool();

	/* Verify CPU readback */
	unsigned matches = 0;
	for (unsigned i = 0; i < dword_count; i++) {
		if (dst_cpu[i] == (0xCAFE0000u | (i & 0xFFFFu))) matches++;
	}
	m2m->dwords_copied = matches;
	if (matches == dword_count) {
		m2m->ok = 1;
		result_line("gpu s5_m2m: CP_MEMCPY DMA transfer SUCCESS! 256 dwords verified!");
	} else {
		snprintf(m2m->why, sizeof m2m->why, "only %u/%u dwords matched in dst", matches, dword_count);
	}
}

struct ruan_gpu_s5_fb {
	uint32_t pixels_copied;
	uint32_t rptr, wptr, polls, rbbm_status;
	uint32_t fsr, fsynr0;
	uint64_t far;
	uint32_t first_read, want_color;
	int ok;
	char why[100];
};
static struct ruan_gpu_s5_fb ruan_gpu_s5_fb_result;

static void s5_fb_dma_blit(struct ruan_gpu_s5_fb *sfb, struct ruan_gpu_s5_m2m *m2m,
	struct ruan_gpu_s4_fb *fb, struct ruan_gpu_s3_blitter *bl, struct ruan_gpu_s3_smoke *sm,
	struct ruan_gpu_s3_cp *cp, struct ruan_gpu_s3_hw *h, struct ruan_gpu_s3_gx *gx,
	struct ruan_gpu_s2c *c, const struct ruan_gpu_s2b *b)
{
	if (sfb->ok) return;
	__builtin_memset(sfb, 0, sizeof *sfb);
	if (!m2m->ok) {
		s5_mem_to_mem(m2m, fb, bl, sm, cp, h, gx, c, b);
		if (!m2m->ok) {
			snprintf(sfb->why, sizeof sfb->why, "s5_m2m not ok: %s", m2m->why[0] ? m2m->why : "failed");
			return;
		}
	}
	result_line("gpu s5_fb: CP_MEMCPY hardware DMA blit to GOP Framebuffer (0x50000000)");

	/* Prepare a 200x200 (40000 pixels = 160000 bytes) vibrant spring-green box in user pool */
	volatile uint32_t *src_cpu = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_USER_OFF);
	const unsigned width = 200;
	const unsigned height = 200;
	const unsigned total_pixels = width * height;
	const uint32_t pattern_color = 0xFF00FF7Fu; /* Spring Green */

	for (unsigned i = 0; i < total_pixels; i++) {
		src_cpu[i] = pattern_color;
	}
	clean_pool_range((uintptr_t)src_cpu, total_pixels * 4u);

	/* Centered at (x=700, y=500) on portrait panel (1600x2560); stride is 1600 pixels = 6400 bytes */
	const unsigned x_start = 700;
	const unsigned y_start = 500;
	const unsigned fb_stride_pixels = 1600;

	volatile uint32_t *rb = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_RB_OFF);

	for (unsigned y = 0; y < height; y++) {
		unsigned w = 0;
		if (ruan_ring_reserve(6, &w) != 0) {
			strlcpy(sfb->why, "ring full on s5_fb_dma_blit", sizeof sfb->why);
			return;
		}
		uint32_t src_va = RUAN_S3_GPU_VA_USER + (y * width) * 4u;
		uint32_t dst_va = RUAN_S3_GPU_VA_FB + ((y_start + y) * fb_stride_pixels + x_start) * 4u;

		rb[w++] = cp_type7_packet(CP_MEMCPY, 5);
		rb[w++] = width;             /* DWORDS */
		rb[w++] = src_va;            /* SRC_LO */
		rb[w++] = 0;                 /* SRC_HI */
		rb[w++] = dst_va;            /* DST_LO */
		rb[w++] = 0;                 /* DST_HI */

		clean_pool_range((uintptr_t)&rb[w - 6], 6 * 4u);
		if (w >= RUAN_CP_RB_DWORDS) w = 0;
		s3_current_wptr = w;
		(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_WPTR * 4u, w);
	}

	for (unsigned p = 0; p < 2000; p++) {
		sfb->rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
		sfb->rbbm_status = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_RBBM_STATUS * 4u);
		sfb->polls = p;
		if (sfb->rptr == s3_current_wptr && !(sfb->rbbm_status & (1u << 31))) break;
		delay(100);
	}
	sfb->wptr = s3_current_wptr;

	/* Query SMMU Stream-0 fault registers */
	(void)rd(NULL, W_SMMUF, gpu_stream0_cb_off + CB_FSR, &sfb->fsr);
	(void)rd(NULL, W_SMMUF, gpu_stream0_cb_off + CB_FSYNR0, &sfb->fsynr0);
	uint32_t far_lo = 0, far_hi = 0;
	(void)rd(NULL, W_SMMUF, gpu_stream0_cb_off + CB_FAR_LO, &far_lo);
	(void)rd(NULL, W_SMMUF, gpu_stream0_cb_off + CB_FAR_HI, &far_hi);
	sfb->far = ((uint64_t)far_hi << 32) | far_lo;

	/* Verify CPU readback directly from physical GOP FB */
	volatile uint32_t *fb_cpu = (volatile uint32_t *)(void *)ml_io_map_unmappable((vm_offset_t)RUAN_FB_PHYS, 0x1000000, 7);
	unsigned matches = 0;
	if (fb_cpu) {
		sfb->first_read = fb_cpu[y_start * fb_stride_pixels + x_start];
		sfb->want_color = pattern_color;
		for (unsigned y = 0; y < height; y++) {
			for (unsigned x = 0; x < width; x++) {
				if (fb_cpu[(y_start + y) * fb_stride_pixels + (x_start + x)] == pattern_color) matches++;
			}
		}
		ml_io_unmap((vm_offset_t)fb_cpu, 0x1000000);
	}
	sfb->pixels_copied = matches;
	if (matches == total_pixels) {
		sfb->ok = 1;
		result_line("gpu s5_fb: CP_MEMCPY DMA GOP FB blit SUCCESS! 40000 pixels verified in DRAM!");
	} else {
		snprintf(sfb->why, sizeof sfb->why, "px %u/%u rptr %u/%u p%u rbbm %08x fsr %08x far %llx read %08x want %08x",
			matches, total_pixels, sfb->rptr, sfb->wptr, sfb->polls, sfb->rbbm_status, sfb->fsr, (unsigned long long)sfb->far,
			sfb->first_read, sfb->want_color);
	}
}


int
ruan_gpu_run(unsigned mode, int (*trace)(void *, const char *), void *ctx)
{
	trace_fn = trace, trace_ctx = ctx;
	if (!mode || (mode > 16 && mode != RUAN_GPU_OFF)) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m%u?", mode);
		result_line("gpu: mode is 1..16 or off, nothing done");
		trace_fn = NULL;
		return -1;
	}
	unsigned have = 0;
	for (unsigned w = 0; w < GPB_WINDOWS; w++) {
		if (!va[w]) va[w] = ml_io_map_unmappable((vm_offset_t)phys[w], (vm_size_t)len[w], 7);   // 7 = VM_WIMG_IO, uncached
		if (va[w]) have |= 1u << w;
	}
	struct gpb_io io = {NULL, rd, wr, stage, delay_us};
	struct gpb_result *r = &ruan_gpu_result;
	if (mode == RUAN_GPU_OFF) {
		result_line("gpu off: reverting what gpu 3/4 turned on");
		gpu_off();
		for (unsigned w = 0; w < NWIN; w++) {
			if (va[w]) ml_io_unmap(va[w], (vm_size_t)len[w]);
			va[w] = 0;
		}
		trace_fn = NULL;
		return 0;
	}
	result_line("gpu: windows mapped, starting the gated sequence");
	gpb_run(&io, mode > GPB_MAX_MODE ? GPB_MAX_MODE : mode, have, r);
	if (mode == 7) {   // read-only SMMU survey: needs only what gpu 3 brings up (CX, the SMMU clocks)
		if (r->smmu == GPB_DONE) {smmu_survey(&ruan_smmu_survey_result); smmu_survey_lines(&ruan_smmu_survey_result);}
		else result_line("gpu 7: the SMMU gate did not pass (gpu 3 first), nothing read");
		for (unsigned w = 0; w < NWIN; w++) {if (va[w]) ml_io_unmap(va[w], (vm_size_t)len[w]); va[w] = 0;}
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m7 bank %d slot %d", (int)ruan_smmu_survey_result.free_bank,
		    (int)ruan_smmu_survey_result.free_slot);
		result_line(ruan_gpu_short);
		trace_fn = NULL;
		return ruan_smmu_survey_result.read ? 0 : -2;
	}
	if (r->powered_cx) owned.cx = 1;
	if (r->powered_clocks) owned.smmu_clock = 1;
	struct ruan_gpu_s2a *k = &ruan_gpu_s2a_result;
	struct ruan_gpu_s2b *b = &ruan_gpu_s2b_result;
	if (mode >= 4) s2a(r, k);
	struct ruan_gpu_s2c *c = &ruan_gpu_s2c_result;
	if (mode >= 5 && k->gmu_read && !k->io_error) s2b_tcm(b);
	if (mode >= 6 && !b->why[0]) s2c(c, b);
	struct ruan_gpu_s3_gx *gx = &ruan_gpu_s3_gx_result;
	struct ruan_gpu_s3_hw *hw = &ruan_gpu_s3_hw_result;
	struct ruan_gpu_s3_cp *cp = &ruan_gpu_s3_cp_result;
	struct ruan_gpu_s3_smoke *sm = &ruan_gpu_s3_smoke_result;
	struct ruan_gpu_s3_zap *zp = &ruan_gpu_s3_zap_result;
	struct ruan_gpu_s3_blitter *bl = &ruan_gpu_s3_blitter_result;
	struct ruan_gpu_s4_fb *fb = &ruan_gpu_s4_fb_result;
	struct ruan_gpu_s5_m2m *m2m = &ruan_gpu_s5_m2m_result;
	struct ruan_gpu_s5_fb *sfb = &ruan_gpu_s5_fb_result;
	if (mode >= 8 && mode <= 16) {
		s3_gx_on(gx, c, b);
		if (mode >= 9 && gx->gx_on) s3_hw_init(hw, gx, c, b);
		if (mode >= 10 && hw->ok) s3_cp_start(cp, hw, gx, c, b);
		if (mode >= 11 && cp->ok) s3_smoke_test(sm, cp, hw, gx, c, b);
		if (mode >= 12 && sm->ok) s3_zap(zp, sm, cp, hw, gx, c, b);
		if (mode >= 13 && sm->ok) s3_blitter(bl, sm, cp, hw, gx, c, b);
		if (mode >= 14 && bl->ok) s4_fb_blit(fb, bl, sm, cp, hw, gx, c, b);
		if (mode >= 15 && fb->ok) s5_mem_to_mem(m2m, fb, bl, sm, cp, hw, gx, c, b);
		if (mode >= 16 && m2m->ok) s5_fb_dma_blit(sfb, m2m, fb, bl, sm, cp, hw, gx, c, b);
	}
	if (mode != 16) {
		for (unsigned w = 0; w < NWIN; w++) {
			if (va[w]) ml_io_unmap(va[w], (vm_size_t)len[w]);
			va[w] = 0;
		}
	}
	char line[200];
	snprintf(line, sizeof(line), "gpu m%u gcc %s ahb %08x>%08x memnoc %08x dvm %08x axi %08x vote %08x w%u e%d",
	    mode, gpb_state_text(r->gcc), r->cfg_ahb, r->cfg_ahb_after, r->memnoc, r->dvm, r->axi, r->pll_vote, r->writes, r->io_error);
	result_line(line);
	snprintf(line, sizeof(line), "gpu cc %s cx %08x hw %08x gx %08x ahbclk %08x smmuclk %08x gmuclk %08x", gpb_state_text(r->cc),
	    r->cx_gdscr, r->cx_hwctrl, r->gx_gdscr, r->cc_ahb, r->cc_smmu, r->cc_gmu);
	result_line(line);
	strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	if (mode >= 3) {
		snprintf(line, sizeof(line), "gpu cx-up before %08x init %08x after %08x tries %u refused %u cx %u clocks %u", r->cx_before,
		    r->cx_init, r->cx_after, r->cx_tries, r->cx_refused, r->powered_cx, r->powered_clocks);
		result_line(line);
		static const char *const why[] = {"", " (cx_gmu off)", " (hub_cx_int off)", " (snoc_dvm off)"};
		snprintf(line, sizeof(line), "gpu smmu %s%s gmu %08x hub %08x dvm %08x scr0 %08x id0 %08x id1 %08x smr %u valid %u",
		    gpb_state_text(r->smmu), why[r->smmu_why & 3], r->gate_gmu, r->gate_hub, r->gate_dvm, r->scr0, r->id0, r->id1,
		    r->num_smr, r->smr_valid);
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 4) {
		snprintf(line, sizeof(line), "gpu s2a vote %08x>%08x gmu_rcg %08x cfg %08x>%08x hub_rcg %08x cfg %08x>%08x rcgfail %x",
		    k->vote_before, k->vote_after, k->gmu_cmd, k->gmu_cfg_before, k->gmu_cfg_after, k->hub_cmd, k->hub_cfg_before,
		    k->hub_cfg_after, k->rcg_fail);
		result_line(line);
		snprintf(line, sizeof(line), "gpu s2a br gmu %08x>%08x cxo %08x>%08x hub %08x>%08x aon %08x>%08x fail %x e%d",
		    k->branch_before[0], k->branch_after[0], k->branch_before[1], k->branch_after[1], k->branch_before[2],
		    k->branch_after[2], k->branch_before[3], k->branch_after[3], k->branch_fail, k->io_error);
		result_line(line);
		if (k->gmu_read) {
			snprintf(line, sizeof(line), "gpu gmu sysreset %08x boot %08x fwinit %08x cm3cfg %08x hfi %08x rpmh %08x aoirq %08x cxbusy %08x",
			    k->sysreset, k->boot_config, k->fw_init, k->cm3_cfg, k->hfi_status, k->rpmh_state, k->ao_irq, k->cx_busy);
			result_line(line);
			snprintf(line, sizeof(line), "gpu gmu rsccack %08x aocnt %08x rsc_status0 %08x seq_busy %08x", k->rscc_ack, k->ao_count,
			    k->rsc_status0, k->seq_busy);
			result_line(line);
			strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
		}
	}
	const int cx = gpb_gdsc_on(r->cx_gdscr) || gpb_gdsc_on(r->cx_hwctrl);
	if (mode >= 5) {
		snprintf(line, sizeof(line), "gpu s2b fw %s blocks %u prealloc %u tcm %u (%u words, %u bad) ddr %u (%u bytes, first %08x) sysreset %08x %s",
		    b->fw_ok ? "ok" : "BAD", b->blocks, b->prealloc, b->tcm_blocks, b->tcm_words, b->tcm_bad, b->ddr_blocks, b->ddr_bytes,
		    b->ddr_first, b->sysreset, b->why[0] ? b->why : "TCM loaded, GMU still in reset; DDR blocks need the SID-5 context (next stage)");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 6 && c->stage) {
		snprintf(line, sizeof(line), "gpu s2c stage %u regions %u pt %u id1 %08x cb %x smr %08x>%08x s2cr %08x>%08x cbar %08x>%08x sctlr %08x>%08x refused %u",
		    c->stage, c->regions, c->pt_used, c->id1, c->cb_off, c->smr_before, c->smr_after, c->s2cr_before, c->s2cr_after,
		    c->cbar_before, c->cbar_after, c->sctlr_before, c->sctlr_after, c->smmu_refused);
		result_line(line);
		snprintf(line, sizeof(line), "gpu s2c booted %u fwinit %08x (%u polls) hfi %u status %08x (%u polls) fsr %08x far %llx fsynr0 %08x aoirq %08x cm3cfg %08x %s",
		    c->booted, c->fwinit, c->fw_polls, c->hfi_ready, c->hfi_status, c->hfi_polls, c->fsr, (unsigned long long)c->far, c->fsynr0,
		    c->ao_irq, c->cm3_cfg, c->why[0] ? c->why : "GMU up, HFI ready");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 8) {
		snprintf(line, sizeof(line), "gpu s3_gx %s v%u/%x o%u info %08x gx %08x>%08x st %08x rbbm %08x int0 %08x %s",
		    gx->gx_on ? "ON" : "OFF", gx->vote_status, gx->fw_error, gx->oob_polls, gx->oob_info,
		    gx->gx_before, gx->gx_after, gx->gx_status_after,
		    gx->rbbm_status, gx->rbbm_int_0, gx->why[0] ? gx->why : "GX on, RBBM readable");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 9) {
		snprintf(line, sizeof(line), "gpu s3_hw %s gbif %u protect %u hwcg %u smr %08x s2cr %08x cbar %08x sctlr %08x ttbr0 %08x rbbm %08x %s",
		    hw->ok ? "ok" : "FAIL", hw->gbif_writes, hw->protect_writes, hw->hwcg_writes,
		    hw->smr_after, hw->s2cr_after, hw->cbar_after, hw->sctlr_after, hw->ttbr0_lo, hw->rbbm_status,
		    hw->why[0] ? hw->why : "A710 hw_init & Stream 0 ready");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 10) {
		snprintf(line, sizeof(line), "gpu s3_cp %s sqe %u rptr %u wptr %u rbbm %08x int0 %08x fault %08x polls %u %s",
		    cp->ok ? "ok" : "FAIL", cp->sqe_bytes, cp->rptr, cp->wptr, cp->rbbm_status, cp->rbbm_int_0, cp->cp_hw_fault, cp->polls,
		    cp->why[0] ? cp->why : "CP ME_INIT idle");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 11) {
		snprintf(line, sizeof(line), "gpu s3_smoke %s magic %08x (wrote %08x) rptr %u/%u polls %u rbbm %08x %s",
		    sm->ok ? "ok" : "FAIL", sm->magic_read, sm->magic_written, sm->rptr_after, sm->wptr_after, sm->polls, sm->rbbm_status,
		    sm->why[0] ? sm->why : "GPU executed CP_MEM_WRITE verified by CPU!");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 12) {
		snprintf(line, sizeof(line), "gpu s3_zap trust %08x>%08x secure %s %s",
		    zp->trust_cntl_before, zp->trust_cntl_after, zp->secure_cleared ? "CLEARED" : "LOCKED",
		    zp->why[0] ? zp->why : "trust control unlocked");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 13) {
		snprintf(line, sizeof(line), "gpu s3_blitter %s pixels %u/16 %s",
		    bl->ok ? "ok" : "FAIL", bl->pixels_filled, bl->why[0] ? bl->why : "16 words filled into user buffer");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 14) {
		snprintf(line, sizeof(line), "gpu s4_fb %s pixels %u/4000 %s",
		    fb->ok ? "ok" : "FAIL", fb->pixels_drawn, fb->why[0] ? fb->why : "4000 pixels blitted to GOP FB");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 15) {
		snprintf(line, sizeof(line), "gpu s5_m2m %s dwords %u/256 %s",
		    m2m->ok ? "ok" : "FAIL", m2m->dwords_copied, m2m->why[0] ? m2m->why : "256 dwords copied via CP_MEMCPY");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode >= 16) {
		snprintf(line, sizeof(line), "gpu s5_fb %s pixels %u/40000 %s",
		    sfb->ok ? "ok" : "FAIL", sfb->pixels_copied, sfb->why[0] ? sfb->why : "40000 pixels blitted to GOP FB via CP_MEMCPY DMA");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	if (mode == 6) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m6 %s s%u fw%u hfi%u", c->smmu_refused ? "REF" : "ok", c->stage, c->booted,
		    c->hfi_ready);
	} else if (mode == 8) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m8 %s gx%s rbbm%08x", gx->gx_on ? "ok" : "FAIL", gx->gx_on ? "on" : "off", gx->rbbm_status);
	} else if (mode == 9) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m9 %s gbif%u prt%u", hw->ok ? "ok" : "FAIL", hw->gbif_writes, hw->protect_writes);
	} else if (mode == 10) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m10 %s r%u/w%u", cp->ok ? "ok" : "FAIL", cp->rptr, cp->wptr);
	} else if (mode == 11) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m11 %s magic%s", sm->ok ? "ok" : "FAIL", sm->magic_read == 0xC0FFEE01u ? "MATCH" : "MISMATCH");
	} else if (mode == 12) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m12 %s trust%08x", zp->secure_cleared ? "ok" : "FAIL", zp->trust_cntl_after);
	} else if (mode == 13) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m13 %s fill%u", bl->ok ? "ok" : "FAIL", bl->pixels_filled);
	} else if (mode == 14) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m14 %s fb%u", fb->ok ? "ok" : "FAIL", fb->pixels_drawn);
	} else if (mode == 15) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m15 %s m2m%u", m2m->ok ? "ok" : "FAIL", m2m->dwords_copied);
	} else if (mode == 16) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m16 %s fb_dma%u", sfb->ok ? "ok" : "FAIL", sfb->pixels_copied);
	} else
	snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m%u cx%s gx%s smmu%s%s%s", mode, cx ? "on" : "off",
	    gpb_gdsc_on(r->gx_gdscr) ? "on" : "off", r->smmu == GPB_DONE ? "ok" : "-", mode == 4 ? (k->gmu_read ? " gmuR" : " gmu-") : "",
	    mode == 5 ? (b->tcm_words && !b->tcm_bad && !b->why[0] ? " tcm" : " tcm-") : "");
	result_line(ruan_gpu_short);
	trace_fn = NULL;
	return (r->io_error || (mode >= 4 && (k->io_error || k->rcg_fail || !k->gmu_read)) || (mode >= 5 && b->why[0]) ||
	    (mode == 6 && (c->why[0] || !c->hfi_ready)) ||
	    (mode == 8 && !gx->gx_on) ||
	    (mode == 9 && !hw->ok) ||
	    (mode == 10 && !cp->ok) ||
	    (mode == 11 && !sm->ok) ||
	    (mode == 12 && !zp->secure_cleared) ||
	    (mode == 13 && !bl->ok) ||
	    (mode == 14 && !fb->ok) ||
	    (mode == 15 && !m2m->ok) ||
	    (mode == 16 && !sfb->ok)) ? -2 : 0;
}

int ruan_gpu_is_s5_ready(void)
{
	return ruan_gpu_s5_fb_result.ok && va[GPB_WIN_GPU] != 0;
}

uint32_t ruan_gpu_ring_wraps(void)
{
	return s_gpu_ring_wraps;
}

int ruan_gpu_stream0_map_page(uint32_t va_addr, uint64_t pa_addr, uint32_t bytes)
{
	return ruan_pt_remap_attr(&stream0_pt, va_addr, pa_addr, bytes, RUAN_PT_ATTRS_USER_RW);
}

void ruan_gpu_stream0_tlb_flush(void)
{
	clean_pool();
	if (va[W_SMMUF] && gpu_stream0_cb_off) {
		(void)wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_TLBIALL, 1);
	}
}

static volatile uint32_t s_gpu_last_int0, s_gpu_last_hw_fault;   /* last error status seen, for debugging */
static volatile uint32_t s_gpu_blits_submitted = 0;
static volatile uint32_t s_gpu_blits_retired = 0;

int ruan_gpu_dma_blit_frame(uint32_t src_va, uint32_t dst_va, uint32_t total_dwords)
{
	if (!va[GPB_WIN_GPU]) return -1;

	/* Chunk into segments <= 32768 dwords (128 KiB) to avoid hardware ROQ/DMA FIFO stalls */
	const uint32_t max_chunk_dwords = 32768u;
	uint32_t chunks = (total_dwords + max_chunk_dwords - 1u) / max_chunk_dwords;
	if (chunks == 0) return -1;
	unsigned needed_dwords = chunks * 6u + 1u /* WAIT_MEM_WRITES */ + 1u /* WAIT_FOR_ME */ + 4u /* MEM_WRITE */;

	unsigned w = 0;
	int res = ruan_ring_reserve(needed_dwords, &w);
	if (res != 0) return res;

	unsigned w_start = w;
	volatile uint32_t *rb = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_RB_OFF);
	volatile uint32_t *fence_ptr = (volatile uint32_t *)(void *)(pool_cpu + RUAN_S3_POOL_FENCE_OFF);
	uint32_t seq = ++s_gpu_blits_submitted;

	uint32_t remaining = total_dwords;
	uint32_t cur_src = src_va;
	uint32_t cur_dst = dst_va;

	while (remaining > 0) {
		uint32_t cur_chunk = remaining > max_chunk_dwords ? max_chunk_dwords : remaining;
		rb[w++] = cp_type7_packet(CP_MEMCPY, 5);
		rb[w++] = cur_chunk;
		rb[w++] = cur_src;
		rb[w++] = 0;
		rb[w++] = cur_dst;
		rb[w++] = 0;

		cur_src += cur_chunk * 4u;
		cur_dst += cur_chunk * 4u;
		remaining -= cur_chunk;
	}

	/* Wait for all DMA memory writes to commit before writing fence */
	rb[w++] = cp_type7_packet(CP_WAIT_MEM_WRITES, 0);
	rb[w++] = cp_type7_packet(CP_WAIT_FOR_ME, 0);

	/* Write monotonic fence to scratch page */
	rb[w++] = cp_type7_packet(CP_MEM_WRITE, 3);
	rb[w++] = RUAN_S3_GPU_VA_FENCE;
	rb[w++] = 0;
	rb[w++] = seq;

	clean_pool_range((uintptr_t)&rb[w_start], (w - w_start) * 4u);
	if (w >= RUAN_CP_RB_DWORDS) w = 0;
	s3_current_wptr = w;
	(void)wr(NULL, GPB_WIN_GPU, RUAN_CP_RB_WPTR * 4u, w);

	/* Poll for completion (typically ~1.2 ms for full 16 MB frame DMA copy) */
	int completed = 0;
	uint32_t rptr = 0, rbbm_status = 0;
	for (unsigned p = 0; p < 25000; p++) {
		rptr = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_CP_RB_RPTR * 4u);
		rbbm_status = *(volatile uint32_t *)(va[GPB_WIN_GPU] + RUAN_RBBM_STATUS * 4u);
		if (rptr == w && !(rbbm_status & (1u << 31))) {
			completed = 1;
			break;
		}
		delay(10);
	}

	/* Invalidate fence cache line so CPU reads fresh DRAM written by GPU */
	clean_pool_range((uintptr_t)fence_ptr, 64);
	__asm__ volatile("dsb sy" ::: "memory");

	if (!completed || *fence_ptr != seq) {
		uint32_t rbbm_int0 = 0, cp_fault = 0, fsr = 0;
		(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_INT_0_STATUS * 4u, &rbbm_int0);
		if (rbbm_int0 & RUAN_A710_INT_HWERROR) (void)rd(NULL, GPB_WIN_GPU, RUAN_CP_HW_FAULT * 4u, &cp_fault);
		if (va[W_SMMUF] && gpu_stream0_cb_off) (void)rd(NULL, W_SMMUF, gpu_stream0_cb_off + CB_FSR, &fsr);
		kprintf("ruan-gpu: timeout blit seq=%u rptr=%u wptr=%u rbbm=0x%08x fence=%u (exp %u) int0=0x%08x fault=0x%08x fsr=0x%08x\n",
		        seq, rptr, w, rbbm_status, *fence_ptr, seq, rbbm_int0, cp_fault, fsr);
		return -60; /* -ETIMEDOUT */
	}

	/* RBBM error interrupts, read and cleared the way KGSL gen7_irq_handler does (status written back to
	 * INT_CLEAR_CMD), so one fault fails one frame and not every frame after it. CP_HW_FAULT is only meaningful
	 * with HWERROR set (gen7_err_callback). */
	uint32_t rbbm_int0 = 0, cp_fault = 0;
	(void)rd(NULL, GPB_WIN_GPU, RUAN_RBBM_INT_0_STATUS * 4u, &rbbm_int0);
	if (rbbm_int0) (void)wr(NULL, GPB_WIN_GPU, RUAN_RBBM_INT_CLEAR_CMD * 4u, rbbm_int0);
	if (rbbm_int0 & RUAN_A710_INT_HWERROR) (void)rd(NULL, GPB_WIN_GPU, RUAN_CP_HW_FAULT * 4u, &cp_fault);
	s_gpu_last_int0 = rbbm_int0, s_gpu_last_hw_fault = cp_fault;
	if (rbbm_int0 & RUAN_A710_INT_ERRORS) {
		kprintf("ruan-gpu: hardware error int0=0x%08x fault=0x%08x\n", rbbm_int0, cp_fault);
		return -5; /* -EIO */
	}

	if (va[W_SMMUF] && gpu_stream0_cb_off) {
		uint32_t fsr = 0;
		(void)rd(NULL, W_SMMUF, gpu_stream0_cb_off + CB_FSR, &fsr);
		if (fsr & 0x0000007Fu) {
			(void)wr(NULL, W_SMMUF, gpu_stream0_cb_off + CB_FSR, fsr); /* clear fault */
			kprintf("ruan-gpu: smmu fault fsr=0x%08x\n", fsr);
			return -14; /* -EFAULT: SMMU translation / permission fault */
		}
	}

	/* Blit and fence confirmed retired by hardware */
	s_gpu_blits_retired = seq;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

int native_adreno_fences(uint32_t *submitted, uint32_t *retired)
{
	if (!submitted || !retired) return 0;
	*submitted = s_gpu_blits_submitted;
	*retired = s_gpu_blits_retired;
	return 1;
}

void
ruan_gpu_survey(void)
{
	unsigned mode = 0, survey = 0, fb_gpu = 0, fb_gpu_stream = 0;
	PE_parse_boot_argn("ruan-gpu", &mode, sizeof(mode));
	if (!mode && PE_parse_boot_argn("ruan-gpu-survey", &survey, sizeof(survey)) && survey == 1) mode = 1;
	if (!mode && PE_parse_boot_argn("fb-gpu", &fb_gpu, sizeof(fb_gpu)) && fb_gpu == 1) mode = 16;
	if (!mode && PE_parse_boot_argn("fb-gpu-stream", &fb_gpu_stream, sizeof(fb_gpu_stream)) && fb_gpu_stream == 1) mode = 16;
	if (mode > GPB_MAX_MODE) {
		boot_progress_stage("gpu: ruan-gpu mode exceeds GPB_MAX_MODE");
		return;
	}
	if (mode) (void)ruan_gpu_run(mode, NULL, NULL);
}
