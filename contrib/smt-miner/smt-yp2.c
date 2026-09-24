/*
 * smt-yp2.c: two-way interleaved Yespower 1.0 kernel for the Smartiecoin miner.
 *
 * WHY: one Yespower hash is a long chain of dependent steps (each pwxform round needs the previous round's result
 * before it can index the S-boxes), so a core spends most of its time waiting: about 1.75 instructions per cycle on
 * a CPU that could issue several times more. Computing TWO independent hashes in the same loop gives the core twice
 * as many independent dependency chains to overlap.
 *
 * HOW: this file includes the node's own reference implementation (yespower-opt.c, by Solar Designer, BSD-2-Clause;
 * it is NOT modified) to reuse its types, SIMD helpers and, above all, its functions for the parts that are not
 * interleaved. It then adds a two-instance version of the block mixing loops. Each instance executes exactly the
 * same operations in exactly the same order as the reference; only the instructions of the two instances are
 * woven together. Nothing here is consensus code: the miner checks at start-up that this kernel produces
 * bit-identical results to the reference (smt-miner --selftest) and falls back to the reference otherwise.
 *
 * Scope: yespower 1.0, no personalization (what Smartiecoin uses from the v0.5.0 fork on), N and r fixed to the
 * values below. x86/SSE2 only: on any other architecture this file compiles to a stub and the miner hashes with
 * the reference kernel one at a time, exactly as it does whenever this kernel is not proven to match.
 */

/* keep the reference implementation's exported names out of the way of the node's own yespower.c */
#define yespower kyp_ref_yespower
#define yespower_tls kyp_ref_yespower_tls
#define yespower_init_local kyp_ref_init_local
#define yespower_free_local kyp_ref_free_local
#include "yespower-opt.c"
#undef yespower
#undef yespower_tls
#undef yespower_init_local
#undef yespower_free_local

#include "smt-yp2.h"

#define KYP_N 256u
#define KYP_R 8u
#define KYP_PERS_TIME_THRESHOLD 1790528400u   /* Smartiecoin v0.5.0 fork time, same constant as the node's yespower.c */

#ifdef __SSE2__

/* ------------------------------------------------------------------ small helpers (per instance state) */
#define KX_READ(P, in) \
	P##0 = (in).q[0]; P##1 = (in).q[1]; P##2 = (in).q[2]; P##3 = (in).q[3];
#define KX_WRITE(P, out) \
	(out).q[0] = P##0; (out).q[1] = P##1; (out).q[2] = P##2; (out).q[3] = P##3;
#define KX_XOR(P, in) \
	P##0 = _mm_xor_si128(P##0, (in).q[0]); P##1 = _mm_xor_si128(P##1, (in).q[1]); \
	P##2 = _mm_xor_si128(P##2, (in).q[2]); P##3 = _mm_xor_si128(P##3, (in).q[3]);
#define KX_XOR2(P, in1, in2) \
	P##0 = _mm_xor_si128((in1).q[0], (in2).q[0]); P##1 = _mm_xor_si128((in1).q[1], (in2).q[1]); \
	P##2 = _mm_xor_si128((in1).q[2], (in2).q[2]); P##3 = _mm_xor_si128((in1).q[3], (in2).q[3]);
/* out = out ^ in (written back), and P ^= that value: the reference's XOR_X_WRITE_XOR_Y_2 */
#define KX_XOR_WRITE_XOR(P, T, out, in) \
	(out).q[0] = T##0 = _mm_xor_si128((out).q[0], (in).q[0]); \
	(out).q[1] = T##1 = _mm_xor_si128((out).q[1], (in).q[1]); \
	(out).q[2] = T##2 = _mm_xor_si128((out).q[2], (in).q[2]); \
	(out).q[3] = T##3 = _mm_xor_si128((out).q[3], (in).q[3]); \
	P##0 = _mm_xor_si128(P##0, T##0); P##1 = _mm_xor_si128(P##1, T##1); \
	P##2 = _mm_xor_si128(P##2, T##2); P##3 = _mm_xor_si128(P##3, T##3);

/* One pwxform lane, identical to the reference's PWXFORM_SIMD: index from the lane, two S-box reads, mul/add/xor. */
#define KX_LANE(X, S0, S1) { \
	uint64_t x_ = (uint64_t)_mm_cvtsi128_si64(X) & M; \
	__m128i h_ = _mm_shuffle_epi32((X), _MM_SHUFFLE(2,3,0,1)); \
	(X) = _mm_mul_epu32(h_, (X)); \
	(X) = _mm_add_epi64((X), *(__m128i *)((S0) + (uint32_t)x_)); \
	(X) = _mm_xor_si128((X), *(__m128i *)((S1) + (x_ >> 32))); \
}
#define KX_LANE_W(X, S0, S1, Sw, w) KX_LANE(X, S0, S1) *(__m128i *)((Sw) + (w)) = (X);

/* The reference's PWXFORM_ROUND_WRITE4 / _WRITE2 with the two instances (A and B) woven together lane by lane. */
#define KX_ROUND_WRITE4 \
	KX_LANE_W(XA0, A_S0, A_S1, A_S0, A_w) KX_LANE_W(XB0, B_S0, B_S1, B_S0, B_w) \
	KX_LANE_W(XA1, A_S0, A_S1, A_S1, A_w) KX_LANE_W(XB1, B_S0, B_S1, B_S1, B_w) \
	A_w += 16; B_w += 16; \
	KX_LANE_W(XA2, A_S0, A_S1, A_S0, A_w) KX_LANE_W(XB2, B_S0, B_S1, B_S0, B_w) \
	KX_LANE_W(XA3, A_S0, A_S1, A_S1, A_w) KX_LANE_W(XB3, B_S0, B_S1, B_S1, B_w) \
	A_w += 16; B_w += 16;
#define KX_ROUND_WRITE2 \
	KX_LANE_W(XA0, A_S0, A_S1, A_S0, A_w) KX_LANE_W(XB0, B_S0, B_S1, B_S0, B_w) \
	KX_LANE_W(XA1, A_S0, A_S1, A_S1, A_w) KX_LANE_W(XB1, B_S0, B_S1, B_S1, B_w) \
	A_w += 16; B_w += 16; \
	KX_LANE(XA2, A_S0, A_S1) KX_LANE(XB2, B_S0, B_S1) \
	KX_LANE(XA3, A_S0, A_S1) KX_LANE(XB3, B_S0, B_S1)
#define KX_PWXFORM2 \
	KX_ROUND_WRITE4 KX_ROUND_WRITE2 KX_ROUND_WRITE2 \
	A_w &= M; B_w &= M; \
	{ uint8_t *t_ = A_S2; A_S2 = A_S1; A_S1 = A_S0; A_S0 = t_; } \
	{ uint8_t *t_ = B_S2; B_S2 = B_S1; B_S1 = B_S0; B_S0 = t_; }

#define KX_DECL \
	__m128i XA0, XA1, XA2, XA3, XB0, XB1, XB2, XB3; \
	const uint64_t M = Smask2_1_0; \
	uint8_t *A_S0 = cA->S0, *A_S1 = cA->S1, *A_S2 = cA->S2; size_t A_w = cA->w; \
	uint8_t *B_S0 = cB->S0, *B_S1 = cB->S1, *B_S2 = cB->S2; size_t B_w = cB->w;
#define KX_SAVE \
	cA->S0 = A_S0; cA->S1 = A_S1; cA->S2 = A_S2; cA->w = A_w; \
	cB->S0 = B_S0; cB->S1 = B_S1; cB->S2 = B_S2; cB->w = B_w;

/* Finish a block with the reference's Salsa20/2 core, one instance at a time (it runs once per blockmix). */
#define KX_FINISH(P, out, jvar) { \
	DECL_X \
	X0 = P##0; X1 = P##1; X2 = P##2; X3 = P##3; \
	SALSA20_2(out) \
	jvar = INTEGERIFY; \
}

/* ------------------------------------------------------------------ two-instance block mixing */
/* blockmix(Bin, Bout, r, ctx), pass 2 (yespower 1.0) */
static void kx_blockmix(const salsa20_blk_t *restrict AI, salsa20_blk_t *restrict AO, pwxform_ctx_t *cA,
    const salsa20_blk_t *restrict BI, salsa20_blk_t *restrict BO, pwxform_ctx_t *cB, size_t r)
{
	KX_DECL
	size_t i;
	uint32_t unused;

	r = r * 2 - 1;
	KX_READ(XA, AI[r])
	KX_READ(XB, BI[r])
	i = 0;
	do {
		KX_XOR(XA, AI[i])
		KX_XOR(XB, BI[i])
		KX_PWXFORM2
		if (unlikely(i >= r))
			break;
		KX_WRITE(XA, AO[i])
		KX_WRITE(XB, BO[i])
		i++;
	} while (1);
	KX_SAVE
	KX_FINISH(XA, AO[i], unused)
	KX_FINISH(XB, BO[i], unused)
	(void)unused;
}

/* blockmix_xor(Bin1, Bin2, Bout, r, ctx): returns integerify for each instance */
static void kx_blockmix_xor(const salsa20_blk_t *restrict A1, const salsa20_blk_t *restrict A2,
    salsa20_blk_t *restrict AO, pwxform_ctx_t *cA,
    const salsa20_blk_t *restrict B1, const salsa20_blk_t *restrict B2,
    salsa20_blk_t *restrict BO, pwxform_ctx_t *cB, size_t r, uint32_t *jA, uint32_t *jB)
{
	KX_DECL
	size_t i;

	r = r * 2 - 1;
	PREFETCH(&A2[r], _MM_HINT_T0)
	PREFETCH(&B2[r], _MM_HINT_T0)
	for (i = 0; i < r; i++) {
		PREFETCH(&A2[i], _MM_HINT_T0)
		PREFETCH(&B2[i], _MM_HINT_T0)
	}
	KX_XOR2(XA, A1[r], A2[r])
	KX_XOR2(XB, B1[r], B2[r])
	i = 0;
	r--;
	do {
		KX_XOR(XA, A1[i]) KX_XOR(XA, A2[i])
		KX_XOR(XB, B1[i]) KX_XOR(XB, B2[i])
		KX_PWXFORM2
		KX_WRITE(XA, AO[i])
		KX_WRITE(XB, BO[i])

		KX_XOR(XA, A1[i + 1]) KX_XOR(XA, A2[i + 1])
		KX_XOR(XB, B1[i + 1]) KX_XOR(XB, B2[i + 1])
		KX_PWXFORM2
		if (unlikely(i >= r))
			break;
		KX_WRITE(XA, AO[i + 1])
		KX_WRITE(XB, BO[i + 1])
		i += 2;
	} while (1);
	i++;
	KX_SAVE
	KX_FINISH(XA, AO[i], *jA)
	KX_FINISH(XB, BO[i], *jB)
}

/* blockmix_xor_save(Bin1out, Bin2, r, ctx): the hot loop of the second phase */
static void kx_blockmix_xor_save(salsa20_blk_t *restrict A1o, salsa20_blk_t *restrict A2, pwxform_ctx_t *cA,
    salsa20_blk_t *restrict B1o, salsa20_blk_t *restrict B2, pwxform_ctx_t *cB,
    size_t r, uint32_t *jA, uint32_t *jB)
{
	KX_DECL
	__m128i YA0, YA1, YA2, YA3, YB0, YB1, YB2, YB3;
	size_t i;

	r = r * 2 - 1;
	PREFETCH(&A2[r], _MM_HINT_T0)
	PREFETCH(&B2[r], _MM_HINT_T0)
	for (i = 0; i < r; i++) {
		PREFETCH(&A2[i], _MM_HINT_T0)
		PREFETCH(&B2[i], _MM_HINT_T0)
	}
	KX_XOR2(XA, A1o[r], A2[r])
	KX_XOR2(XB, B1o[r], B2[r])
	i = 0;
	r--;
	do {
		KX_XOR_WRITE_XOR(XA, YA, A2[i], A1o[i])
		KX_XOR_WRITE_XOR(XB, YB, B2[i], B1o[i])
		KX_PWXFORM2
		KX_WRITE(XA, A1o[i])
		KX_WRITE(XB, B1o[i])

		KX_XOR_WRITE_XOR(XA, YA, A2[i + 1], A1o[i + 1])
		KX_XOR_WRITE_XOR(XB, YB, B2[i + 1], B1o[i + 1])
		KX_PWXFORM2
		if (unlikely(i >= r))
			break;
		KX_WRITE(XA, A1o[i + 1])
		KX_WRITE(XB, B1o[i + 1])
		i += 2;
	} while (1);
	i++;
	KX_SAVE
	KX_FINISH(XA, A1o[i], *jA)
	KX_FINISH(XB, B1o[i], *jB)
}

/* ------------------------------------------------------------------ two-instance smix (pass 2 logic) */
static inline void kx_load(const uint8_t *B, salsa20_blk_t *tmp, salsa20_blk_t *dst)
{
	const salsa20_blk_t *src = (const salsa20_blk_t *)B;
	size_t k;
	for (k = 0; k < 16; k++)
		tmp->w[k] = le32dec(&src->w[k]);
	salsa20_simd_shuffle(tmp, dst);
}

static inline void kx_store(const salsa20_blk_t *src, salsa20_blk_t *tmp, uint8_t *B)
{
	size_t k;
	for (k = 0; k < 16; k++)
		le32enc(&tmp->w[k], src->w[k]);
	salsa20_simd_unshuffle(tmp, (salsa20_blk_t *)B);
}

static void kx_smix1(uint8_t *BA, uint8_t *BB, size_t r, uint32_t N,
    salsa20_blk_t *VA, salsa20_blk_t *VB, salsa20_blk_t *XYA, salsa20_blk_t *XYB,
    pwxform_ctx_t *cA, pwxform_ctx_t *cB)
{
	size_t s = 2 * r;
	salsa20_blk_t *XA = VA, *YA = &VA[s], *XB = VB, *YB = &VB[s], *VjA, *VjB;
	uint32_t i, jA, jB, n;

	for (i = 0; i < 2; i++) {
		kx_load(&BA[i * 64], YA, &XA[i]);
		kx_load(&BB[i * 64], YB, &XB[i]);
	}
	for (i = 1; i < r; i++)
		kx_blockmix(&XA[(i - 1) * 2], &XA[i * 2], cA, &XB[(i - 1) * 2], &XB[i * 2], cB, 1);

	kx_blockmix(XA, YA, cA, XB, YB, cB, r);
	XA = YA + s; XB = YB + s;
	kx_blockmix(YA, XA, cA, YB, XB, cB, r);
	jA = integerify(XA, r);
	jB = integerify(XB, r);

	for (n = 2; n < N; n <<= 1) {
		uint32_t m = (n < N / 2) ? n : (N - 1 - n);
		for (i = 1; i < m; i += 2) {
			YA = XA + s; YB = XB + s;
			jA &= n - 1; jA += i - 1; VjA = &VA[jA * s];
			jB &= n - 1; jB += i - 1; VjB = &VB[jB * s];
			kx_blockmix_xor(XA, VjA, YA, cA, XB, VjB, YB, cB, r, &jA, &jB);
			jA &= n - 1; jA += i; VjA = &VA[jA * s];
			jB &= n - 1; jB += i; VjB = &VB[jB * s];
			XA = YA + s; XB = YB + s;
			kx_blockmix_xor(YA, VjA, XA, cA, YB, VjB, XB, cB, r, &jA, &jB);
		}
	}
	n >>= 1;

	jA &= n - 1; jA += N - 2 - n; VjA = &VA[jA * s];
	jB &= n - 1; jB += N - 2 - n; VjB = &VB[jB * s];
	YA = XA + s; YB = XB + s;
	kx_blockmix_xor(XA, VjA, YA, cA, XB, VjB, YB, cB, r, &jA, &jB);
	jA &= n - 1; jA += N - 1 - n; VjA = &VA[jA * s];
	jB &= n - 1; jB += N - 1 - n; VjB = &VB[jB * s];
	kx_blockmix_xor(YA, VjA, XYA, cA, YB, VjB, XYB, cB, r, &jA, &jB);

	for (i = 0; i < 2 * r; i++) {
		kx_store(&XYA[i], &XYA[s], &BA[i * 64]);
		kx_store(&XYB[i], &XYB[s], &BB[i * 64]);
	}
}

static void kx_smix2(uint8_t *BA, uint8_t *BB, size_t r, uint32_t N, uint32_t Nloop,
    salsa20_blk_t *VA, salsa20_blk_t *VB, salsa20_blk_t *XYA, salsa20_blk_t *XYB,
    pwxform_ctx_t *cA, pwxform_ctx_t *cB)
{
	size_t s = 2 * r;
	salsa20_blk_t *XA = XYA, *YA = &XYA[s], *XB = XYB, *YB = &XYB[s];
	uint32_t i, jA, jB;

	for (i = 0; i < 2 * r; i++) {
		kx_load(&BA[i * 64], YA, &XA[i]);
		kx_load(&BB[i * 64], YB, &XB[i]);
	}
	jA = integerify(XA, r) & (N - 1);
	jB = integerify(XB, r) & (N - 1);

	do {
		salsa20_blk_t *VjA = &VA[jA * s], *VjB = &VB[jB * s];
		kx_blockmix_xor_save(XA, VjA, cA, XB, VjB, cB, r, &jA, &jB);
		jA &= N - 1; jB &= N - 1;
		VjA = &VA[jA * s]; VjB = &VB[jB * s];
		kx_blockmix_xor_save(XA, VjA, cA, XB, VjB, cB, r, &jA, &jB);
		jA &= N - 1; jB &= N - 1;
	} while (Nloop -= 2);

	for (i = 0; i < 2 * r; i++) {
		kx_store(&XA[i], YA, &BA[i * 64]);
		kx_store(&XB[i], YB, &BB[i * 64]);
	}
}

/* ------------------------------------------------------------------ public entry point */
typedef struct {
	yespower_local_t region;
	int init;
} kx_thread_region_t;

int smt_yp2_supported(void)
{
	return 1;
}

int smt_yespower_hash2(const uint8_t *in0, const uint8_t *in1, uint8_t *out0, uint8_t *out1)
{
	static __thread kx_thread_region_t tl[2];
	const uint32_t N = KYP_N, r = KYP_R;
	const size_t B_size = (size_t)128 * r, V_size = B_size * N, XY_size = B_size + 64;
	const size_t Sbytes1 = Swidth_to_Sbytes1(Swidth_1_0), Sbytes = 3 * Sbytes1;
	const size_t need = B_size + V_size + XY_size + Sbytes;
	const uint8_t *in[2] = {in0, in1};
	uint8_t *out[2] = {out0, out1};
	uint8_t *B[2];
	salsa20_blk_t *V[2], *XY[2];
	pwxform_ctx_t ctx[2];
	uint8_t sha256[2][32];
	int k;

	/* only the post-fork parameters (N=256, r=8) are implemented here: decline anything older and let the caller
	 * fall back to the reference yespower_hash(), which handles every parameter set. The node switches at
	 * nTime >= KYP_PERS_TIME_THRESHOLD, so declining at exactly that second is conservative, never wrong. */
	if (le32dec(in0 + 68) <= KYP_PERS_TIME_THRESHOLD || le32dec(in1 + 68) <= KYP_PERS_TIME_THRESHOLD)
		return -1;

	for (k = 0; k < 2; k++) {
		uint8_t *S;
		if (!tl[k].init) {
			init_region(&tl[k].region);
			tl[k].init = 1;
		}
		if (tl[k].region.aligned_size < need) {
			if (free_region(&tl[k].region))
				return -1;
			if (!alloc_region(&tl[k].region, need))
				return -1;
		}
		B[k] = (uint8_t *)tl[k].region.aligned;
		V[k] = (salsa20_blk_t *)(B[k] + B_size);
		XY[k] = (salsa20_blk_t *)((uint8_t *)V[k] + V_size);
		S = (uint8_t *)XY[k] + XY_size;
		ctx[k].S0 = S;
		ctx[k].S1 = S + Sbytes1;
		ctx[k].S2 = S + 2 * Sbytes1;
		ctx[k].w = 0;
		ctx[k].Sbytes = (uint32_t)Sbytes;

		SHA256_Buf(in[k], 80, sha256[k]);
		PBKDF2_SHA256(sha256[k], sizeof(sha256[k]), in[k], 0, 1, B[k], 128);
		memcpy(sha256[k], B[k], sizeof(sha256[k]));
		/* S-box initialisation: independent per instance, uses the reference code directly */
		smix1_1_0(B[k], 1, ctx[k].Sbytes / 128, (salsa20_blk_t *)ctx[k].S0, XY[k], NULL);
	}

	{
		uint32_t Nloop_rw = (N + 2) / 3;   /* 1/3, round up, then up to even (as in smix()) */
		Nloop_rw++;
		Nloop_rw &= ~(uint32_t)1;
		kx_smix1(B[0], B[1], r, N, V[0], V[1], XY[0], XY[1], &ctx[0], &ctx[1]);
		kx_smix2(B[0], B[1], r, N, Nloop_rw, V[0], V[1], XY[0], XY[1], &ctx[0], &ctx[1]);
	}

	for (k = 0; k < 2; k++)
		HMAC_SHA256_Buf(B[k] + B_size - 64, 64, sha256[k], sizeof(sha256[k]), out[k]);
	return 0;
}

#else /* !__SSE2__ */

int smt_yp2_supported(void)
{
	return 0;
}

int smt_yespower_hash2(const uint8_t *in0, const uint8_t *in1, uint8_t *out0, uint8_t *out1)
{
	(void)in0; (void)in1; (void)out0; (void)out1;
	return -1;
}

#endif
