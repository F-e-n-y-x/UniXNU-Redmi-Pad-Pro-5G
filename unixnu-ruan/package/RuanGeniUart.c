/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The Bluetooth UART's clock on ruan: the RCG sequence of clk-rcg2.c for gcc_qupv3_wrap1_s5_clk_src (gcc-parrot.c) and the shared GCC
 * vote register. From filipnavara/UniXNU-asteroids 9f07af3 package/AsteroidsGeniUart.c (BSD-2-Clause-Patent; itself UniXNU-sm8550-common
 * Sm8550GeniUart.c); only the registers, vote bits and the post divider differ. */
#include "RuanGeniUart.h"
#include <sys/errno.h>
#include <IOKit/IOLocks.h>
int ruan_geni_bt_uart_source(const struct ruan_geni_clock_io *io,uint32_t baud,uint32_t *hz) {
    if(!io || !io->read || !io->write || !io->delay_us || !hz)return -EINVAL;
    uint32_t m,n,rate;
    /* GPLL0-even 300 MHz * M/N. 51.2 MHz is not in parrot's table but stays under its 75 MHz low-corner limit (102.4 MHz would need a
     * CX vote this board does not make); asteroids runs its WCN6750 at the same 64/375. */
    switch(baud) {
        case 115200:m=384;n=15625;rate=7372800;break;
        case 3000000:m=4;n=25;rate=48000000;break;
        case 3200000:m=64;n=375;rate=51200000;break;
        default:return -EINVAL;
    }
    const uint32_t root=RUAN_GCC_QUP1_S5_RCG;
    /* This SE is exclusive to the Bluetooth UART; manual RCG mode owns its source (DFSR bit 0 off). */
    uint32_t dfs=io->read(io->context,root+0x14);
    io->write(io->context,root+0x14,dfs&~1u);
    if(io->read(io->context,root+0x14)&1u)return -EIO;
    io->write(io->context,root+8,(io->read(io->context,root+8)&~0xffffu)|m);
    io->write(io->context,root+12,(io->read(io->context,root+12)&~0xffffu)|(~(n-m)&0xffffu));
    io->write(io->context,root+16,(io->read(io->context,root+16)&~0xffffu)|(~n&0xffffu));
    /* CFG: divider 1 (hid 1), source 6 (GPLL0-even, gcc_parent_map_0), dual-edge M/N mode. */
    uint32_t cfg=io->read(io->context,root+4);
    cfg=(cfg&~(0x1fu|0x700u|0x3000u|0x100000u))|1u|(6u<<8)|(2u<<12);
    io->write(io->context,root+4,cfg);
    io->write(io->context,root,io->read(io->context,root)|1u);
    for(unsigned i=0;i<1000;i++) {
        if(!(io->read(io->context,root)&1u)) {
            /* gcc_qupv3_wrap1_s5_div_clk_src sits between the RCG and the branch (Linux only reads it). */
            *hz=rate/((io->read(io->context,RUAN_GCC_QUP1_S5_DIV)&0xfu)+1u);
            return 0;
        }
        io->delay_us(io->context,1);
    }
    return -ETIMEDOUT;
}
static IOLock *gcc_lock;
static IOLock *smmu_lock;
static uint32_t bt_uart_initial_bus;
static bool bt_uart_claimed;
static IOLock *get_lock(IOLock **slot) {
    IOLock *lock=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    if(!lock) {
        IOLock *candidate=IOLockAlloc();if(!candidate)return NULL;
        if(__atomic_compare_exchange_n(slot,&lock,candidate,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))lock=candidate;
        else IOLockFree(candidate);
    }
    return lock;
}
/* AppsSmmuLock.h's provider (sheng's names): the Bluetooth DMA's bypass slot is planned under it. RuanWifi.cpp plans its streams without
 * it, earlier (ruan-bt waits for the WPSS); shortcut: no shared lock with Wi-Fi, add one if both ever plan at the same time. */
bool sm8550_apps_smmu_lock(void) {
    IOLock *lock=get_lock(&smmu_lock);if(!lock)return false;
    IOLockLock(lock);return true;
}
void sm8550_apps_smmu_unlock(void) {IOLockUnlock(smmu_lock);}
bool ruan_gcc_update32(volatile uint32_t *reg,uint32_t clear,uint32_t set) {
    if(!reg)return false;
    IOLock *lock=get_lock(&gcc_lock);if(!lock)return false;
    IOLockLock(lock);*reg=(*reg&~clear)|set;
    __asm__ volatile("dsb sy" ::: "memory");
    bool ok=(*reg&set)==set && (*reg&(clear&~set))==0;
    IOLockUnlock(lock);return ok;
}
#define BT_UART_BUS (0xfu<<18)                  /* wrap1 core_2x, core, m_ahb, s_ahb */
#define BT_UART_BITS (BT_UART_BUS|(1u<<27))     /* and SE5 */
#define QUP1_OTHER_SES (0x1fu<<22)              /* SE0-SE4 (SE3: touch) */
bool ruan_geni_bt_uart_vote(volatile uint32_t *reg,bool enabled) {
    if(!reg)return false;
    IOLock *lock=get_lock(&gcc_lock);if(!lock)return false;
    IOLockLock(lock);uint32_t value=*reg;
    if(!enabled && !bt_uart_claimed){IOLockUnlock(lock);return true;}
    if(enabled) {
        if(!bt_uart_claimed){bt_uart_initial_bus=value&BT_UART_BUS;bt_uart_claimed=true;}
        value|=BT_UART_BITS;
    } else {
        value&=~(1u<<27);
        /* RuanTouch.c enables SE3 through its own branch register, not this vote, so the bus bits stay unless we added them and no other
         * SE is voted here. */
        if(!(value&QUP1_OTHER_SES))value&=~(BT_UART_BUS&~bt_uart_initial_bus);
    }
    *reg=value;__asm__ volatile("dsb sy" ::: "memory");
    bool ok=(*reg&BT_UART_BITS)==(value&BT_UART_BITS);IOLockUnlock(lock);return ok;
}
