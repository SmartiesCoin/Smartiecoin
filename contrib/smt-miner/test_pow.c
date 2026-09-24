/* Randomized parity and timestamp/fallback tests for the ACTUAL native miner path.
 * Single-threaded; no RPC and no mining. --require-optimized exits 77 on ARM/stubs.
 * Build with test.sh, which links the same reference and two-way source as miner.
 */
#define main smt_miner_program_main
#include "smt-miner.c"
#undef main
#include "yespower.h"
#include <assert.h>

static uint32_t rng_state;
static uint32_t random32(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void random_header(uint8_t h[80])
{
    for (int i = 0; i < 80; i++) h[i] = (uint8_t)random32();
}

static void explicit_reference(const uint8_t h[80], uint8_t out[32], int armed)
{
    uint32_t t = (uint32_t)h[68] | (uint32_t)h[69] << 8 | (uint32_t)h[70] << 16 | (uint32_t)h[71] << 24;
    yespower_params_t p = {YESPOWER_1_0, 2048, 32, NULL, 0};
    if (armed && t >= SMT_V050_FORK_TIME) { p.N = 256; p.r = 8; }
    else if (t <= 1546539305u) {
        p.version = YESPOWER_0_5; p.N = 4096;
        p.pers = (const uint8_t *)"WaviBanana"; p.perslen = 10;
    }
    assert(yespower_tls(h, 80, &p, (yespower_binary_t *)out) == 0);
}

static void check_dispatch(const uint8_t h[80], int armed)
{
    uint8_t actual[32], reference[32];
    assert(yespower_hash((const char *)h, (char *)actual) == 0);
    explicit_reference(h, reference, armed);
    assert(!memcmp(actual, reference, 32));
}

static void check_nonce_path(uint8_t h[80])
{
    uint32_t a = random32(), b = random32();
    uint8_t actual[2][32], reference[32];
    g_ways = 2;
    hash_nonces(h, a, b, actual);
    put_le32(h + 76, a);
    explicit_reference(h, reference, 1);
    assert(!memcmp(actual[0], reference, 32));
    put_le32(h + 76, b);
    explicit_reference(h, reference, 1);
    assert(!memcmp(actual[1], reference, 32));
    g_ways = 1;
    hash_nonces(h, a, b, actual);
    put_le32(h + 76, a);
    explicit_reference(h, reference, 1);
    assert(!memcmp(actual[0], reference, 32));
}

int main(int argc, char **argv)
{
    int require_optimized = argc > 1 && !strcmp(argv[1], "--require-optimized");
    uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x51a7cafeu;
    if (!seed) seed = 1;
    rng_state = seed;
    init_pow_params();
    const int pairs = 1024;
    int optimized = smt_yp2_supported();
    uint8_t all[1024 * 64], digest[32];
    for (int i = 0; i < pairs; i++) {
        uint8_t a[80], b[80], oa[32], ob[32], ra[32], rb[32];
        random_header(a); random_header(b);
        put_le32(a + 68, SMT_V050_FORK_TIME + random32() % 100000000u);
        put_le32(b + 68, SMT_V050_FORK_TIME + random32() % 100000000u);
        if (i == 0) put_le32(a + 68, SMT_V050_FORK_TIME);
        explicit_reference(a, ra, 1); explicit_reference(b, rb, 1);
        check_dispatch(a, 1); check_dispatch(b, 1);
        int rc = smt_yespower_hash2(a, b, oa, ob);
        if (optimized) {
            assert(rc == 0);
            assert(!memcmp(oa, ra, 32) && !memcmp(ob, rb, 32));
        } else assert(rc != 0);
        memcpy(all + i * 64, ra, 32); memcpy(all + i * 64 + 32, rb, 32);
        check_nonce_path(a);
    }
    const uint32_t times[] = {1546539304u, 1546539305u, 1546539306u,
                             SMT_V050_FORK_TIME - 1u, SMT_V050_FORK_TIME,
                             SMT_V050_FORK_TIME + 1u, UINT32_MAX};
    for (size_t i = 0; i < sizeof times / sizeof *times; ++i) {
        uint8_t a[80], b[80], oa[32], ob[32];
        random_header(a); random_header(b);
        put_le32(a + 68, times[i]); put_le32(b + 68, SMT_V050_FORK_TIME);
        check_dispatch(a, 1);
        check_nonce_path(a);
        if (times[i] < SMT_V050_FORK_TIME) {
            /* Mixed-era pairs must decline even when only one lane is legacy. */
            assert(smt_yespower_hash2(a, b, oa, ob) != 0);
            assert(smt_yespower_hash2(b, a, ob, oa) != 0);
        }
    }
    yespower_set_v050_fork_time(0);
    for (int i = 0; i < 4; i++) {
        uint8_t h[80]; random_header(h);
        put_le32(h + 68, SMT_V050_FORK_TIME + (uint32_t)i);
        check_dispatch(h, 0);
    }
    init_pow_params();
    SHA256(all, sizeof all, digest);
    char hex[65]; bin2hex(digest, 32, hex);
    char fixed[65]; selftest_digest(fixed);
    assert(!strcmp(fixed, "0259fa5d24fff45e5c1cfd8d0dc010e22df581424f5d6e64331e8fdb8f7731cb"));
    printf("{\"seed\":%u,\"random_pairs\":%d,\"boundary_timestamps\":7,\"disabled_fork_headers\":4,"
           "\"nonce_path\":\"PASS\",\"explicit_parameter_dispatch\":\"PASS\","
           "\"optimized_parity\":\"%s\",\"random_digest\":\"%s\",\"fixed_digest\":\"%s\"}\n",
           seed, pairs, optimized ? "PASS" : "UNAVAILABLE", hex, fixed);
    return require_optimized && !optimized ? 77 : 0;
}
