// Host test for package/RuanSha256.h: FIPS 180-4 vectors (empty, "abc", 448- and 896-bit messages, a million 'a'), and, when a
// path is given, a file compared against RuanGpuPower.c's pinned gmu_gen70000.bin digest. cc -Wall -Wextra -Werror -I../package
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "RuanSha256.h"
#include "RuanGmuBoard.h"
static void hex(const uint8_t d[32], char out[65]) {for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);}
static void check(const void *m, size_t n, const char *want) {
    uint8_t d[32]; char h[65];
    ruan_sha256(m, n, d); hex(d, h);
    if (strcmp(h, want)) {fprintf(stderr, "len %zu: got %s want %s\n", n, h, want); exit(1);}
}
int main(int argc, char **argv) {
    check("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const char *m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    check(m896, strlen(m896), "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    char *a = malloc(1000000); memset(a, 'a', 1000000);
    check(a, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(a);
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb"); assert(f);
        static uint8_t buf[1 << 20]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
        check(buf, n, "b2d3f0e2a98fd6259ab9ffef16df63265542e6cc9a970d53d476e823d432042a");
        // the exact check gpu 5 runs (RuanGmuBoard.h ruan_gmu_fw_ok): passes on the file, fails on any change
        assert(ruan_gmu_fw_ok(buf, n));
        assert(!ruan_gmu_fw_ok(buf, n - 4) && !ruan_gmu_fw_ok(NULL, n));
        buf[1000] ^= 1; assert(!ruan_gmu_fw_ok(buf, n)); buf[1000] ^= 1;
        printf("file %zu bytes ok, ruan_gmu_fw_ok passes\n", n);
    }
    assert(ruan_sha256_matches_hex("abc", 3, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    assert(!ruan_sha256_matches_hex("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ae"));
    assert(!ruan_sha256_matches_hex("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad0"));
    puts("sha256_host: ok");
    return 0;
}
