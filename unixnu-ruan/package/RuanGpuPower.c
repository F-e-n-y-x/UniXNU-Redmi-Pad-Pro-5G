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
#include "RuanSha256.h"   // self-contained: libkern SHA256_* gave rbu5 a wrong digest for correct bytes

extern void boot_progress_stage(const char *s);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern void delay(int usec);

#define RUAN_GPU_OFF 99u   // the USB link's `gpu off`
char ruan_gpu_row[128], ruan_gpu_short[32];
struct gpb_result ruan_gpu_result;

// Stage S2a adds two read-only windows: the GMU block (GMU core and AO registers; it also spans GPUCC and CX_MISC, so writes
// are allowed by register, never by window) and the GMU's RSCC. Both are mapped only by mode 4, after the GMU clocks run.
enum {W_GMU = GPB_WINDOWS, W_RSCC, NWIN};
static const uint64_t phys[NWIN] = {GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE, RUAN_GMU_BASE,
                                    RUAN_GMU_RSCC_BASE};
static const uint64_t len[NWIN] = {0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, GPB_GPU_WINDOW, RUAN_GMU_SIZE, RUAN_GMU_RSCC_SIZE};
static const char *const name[NWIN] = {"GCC GPU page 0x181000", "GCC vote page 0x162000", "GPUCC 0x3d90000",
                                       "GPU SMMU GR0 0x3da0000", "GPU RBBM 0x3d00000", "GMU 0x3d68000", "GMU RSCC 0x3d50000"};
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
// UsbDebugGpu.h hands over the bytes of /usr/standalone/firmware/ruan/gmu_gen70000.bin before `gpu 5` (it keeps the buffer).
void ruan_gpu_set_firmware(const void *data, size_t size)
{
	fw_data = (const uint8_t *)data, fw_size = size;
}
static const uint32_t s2a_branch[4] = {RUAN_CC_CX_GMU_CLK, RUAN_CC_CXO_CLK, RUAN_CC_HUB_CX_INT_CLK, RUAN_CC_HUB_AON_CLK};

static int allowed(unsigned w, uint32_t off)
{
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
static const char *const wshort[NWIN] = {"GCC+0x81000", "GCC+0x62000", "GPUCC", "SMMU", "GPU", "GMU", "RSCC"};
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
	unsigned cx, smmu_clock, vote_bits, branch_mask, gmu_cfg_valid, hub_cfg_valid;
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

// `gpu off`: undo, in reverse, only what this code turned on: GMU branches, RCG rates, GPLL0 votes, the SMMU vote clock, CX.
static void gpu_off(void)
{
	uint32_t v = 0;
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

int
ruan_gpu_run(unsigned mode, int (*trace)(void *, const char *), void *ctx)
{
	trace_fn = trace, trace_ctx = ctx;
	if (!mode || (mode > 5 && mode != RUAN_GPU_OFF)) {
		snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m%u?", mode);
		result_line("gpu: mode is 1..5 or off, nothing done");
		trace_fn = NULL;
		return -1;
	}
	unsigned have = 0;
	for (unsigned w = 0; w < GPB_WINDOWS; w++) {
		va[w] = ml_io_map_unmappable((vm_offset_t)phys[w], (vm_size_t)len[w], 7);   // 7 = VM_WIMG_IO, uncached
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
	if (r->powered_cx) owned.cx = 1;
	if (r->powered_clocks) owned.smmu_clock = 1;
	struct ruan_gpu_s2a *k = &ruan_gpu_s2a_result;
	struct ruan_gpu_s2b *b = &ruan_gpu_s2b_result;
	if (mode >= 4) s2a(r, k);
	if (mode == 5 && k->gmu_read && !k->io_error) s2b_tcm(b);
	for (unsigned w = 0; w < NWIN; w++) {
		if (va[w]) ml_io_unmap(va[w], (vm_size_t)len[w]);
		va[w] = 0;
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
	if (mode == 5) {
		snprintf(line, sizeof(line), "gpu s2b fw %s blocks %u prealloc %u tcm %u (%u words, %u bad) ddr %u (%u bytes, first %08x) sysreset %08x %s",
		    b->fw_ok ? "ok" : "BAD", b->blocks, b->prealloc, b->tcm_blocks, b->tcm_words, b->tcm_bad, b->ddr_blocks, b->ddr_bytes,
		    b->ddr_first, b->sysreset, b->why[0] ? b->why : "TCM loaded, GMU still in reset; DDR blocks need the SID-5 context (next stage)");
		result_line(line);
		strlcpy(ruan_gpu_row, line, sizeof(ruan_gpu_row));
	}
	snprintf(ruan_gpu_short, sizeof(ruan_gpu_short), "gpu m%u cx%s gx%s smmu%s%s%s", mode, cx ? "on" : "off",
	    gpb_gdsc_on(r->gx_gdscr) ? "on" : "off", r->smmu == GPB_DONE ? "ok" : "-", mode == 4 ? (k->gmu_read ? " gmuR" : " gmu-") : "",   /* 31 chars max: m5 drops gmuR (tcm implies it) */
	    mode == 5 ? (b->tcm_words && !b->tcm_bad && !b->why[0] ? " tcm" : " tcm-") : "");
	result_line(ruan_gpu_short);
	trace_fn = NULL;
	return (r->io_error || (mode >= 4 && (k->io_error || k->rcg_fail || !k->gmu_read)) || (mode == 5 && b->why[0])) ? -2 : 0;
}

void
ruan_gpu_survey(void)
{
	unsigned mode = 0, survey = 0;
	PE_parse_boot_argn("ruan-gpu", &mode, sizeof(mode));
	if (!mode && PE_parse_boot_argn("ruan-gpu-survey", &survey, sizeof(survey)) && survey == 1) mode = 1;
	if (mode > GPB_MAX_MODE) {   // S2 and `off` run only from the USB link (UsbDebugGpu.h), never at boot
		boot_progress_stage("gpu: ruan-gpu above 3 is USB-command only, nothing done at boot");
		return;
	}
	if (mode) (void)ruan_gpu_run(mode, NULL, NULL);
}
