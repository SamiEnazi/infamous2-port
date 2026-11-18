/* inFAMOUS 2: the pose blend of the character-animation job 218 (jobmod_ab654d30fd90_efe70, LS 0x16320),
 * natively.
 *
 * 0x16320 is an ABI leaf function: it blends two skeleton poses B and C into A (in place, A = B in every
 * recorded call), bone by bone, four bones per SIMD group: a shortest-path quaternion slerp (acos and sin
 * by polynomials, the reciprocal estimates frest/frsqest + fi) and lerps of translation and scale, all
 * weighted by per-bone weight bytes, and re-quantised output weight bytes. No calls, channels or DMA.
 * Spec: work/bench/job218-analysis.md section 2 (dev tree), verified bit-exact there on 709 recorded calls
 * by an emulator; here every float operation is the engine's exact SPU helper, in the same order and on
 * the same lanes (lane = bone), so the results are the lifted code's bit for bit. The 1-3 padding bones of
 * the last group are computed and written as the SPU does, from whatever B/C/weights hold there.
 *
 * PS3RECOMP_NATIVE_J218=0: off (the lifted blend). PS3_SPU_NATIVE=0: every rewrite off.
 * J218_CHECK=1: the native computes into scratch, the lifted function runs, and every written byte is
 * compared (the lifted result is kept); J218_STATS=1: a summary line every 20000 calls; J218_SPEC=0: the
 * per-op exact math only (no speculative run). J218_WIDE=0: one group per pass (the 128-bit run) instead of two
 * (deck, the speculative math on 256-bit registers). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "native_float.h"
#include "spu_jobrec.h"
#include <immintrin.h>

int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void jobmod_ab654d30fd90_efe70_spu_func_00016320(spu_context* ctx);   /* the lifted functions (check mode) */
void jobmod_ab654d30fd90_efe70_spu_func_000179C0(spu_context* ctx);
void jobmod_ab654d30fd90_efe70_spu_func_00017780(spu_context* ctx);
void jobmod_ab654d30fd90_efe70_spu_func_00018B80(spu_context* ctx);

#define J218_IMAGE  218
#define J218_BLEND  0x16320u
#define LSM(a)      ((a) & SPU_LS_MASK)

static inline uint32_t R32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void P32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 SPLW(uint32_t x) { return V4(x, x, x, x); }
static inline void SQ(uint8_t* p, u128 v) { P32(p, v._u32[0]); P32(p + 4, v._u32[1]); P32(p + 8, v._u32[2]); P32(p + 12, v._u32[3]); }

#define F_ONE  0x3F800000u
#define F_NEG1 0xBF800000u
#define F_HALF 0x3F000000u
#define K255   0x3F808081u     /* 256/255 */
#define A0     0x3FC90FDAu
#define A1     0x3E5BBFCAu
#define A2     0x3DB63A9Eu
#define A3     0x3D4D8392u
#define S1     0xBE2AAAABu
#define S2     0x3C088887u
#define S3     0xB9500C03u
#define S4     0x3638B94Fu
#define LIN    0x3F7FBE77u     /* 0.999 */
#define Q0     0x3F7F0000u     /* 255/256 */
#define Q1     0x3B000000u     /* 1/512 */

static int s_mode = -1, s_check, s_stats, s_spec = 1;
static unsigned long long s_every = 20000;
static unsigned long long s_calls, s_checked, s_bad, s_declined, s_groups, s_spec_fail, s_why[4];
static _Thread_local int t_bypass;

static inline u128 SIN(u128 x)
{
    const u128 x2 = spu_fm(x, x);
    return spu_fm(spu_fma(spu_fma(spu_fma(spu_fma(SPLW(S4), x2, SPLW(S3)), x2, SPLW(S2)), x2, SPLW(S1)), x2, SPLW(F_ONE)), x);
}

/* LS quads <-> SoA lanes with SSE: a big-endian quad byte-swapped per word (pshufb), then 4x4 transposes */
static inline __m128i BSWAP32(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
static inline u128 U(__m128i v) { u128 r; memcpy(&r, &v, 16); return r; }
static inline __m128i M(u128 v) { __m128i r; memcpy(&r, &v, 16); return r; }
static inline void TRANSPOSE(__m128i* a, __m128i* b, __m128i* c, __m128i* d)
{
    __m128 x = _mm_castsi128_ps(*a), y = _mm_castsi128_ps(*b), z = _mm_castsi128_ps(*c), w = _mm_castsi128_ps(*d);
    _MM_TRANSPOSE4_PS(x, y, z, w);
    *a = _mm_castps_si128(x); *b = _mm_castps_si128(y); *c = _mm_castps_si128(z); *d = _mm_castps_si128(w);
}
/* the 3 quads (q, t, s) of bones i0..i0+3 at P -> v[0..11] (component k across the 4 bones) */
static inline void load_soa(const uint8_t* ls, uint32_t P, uint32_t i0, u128 v[12])
{
    for (int q = 0; q < 3; q++) {
        __m128i r[4];
        for (int j = 0; j < 4; j++) r[j] = BSWAP32(_mm_loadu_si128((const __m128i*)(const void*)(ls + LSM(P + 48u * (i0 + j) + 16u * q))));
        TRANSPOSE(&r[0], &r[1], &r[2], &r[3]);
        for (int k = 0; k < 4; k++) v[4 * q + k] = U(r[k]);
    }
}

/* the blend's float math for one group, exact: every op its per-op lean form (the reference) */
static __attribute__((noinline)) void blend_math_exact(const u128* Bv, const u128* Cv, u128 vmB, u128 vmC, u128 T,
                                                        u128 O[12], u128* qbo)
{
    const u128 one = SPLW(F_ONE), zero = SPLW(0);
    const u128 wB = spu_fm(spu_cuflt(vmB, 147), SPLW(K255));
    const u128 wC = spu_fm(spu_cuflt(vmC, 147), SPLW(K255));
    const u128 d = spu_fma(Bv[3], Cv[3], spu_fma(Bv[2], Cv[2], spu_fma(Bv[1], Cv[1], spu_fm(Bv[0], Cv[0]))));
    const u128 gt = spu_fcgt_lean(wC, wB);
    const u128 mx = spu_selb(wB, wC, gt);
    const u128 r = spu_fi(mx, spu_frest(mx));
    const u128 rc = spu_fma(spu_fnms(mx, r, one), r, r);
    const u128 u = spu_selb(spu_fm(T, wC), spu_fs(spu_fma(T, wB, wC), wB), gt);
    const u128 s = spu_fm(u, rc);
    const u128 oms = spu_fs(one, s);
    const u128 sg = spu_selb(one, SPLW(F_NEG1), spu_fcgt_lean(zero, d));
    const u128 ad = spu_fm(d, sg);
    const u128 p = spu_fnms(ad, spu_fnms(ad, spu_fnms(ad, spu_fm(ad, SPLW(A3)), SPLW(A2)), SPLW(A1)), SPLW(A0));
    const u128 e = spu_fs(one, ad);
    const u128 y = spu_fi(e, spu_frsqest(e));
    const u128 th = spu_fm(p, spu_fnms(y, ad, y));
    const u128 sth = SIN(th), sa = SIN(spu_fm(oms, th)), sb = SIN(spu_fm(s, th));
    const u128 ri = spu_fi(sth, spu_frest(sth));
    const u128 lin = spu_fcgt_lean(ad, SPLW(LIN));
    const u128 kB = spu_selb(spu_fm(sa, ri), oms, lin);
    const u128 kC = spu_selb(spu_fm(sb, ri), s, lin);
    for (int k = 0; k < 4; k++) O[k] = spu_fma(spu_fm(Cv[k], sg), kC, spu_fm(Bv[k], kB));
    for (int k = 4; k < 12; k++) O[k] = spu_fnms(Bv[k], s, spu_fma(Cv[k], s, Bv[k]));
    const u128 wo = spu_fnms(wB, s, spu_fma(wC, s, wB));
    *qbo = spu_cflts(spu_fma(wo, SPLW(Q0), SPLW(Q1)), 165);
}

/* The same math speculatively (spu_helpers.h, "speculative float regions"): every checked op is its fast form
 * with no branch, folding its range verdict into one accumulator; one test at the end. All fast -> these are
 * exactly the per-op results; else the caller runs blend_math_exact. One change, for the lanes only: a bone
 * whose poses are almost equal (lin: |d| > 0.999) takes kB = 1-s, kC = s and never uses the acos/sin path,
 * where its e = 1-|d| can be 0 and frsqest(0) leaves the float range (a failed run for nothing). Those lanes
 * feed 0.5 into that path instead; every other lane computes on its own values, so every output bit is the
 * exact form's. Likewise a bone with both weights 0 (39% of the groups in the recordings failed on it). */
#define SFM(a, b)      spu_fm_spec((a), (b), acc)
#define SFS(a, b)      spu_fs_spec((a), (b), acc)
#define SFMA(a, b, c)  spu_fma_spec((a), (b), (c), acc)
#define SFNMS(a, b, c) spu_fnms_spec((a), (b), (c), acc)
static inline u128 SIN_S(u128 x, spu_spec_acc* acc)
{
    const u128 x2 = SFM(x, x);
    return SFM(SFMA(SFMA(SFMA(SFMA(SPLW(S4), x2, SPLW(S3)), x2, SPLW(S2)), x2, SPLW(S1)), x2, SPLW(F_ONE)), x);
}
static inline int blend_math_spec(const u128* Bv, const u128* Cv, u128 vmB, u128 vmC, u128 T, u128 O[12], u128* qbo)
{
    spu_spec_acc a = SPU_SPEC_INIT(), *acc = &a;
    const u128 one = SPLW(F_ONE), zero = SPLW(0);
    const u128 wB = SFM(spu_cuflt(vmB, 147), SPLW(K255));
    const u128 wC = SFM(spu_cuflt(vmC, 147), SPLW(K255));
    const u128 d = SFMA(Bv[3], Cv[3], SFMA(Bv[2], Cv[2], SFMA(Bv[1], Cv[1], SFM(Bv[0], Cv[0]))));
    const u128 gt = spu_fcgt_lean(wC, wB);
    const u128 mx = spu_selb(wB, wC, gt);
    /* both weights 0 (mx = +0): the exact form's 1/mx leaves the range, and s = u * rc = (+0) * that = +0;
     * u is +0 there whatever rc is (T * +0, flushed), so a 1.0 in place of mx gives the same s = +0 */
    const u128 mx1 = spu_selb(mx, one, U(_mm_cmpeq_epi32(M(mx), _mm_setzero_si128())));
    const u128 r = spu_fi(mx1, spu_frest(mx1));
    const u128 rc = SFMA(SFNMS(mx1, r, one), r, r);
    const u128 u = spu_selb(SFM(T, wC), SFS(SFMA(T, wB, wC), wB), gt);
    const u128 s = SFM(u, rc);
    const u128 oms = SFS(one, s);
    const u128 sg = spu_selb(one, SPLW(F_NEG1), spu_fcgt_lean(zero, d));
    const u128 ad = SFM(d, sg);
    const u128 lin = spu_fcgt_lean(ad, SPLW(LIN));
    const u128 adt = spu_selb(ad, SPLW(F_HALF), lin);
    const u128 p = SFNMS(adt, SFNMS(adt, SFNMS(adt, SFM(adt, SPLW(A3)), SPLW(A2)), SPLW(A1)), SPLW(A0));
    const u128 e = SFS(one, adt);
    const u128 y = spu_fi(e, spu_frsqest(e));
    const u128 th = SFM(p, SFNMS(y, adt, y));
    const u128 sth = SIN_S(th, acc), sa = SIN_S(SFM(oms, th), acc), sb = SIN_S(SFM(s, th), acc);
    const u128 ri = spu_fi(sth, spu_frest(sth));
    const u128 kB = spu_selb(SFM(sa, ri), oms, lin);
    const u128 kC = spu_selb(SFM(sb, ri), s, lin);
    for (int k = 0; k < 4; k++) O[k] = SFMA(SFM(Cv[k], sg), kC, SFM(Bv[k], kB));
    for (int k = 4; k < 12; k++) O[k] = SFNMS(Bv[k], s, SFMA(Cv[k], s, Bv[k]));
    const u128 wo = SFNMS(wB, s, SFMA(wC, s, wB));
    *qbo = spu_cflts_spec(SFMA(wo, SPLW(Q0), SPLW(Q1)), 165, acc);
    return SPU_SPEC_OK(a);
}

/* (deck) The speculative math on two groups at once: 8 lanes in 256-bit registers (lanes 0-3 the first group,
 * 4-7 the second). Every operation is blend_math_spec's, lane for lane: the same IEEE single operations under
 * the same MXCSR (round toward zero, FTZ, DAZ), the same range verdict folded into one accumulator, the same
 * exponent-0 flush; the estimate tables (frest / frsqest) and fi are the helpers' own definitions. A failed
 * verdict sends both groups to blend_math_exact, so the results are always the per-op forms' bits.
 * J218_WIDE=0: one group at a time (blend_math_spec), as before. */
typedef __m256i Y8;
#define Y1(x) _mm256_set1_epi32((int)(x))
static inline Y8 y8_flush(Y8 x) { return _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(x, Y1(0x7F800000u)), _mm256_setzero_si256()), x); }
static inline Y8 y8_out(Y8 s, Y8* acc) { *acc = _mm256_max_epu32(*acc, _mm256_and_si256(s, Y1(0x7FFFFFFFu))); return y8_flush(s); }
static inline Y8 y8_fm(Y8 a, Y8 b, Y8* acc)
{
    __m256 x = _mm256_castsi256_ps(a); __asm__("" : "+x"(x));
    return y8_out(_mm256_castps_si256(_mm256_mul_ps(x, _mm256_castsi256_ps(b))), acc);
}
static inline Y8 y8_fa(Y8 a, Y8 b, uint32_t negb, Y8* acc)
{
    __m256 x = _mm256_castsi256_ps(a); __asm__("" : "+x"(x));
    return y8_out(_mm256_castps_si256(_mm256_add_ps(x, _mm256_castsi256_ps(_mm256_xor_si256(b, Y1(negb))))), acc);
}
static inline Y8 y8_fmadd(Y8 a, Y8 b, Y8 c, uint32_t negp, uint32_t negc, Y8* acc)
{
    __m256 x = _mm256_castsi256_ps(_mm256_xor_si256(a, Y1(negp))); __asm__("" : "+x"(x));
    return y8_out(_mm256_castps_si256(_mm256_fmadd_ps(x, _mm256_castsi256_ps(b), _mm256_castsi256_ps(_mm256_xor_si256(c, Y1(negc))))), acc);
}
#define Y8FM(a, b)      y8_fm((a), (b), acc)
#define Y8FS(a, b)      y8_fa((a), (b), 0x80000000u, acc)
#define Y8FMA(a, b, c)  y8_fmadd((a), (b), (c), 0, 0, acc)
#define Y8FNMS(a, b, c) y8_fmadd((a), (b), (c), 0x80000000u, 0, acc)
static inline Y8 y8_selb(Y8 a, Y8 b, Y8 m) { return _mm256_or_si256(_mm256_andnot_si256(m, a), _mm256_and_si256(b, m)); }
static inline Y8 y8_key(Y8 x) { const Y8 f = y8_flush(x); return _mm256_xor_si256(f, _mm256_srli_epi32(_mm256_srai_epi32(f, 31), 1)); }
static inline Y8 y8_fcgt(Y8 a, Y8 b) { return _mm256_cmpgt_epi32(y8_key(a), y8_key(b)); }
/* spu__xcg_cuflt(v, 147) for v < 2^31 (weight bytes): the conversion, then 2^-8 in the exponent field */
static inline Y8 y8_cuflt147(Y8 v)
{
    __asm__("" : "+x"(v));
    const Y8 fb = _mm256_castps_si256(_mm256_cvtepi32_ps(v));
    const Y8 s23 = Y1(8u << 23);
    return _mm256_and_si256(_mm256_sub_epi32(fb, s23), _mm256_cmpgt_epi32(_mm256_and_si256(fb, Y1(0x7F800000u)), s23));
}
static inline Y8 y8_frest(Y8 x)          /* spu_frest */
{
    const Y8 e = _mm256_and_si256(_mm256_srli_epi32(x, 23), Y1(0xFF));
    const Y8 f = _mm256_i32gather_epi32((const int*)spu_frest_fraction_lut, _mm256_and_si256(_mm256_srli_epi32(x, 18), Y1(31)), 4);
    Y8 re = _mm256_sub_epi32(Y1(253), e);
    re = _mm256_andnot_si256(_mm256_cmpgt_epi32(e, Y1(252)), re);                 /* exp >= 253: 0 */
    re = _mm256_blendv_epi8(re, Y1(255), _mm256_cmpeq_epi32(e, _mm256_setzero_si256()));
    return _mm256_or_si256(_mm256_and_si256(x, Y1(0x80000000u)), _mm256_or_si256(_mm256_slli_epi32(re, 23), _mm256_and_si256(f, Y1(0x7FFFFF))));
}
static inline Y8 y8_frsqest(Y8 x)        /* spu_frsqest */
{
    const Y8 e = _mm256_and_si256(_mm256_srli_epi32(x, 23), Y1(0xFF));
    const Y8 f = _mm256_i32gather_epi32((const int*)spu_frsqest_fraction_lut, _mm256_and_si256(_mm256_srli_epi32(x, 18), Y1(63)), 4);
    Y8 re = _mm256_sub_epi32(Y1(190), _mm256_srli_epi32(_mm256_add_epi32(e, Y1(1)), 1));
    re = _mm256_blendv_epi8(re, Y1(255), _mm256_cmpeq_epi32(e, _mm256_setzero_si256()));
    return _mm256_or_si256(_mm256_slli_epi32(re, 23), _mm256_and_si256(f, Y1(0x7FFFFF)));
}
static inline Y8 y8_fi(Y8 a, Y8 est)     /* spu_fi (the SSE form, lane for lane) */
{
    const Y8 y = _mm256_and_si256(a, Y1(0x7FFFF));
    const Y8 hi = _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(est, 10), Y1(0x1FFF)), 19);
    const Y8 p = _mm256_mullo_epi32(_mm256_and_si256(est, Y1(0x3FF)), y);
    const Y8 d = _mm256_sub_epi32(hi, p);
    const Y8 bias = Y1(0x80000000u);
    const Y8 brw = _mm256_cmpgt_epi32(_mm256_xor_si256(p, bias), _mm256_xor_si256(hi, bias));
    const Y8 frac = _mm256_and_si256(_mm256_blendv_epi8(_mm256_srli_epi32(d, 9), _mm256_srli_epi32(d, 8), brw), Y1(0x7FFFFF));
    const Y8 e = _mm256_and_si256(_mm256_srli_epi32(est, 23), Y1(0xFF));
    const Y8 e2 = _mm256_add_epi32(e, brw);
    const Y8 zero = _mm256_or_si256(_mm256_cmpeq_epi32(e, _mm256_setzero_si256()), _mm256_cmpeq_epi32(e2, _mm256_setzero_si256()));
    const Y8 res = _mm256_or_si256(_mm256_and_si256(est, bias), _mm256_or_si256(_mm256_slli_epi32(e2, 23), frac));
    return _mm256_andnot_si256(zero, res);
}
static inline Y8 y8_sin(Y8 x, Y8* acc)
{
    const Y8 x2 = Y8FM(x, x);
    return Y8FM(Y8FMA(Y8FMA(Y8FMA(Y8FMA(Y1(S4), x2, Y1(S3)), x2, Y1(S2)), x2, Y1(S1)), x2, Y1(F_ONE)), x);
}
/* spu_cflts_spec(x, 165): scale 8 */
static inline Y8 y8_cflts165(Y8 x, Y8* acc)
{
    const Y8 e = _mm256_and_si256(x, Y1(0x7F800000u));
    *acc = _mm256_max_epu32(*acc, e);
    const __m256 f = _mm256_mul_ps(_mm256_castsi256_ps(x), _mm256_castsi256_ps(Y1((127u + 8u) << 23)));
    Y8 r = _mm256_cvttps_epi32(f);
    r = _mm256_blendv_epi8(r, Y1(0x7FFFFFFF), _mm256_castps_si256(_mm256_cmp_ps(f, _mm256_set1_ps(2147483648.0f), _CMP_GE_OQ)));
    return _mm256_andnot_si256(_mm256_cmpeq_epi32(e, _mm256_setzero_si256()), r);
}
static inline int blend_math_spec8(const Y8* Bv, const Y8* Cv, Y8 vmB, Y8 vmC, Y8 T, Y8 O[12], Y8* qbo)
{
    Y8 a = _mm256_setzero_si256(), *acc = &a;
    const Y8 one = Y1(F_ONE), zero = _mm256_setzero_si256();
    const Y8 wB = Y8FM(y8_cuflt147(vmB), Y1(K255));
    const Y8 wC = Y8FM(y8_cuflt147(vmC), Y1(K255));
    const Y8 d = Y8FMA(Bv[3], Cv[3], Y8FMA(Bv[2], Cv[2], Y8FMA(Bv[1], Cv[1], Y8FM(Bv[0], Cv[0]))));
    const Y8 gt = y8_fcgt(wC, wB);
    const Y8 mx = y8_selb(wB, wC, gt);
    const Y8 mx1 = y8_selb(mx, one, _mm256_cmpeq_epi32(mx, zero));
    const Y8 r = y8_fi(mx1, y8_frest(mx1));
    const Y8 rc = Y8FMA(Y8FNMS(mx1, r, one), r, r);
    const Y8 u = y8_selb(Y8FM(T, wC), Y8FS(Y8FMA(T, wB, wC), wB), gt);
    const Y8 s = Y8FM(u, rc);
    const Y8 oms = Y8FS(one, s);
    const Y8 sg = y8_selb(one, Y1(F_NEG1), y8_fcgt(zero, d));
    const Y8 ad = Y8FM(d, sg);
    const Y8 lin = y8_fcgt(ad, Y1(LIN));
    const Y8 adt = y8_selb(ad, Y1(F_HALF), lin);
    const Y8 p = Y8FNMS(adt, Y8FNMS(adt, Y8FNMS(adt, Y8FM(adt, Y1(A3)), Y1(A2)), Y1(A1)), Y1(A0));
    const Y8 e = Y8FS(one, adt);
    const Y8 y = y8_fi(e, y8_frsqest(e));
    const Y8 th = Y8FM(p, Y8FNMS(y, adt, y));
    const Y8 sth = y8_sin(th, acc), sa = y8_sin(Y8FM(oms, th), acc), sb = y8_sin(Y8FM(s, th), acc);
    const Y8 ri = y8_fi(sth, y8_frest(sth));
    const Y8 kB = y8_selb(Y8FM(sa, ri), oms, lin);
    const Y8 kC = y8_selb(Y8FM(sb, ri), s, lin);
    for (int k = 0; k < 4; k++) O[k] = Y8FMA(Y8FM(Cv[k], sg), kC, Y8FM(Bv[k], kB));
    for (int k = 4; k < 12; k++) O[k] = Y8FNMS(Bv[k], s, Y8FMA(Cv[k], s, Bv[k]));
    const Y8 wo = Y8FNMS(wB, s, Y8FMA(wC, s, wB));
    *qbo = y8_cflts165(Y8FMA(wo, Y1(Q0), Y1(Q1)), acc);
    return _mm256_testz_si256(_mm256_cmpgt_epi32(a, Y1(0x7F7FFFFEu)), _mm256_set1_epi32(-1));
}
static int s_wide = -1;   /* J218_WIDE */

static void blend_store(const u128* Bv, const u128* Cv, const uint32_t* mB, const uint32_t* mC, const u128* O, u128 qb,
                        uint8_t* dst, uint8_t wout[4]);
static void blend_spec_fail_note(const u128* Bv, const u128* Cv, const uint32_t* mB, const uint32_t* mC)
{
    __atomic_add_fetch(&s_spec_fail, 1, __ATOMIC_RELAXED);
    /* why: 0 both weights 0 in a lane, 1 an input word with exponent 255, 2 padding lane, 3 other */
    int why = 3;
    for (int j = 0; j < 4; j++) if (mB[j] == 0 && mC[j] == 0) why = 0;
    if (why == 3)
        for (int k = 0; k < 12; k++)
            for (int j = 0; j < 4; j++)
                if (((Bv[k]._u32[j] | Cv[k]._u32[j]) & 0x7F800000u) == 0x7F800000u) why = 1;
    __atomic_add_fetch(&s_why[why], 1, __ATOMIC_RELAXED);
}
/* groups g and g + 1 (bones i0 .. i0 + 7): both read before either is written (A may be B) */
static void blend_group2(const uint8_t* ls, uint32_t B, uint32_t MB, uint32_t C, uint32_t MC, uint32_t i0,
                         u128 T, uint8_t* dst0, uint8_t* dst1, uint8_t w0[4], uint8_t w1[4])
{
    u128 Bv[2][12], Cv[2][12];
    uint32_t mB[2][4], mC[2][4];
    for (int h = 0; h < 2; h++) {
        load_soa(ls, B, i0 + 4u * (uint32_t)h, Bv[h]);
        load_soa(ls, C, i0 + 4u * (uint32_t)h, Cv[h]);
        for (int j = 0; j < 4; j++) {
            mB[h][j] = MB ? ls[LSM(MB + i0 + 4u * (uint32_t)h + (uint32_t)j)] : 255u;
            mC[h][j] = MC ? ls[LSM(MC + i0 + 4u * (uint32_t)h + (uint32_t)j)] : 255u;
        }
    }
    u128 O[2][12], qb[2];
    Y8 YB[12], YC[12], YO[12], yq;
    for (int k = 0; k < 12; k++) {
        YB[k] = _mm256_set_m128i(M(Bv[1][k]), M(Bv[0][k]));
        YC[k] = _mm256_set_m128i(M(Cv[1][k]), M(Cv[0][k]));
    }
    const Y8 vmB = _mm256_setr_epi32((int)mB[0][0], (int)mB[0][1], (int)mB[0][2], (int)mB[0][3], (int)mB[1][0], (int)mB[1][1], (int)mB[1][2], (int)mB[1][3]);
    const Y8 vmC = _mm256_setr_epi32((int)mC[0][0], (int)mC[0][1], (int)mC[0][2], (int)mC[0][3], (int)mC[1][0], (int)mC[1][1], (int)mC[1][2], (int)mC[1][3]);
    if (__builtin_expect(blend_math_spec8(YB, YC, vmB, vmC, Y1(T._u32[0]), YO, &yq), 1)) {
        for (int k = 0; k < 12; k++) { O[0][k] = U(_mm256_castsi256_si128(YO[k])); O[1][k] = U(_mm256_extracti128_si256(YO[k], 1)); }
        qb[0] = U(_mm256_castsi256_si128(yq)); qb[1] = U(_mm256_extracti128_si256(yq, 1));
    } else {
        for (int h = 0; h < 2; h++) {
            if (s_stats) blend_spec_fail_note(Bv[h], Cv[h], mB[h], mC[h]);
            const u128 vb = V4(mB[h][0], mB[h][1], mB[h][2], mB[h][3]), vc = V4(mC[h][0], mC[h][1], mC[h][2], mC[h][3]);
            blend_math_exact(Bv[h], Cv[h], vb, vc, T, O[h], &qb[h]);
        }
    }
    blend_store(Bv[0], Cv[0], mB[0], mC[0], O[0], qb[0], dst0, w0);
    blend_store(Bv[1], Cv[1], mB[1], mC[1], O[1], qb[1], dst1, w1);
}

/* one group of 4 bones (lanes); out: 12 quads (bone j: q, t, s at 12j..), wout: the 4 weight bytes */
static void blend_group(const uint8_t* ls, uint32_t B, uint32_t MB, uint32_t C, uint32_t MC, uint32_t i0,
                        u128 T, uint8_t* dst, uint8_t wout[4])
{
    u128 Bv[12], Cv[12];                 /* SoA: [k] = component k (0-3 quat, 4-7 translation, 8-11 scale) */
    uint32_t mB[4], mC[4];
    load_soa(ls, B, i0, Bv);
    load_soa(ls, C, i0, Cv);
    for (int j = 0; j < 4; j++) {
        mB[j] = MB ? ls[LSM(MB + i0 + j)] : 255u;
        mC[j] = MC ? ls[LSM(MC + i0 + j)] : 255u;
    }
    const u128 vmB = V4(mB[0], mB[1], mB[2], mB[3]), vmC = V4(mC[0], mC[1], mC[2], mC[3]);
    u128 O[12], qb;
    if (__builtin_expect(!s_spec || !blend_math_spec(Bv, Cv, vmB, vmC, T, O, &qb), 0)) {
        if (s_stats) blend_spec_fail_note(Bv, Cv, mB, mC);
        blend_math_exact(Bv, Cv, vmB, vmC, T, O, &qb);
    }
    blend_store(Bv, Cv, mB, mC, O, qb, dst, wout);
}
static void blend_store(const u128* Bv, const u128* Cv, const uint32_t* mB, const uint32_t* mC, const u128* O, u128 qb,
                        uint8_t* dst, uint8_t wout[4])
{
    /* a weight byte 0: that bone is the other pose's raw bits (C's 0 first, then B's 0 wins) */
    const __m128i mbv = _mm_setr_epi32((int)mB[0], (int)mB[1], (int)mB[2], (int)mB[3]);
    const __m128i mcv = _mm_setr_epi32((int)mC[0], (int)mC[1], (int)mC[2], (int)mC[3]);
    const __m128i b0 = _mm_cmpeq_epi32(mbv, _mm_setzero_si128()), c0 = _mm_cmpeq_epi32(mcv, _mm_setzero_si128());
    __m128i R[12];
    for (int k = 0; k < 12; k++) {
        __m128i v = M(O[k]);
        v = _mm_blendv_epi8(v, M(Bv[k]), c0);
        v = _mm_blendv_epi8(v, M(Cv[k]), b0);
        R[k] = v;
    }
    for (int q = 0; q < 3; q++) {
        TRANSPOSE(&R[4 * q], &R[4 * q + 1], &R[4 * q + 2], &R[4 * q + 3]);
        for (int j = 0; j < 4; j++) _mm_storeu_si128((__m128i*)(void*)(dst + 48 * j + 16 * q), BSWAP32(R[4 * q + j]));
    }
    for (int j = 0; j < 4; j++) {
        int32_t q = (int32_t)qb._u32[j];
        if (q > 255) q = 255;
        if (0 > q) q = 0;
        wout[j] = (uint8_t)q;
    }
}

static inline int disjoint(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a + an <= b || b + bn <= a; }

static void j218_init(void)
{
    const char* e = getenv("PS3RECOMP_NATIVE_J218");
    s_mode = e ? atoi(e) : 1;
    s_check = getenv("J218_CHECK") != 0;
    s_stats = getenv("J218_STATS") != 0;
    if (s_stats) { const long long v = atoll(getenv("J218_STATS")); if (v > 1) s_every = (unsigned long long)v; }
    { const char* v = getenv("J218_SPEC"); s_spec = !(v && v[0] == '0'); }
    { const char* v = getenv("J218_WIDE"); s_wide = !(v && v[0] == '0'); }
    fprintf(stderr, "[j218] natives (blend 0x16320, hierarchy 0x179C0, matrices 0x17780 / 0x18B80, root loop 0x5814): %s%s\n",
            s_mode ? "on" : "off (PS3RECOMP_NATIVE_J218=0)", s_mode && s_check ? ", CHECK mode (the lifted result is kept)" : "");
}

static int j218_blend(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) j218_init();
    if (!s_mode || t_bypass || (g_spu_jobrec_mode & SPU_JOBREC_RECORD)) return 0;
    uint8_t* ls = ctx->ls;
    const uint32_t A = LSM(ctx->gpr[3]._u32[0]), MA = LSM(ctx->gpr[4]._u32[0]), B = LSM(ctx->gpr[5]._u32[0]);
    const uint32_t MB = LSM(ctx->gpr[6]._u32[0]), C = LSM(ctx->gpr[7]._u32[0]), MC = LSM(ctx->gpr[8]._u32[0]);
    const u128 T = SPLW(ctx->gpr[9]._u32[0]);
    const uint32_t n = ctx->gpr[10]._u32[0], groups = (n + 3u) >> 2;
    const uint32_t sp = LSM(ctx->gpr[1]._u32[0]);
    const unsigned long long c = s_stats ? __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED) : 0;
    /* what the group order of this rewrite may not reproduce: decline (the lifted blend runs) */
    const uint32_t an = 192u * groups, wn = 4u * groups;
    if (n > 4096u || A + an > SPU_LS_SIZE || B + an + 192u > SPU_LS_SIZE || C + an + 192u > SPU_LS_SIZE ||
        MA + wn > SPU_LS_SIZE || (MB && MB + wn > SPU_LS_SIZE) || (MC && MC + wn > SPU_LS_SIZE) ||
        !(A == B || disjoint(A, an, B, an)) || !disjoint(A, an, C, an) ||
        !(MA == MB || !MB || disjoint(MA, wn, MB, wn)) || (MC && !disjoint(MA, wn, MC, wn)) ||
        !disjoint(A, an, MA, wn) || (MB && !disjoint(A, an, MB, wn)) || (MC && !disjoint(A, an, MC, wn)) ||
        sp < 0x200u) {
        if (s_stats) __atomic_add_fetch(&s_declined, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (s_check) {
        /* the native into scratch (and the inputs it read as they were), then the lifted function */
        /* (deck-merge) on the heap, made at the first check: the module's static TLS has to fit glibc's surplus */
        static _Thread_local uint8_t* o;                 /* 1024 * 192: n <= 4096, at most 1024 groups */
        static _Thread_local uint8_t* w;                 /* 1024 * 4 */
        if (!o && !(o = (uint8_t*)malloc(1024 * 192))) return 0;
        if (!w && !(w = (uint8_t*)malloc(1024 * 4))) return 0;
        { uint32_t g = 0;
          if (s_wide && s_spec && SPU__RZ_ON)
              for (; g + 1 < groups; g += 2)
                  blend_group2(ls, B, MB, C, MC, 4u * g, T, o + 192u * g, o + 192u * (g + 1), w + 4u * g, w + 4u * (g + 1));
          for (; g < groups; g++) blend_group(ls, B, MB, C, MC, 4u * g, T, o + 192u * g, w + 4u * g); }
        const u128 r0 = ctx->gpr[0];
        t_bypass = 1;
        { const int32_t si = (int32_t)ctx->image_id; ctx->host_depth++;
          jobmod_ab654d30fd90_efe70_spu_func_00016320(ctx); spu_drain_call(ctx, r0._u32[0]); ctx->host_depth--;
          spu_img_restore(ctx, si); }
        t_bypass = 0;
        int bad = 0;
        for (uint32_t g = 0; g < groups && !bad; g++) {
            if (memcmp(ls + A + 192u * g, o + 192u * g, 192)) bad = 1;
            for (int j = 0; j < 4 && !bad; j++) if (ls[MA + 4u * g + j] != w[4u * g + j]) bad = 2;
        }
        __atomic_add_fetch(&s_checked, 1, __ATOMIC_RELAXED);
        if (bad && __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED) <= 8)
            fprintf(stderr, "[j218] CHECK: native differs (%s) n=%u t=%08X A=%05X B=%05X C=%05X MB=%05X MC=%05X\n",
                    bad == 1 ? "bones" : "weights", n, T._u32[0], A, B, C, MB, MC);
        if (s_stats && c % s_every == 0)
            fprintf(stderr, "[j218] %llu calls, %llu declined, check %llu/%llu differ, %llu groups, %llu speculative runs failed (w00 %llu e255 %llu other %llu)\n", c, s_declined, s_bad, s_checked, s_groups, s_spec_fail, s_why[0], s_why[1], s_why[3]);
        ctx->gpr[0] = r0;
        return 1;
    }
    /* the prologue's red zone: r80..r109 at sp-0x10 .. sp-0x1E0 (sp not moved), as the lifted code leaves it */
    for (int k = 0; k < 30; k++) SQ(ls + LSM(sp - 0x10u * (uint32_t)(k + 1)), ctx->gpr[80 + k]);
    uint8_t w[4];
    if (s_stats) __atomic_add_fetch(&s_groups, groups, __ATOMIC_RELAXED);
    uint32_t g = 0;
    if (s_wide && s_spec && SPU__RZ_ON)
        for (; g + 1 < groups; g += 2) {    /* (deck) two groups per pass, both read before either is written */
            uint8_t w1[4];
            blend_group2(ls, B, MB, C, MC, 4u * g, T, ls + A + 192u * g, ls + A + 192u * (g + 1), w, w1);
            memcpy(ls + MA + 4u * g, w, 4);
            memcpy(ls + MA + 4u * (g + 1), w1, 4);
        }
    for (; g < groups; g++) {
        blend_group(ls, B, MB, C, MC, 4u * g, T, ls + A + 192u * g, w);   /* group g is read before it is written: A = B is fine */
        memcpy(ls + MA + 4u * g, w, 4);
    }
    if (s_stats && c % s_every == 0)
        fprintf(stderr, "[j218] %llu calls, %llu declined, %llu groups, %llu speculative runs failed (w00 %llu e255 %llu other %llu)\n", c, s_declined, s_groups, s_spec_fail, s_why[0], s_why[1], s_why[3]);
    return 1;
}

/* ===================================================================================================
 * The hierarchy and finalize kernels (spec: work/bench/job218-fk-spec.md, every formula verified there
 * bit-exact on every recorded call and in a whole-job Python replay). Float math: the speculative forms
 * with one verdict per group/matrix and the per-op exact forms as the fallback (FK_* below take a constant
 * `spec`, so each body is compiled twice). LS quads are handled in host lane order (BSWAP32: u32 lane i =
 * SPU word i).
 * =================================================================================================== */
#define F_TWO 0x40000000u
#define LSQ(a) ((a) & 0x3FFF0u)
static inline __m128i LDQ(const uint8_t* ls, uint32_t a) { return BSWAP32(_mm_loadu_si128((const __m128i*)(const void*)(ls + LSQ(a)))); }
static inline void STQ(uint8_t* ls, uint32_t a, __m128i v) { _mm_storeu_si128((__m128i*)(void*)(ls + LSQ(a)), BSWAP32(v)); }
static inline u128 SPL(u128 v, int i) { return SPLW(v._u32[i]); }
#define XFM(a, b)      (spec ? spu_fm_spec((a), (b), acc) : spu_fm((a), (b)))
#define XFA(a, b)      (spec ? spu_fa_spec((a), (b), acc) : spu_fa((a), (b)))
#define XFMA(a, b, c)  (spec ? spu_fma_spec((a), (b), (c), acc) : spu_fma((a), (b), (c)))
#define XFNMS(a, b, c) (spec ? spu_fnms_spec((a), (b), (c), acc) : spu_fnms((a), (b), (c)))
static unsigned long long s_fk_calls, s_fk_decl, s_fk_fail, s_mat_calls, s_mat_decl, s_mat_fail,
                          s_skin_calls, s_skin_decl, s_skin_fail, s_root_calls, s_root_decl, s_root_fail, s_kchk, s_kbad;
#define CNT(v) do { if (s_stats) __atomic_add_fetch(&(v), 1, __ATOMIC_RELAXED); } while (0)

/* ---- 0x179C0: local -> model space of a (child, parent) pair list, 4 pairs (lanes) per group ---- */
static inline __attribute__((always_inline)) int fk_math(const int spec, const u128* Pq, const u128* Pt, const u128* Ps,
                                                         const u128* PLs_in, const u128* Cq, const u128* Ct, const u128* Cs,
                                                         u128 flag, u128* Oq, u128* Ot, u128* Os)
{
    spu_spec_acc av = SPU_SPEC_INIT(), *acc = &av;
    (void)acc;
    const u128 one = SPLW(F_ONE), two = SPLW(F_TWO), half = SPLW(F_HALF);
    for (int k = 0; k < 4; k++) {
        /* a lane without the flag takes F = 1.0 and never uses its 1/PLs: it gets 1.0 there (frest(0) would
         * leave the range for nothing); a flagged lane computes on its own PLs */
        const u128 PLs = spu_selb(one, PLs_in[k], flag);
        const u128 y = spu_fi(PLs, spu_frest(PLs));
        const u128 F = spu_selb(one, XFMA(XFNMS(PLs, y, one), y, y), flag);
        Os[k] = XFM(XFM(Cs[k], Ps[k]), F);
    }
    const u128 Sx = XFM(Ct[0], Ps[0]), Sy = XFM(Ct[1], Ps[1]), Sz = XFM(Ct[2], Ps[2]);
    const u128 px = Pq[0], py = Pq[1], pz = Pq[2], pw = Pq[3], cx = Cq[0], cy = Cq[1], cz = Cq[2], cw = Cq[3];
    const u128 tx = XFMA(pw, Sx, XFNMS(pz, Sy, XFM(py, Sz)));
    const u128 ty = XFMA(pw, Sy, XFNMS(px, Sz, XFM(pz, Sx)));
    const u128 tz = XFMA(pw, Sz, XFNMS(py, Sx, XFM(px, Sy)));
    const u128 ux = XFNMS(pz, ty, XFM(py, tz));
    const u128 uy = XFNMS(px, tz, XFM(pz, tx));
    const u128 uz = XFNMS(py, tx, XFM(px, ty));
    Ot[0] = XFA(XFMA(two, ux, Sx), Pt[0]);
    Ot[1] = XFA(XFMA(two, uy, Sy), Pt[1]);
    Ot[2] = XFA(XFMA(two, uz, Sz), Pt[2]);
    Ot[3] = XFA(one, Pt[3]);
    const u128 qx = XFNMS(pz, cy, XFMA(py, cz, XFMA(px, cw, XFM(pw, cx))));
    const u128 qy = XFMA(pz, cx, XFMA(py, cw, XFNMS(px, cz, XFM(pw, cy))));
    const u128 qz = XFMA(pz, cw, XFNMS(py, cx, XFMA(px, cy, XFM(pw, cz))));
    const u128 qw = XFNMS(pz, cz, XFNMS(py, cy, XFNMS(px, cx, XFM(pw, cw))));
    const u128 n2 = XFMA(qw, qw, XFMA(qz, qz, XFMA(qy, qy, XFM(qx, qx))));
    const u128 y0 = spu_fi(n2, spu_frsqest(n2));
    const u128 r = XFMA(XFM(y0, half), XFNMS(XFM(n2, y0), y0, one), y0);
    Oq[0] = XFM(qx, r); Oq[1] = XFM(qy, r); Oq[2] = XFM(qz, r); Oq[3] = XFM(qw, r);
    return spec ? SPU_SPEC_OK(av) : 1;
}
/* four quads at a[j] (+off) -> v[0..3] = component k across the lanes */
static inline void gather4(const uint8_t* ls, const uint32_t a[4], uint32_t off, u128 v[4])
{
    __m128i r0 = LDQ(ls, a[0] + off), r1 = LDQ(ls, a[1] + off), r2 = LDQ(ls, a[2] + off), r3 = LDQ(ls, a[3] + off);
    TRANSPOSE(&r0, &r1, &r2, &r3);
    v[0] = U(r0); v[1] = U(r1); v[2] = U(r2); v[3] = U(r3);
}
/* 1 = done; 0 = declined (nothing written) */
static int fk_core(uint8_t* ls, const u128* gpr, uint32_t OUT, uint32_t IN, uint32_t ID, uint32_t PL, uint32_t n, uint32_t sp)
{
    if (n % 4u || n > 0x4000u || (OUT | IN | ID | PL | sp) & 15u || sp < 0x200u || sp > SPU_LS_SIZE) return 0;
    const uint32_t groups = n >> 2;
    uint32_t maxidx = 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t p = R32(ls + LSM(PL + 4u * k)), child = p >> 16, par = p & 0x7FFFu;
        if (child > maxidx) maxidx = child;
        if (par <= 0x3FFFu && par > maxidx) maxidx = par;
    }
    const uint32_t span = 48u * (maxidx + 1u), pl_n = 16u * groups + 32u, rz = sp - 0x1F0u;
    if (OUT + span > SPU_LS_SIZE || IN + span > SPU_LS_SIZE || PL + pl_n > SPU_LS_SIZE || ID + 48u > SPU_LS_SIZE ||
        !disjoint(IN, span, OUT, span) || !disjoint(PL, pl_n, OUT, span) || !disjoint(ID, 48u, OUT, span) ||
        !disjoint(rz, 0x1F0u, OUT, span) || !disjoint(rz, 0x1F0u, IN, span) || !disjoint(rz, 0x1F0u, ID, 48u) ||
        !disjoint(rz, 0x1F0u, PL, pl_n))
        return 0;
    /* the prologue's red zone, written before the n == 0 test: r_k at sp - 0x10*(k-79), k = 80..110 */
    for (int k = 80; k <= 110; k++) SQ(ls + LSM(sp - 0x10u * (uint32_t)(k - 79)), gpr[k]);
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t child[4], pa[4], pls[4], ca[4];
        int fm_[4];
        for (int j = 0; j < 4; j++) {
            const uint32_t p = R32(ls + LSM(PL + 16u * g + 4u * (uint32_t)j)), par = p & 0x7FFFu, root = par > 0x3FFFu;
            child[j] = p >> 16;
            pa[j] = root ? ID : OUT + 48u * par;
            pls[j] = (root ? ID : IN + 48u * par) + 32u;
            ca[j] = IN + 48u * child[j];
            fm_[j] = (p & 0x8000u) ? -1 : 0;     /* the flag: 1/PLs applies to this lane's scale */
        }
        const __m128i fl = _mm_setr_epi32(fm_[0], fm_[1], fm_[2], fm_[3]);
        u128 Pq[4], Pt[4], Ps[4], PLs[4], Cq[4], Ct[4], Cs[4], Oq[4], Ot[4], Os[4];
        gather4(ls, pa, 0, Pq); gather4(ls, pa, 16, Pt); gather4(ls, pa, 32, Ps); gather4(ls, pls, 0, PLs);
        gather4(ls, ca, 0, Cq); gather4(ls, ca, 16, Ct); gather4(ls, ca, 32, Cs);
        if (__builtin_expect(!s_spec || !fk_math(1, Pq, Pt, Ps, PLs, Cq, Ct, Cs, U(fl), Oq, Ot, Os), 0)) {
            CNT(s_fk_fail);
            fk_math(0, Pq, Pt, Ps, PLs, Cq, Ct, Cs, U(fl), Oq, Ot, Os);
        }
        __m128i q[4] = {M(Oq[0]), M(Oq[1]), M(Oq[2]), M(Oq[3])}, t[4] = {M(Ot[0]), M(Ot[1]), M(Ot[2]), M(Ot[3])},
                sc[4] = {M(Os[0]), M(Os[1]), M(Os[2]), M(Os[3])};
        TRANSPOSE(&q[0], &q[1], &q[2], &q[3]); TRANSPOSE(&t[0], &t[1], &t[2], &t[3]); TRANSPOSE(&sc[0], &sc[1], &sc[2], &sc[3]);
        for (int j = 0; j < 4; j++) {          /* lane order: a duplicate child keeps the last lane's record */
            const uint32_t o = OUT + 48u * child[j];
            STQ(ls, o, q[j]); STQ(ls, o + 16u, t[j]); STQ(ls, o + 32u, sc[j]);
        }
    }
    return 1;
}

/* ---- 0x17780: model pose -> 3x4 matrices, 4 bones (lanes) per group, ceil(n/4)*4 written ---- */
static inline __attribute__((always_inline)) int mat_math(const int spec, const u128* q, const u128* t, const u128* s, u128 R[3][4])
{
    spu_spec_acc av = SPU_SPEC_INIT(), *acc = &av;
    (void)acc;
    const u128 two = SPLW(F_TWO), half = SPLW(F_HALF), x = q[0], y = q[1], z = q[2], w = q[3];
    const u128 sx2 = XFM(s[0], two), sy2 = XFM(s[1], two), sz2 = XFM(s[2], two);
    const u128 hx = XFNMS(x, x, half), hy = XFNMS(y, y, half);
    const u128 xy = XFM(x, y), xz = XFM(x, z), yz = XFM(y, z);
    R[0][0] = XFM(XFNMS(z, z, hy), sx2); R[0][1] = XFM(XFNMS(w, z, xy), sy2); R[0][2] = XFM(XFMA(w, y, xz), sz2); R[0][3] = t[0];
    R[1][0] = XFM(XFMA(w, z, xy), sx2);  R[1][1] = XFM(XFNMS(z, z, hx), sy2); R[1][2] = XFM(XFNMS(w, x, yz), sz2); R[1][3] = t[1];
    R[2][0] = XFM(XFNMS(w, y, xz), sx2); R[2][1] = XFM(XFMA(w, x, yz), sy2);  R[2][2] = XFM(XFNMS(y, y, hx), sz2); R[2][3] = t[2];
    return spec ? SPU_SPEC_OK(av) : 1;
}
static int mat_core(uint8_t* ls, uint32_t OUT, uint32_t POSE, uint32_t n)
{
    if (n == 0) return 1;
    const uint32_t groups = (n + 3u) >> 2, on = 192u * groups, in_n = 192u * (groups + 1u);
    if (n > 4096u || (OUT | POSE) & 15u || OUT + on > SPU_LS_SIZE || POSE + in_n > SPU_LS_SIZE ||
        !(OUT == POSE || disjoint(OUT, on, POSE, in_n)))
        return 0;
    for (uint32_t g = 0; g < groups; g++) {
        u128 v[12], R[3][4];
        load_soa(ls, POSE, 4u * g, v);
        if (__builtin_expect(!s_spec || !mat_math(1, v, v + 4, v + 8, R), 0)) { CNT(s_mat_fail); mat_math(0, v, v + 4, v + 8, R); }
        for (int r = 0; r < 3; r++) {
            __m128i a = M(R[r][0]), b = M(R[r][1]), c = M(R[r][2]), d = M(R[r][3]);
            TRANSPOSE(&a, &b, &c, &d);
            const __m128i rows[4] = {a, b, c, d};
            for (int j = 0; j < 4; j++) STQ(ls, OUT + 48u * (4u * g + (uint32_t)j) + 16u * (uint32_t)r, rows[j]);
        }
    }
    return 1;
}

/* ---- 0x18B80: OUT_i = A_i x [B_i; 0 0 0 1], n matrices (lanes = columns), OUT == B allowed ---- */
static inline __attribute__((always_inline)) int skin_math(const int spec, const u128* A, const u128* B, u128 K, u128* O)
{
    spu_spec_acc av = SPU_SPEC_INIT(), *acc = &av;
    (void)acc;
    for (int r = 0; r < 3; r++)
        O[r] = XFMA(K, SPL(A[r], 3), XFMA(B[2], SPL(A[r], 2), XFMA(B[1], SPL(A[r], 1), XFM(B[0], SPL(A[r], 0)))));
    return spec ? SPU_SPEC_OK(av) : 1;
}
static int skin_core(uint8_t* ls, uint32_t OUT, uint32_t A, uint32_t B, uint32_t n)
{
    if (n == 0) return 1;
    const uint32_t on = 48u * n, in_n = 48u * (n + 1u);
    if (n > 4096u || (OUT | A | B) & 15u || OUT + on > SPU_LS_SIZE || A + in_n > SPU_LS_SIZE || B + in_n > SPU_LS_SIZE ||
        !disjoint(OUT, on, A, in_n) || !(OUT == B || disjoint(OUT, on, B, in_n)))
        return 0;
    const u128 K = U(LDQ(ls, 0x1E250u));
    for (uint32_t i = 0; i < n; i++) {
        u128 a[3], b[3], o[3];
        for (int r = 0; r < 3; r++) { a[r] = U(LDQ(ls, A + 48u * i + 16u * (uint32_t)r)); b[r] = U(LDQ(ls, B + 48u * i + 16u * (uint32_t)r)); }
        if (__builtin_expect(!s_spec || !skin_math(1, a, b, K, o), 0)) { CNT(s_skin_fail); skin_math(0, a, b, K, o); }
        for (int r = 0; r < 3; r++) STQ(ls, OUT + 48u * i + 16u * (uint32_t)r, M(o[r]));
    }
    return 1;
}

/* ---- 0x5814 (fragment, continues at 0x5928): in place, M = W x [M; X] per 3x4 matrix, do-while ---- */
static inline __attribute__((always_inline)) int root_math(const int spec, const u128* R, u128 X, const u128* Mm, u128* O)
{
    spu_spec_acc av = SPU_SPEC_INIT(), *acc = &av;
    (void)acc;
    for (int i = 0; i < 3; i++)
        O[i] = XFMA(SPL(R[3], i), X, XFMA(SPL(R[2], i), Mm[2], XFMA(SPL(R[1], i), Mm[1], XFM(SPL(R[0], i), Mm[0]))));
    return spec ? SPU_SPEC_OK(av) : 1;
}
static int root_core(uint8_t* ls, uint32_t a, uint32_t end, const u128 R[4], u128 X)
{
    if ((a & 15u) || !(end > a) || end > SPU_LS_SIZE || (end - a) > 48u * 4096u) return 0;
    do {
        u128 Mm[3], O[3];
        for (int r = 0; r < 3; r++) Mm[r] = U(LDQ(ls, a + 16u * (uint32_t)r));
        if (__builtin_expect(!s_spec || !root_math(1, R, X, Mm, O), 0)) { CNT(s_root_fail); root_math(0, R, X, Mm, O); }
        for (int r = 0; r < 3; r++) STQ(ls, a + 16u * (uint32_t)r, M(O[r]));
        a += 48u;
    } while (end > a);
    return 1;
}

/* check mode for the function natives: the native on a copy of the LS, the lifted function on the real one,
 * the whole LS compared (the lifted result is kept) */
static _Thread_local uint8_t* t_lscopy;   /* (deck-merge) SPU_LS_SIZE + 64, made at the first check (module static TLS) */
#define LSCOPY_OR_DECLINE() do { if (!t_lscopy && !(t_lscopy = (uint8_t*)malloc(SPU_LS_SIZE + 64))) return 0; } while (0)
static void kcheck(spu_context* ctx, const char* what, int ok, void (*lifted)(spu_context*))
{
    const u128 r0 = ctx->gpr[0];
    t_bypass = 1;
    { const int32_t si = (int32_t)ctx->image_id; ctx->host_depth++;
      lifted(ctx); spu_drain_call(ctx, r0._u32[0]); ctx->host_depth--; spu_img_restore(ctx, si); }
    t_bypass = 0;
    ctx->gpr[0] = r0;
    if (!ok) return;                         /* declined: nothing to compare */
    const unsigned long long c = __atomic_add_fetch(&s_kchk, 1, __ATOMIC_RELAXED);
    if (memcmp(t_lscopy, ctx->ls, SPU_LS_SIZE)) {
        uint32_t first = 0; while (first < SPU_LS_SIZE && t_lscopy[first] == ctx->ls[first]) first++;
        if (__atomic_add_fetch(&s_kbad, 1, __ATOMIC_RELAXED) <= 8)
            fprintf(stderr, "[j218] CHECK %s: native differs, first byte 0x%05X\n", what, first);
    }
    if (s_stats && c % s_every == 0)
        fprintf(stderr, "[j218] kernel checks %llu, %llu differ\n", c, s_kbad);
}
#define KSTAT() do { if (s_stats && ((s_fk_calls + s_mat_calls) % s_every) == 0) \
    fprintf(stderr, "[j218] fk %llu (decl %llu, spec fail %llu) mat %llu (%llu, %llu) skin %llu (%llu, %llu) root %llu (%llu, %llu)\n", \
            s_fk_calls, s_fk_decl, s_fk_fail, s_mat_calls, s_mat_decl, s_mat_fail, s_skin_calls, s_skin_decl, s_skin_fail, \
            s_root_calls, s_root_decl, s_root_fail); } while (0)

static int j218_fk(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) j218_init();
    if (!s_mode || t_bypass || (g_spu_jobrec_mode & SPU_JOBREC_RECORD)) return 0;
    CNT(s_fk_calls);
    const uint32_t OUT = LSM(ctx->gpr[3]._u32[0]), IN = LSM(ctx->gpr[4]._u32[0]), ID = LSM(ctx->gpr[5]._u32[0]);
    const uint32_t PL = LSM(ctx->gpr[6]._u32[0]), n = ctx->gpr[7]._u32[0], sp = LSM(ctx->gpr[1]._u32[0]);
    if (s_check) {
        LSCOPY_OR_DECLINE();
        memcpy(t_lscopy, ctx->ls, SPU_LS_SIZE);
        const int ok = fk_core(t_lscopy, ctx->gpr, OUT, IN, ID, PL, n, sp);
        kcheck(ctx, "0x179C0", ok, jobmod_ab654d30fd90_efe70_spu_func_000179C0);
        return 1;
    }
    const int ok = fk_core(ctx->ls, ctx->gpr, OUT, IN, ID, PL, n, sp);
    if (!ok) CNT(s_fk_decl);
    KSTAT();
    return ok;
}
static int j218_mat(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) j218_init();
    if (!s_mode || t_bypass || (g_spu_jobrec_mode & SPU_JOBREC_RECORD)) return 0;
    CNT(s_mat_calls);
    const uint32_t OUT = LSM(ctx->gpr[3]._u32[0]), POSE = LSM(ctx->gpr[4]._u32[0]), n = ctx->gpr[5]._u32[0];
    if (s_check) {
        LSCOPY_OR_DECLINE();
        memcpy(t_lscopy, ctx->ls, SPU_LS_SIZE);
        const int ok = mat_core(t_lscopy, OUT, POSE, n);
        kcheck(ctx, "0x17780", ok, jobmod_ab654d30fd90_efe70_spu_func_00017780);
        return 1;
    }
    const int ok = mat_core(ctx->ls, OUT, POSE, n);
    if (!ok) CNT(s_mat_decl);
    return ok;
}
static int j218_skin(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) j218_init();
    if (!s_mode || t_bypass || (g_spu_jobrec_mode & SPU_JOBREC_RECORD)) return 0;
    CNT(s_skin_calls);
    const uint32_t OUT = LSM(ctx->gpr[3]._u32[0]), A = LSM(ctx->gpr[4]._u32[0]), B = LSM(ctx->gpr[5]._u32[0]), n = ctx->gpr[6]._u32[0];
    if (s_check) {
        LSCOPY_OR_DECLINE();
        memcpy(t_lscopy, ctx->ls, SPU_LS_SIZE);
        const int ok = skin_core(t_lscopy, OUT, A, B, n);
        kcheck(ctx, "0x18B80", ok, jobmod_ab654d30fd90_efe70_spu_func_00018B80);
        return 1;
    }
    const int ok = skin_core(ctx->ls, OUT, A, B, n);
    if (!ok) CNT(s_skin_decl);
    return ok;
}
/* fragment: 1 = the loop is done (the lifted code continues at 0x5928); 0 = the lifted loop runs (also in check
 * mode: a fragment has no lifted function to run on the side; the job replay checks it) */
static int j218_root(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) j218_init();
    if (!s_mode || s_check || t_bypass || (g_spu_jobrec_mode & SPU_JOBREC_RECORD)) return 0;
    CNT(s_root_calls);
    const u128 R[4] = {ctx->gpr[22], ctx->gpr[21], ctx->gpr[24], ctx->gpr[23]};
    const uint32_t a = LSM(ctx->gpr[18]._u32[0]), end = ctx->gpr[25]._u32[0];
    const int ok = root_core(ctx->ls, a, end, R, ctx->gpr[86]);
    if (!ok) CNT(s_root_decl);
    return ok;
}

/* (deck-merge) J218_PICK: whose native takes 0x16320 (blend), 0x17780 (pose -> 3x4) and 0x18B80 (palette), both
 * rewrites have one (this file and natives/spu/{job218_native.c,native_218.c}); spu_native_entry runs the FIRST
 * registered native of an address and never a second one, so each address is registered by one file only.
 * Three letters in that order, o = ours (job218_native.c), t = upstream's (native_218.c), e.g. J218_PICK=ott;
 * "ours" / "theirs" = all three. Default J218_PICK_DEFAULT. The other functions are one file's only (ours 0x179C0,
 * 0x5814, 0x18EC0; upstream's 0x13FC0, 0x1D8E8, 0x1DAC0) and always registered. */
#define J218_PICK_DEFAULT "ooo"
static int j218_pick_theirs(int k)
{
    const char* e = getenv("J218_PICK");
    if (!e || !*e) e = J218_PICK_DEFAULT;
    if (!strcmp(e, "ours")) return 0;
    if (!strcmp(e, "theirs")) return 1;
    const char* d = J218_PICK_DEFAULT;
    const char c = (int)strlen(e) > k ? e[k] : d[k];
    return c == 't' || c == 'T';
}
__attribute__((constructor)) static void j218_register(void)
{
    if (!j218_pick_theirs(0)) spu_native_register(J218_IMAGE, J218_BLEND, j218_blend);
    spu_native_register(J218_IMAGE, 0x179C0u, j218_fk);
    if (!j218_pick_theirs(1)) spu_native_register(J218_IMAGE, 0x17780u, j218_mat);
    if (!j218_pick_theirs(2)) spu_native_register(J218_IMAGE, 0x18B80u, j218_skin);
    spu_native_register(J218_IMAGE, 0x5814u, j218_root);
    fprintf(stderr, "[j218] J218_PICK: blend 0x16320 %s, pose 0x17780 %s, palette 0x18B80 %s (o = job218_native.c, t = native_218.c)\n",
            j218_pick_theirs(0) ? "t" : "o", j218_pick_theirs(1) ? "t" : "o", j218_pick_theirs(2) ? "t" : "o");
}
