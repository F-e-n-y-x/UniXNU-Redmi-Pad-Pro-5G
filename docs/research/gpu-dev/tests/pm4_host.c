// Host test for package/RuanA710Pm4.h: type-7 header layout and odd parity, checked against an independent popcount
// encoder and known headers (rbu24 shipped swapped opcode/count fields; this catches that). cc -Wall -Wextra -Werror -I../package
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "RuanA710Pm4.h"
static uint32_t odd(uint32_t v) {return (__builtin_popcount(v) & 1) ^ 1;}   /* bit that makes the total count of ones odd */
static uint32_t ref7(uint32_t op, uint32_t cnt) {return 7u << 28 | cnt | odd(cnt) << 15 | op << 16 | odd(op) << 23;}
int main(void) {
    assert(cp_type7_packet(CP_ME_INIT, 8) == 0x70c80008u);     /* the ME_INIT header seen in KGSL/freedreno ring dumps */
    assert(cp_type7_packet(CP_NOP, 0) == 0x70108000u);
    for (uint32_t op = 0; op < 0x80; op++)
        for (uint32_t cnt = 0; cnt < 0x4000; cnt++) {
            const uint32_t h = cp_type7_packet(op, cnt);
            assert(h == ref7(op, cnt));
            assert((h >> 28) == 7 && (h & 0x3fff) == cnt && ((h >> 16) & 0x7f) == op);
            assert(!(h & (1u << 14)) && !(h & 0x0f000000u));          /* reserved bits stay clear */
        }
    assert((cp_type7_packet(CP_NOP, 0x4000) & 0x3fff) == 0);           /* oversized counts cannot spill into the parity bit */
    puts("pm4_host: ok");
    return 0;
}
