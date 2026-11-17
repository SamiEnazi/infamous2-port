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
 * compared (the lifted result is kept); J218_STATS=1: a summary line every 20000 calls. */
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
void jobmod_ab654d30fd90_efe70_spu_func_00016320(spu_context* ctx);   /* the lifted blend (check mode) */

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

static int s_mode = -1, s_check, s_stats;
static unsigned long long s_calls, s_checked, s_bad, s_declined;
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
    const u128 one = SPLW(F_ONE), zero = SPLW(0);
    const u128 wB = spu_fm(spu_cuflt(V4(mB[0], mB[1], mB[2], mB[3]), 147), SPLW(K255));
    const u128 wC = spu_fm(spu_cuflt(V4(mC[0], mC[1], mC[2], mC[3]), 147), SPLW(K255));
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
    u128 O[12];
    for (int k = 0; k < 4; k++) O[k] = spu_fma(spu_fm(Cv[k], sg), kC, spu_fm(Bv[k], kB));
    for (int k = 4; k < 12; k++) O[k] = spu_fnms(Bv[k], s, spu_fma(Cv[k], s, Bv[k]));
    const u128 wo = spu_fnms(wB, s, spu_fma(wC, s, wB));
    const u128 qb = spu_cflts(spu_fma(wo, SPLW(Q0), SPLW(Q1)), 165);
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

static int j218_blend(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_J218");
        s_mode = e ? atoi(e) : 1;
        s_check = getenv("J218_CHECK") != 0;
        s_stats = getenv("J218_STATS") != 0;
        fprintf(stderr, "[j218] native pose blend (0x16320): %s%s\n", s_mode ? "on" : "off (PS3RECOMP_NATIVE_J218=0)",
                s_mode && s_check ? ", CHECK mode (the lifted result is kept)" : "");
    }
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
        static _Thread_local uint8_t o[1024 * 192];      /* n <= 4096: at most 1024 groups */
        static _Thread_local uint8_t w[1024 * 4];
        for (uint32_t g = 0; g < groups; g++) blend_group(ls, B, MB, C, MC, 4u * g, T, o + 192u * g, w + 4u * g);
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
        if (s_stats && c % 20000 == 0)
            fprintf(stderr, "[j218] %llu calls, %llu declined, check %llu/%llu differ\n", c, s_declined, s_bad, s_checked);
        ctx->gpr[0] = r0;
        return 1;
    }
    /* the prologue's red zone: r80..r109 at sp-0x10 .. sp-0x1E0 (sp not moved), as the lifted code leaves it */
    for (int k = 0; k < 30; k++) SQ(ls + LSM(sp - 0x10u * (uint32_t)(k + 1)), ctx->gpr[80 + k]);
    uint8_t w[4];
    for (uint32_t g = 0; g < groups; g++) {
        blend_group(ls, B, MB, C, MC, 4u * g, T, ls + A + 192u * g, w);   /* group g is read before it is written: A = B is fine */
        memcpy(ls + MA + 4u * g, w, 4);
    }
    if (s_stats && c % 20000 == 0)
        fprintf(stderr, "[j218] %llu calls, %llu declined\n", c, s_declined);
    return 1;
}

__attribute__((constructor)) static void j218_register(void)
{
    spu_native_register(J218_IMAGE, J218_BLEND, j218_blend);
}
