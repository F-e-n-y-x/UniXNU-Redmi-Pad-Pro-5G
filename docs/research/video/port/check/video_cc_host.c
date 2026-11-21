/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of package/RuanVideoCc.c against a register model: a GDSC's PWR_ON follows SW_COLLAPSE, a branch's CLK_OFF follows its
 * enable, the PLL locks when it runs, the root latches at once. Checks the rate table, the survey gate, the PLL values for 240 and
 * 133.33 MHz, the root configuration, the power-on/off order and the refusal of a PLL someone else runs.
 *   cc -std=gnu11 -Wall -Werror -I../package video_cc_host.c ../package/RuanVideoCc.c && ./a.out */
#include "RuanVideoCc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t gcc[0x1000 / 4], cc[0x10000 / 4];
static unsigned writes;
static uint32_t rd(void *c, enum ruan_video_cc_window w, uint32_t o)
{
    (void)c;
    uint32_t *r = w == RuanVideoGcc ? gcc : cc;
    assert(o < (w == RuanVideoGcc ? sizeof gcc : sizeof cc) && !(o & 3));
    return r[o / 4];
}
static void wr(void *c, enum ruan_video_cc_window w, uint32_t o, uint32_t v)
{
    (void)c;
    uint32_t *r = w == RuanVideoGcc ? gcc : cc;
    assert(o < (w == RuanVideoGcc ? sizeof gcc : sizeof cc) && !(o & 3));
    ++writes;
    if (w == RuanVideoCc && (o == 0x5004 || o == 0x6004)) v = (v & ~(1u << 31)) | (v & 1 ? 0 : 1u << 31);          /* GDSC */
    else if (w == RuanVideoCc && o == 0x0004 && (v & 1)) cc[0] |= 1u << 31;                                       /* PLL lock */
    else if (w == RuanVideoCc && o == 0x4000) v &= ~1u;                                                            /* RCG latched */
    else if ((o & 0xf) == 0x4 || (o & 0xf) == 0xc || (o & 0xf) == 0x8 || o == 0x501c || o == 0x6018)              /* branches */
        if (!(w == RuanVideoCc && (o == 0x0004 || o == 0x4004 || o < 0x40))) v = (v & ~(1u << 31)) | (v & 1 ? 0 : 1u << 31);
    r[o / 4] = v;
}
static void delay(void *c, unsigned us) {(void)c; (void)us;}

int main(void)
{
    const struct ruan_video_cc_io io = {NULL, rd, wr, delay};
    struct ruan_video_cc_state s;
    struct ruan_video_cc_survey sv;

    assert(ruan_video_cc_rate_supported(240000000u) && !ruan_video_cc_rate_supported(250000000u));
    assert(ruan_video_cc_rail_level(133333333u) == 64 && ruan_video_cc_rail_level(240000000u) == 128 &&
           ruan_video_cc_rail_level(460000000u) == 384 && ruan_video_cc_rail_level(1) == 0);

    /* everything off: the survey says the VPU must not be read, and wrote nothing */
    for (unsigned o = 0x5004; o <= 0xb01c; o += 4) cc[o / 4] = 1u << 31;
    gcc[0x004 / 4] = 1u << 31;
    writes = 0;
    ruan_video_cc_survey(&io, &sv);
    assert(!writes && !ruan_video_cc_vpu_accessible(&sv));

    /* 240 MHz: VCO 480 MHz = L 25, alpha 0; the root takes the even output (source 2) divided by 2 (hid 3) */
    memset(&s, 0, sizeof s);
    assert(!ruan_video_cc_configure(&io, &s, 250000000u) && s.failed_step == RuanVideoStepRate);
    memset(&s, 0, sizeof s);
    assert(ruan_video_cc_configure(&io, &s, 240000000u));
    assert((cc[0x10 / 4] & 0xffff) == 25 && cc[0x14 / 4] == 0 && cc[0x20 / 4] == 0x20485699 && (cc[0x18 / 4] & 7) == 7);
    assert((gcc[0x004 / 4] & 1) && (gcc[0x018 / 4] & 1) && (gcc[0x00c / 4] & 3) == 3 && (cc[0x9018 / 4] & 1));
    assert(ruan_video_cc_controller_on(&io, &s));
    assert((cc[0x4004 / 4] & 0x71f) == (2u << 8 | 3) && (cc[0x5004 / 4] & (1u << 31)) && (cc[0x5004 / 4] & (1u << 11)));
    assert((cc[0xb004 / 4] & 1) && (cc[0x501c / 4] & 1) && (cc[0xb01c / 4] & 1) && (cc[0x4 / 4] & 1));
    ruan_video_cc_survey(&io, &sv);
    assert(ruan_video_cc_vpu_accessible(&sv));
    assert(ruan_video_cc_hardware_on(&io, &s));
    assert((cc[0x6004 / 4] & (1u << 31)) && (cc[0xb00c / 4] & 1) && (cc[0x6018 / 4] & 1));
    assert(ruan_video_cc_hardware_mode(&io, &s, true) && (cc[0x6004 / 4] & 2));
    ruan_video_cc_hardware_off(&io, &s);
    assert(!(cc[0x6004 / 4] & (1u << 31)) && !(cc[0x6004 / 4] & 2) && !(cc[0x6018 / 4] & 1) && !(cc[0xb00c / 4] & 1));
    ruan_video_cc_controller_off(&io, &s);
    assert(!(cc[0x5004 / 4] & (1u << 31)) && !(cc[0x501c / 4] & 1) && !(cc[0x4 / 4] & 1) && (cc[0x4004 / 4] & 0x71f) == 0);

    /* 133.33 MHz: VCO 400 MHz = videocc-parrot's own config (L 0x14, alpha 0xd555), divider 3 (hid 5) */
    memset(&s, 0, sizeof s);
    assert(ruan_video_cc_configure(&io, &s, 133333333u));
    assert((cc[0x10 / 4] & 0xffff) == 0x14 && cc[0x14 / 4] == 0xd555);
    assert(ruan_video_cc_controller_on(&io, &s) && (cc[0x4004 / 4] & 0x1f) == 5);

    /* a PLL that already runs belongs to someone else */
    memset(&s, 0, sizeof s);
    cc[0x0 / 4] |= 1; cc[0x4 / 4] = 1;
    assert(!ruan_video_cc_configure(&io, &s, 240000000u) && s.failed_step == RuanVideoStepPllBusy);
    printf("video cc: rates, survey gate, PLL values (240, 133.33 MHz), root, power on/off order, busy PLL: ok\n");
    return 0;
}
