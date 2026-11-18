/* natives/spu/spu_avx2.h -- the SPU's single-precision rules, eight lanes at a time (AVX2).
 *
 * For the native rewrites of whole SPU loops (native_218.c, native_query.c): where the lifted
 * code works on one quad (4 lanes) per SPU instruction, a rewrite here runs 8 lanes (two groups
 * of the SPU loop, or 8 bones in structure-of-arrays form) per host instruction, with the same
 * operation per lane, so every lane's bits are what the SPU computes.
 *
 * The rules (engine/runtime/spu/spu_helpers.h, "SPU_FLOAT_RZ", and tests/test_spu_float_rz_x86.c):
 *  - SPU code runs with MXCSR at round toward zero + flush to zero + denormals are zero
 *    (spu_float_mode_enter). In that mode a packed x86 op IS the SPU's single-precision op for
 *    every ordinary result. The fused fma is one truncation of the exact a*b+c, as on the SPU.
 *  - The SPU's zero is always +0: every result goes through the flush (exponent 0 -> +0).
 *  - Extended range: an exponent-255 operand is Inf/NaN to x86, and a result at or past 2^128
 *    comes out as FLT_MAX (RZ). Any lane whose magnitude is not below FLT_MAX is redone with the
 *    helpers' exact fix-up (spu__xv_*_slow, all four lanes of each half, bit-exact).
 *  - Compares are the SPU's (no NaN, -0 == +0, denormals are zero): integer keys.
 *  - frest / frsqest / fi: the hardware tables and the interpolation, as spu_frest / spu_frsqest /
 *    spu_fi (the helpers' x86 forms, lane for lane).
 * Built like the lifted SPU TUs (natives/spu: -mavx2 -mfma, SPU_FLOAT_RZ_ONLY). Include after
 * spu_helpers.h. Every op takes and returns raw SPU words (__m256i, host lane order = whatever
 * the caller chose; each lane is independent). */
#ifndef INF2_SPU_AVX2_H
#define INF2_SPU_AVX2_H

#include <immintrin.h>
#include <stdint.h>

#if !defined(__AVX2__) || !defined(__FMA__)
#error "spu_avx2.h needs AVX2 and FMA (built with the lifted SPU flags)"
#endif
#if !SPU_X86_SIMD
#error "spu_avx2.h needs spu_helpers.h's x86 forms (SPU_X86_SIMD)"
#endif

typedef __m256i V8;
#define SA_INLINE static inline __attribute__((always_inline))

SA_INLINE V8 v8_set1(uint32_t x) { return _mm256_set1_epi32((int)x); }
SA_INLINE V8 v8_zero(void) { return _mm256_setzero_si256(); }
SA_INLINE __m256 v8_f(V8 v) { return _mm256_castsi256_ps(v); }
SA_INLINE V8 v8_i(__m256 v) { return _mm256_castps_si256(v); }
SA_INLINE V8 v8_and(V8 a, V8 b) { return _mm256_and_si256(a, b); }
SA_INLINE V8 v8_or(V8 a, V8 b) { return _mm256_or_si256(a, b); }
SA_INLINE V8 v8_xor(V8 a, V8 b) { return _mm256_xor_si256(a, b); }
/* selb(a, b, m): b where m is set, else a (bitwise) */
SA_INLINE V8 v8_selb(V8 a, V8 b, V8 m) { return _mm256_or_si256(_mm256_andnot_si256(m, a), _mm256_and_si256(m, b)); }
/* word masks only (each lane 0 or ~0): a byte blend */
SA_INLINE V8 v8_selw(V8 a, V8 b, V8 m) { return _mm256_blendv_epi8(a, b, m); }

/* exponent 0 -> +0 (the SPU has no -0 and no denormals) */
SA_INLINE V8 v8_flush(V8 x) {
    return _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(x, v8_set1(0x7F800000u)), _mm256_setzero_si256()), x);
}
/* nonzero when a lane's magnitude is not below FLT_MAX: Inf/NaN (an exponent-255 operand) or
 * an overflow, both of which x86 and the SPU answer differently */
SA_INLINE int v8_bad(V8 r) {
    const V8 m = _mm256_and_si256(r, v8_set1(0x7FFFFFFFu));
    return _mm256_movemask_epi8(_mm256_cmpgt_epi32(m, v8_set1(0x7F7FFFFEu))) != 0;
}

/* the exact fix-ups, two halves of four */
static __attribute__((noinline, cold)) V8 v8_fa_slow(V8 a, V8 b, uint32_t negb) {
    const __m128i lo = spu__xv_fa_slow(_mm256_castsi256_si128(a), _mm256_castsi256_si128(b), negb);
    const __m128i hi = spu__xv_fa_slow(_mm256_extracti128_si256(a, 1), _mm256_extracti128_si256(b, 1), negb);
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}
static __attribute__((noinline, cold)) V8 v8_fm_slow(V8 a, V8 b) {
    const __m128i lo = spu__xv_fm_slow(_mm256_castsi256_si128(a), _mm256_castsi256_si128(b));
    const __m128i hi = spu__xv_fm_slow(_mm256_extracti128_si256(a, 1), _mm256_extracti128_si256(b, 1));
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}
static __attribute__((noinline, cold)) V8 v8_fma_slow(V8 a, V8 b, V8 c, uint32_t negp, uint32_t negc) {
    const __m128i lo = spu__xv_fma_slow(_mm256_castsi256_si128(a), _mm256_castsi256_si128(b), _mm256_castsi256_si128(c), negp, negc);
    const __m128i hi = spu__xv_fma_slow(_mm256_extracti128_si256(a, 1), _mm256_extracti128_si256(b, 1),
                                        _mm256_extracti128_si256(c, 1), negp, negc);
    return _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
}

/* The empty asm keeps the compiler from evaluating an op at compile time (it would round to
 * nearest) and from contracting a mul and an add into one fma. */
SA_INLINE V8 v8_fa(V8 a, V8 b) {
    __m256 x = v8_f(a); __asm__("" : "+x"(x));
    const V8 r = v8_i(_mm256_add_ps(x, v8_f(b)));
    return __builtin_expect(v8_bad(r), 0) ? v8_fa_slow(a, b, 0) : v8_flush(r);
}
SA_INLINE V8 v8_fs(V8 a, V8 b) {
    __m256 x = v8_f(a); __asm__("" : "+x"(x));
    const V8 r = v8_i(_mm256_sub_ps(x, v8_f(b)));
    return __builtin_expect(v8_bad(r), 0) ? v8_fa_slow(a, b, 0x80000000u) : v8_flush(r);
}
SA_INLINE V8 v8_fm(V8 a, V8 b) {
    __m256 x = v8_f(a); __asm__("" : "+x"(x));
    const V8 r = v8_i(_mm256_mul_ps(x, v8_f(b)));
    return __builtin_expect(v8_bad(r), 0) ? v8_fm_slow(a, b) : v8_flush(r);
}
/* fma(a, b, c) = a*b + c */
SA_INLINE V8 v8_fma(V8 a, V8 b, V8 c) {
    __m256 x = v8_f(a); __asm__("" : "+x"(x));
    const V8 r = v8_i(_mm256_fmadd_ps(x, v8_f(b), v8_f(c)));
    return __builtin_expect(v8_bad(r), 0) ? v8_fma_slow(a, b, c, 0, 0) : v8_flush(r);
}
/* fms(a, b, c) = a*b - c */
SA_INLINE V8 v8_fms(V8 a, V8 b, V8 c) {
    __m256 x = v8_f(a); __asm__("" : "+x"(x));
    const V8 r = v8_i(_mm256_fmsub_ps(x, v8_f(b), v8_f(c)));
    return __builtin_expect(v8_bad(r), 0) ? v8_fma_slow(a, b, c, 0, 0x80000000u) : v8_flush(r);
}
/* fnms(a, b, c) = c - a*b */
SA_INLINE V8 v8_fnms(V8 a, V8 b, V8 c) {
    __m256 x = v8_f(a); __asm__("" : "+x"(x));
    const V8 r = v8_i(_mm256_fnmadd_ps(x, v8_f(b), v8_f(c)));
    return __builtin_expect(v8_bad(r), 0) ? v8_fma_slow(a, b, c, 0x80000000u, 0) : v8_flush(r);
}

/* SPU float compares: exponent-0 words are zero, then sign-magnitude as an ordered key */
SA_INLINE V8 v8_key(V8 x) {
    const V8 f = v8_flush(x);
    return _mm256_xor_si256(f, _mm256_srli_epi32(_mm256_srai_epi32(f, 31), 1));
}
SA_INLINE V8 v8_fcgt(V8 a, V8 b) { return _mm256_cmpgt_epi32(v8_key(a), v8_key(b)); }
SA_INLINE V8 v8_fcmgt(V8 a, V8 b) {
    const V8 m = v8_set1(0x7FFFFFFFu);
    return _mm256_cmpgt_epi32(_mm256_and_si256(v8_flush(a), m), _mm256_and_si256(v8_flush(b), m));
}
SA_INLINE V8 v8_fceq(V8 a, V8 b) { return _mm256_cmpeq_epi32(v8_flush(a), v8_flush(b)); }

/* integer compares (SPU ceq / cgt / clgt) */
SA_INLINE V8 v8_ceq(V8 a, V8 b) { return _mm256_cmpeq_epi32(a, b); }
SA_INLINE V8 v8_cgt(V8 a, V8 b) { return _mm256_cmpgt_epi32(a, b); }
SA_INLINE V8 v8_clgt(V8 a, V8 b) {
    const V8 s = v8_set1(0x80000000u);
    return _mm256_cmpgt_epi32(_mm256_xor_si256(a, s), _mm256_xor_si256(b, s));
}

/* frest: sign | (253 - e, 0 for e >= 253, 255 for e == 0) | table[(x >> 18) & 31] */
static const uint32_t g_sa_frest_lut[32] __attribute__((aligned(32))) = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07 };
static const uint32_t g_sa_frsq_lut[64] __attribute__((aligned(32))) = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA };
SA_INLINE V8 v8_frest(V8 x) {
    const V8 e = _mm256_and_si256(_mm256_srli_epi32(x, 23), v8_set1(0xFF));
    V8 re = _mm256_sub_epi32(v8_set1(253), e);
    re = _mm256_andnot_si256(_mm256_cmpgt_epi32(e, v8_set1(252)), re);                  /* e >= 253: 0 */
    re = v8_selw(re, v8_set1(255), _mm256_cmpeq_epi32(e, _mm256_setzero_si256()));     /* e == 0: 255 */
    const V8 f = _mm256_i32gather_epi32((const int*)g_sa_frest_lut, _mm256_and_si256(_mm256_srli_epi32(x, 18), v8_set1(31)), 4);
    return _mm256_or_si256(_mm256_and_si256(x, v8_set1(0x80000000u)),
                           _mm256_or_si256(_mm256_slli_epi32(re, 23), _mm256_and_si256(f, v8_set1(0x7FFFFF))));
}
/* frsqest: (190 - (e + 1) / 2, 255 for e == 0) | table[(x >> 18) & 63]; the sign is dropped */
SA_INLINE V8 v8_frsqest(V8 x) {
    const V8 e = _mm256_and_si256(_mm256_srli_epi32(x, 23), v8_set1(0xFF));
    V8 re = _mm256_sub_epi32(v8_set1(190), _mm256_srli_epi32(_mm256_add_epi32(e, v8_set1(1)), 1));
    re = v8_selw(re, v8_set1(255), _mm256_cmpeq_epi32(e, _mm256_setzero_si256()));
    const V8 f = _mm256_i32gather_epi32((const int*)g_sa_frsq_lut, _mm256_and_si256(_mm256_srli_epi32(x, 18), v8_set1(63)), 4);
    return _mm256_or_si256(_mm256_slli_epi32(re, 23), _mm256_and_si256(f, v8_set1(0x7FFFFF)));
}
/* fi(x, est): the interpolation step (spu_fi's x86 form, lane for lane) */
SA_INLINE V8 v8_fi(V8 a, V8 est) {
    const V8 y    = _mm256_and_si256(a, v8_set1(0x7FFFF));
    const V8 hi   = _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(est, 10), v8_set1(0x1FFF)), 19);
    const V8 p    = _mm256_mullo_epi32(_mm256_and_si256(est, v8_set1(0x3FF)), y);
    const V8 d    = _mm256_sub_epi32(hi, p);
    const V8 brw  = v8_clgt(p, hi);                                              /* hi < p, unsigned */
    const V8 frac = _mm256_and_si256(_mm256_blendv_epi8(_mm256_srli_epi32(d, 9), _mm256_srli_epi32(d, 8), brw), v8_set1(0x7FFFFF));
    const V8 e    = _mm256_and_si256(_mm256_srli_epi32(est, 23), v8_set1(0xFF));
    const V8 e2   = _mm256_add_epi32(e, brw);
    const V8 zero = _mm256_or_si256(_mm256_cmpeq_epi32(e, _mm256_setzero_si256()), _mm256_cmpeq_epi32(e2, _mm256_setzero_si256()));
    const V8 res  = _mm256_or_si256(_mm256_and_si256(est, v8_set1(0x80000000u)), _mm256_or_si256(_mm256_slli_epi32(e2, 23), frac));
    return _mm256_andnot_si256(zero, res);
}

/* cuflt(x, i8) for x below 2^24 (exact as a float): x * 2^-(155 - i8), a power of two (exact
 * unless it leaves the range; callers pass small scales) */
SA_INLINE V8 v8_cuflt_small(V8 x, int i8) {
    const __m256 f = _mm256_cvtepi32_ps(x);
    return v8_i(_mm256_mul_ps(f, _mm256_castsi256_ps(v8_set1((uint32_t)(127 - (155 - i8)) << 23))));
}
/* cflts(x, i8): float * 2^(173 - i8) -> int32, truncated, saturated (the SPU's). Exponent 0 is
 * zero (DAZ). x86 gives 0x80000000 for an overflow, Inf or NaN (exponent 255): the sign of x
 * decides the saturation. The scale is exact (a power of two) unless the product overflows,
 * which RZ turns into FLT_MAX -> 0x80000000 -> saturated: the SPU's answer too. */
SA_INLINE V8 v8_cflts(V8 x, int i8) {
    const int sc = 173 - i8;                 /* 0..127 for the scales the callers use */
    const __m256 f = _mm256_mul_ps(v8_f(v8_flush(x)), _mm256_castsi256_ps(v8_set1((uint32_t)(127 + sc) << 23)));
    const V8 r = _mm256_cvttps_epi32(f);
    const V8 ovf = _mm256_cmpeq_epi32(r, v8_set1(0x80000000u));
    const V8 sat = _mm256_xor_si256(v8_set1(0x7FFFFFFFu), _mm256_srai_epi32(x, 31));   /* x >= 0: 0x7FFFFFFF, else 0x80000000 */
    return v8_selw(r, sat, ovf);
}

/* local store, big-endian words <-> lanes (word 0 of a quad = lane 0) */
#define SA_BSW128 _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)
SA_INLINE __m128i sa_ld4(const uint8_t* ls, uint32_t a) {
    return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (a & 0x3FFF0u))), SA_BSW128);
}
SA_INLINE void sa_st4(uint8_t* ls, uint32_t a, __m128i v) {
    _mm_storeu_si128((__m128i*)(ls + (a & 0x3FFF0u)), _mm_shuffle_epi8(v, SA_BSW128));
}
/* two quads (lo at a, hi at b) as one V8 */
SA_INLINE V8 sa_ld8(const uint8_t* ls, uint32_t a, uint32_t b) {
    return _mm256_inserti128_si256(_mm256_castsi128_si256(sa_ld4(ls, a)), sa_ld4(ls, b), 1);
}
SA_INLINE void sa_st8(uint8_t* ls, uint32_t a, uint32_t b, V8 v) {
    sa_st4(ls, a, _mm256_castsi256_si128(v));
    sa_st4(ls, b, _mm256_extracti128_si256(v, 1));
}
/* a u128 register (lane i = word i) to the local store, as stqd */
SA_INLINE void sa_stq_reg(uint8_t* ls, uint32_t a, const u128* r) {
    _mm_storeu_si128((__m128i*)(ls + (a & 0x3FFF0u)), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)r->_u8), SA_BSW128));
}

/* 8x4 transposes: Q[j] = quad of record j (x y z w), j = 0..7, lanes of the result = j */
SA_INLINE void sa_aos_to_soa(const __m128i q[8], V8* X, V8* Y, V8* Z, V8* W) {
    const V8 r0 = _mm256_inserti128_si256(_mm256_castsi128_si256(q[0]), q[4], 1);
    const V8 r1 = _mm256_inserti128_si256(_mm256_castsi128_si256(q[1]), q[5], 1);
    const V8 r2 = _mm256_inserti128_si256(_mm256_castsi128_si256(q[2]), q[6], 1);
    const V8 r3 = _mm256_inserti128_si256(_mm256_castsi128_si256(q[3]), q[7], 1);
    const V8 t0 = _mm256_unpacklo_epi32(r0, r1), t1 = _mm256_unpackhi_epi32(r0, r1);
    const V8 t2 = _mm256_unpacklo_epi32(r2, r3), t3 = _mm256_unpackhi_epi32(r2, r3);
    *X = _mm256_unpacklo_epi64(t0, t2); *Y = _mm256_unpackhi_epi64(t0, t2);
    *Z = _mm256_unpacklo_epi64(t1, t3); *W = _mm256_unpackhi_epi64(t1, t3);
}
SA_INLINE void sa_soa_to_aos(V8 X, V8 Y, V8 Z, V8 W, __m128i q[8]) {
    const V8 t0 = _mm256_unpacklo_epi32(X, Y), t1 = _mm256_unpackhi_epi32(X, Y);
    const V8 t2 = _mm256_unpacklo_epi32(Z, W), t3 = _mm256_unpackhi_epi32(Z, W);
    const V8 r0 = _mm256_unpacklo_epi64(t0, t2), r1 = _mm256_unpackhi_epi64(t0, t2);
    const V8 r2 = _mm256_unpacklo_epi64(t1, t3), r3 = _mm256_unpackhi_epi64(t1, t3);
    q[0] = _mm256_castsi256_si128(r0); q[4] = _mm256_extracti128_si256(r0, 1);
    q[1] = _mm256_castsi256_si128(r1); q[5] = _mm256_extracti128_si256(r1, 1);
    q[2] = _mm256_castsi256_si128(r2); q[6] = _mm256_extracti128_si256(r2, 1);
    q[3] = _mm256_castsi256_si128(r3); q[7] = _mm256_extracti128_si256(r3, 1);
}
/* lane j of v broadcast over a quad */
SA_INLINE __m128i sa_lane4(V8 v, int j) {
    const __m128i h = j < 4 ? _mm256_castsi256_si128(v) : _mm256_extracti128_si256(v, 1);
    switch (j & 3) {
    case 0: return _mm_shuffle_epi32(h, 0x00);
    case 1: return _mm_shuffle_epi32(h, 0x55);
    case 2: return _mm_shuffle_epi32(h, 0xAA);
    default: return _mm_shuffle_epi32(h, 0xFF);
    }
}

#endif /* INF2_SPU_AVX2_H */
