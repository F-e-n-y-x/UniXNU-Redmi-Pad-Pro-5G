// Host self-test of native_legacy_copy_rotated: cc -I b rotate-test.c && ./a.out
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "NativeLegacyRequest.h"
int main(void) {
    enum {W=6,H=4,SS=W*4+8,DS=H*4+8};   // padded strides
    uint32_t src[H*SS/4],dst[W*DS/4];
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) src[y*SS/4+x]=(uint32_t)(y<<8|x);
    for (uint32_t r=1;r<=3;r+=2) {
        memset(dst,0xee,sizeof dst);
        assert(native_legacy_copy_rotated(dst,sizeof dst,DS,src,sizeof src,SS,W,H,r));
        for (int py=0;py<W;++py) for (int px=0;px<H;++px) {
            int sx=r==1?py:W-1-py, sy=r==1?H-1-px:px;
            assert(dst[py*DS/4+px]==src[sy*SS/4+sx]);
        }
    }
    assert(!native_legacy_copy_rotated(dst,sizeof dst,DS,src,sizeof src,SS,W,H,2));
    assert(!native_legacy_copy_rotated(dst,sizeof dst-1,DS,src,sizeof src,SS,W,H,1));
    puts("rotate-test ok");return 0;
}
