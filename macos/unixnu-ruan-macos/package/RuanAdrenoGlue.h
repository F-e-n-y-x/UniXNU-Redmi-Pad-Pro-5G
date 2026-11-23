// SPDX-License-Identifier: BSD-2-Clause-Patent
// ruan: the bridge from package/RuanGpuPower.c (the A710 power-up, proven to GX on / CP running) to UniXNU's Adreno740 driver
// (NativeAdrenoGpuContext, NativeAdrenoCpInit, NativeAdrenoRing, native_adreno_publish). See package/RuanAdrenoGlue.cpp.
#ifndef RUAN_ADRENO_GLUE_H
#define RUAN_ADRENO_GLUE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// RuanGpuPower.c's accessors: its allowlist and its USB trace stay in front of every register access the driver makes.
struct ruan_adreno_glue_io {
	int (*gpu_rd)(uint32_t reg, uint32_t *value);    // GPU register, dword index (as the driver's ring I/O)
	int (*gpu_wr)(uint32_t reg, uint32_t value);
	int (*smmu_rd)(uint32_t off, uint32_t *value);   // byte offset in the GPU SMMU window (kgsl-smmu@3da0000)
	int (*smmu_wr)(uint32_t off, uint32_t value);
	uint32_t smmu_bytes;
	unsigned bank, slot, sid, mask;                   // the GPU context: bank 2, SMR slot, SID 0 / mask 0x400
	const uint8_t *sqe;                               // a710_sqe.fw as loaded (first dword included)
	size_t sqe_bytes;
	uint64_t pool_base, pool_bytes;                   // the part of the reserved GPU pool no RuanGpuPower.c stage uses
	void (*line)(const char *);                       // a result line (boot page + USB)
};
// stage 1: GPU context on io->bank, SQE, ring, CP init with the a7xx power-up list, one fenced CP_MEM_WRITE.
// stage 2: stage 1, then native_adreno_publish (no Metal). Repeating a stage that passed only reports it again.
// row gets the short result. 0 when the stage passed.
int ruan_adreno_glue_run(unsigned stage, const struct ruan_adreno_glue_io *io, char *row, size_t row_bytes);
#ifdef __cplusplus
}
#endif
#endif
