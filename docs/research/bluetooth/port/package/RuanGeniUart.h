/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* ruan's Bluetooth UART: QUPv3 wrapper 1 SE5 (live DT qcom,qup_uart@a94000, alias hsuart0, pinctrl qupv3_se11_4uart), wired to the
 * WCN6750. From filipnavara/UniXNU-asteroids 9f07af3 package/AsteroidsGeniUart.h (BSD-2-Clause-Patent), with ruan's addresses:
 * gcc-parrot.c and qcom,gcc-parrot.h (MotorolaMobilityLLC/kernel-msm; DT clocks <&gcc 88 93 94> = WRAP1_S5, WRAP_1_M_AHB, WRAP_1_S_AHB)
 * and pinctrl-parrot.h (same tree). */
#ifndef RUAN_GENI_UART_H
#define RUAN_GENI_UART_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RUAN_BT_UART_BASE 0x00a94000ULL       /* qcom,qup_uart@a94000 reg <0xa94000 0x4000> */
#define RUAN_QUP1_BASE 0x00ac0000ULL          /* qcom,qupv3_1_geni_se@ac0000 reg <0xac0000 0x2000> */
#define RUAN_BT_UART_INTID (0x166u + 32u)     /* interrupts-extended <&intc 0 0x166 4>: SPI 358, INTID 390 */
#define RUAN_QUP1_SID 0x403u                  /* qupv3_1 iommus = <&apps_smmu 0x403 0x0> */
/* GCC (base 0x100000, as RuanTouch.c): the shared branch vote register 0x62008 (wrap1 core_2x 18, core 19, m_ahb 20, s_ahb 21,
 * s0-s5 22-27), SE5's RCG (cmd_rcgr 0x28618), its branch (halt 0x28610) and its read-only post divider (0x28744, bits 3:0 = div-1). */
#define RUAN_GCC_QUP_VOTE 0x62008u
#define RUAN_GCC_QUP1_S5_RCG 0x28618u
#define RUAN_GCC_QUP1_S5_CBCR 0x28610u
#define RUAN_GCC_QUP1_S5_DIV 0x28744u
#define RUAN_GCC_GPLL0_VOTE 0x62018u          /* gcc_gpll0 enable_reg, bit 0; its mode register (offset 0) bit 31 = locked */
struct ruan_geni_clock_io {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    void (*delay_us)(void *,unsigned);
};
/* Sets SE5's RCG for baud (16x oversampling) from GPLL0-even (300 MHz) with the M/N pairs of ftbl_gcc_qupv3_wrap0_s5_clk_src, and
 * returns the engine's input clock (after the post divider) in *hz. The UART engine is stopped before the source changes. */
int ruan_geni_bt_uart_source(const struct ruan_geni_clock_io *,uint32_t baud,uint32_t *hz);
bool ruan_gcc_update32(volatile uint32_t *,uint32_t clear,uint32_t set);
/* Own SE5 and the wrapper's core/AHB votes it newly acquired; the other QUP1 engines (touch on SE3) keep theirs. */
bool ruan_geni_bt_uart_vote(volatile uint32_t *,bool enabled);
#ifdef __cplusplus
}
#endif
#endif
