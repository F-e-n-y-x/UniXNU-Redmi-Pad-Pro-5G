// Host test for package/RuanGmuMmu.h: builds the gpu 6 layout from the real gmu_gen70000.bin (argv[1]) on a fake pool and walks
// every page as the SMMU would; checks attributes, overlap refusal, bounds and exhaustion. cc -Wall -Wextra -Werror -I../package
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "RuanGmuMmu.h"
#define POOL (128u * 4096u)
#define PA 0x8a0000000ull   /* any page-aligned physical address below 2^40 */
int main(int argc, char **argv) {
    uint8_t *cpu = aligned_alloc(4096, POOL);
    struct ruan_pt p;
    assert(cpu && !ruan_pt_init(&p, cpu, PA, POOL));
    assert(ruan_pt_init(&p, cpu + 8, PA, POOL) && ruan_pt_init(&p, cpu, PA + 8, POOL) && ruan_pt_init(&p, cpu, 1ull << 40, POOL));
    assert(!ruan_pt_init(&p, cpu, PA, POOL));
    // simple map/walk, attributes, refusal of overlap / misalignment / >4 GiB
    uint64_t d = ruan_pt_alloc(&p, 2);
    assert(d && !ruan_pt_map(&p, 0x4000, d, 0x2000));
    assert(ruan_pt_walk(&p, 0x4000) == d && ruan_pt_walk(&p, 0x5ffc) == d + 0x1ffc && ruan_pt_walk(&p, 0x6000) == ~0ull);
    assert(ruan_pt_map(&p, 0x5000, d, 0x1000) == -1);            // overlap refused
    assert(ruan_pt_map(&p, 0x7004, d, 0x1000) == -1 && ruan_pt_map(&p, 0xfffff000u, d, 0x2000) == -1);
    {   // the leaf descriptor's attributes: page, AttrIndx 0, AP 01, SH outer, AF, executable (no XN bits)
        const uint64_t *l1 = ruan_pt_tbl(&p, p.pa), *l2 = ruan_pt_tbl(&p, l1[0] & RUAN_PT_ADDR_MASK), *l3 = ruan_pt_tbl(&p, l2[0] & RUAN_PT_ADDR_MASK);
        assert((l3[4] & ~RUAN_PT_ADDR_MASK) == RUAN_PT_PAGE_ATTRS && RUAN_PT_PAGE_ATTRS == 0x643ull && !(l3[4] >> 52));
    }
    // exhaustion returns 0 and leaves the pool usable
    assert(ruan_pt_alloc(&p, p.pages) == 0);
    // the real layout
    assert(!ruan_pt_init(&p, cpu, PA, POOL));
    static uint8_t fw[1 << 20];
    size_t n = 0;
    if (argc > 1) {FILE *f = fopen(argv[1], "rb"); assert(f); n = fread(fw, 1, sizeof fw, f); fclose(f);}
    assert(n == 57272);
    struct ruan_gmu_region r[RUAN_GMU_MAX_REGIONS];
    int k = ruan_gmu_layout(&p, fw, n, r);
    assert(k == 8);
    const uint32_t want_va[8] = {0x4000, 0x100000, 0x200000, 0x300000, 0x60000000, 0x60001000, RUAN_GMU_HFI_VA, RUAN_GMU_LOG_VA};
    const uint32_t want_b[8] = {0xe000, 0x9000, 0x13000, 0x7000, 0x1000, 0x1000, 0x4000, 0x4000};
    for (int i = 0; i < k; i++) {
        assert(r[i].va == want_va[i] && r[i].bytes == want_b[i]);
        for (uint32_t o = 0; o < r[i].bytes; o += 4096) assert(ruan_pt_walk(&p, r[i].va + o) == r[i].pa + o);
        assert(ruan_pt_walk(&p, r[i].va + r[i].bytes) == ~0ull || (i + 1 < k && r[i + 1].va == r[i].va + r[i].bytes));
    }
    assert(ruan_pt_walk(&p, 0x0) == ~0ull && ruan_pt_walk(&p, 0x10004000) == ~0ull);   // the TCMs are not DDR
    // the 0x4000 code block lands where the GMU fetches it
    uint8_t *code = ruan_gmu_cpu(&p, r, (unsigned)k, 0x4000, 0xdcac);
    assert(code && !ruan_gmu_cpu(&p, r, (unsigned)k, 0x11000, 0x2000));
    printf("gmu_mmu_host: %d regions, %u of %u pool pages used\n", k, p.used, p.pages);
    assert(p.used <= 72);
    free(cpu);
    puts("gmu_mmu_host: ok");
    return 0;
}
