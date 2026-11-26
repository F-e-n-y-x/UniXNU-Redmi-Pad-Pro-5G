// Host self-test of native_legacy_turn180: rotate 3 == rotate 1 turned 180. cc -I b turn-test.c && ./a.out
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "NativeLegacyRequest.h"
static void check(int W,int H) {   // source W x H (landscape), panel H x W
    const int SS=W*4+8,DS=H*4+8;
    uint32_t src[64*64],p1[64*64],p3[64*64];
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) src[y*SS/4+x]=(uint32_t)(y<<8|x);
    memset(p1,0xee,sizeof p1);memset(p3,0xee,sizeof p3);
    assert(native_legacy_copy_rotated(p1,(uint64_t)W*DS,DS,src,(uint64_t)H*SS,SS,W,H,1));
    assert(native_legacy_copy_rotated(p3,(uint64_t)W*DS,DS,src,(uint64_t)H*SS,SS,W,H,3));
    assert(native_legacy_turn180(p1,(uint64_t)W*DS,DS,H,W));
    for (int y=0;y<W;++y) assert(!memcmp(p1+y*DS/4,p3+y*DS/4,(size_t)H*4));
    assert(native_legacy_turn180(p1,(uint64_t)W*DS,DS,H,W));   // and back
    assert(native_legacy_copy_rotated(p3,(uint64_t)W*DS,DS,src,(uint64_t)H*SS,SS,W,H,1));
    for (int y=0;y<W;++y) assert(!memcmp(p1+y*DS/4,p3+y*DS/4,(size_t)H*4));
}
int main(void) {
    check(6,4);check(7,4);check(5,8);   // even and odd panel heights
    uint32_t b[16];assert(!native_legacy_turn180(b,sizeof b,12,3,4));   // odd width refused
    puts("turn-test ok");return 0;
}
