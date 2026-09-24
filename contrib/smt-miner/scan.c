/*
 * Nonce scanner linked against the node's own yespower_hash() so the miner and the
 * consensus code can never disagree about the PoW function.
 * Build: see build.sh
 */
#include <stdint.h>
#include <string.h>
#include <omp.h>

extern int yespower_hash(const char *input, char *output);

/* hash and target are little-endian uint256 (same layout the node uses) */
static int hash_le_target(const unsigned char *h, const unsigned char *t)
{
    for (int i = 31; i >= 0; i--) {
        if (h[i] < t[i]) return 1;
        if (h[i] > t[i]) return 0;
    }
    return 1;
}

/* Scan nonces [start, start+count). Returns 1 and sets *out on the first solution found. */
int scan(const unsigned char *hdr, const unsigned char *target_le, uint32_t start, uint32_t count,
         int nthreads, uint32_t *out)
{
    int found = 0;
    #pragma omp parallel num_threads(nthreads)
    {
        unsigned char h[80], r[32];
        memcpy(h, hdr, 80);
        #pragma omp for schedule(static, 16)
        for (int64_t i = 0; i < (int64_t)count; i++) {
            int f;
            #pragma omp atomic read
            f = found;
            if (f) continue;
            uint32_t n = start + (uint32_t)i;
            memcpy(h + 76, &n, 4);
            yespower_hash((const char *)h, (char *)r);
            if (hash_le_target(r, target_le)) {
                #pragma omp critical
                {
                    if (!found) { found = 1; *out = n; }
                }
            }
        }
    }
    return found;
}
