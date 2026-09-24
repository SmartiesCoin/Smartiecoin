#ifndef SMT_YP2_H
#define SMT_YP2_H

#include <stdint.h>

/* 1 when the two-way kernel is compiled in (x86 with SSE2), 0 otherwise. */
int smt_yp2_supported(void);

/*
 * Compute yespower 1.0 (N=256, r=8, no personalization: the Smartiecoin v0.5.0 parameters) of two 80-byte block
 * headers at once. Returns 0 on success, -1 if the inputs are not supported, which is the case for any header
 * whose nTime is before the v0.5.0 fork time (those use the pre-fork parameters and this kernel only implements
 * the post-fork ones): the caller must then hash one at a time with the reference yespower_hash().
 */
int smt_yespower_hash2(const uint8_t *in0, const uint8_t *in1, uint8_t *out0, uint8_t *out1);

#endif
