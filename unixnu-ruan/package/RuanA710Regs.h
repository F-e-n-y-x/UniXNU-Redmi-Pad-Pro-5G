/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Adreno 710 / Gen7 GPU, CP, GMU and RBBM registers for Stage S3.
 * Values verified against Qualcomm KGSL (OnePlusOSS/android_kernel_oneplus_sm8475 gen7_reg.h). */
#ifndef RUAN_A710_REGS_H
#define RUAN_A710_REGS_H
#include <stdint.h>

/* GMU interrupt and status registers (dword offsets from GPU base) */
#define RUAN_GMU_PWR_CLK_STATUS             0x1f7e8u
#define RUAN_GMU_HFI_CTRL_STATUS            0x1f980u
#define RUAN_GMU_HFI_QTBL_INFO              0x1f984u
#define RUAN_GMU_HFI_QTBL_ADDR              0x1f985u
#define RUAN_GMU_HFI_CTRL_INIT              0x1f986u
#define RUAN_GMU_GMU2HOST_INTR_CLR          0x1f991u
#define RUAN_GMU_GMU2HOST_INTR_INFO         0x1f992u
#define RUAN_GMU_GMU2HOST_INTR_MASK         0x1f993u
#define RUAN_GMU_HOST2GMU_INTR_SET          0x1f994u

/* OOB GPU SET constants */
#define RUAN_OOB_GPU_SET_REQUEST            (1u << 30)
#define RUAN_OOB_GPU_SET_ACK                (1u << 31)
#define RUAN_GX_OFF_BITS                    0x3u  /* GX_HM_GDSC_POWER_OFF | GX_HM_CLK_OFF */

/* RBBM / GPU registers (dword offsets from GPU base 0x3d00000; byte = dword * 4) */
#define RUAN_RBBM_STATUS                    0x00210u
#define RUAN_RBBM_STATUS3                   0x00213u
#define RUAN_RBBM_INT_0_STATUS              0x00201u
#define RUAN_RBBM_PERFCTR_CNTL              0x00500u
#define RUAN_RBBM_PERFCTR_GPU_BUSY_MASKED   0x0050bu
#define RUAN_RBBM_INTERFACE_HANG_INT_CNTL   0x0001fu
#define RUAN_RBBM_SECVID_TRUST_CNTL         0x0f400u
#define RUAN_RBBM_GBIF_HALT                 0x0000au
#define RUAN_GBIF_HALT                      0x03c45u

/* CP registers (dword offsets from GPU base) */
#define RUAN_CP_RB_BASE                     0x00800u
#define RUAN_CP_RB_BASE_HI                  0x00801u
#define RUAN_CP_RB_CNTL                     0x00802u
#define RUAN_CP_RB_RPTR_ADDR_LO             0x00804u
#define RUAN_CP_RB_RPTR_ADDR_HI             0x00805u
#define RUAN_CP_RB_RPTR                     0x00806u
#define RUAN_CP_RB_WPTR                     0x00807u
#define RUAN_CP_SQE_CNTL                    0x00808u
#define RUAN_CP_HW_FAULT                    0x00821u
#define RUAN_CP_INTERRUPT_STATUS            0x00823u
#define RUAN_CP_SQE_INSTR_BASE_LO           0x00830u
#define RUAN_CP_SQE_INSTR_BASE_HI           0x00831u
#define RUAN_CP_AHB_CNTL                    0x00840u
#define RUAN_CP_APRIV_CNTL                  0x00844u
#define RUAN_CP_PROTECT_CNTL                0x0084fu

/* UCHE registers */
#define RUAN_UCHE_WRITE_THRU_BASE_LO        0x00e87u
#define RUAN_UCHE_WRITE_THRU_BASE_HI        0x00e88u
#define RUAN_UCHE_TRAP_BASE_LO              0x00e83u
#define RUAN_UCHE_TRAP_BASE_HI              0x00e84u
#define RUAN_UCHE_MODE_CNTL                 0x00e80u

/* Defaults */
#define RUAN_CP_RB_CNTL_DEFAULT             ((2u << 8) | 12u)  /* 32 KiB ring */
#define RUAN_BR_APRIV_DEFAULT               0x00000001u

#endif
