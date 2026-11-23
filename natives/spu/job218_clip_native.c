/* inFAMOUS 2: the clip sampler of the character-animation job 218 (jobmod_ab654d30fd90_efe70, LS 0x18EC0),
 * natively -- Sony's Edge Animation "evaluate" of one compressed clip (frame set) at one time into a pose slot:
 * constant / animated rotations (48-bit smallest-three, slerp), translations, scales (variable-width bit
 * codec, lerp) and float channels, with the per-track key search over n-bit key masks.
 *
 * Spec: work/bench/job218-decoders-spec.md; this is a line-for-line port of the reference
 * work/bench/emu218/clipref.py, which is bit-exact on all 811 recorded calls (and its native simulation on
 * all 102 recorded jobs). Integer / bit-stream work in plain C (128-bit masks as unsigned __int128); every
 * float op is the engine's exact SPU helper (per lane, lane 0 of a splat), in the reference's order.
 * Padding tracks are computed and stored, in each decoder's store order (spec 8.3).
 * The call's scratch stack [sp-0x6E0, sp-0x300) is declared dead to the job replay; the lr slot sp+0x10
 * and the r80..r127 save area [sp-0x300, sp) are written as the SPU does.
 *
 * The integer work (key search, pointers, bit fields) per track; the float work over the 4 tracks of a group
 * in SIMD lanes (as the SPU code), speculative with the per-op exact form as the fallback. PS3_SPU_NATIVE=0: every rewrite off.
 * J218CLIP_CHECK=1: the native on a copy of the LS, the lifted sampler on the real one, the whole LS compared
 * outside the scratch window (the lifted result is kept). J218CLIP_STATS=N: a summary every N calls. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "native_float.h"
#include "spu_jobrec.h"

int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void jobmod_ab654d30fd90_efe70_spu_func_00018EC0(spu_context* ctx);   /* the lifted sampler (check mode) */

typedef unsigned __int128 u128i;
#define ALL128 (~(u128i)0)

/* ---- exact SPU float ops on one lane ---- */
static inline u128 S(uint32_t x) { u128 v; v._u32[0] = v._u32[1] = v._u32[2] = v._u32[3] = x; return v; }
#define ONE   0x3F800000u
#define HALF  0x3F000000u
#define MONE  0xBF800000u
#define SQ2   0x3FB504F3u
#define NSQ2H 0xBF3504F3u
#define A0 0x3FC90FDAu
#define A1 0x3E5BBFCAu
#define A2 0x3DB63A9Eu
#define A3 0x3D4D8392u
#define S1 0xBE2AAAABu
#define S2 0x3C088887u
#define S3 0xB9500C03u
#define S4 0x3638B94Fu
#define LIN 0x3F7FBE77u

/* ---- LS access, wrapping like the SPU ---- */
static inline uint32_t B8(const uint8_t* L, uint32_t a) { return L[a & 0x3FFFFu]; }
static inline uint32_t U16(const uint8_t* L, uint32_t a) { return B8(L, a) << 8 | B8(L, a + 1); }
static inline uint32_t U32(const uint8_t* L, uint32_t a) { return B8(L, a) << 24 | B8(L, a + 1) << 16 | B8(L, a + 2) << 8 | B8(L, a + 3); }
static inline void SQ4(uint8_t* L, uint32_t a, const uint32_t w[4])
{
    uint8_t* p = L + (a & 0x3FFF0u);
    for (int k = 0; k < 4; k++) { p[4 * k] = (uint8_t)(w[k] >> 24); p[4 * k + 1] = (uint8_t)(w[k] >> 16); p[4 * k + 2] = (uint8_t)(w[k] >> 8); p[4 * k + 3] = (uint8_t)w[k]; }
}
static inline void SW(uint8_t* L, uint32_t a, uint32_t w)
{
    uint8_t* p = L + (a & 0x3FFFCu);
    p[0] = (uint8_t)(w >> 24); p[1] = (uint8_t)(w >> 16); p[2] = (uint8_t)(w >> 8); p[3] = (uint8_t)w;
}
/* the 128 bits of the LS bit string starting at bit address c (MSB first) */
static inline u128i bits128(const uint8_t* L, uint32_t c)
{
    const uint32_t a = c >> 3, sh = c & 7u;
    u128i hi = 0;
    uint32_t b16;
    if (a + 17u <= SPU_LS_SIZE) {
        uint64_t x, y; memcpy(&x, L + a, 8); memcpy(&y, L + a + 8, 8);
        hi = (u128i)__builtin_bswap64(x) << 64 | __builtin_bswap64(y);
        b16 = L[a + 16];
    } else {
        for (uint32_t k = 0; k < 16; k++) hi = hi << 8 | B8(L, a + k);
        b16 = B8(L, a + 16);
    }
    return sh ? (hi << sh) | (u128i)(b16 >> (8u - sh)) : hi;
}
static inline uint32_t top32(u128i w, uint32_t sh) { return (uint32_t)((w << sh) >> 96); }
/* u16 number j of the 8-byte index group at base, rotqby semantics (wraps inside the quad) */
static inline uint32_t u16q(const uint8_t* L, uint32_t base, uint32_t j)
{
    const uint32_t q = base & ~15u, o = (base + 2u * j) & 15u;
    return B8(L, q + o) << 8 | B8(L, q + ((o + 1u) & 15u));
}
static inline int popc128(u128i x) { return __builtin_popcountll((uint64_t)x) + __builtin_popcountll((uint64_t)(x >> 64)); }
static inline int ctz128(u128i x) { const uint64_t lo = (uint64_t)x; return lo ? __builtin_ctzll(lo) : 64 + __builtin_ctzll((uint64_t)(x >> 64)); }
static inline int clz128(u128i x) { const uint64_t hi = (uint64_t)(x >> 64); return hi ? __builtin_clzll(hi) : 64 + __builtin_clzll((uint64_t)x); }

/* ---- shared pieces (clipref.py), vectorised: lane j = track j of a group of 4 ----
 * Every float op is the engine's exact SPU helper; X* below take a constant `spec`: 1 = the speculative form
 * (one range verdict for the whole group, spu_helpers.h), 0 = the per-op exact form (the fallback when the
 * verdict fails). In the speculative pass only, lanes whose values are discarded or multiplied by +0 get a
 * harmless input (marked SANITISE): the near-identical rotations of a slerp (lin lanes never use acos/sin),
 * the zero / denormal s of the w reconstruction (w = (..) * s = +0 whatever the rsqrt estimate was). */
#define XFM(a, b)      (spec ? spu_fm_spec((a), (b), acc) : spu_fm((a), (b)))
#define XFA(a, b)      (spec ? spu_fa_spec((a), (b), acc) : spu_fa((a), (b)))
#define XFS(a, b)      (spec ? spu_fs_spec((a), (b), acc) : spu_fs((a), (b)))
#define XFMA(a, b, c)  (spec ? spu_fma_spec((a), (b), (c), acc) : spu_fma((a), (b), (c)))
#define XFNMS(a, b, c) (spec ? spu_fnms_spec((a), (b), (c), acc) : spu_fnms((a), (b), (c)))
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 VA(const uint32_t x[4]) { return V4(x[0], x[1], x[2], x[3]); }
static inline u128 VEQ0(u128 x) { u128 m; for (int i = 0; i < 4; i++) m._u32[i] = x._u32[i] == 0 ? 0xFFFFFFFFu : 0u; return m; }
static inline u128 VEXP0(u128 x) { u128 m; for (int i = 0; i < 4; i++) m._u32[i] = (x._u32[i] & 0x7F800000u) == 0 ? 0xFFFFFFFFu : 0u; return m; }
#define ONEV S(ONE)

static inline __attribute__((always_inline)) u128 v_rnr(const int spec, spu_spec_acc* acc, u128 x)
{
    const u128 r = spu_fi(x, spu_frest(x));
    return XFMA(XFNMS(x, r, ONEV), r, r);
}
static inline __attribute__((always_inline)) u128 v_wfrom(const int spec, spu_spec_acc* acc, u128 a, u128 b, u128 c, int clamp)
{
    u128 s = XFNMS(c, c, XFNMS(b, b, XFNMS(a, a, ONEV)));
    if (clamp) s = spu_selb(s, S(0), spu_fcgt_lean(S(0), s));
    const u128 se = spec ? spu_selb(s, ONEV, VEXP0(s)) : s;            /* SANITISE: s = 0 -> w = +0 anyway */
    const u128 y = spu_fi(se, spu_frsqest(se)), h = XFM(s, y), y2 = XFM(y, S(HALF));
    return XFM(XFMA(y2, XFNMS(h, y, ONEV), y), s);
}
/* reorder (a,b,c,w) by each lane's idx (q(0x1E300 + 16*idx)): idx 0 (w,a,b,c) 1 (a,w,b,c) 2 (a,b,w,c) 3 (a,b,c,w) */
static inline void v_reorder(u128 a, u128 b, u128 c, u128 w, u128 idx, u128 out[4])
{
    u128 i0, i1, i2;
    for (int j = 0; j < 4; j++) {
        const uint32_t x = idx._u32[j] & 3u;
        i0._u32[j] = x == 0 ? ~0u : 0u; i1._u32[j] = x == 1 ? ~0u : 0u; i2._u32[j] = x == 2 ? ~0u : 0u;
    }
    out[0] = spu_selb(a, w, i0);
    out[1] = spu_selb(spu_selb(b, w, i1), a, i0);
    out[2] = spu_selb(spu_selb(c, w, i2), b, spu_or(i0, i1));
    out[3] = spu_selb(w, c, spu_or(spu_or(i0, i1), i2));
}
/* 48-bit smallest-three quaternions of 4 lanes -> out[k] = component k across the lanes */
static inline __attribute__((always_inline)) void v_deq48(const int spec, spu_spec_acc* acc, const uint32_t h0[4], const uint32_t w1[4], u128 out[4])
{
    const u128 b15 = V4((w1[0] >> 17) & 0x7FFFu, (w1[1] >> 17) & 0x7FFFu, (w1[2] >> 17) & 0x7FFFu, (w1[3] >> 17) & 0x7FFFu);
    const u128 c15 = V4((w1[0] >> 2) & 0x7FFFu, (w1[1] >> 2) & 0x7FFFu, (w1[2] >> 2) & 0x7FFFu, (w1[3] >> 2) & 0x7FFFu);
    const u128 a = XFMA(spu_cuflt(VA(h0), 140), S(SQ2), S(NSQ2H));
    const u128 b = XFMA(spu_cuflt(b15, 140), S(SQ2), S(NSQ2H));
    const u128 c = XFMA(spu_cuflt(c15, 140), S(SQ2), S(NSQ2H));
    v_reorder(a, b, c, v_wfrom(spec, acc, a, b, c, 0), VA(w1), out);
}
static inline void read6(const uint8_t* L, uint32_t p, uint32_t* h0, uint32_t* w1)
{
    *h0 = B8(L, p) << 8 | B8(L, p + 1);
    *w1 = B8(L, p + 2) << 24 | B8(L, p + 3) << 16 | B8(L, p + 4) << 8 | B8(L, p + 5);
}
typedef struct { uint32_t s, e, m; } Fld;
static inline void desc_fields(uint32_t D, Fld f[3])
{
    f[0].s = (D >> 31) & 1u; f[0].e = (D >> 27) & 15u; f[0].m = (D >> 22) & 31u;
    f[1].s = (D >> 21) & 1u; f[1].e = (D >> 17) & 15u; f[1].m = (D >> 12) & 31u;
    f[2].s = (D >> 11) & 1u; f[2].e = (D >> 7) & 15u;  f[2].m = (D >> 2) & 31u;
}
static inline uint32_t rot32(uint32_t x, uint32_t n) { n &= 31u; return n ? (x << n | x >> (32u - n)) : x; }
static inline uint32_t kmask(uint32_t k) { return k ? 0xFFFFFFFFu >> (32u - k) : 0u; }
static inline uint32_t shl32(uint32_t x, uint32_t n) { n &= 63u; return n < 32u ? x << n : 0u; }
static inline uint32_t sra32(uint32_t x, uint32_t n) { n &= 63u; if (n > 31u) n = 31u; return (uint32_t)((int32_t)x >> n); }
/* the variable-width field codec (spec 3.3): the integer half per lane, the float half (fixed point) per vector */
typedef struct { uint32_t isf[4], fbits[4], iv[4], mxi[4]; } Dec;
static inline void dec_int(Dec* d, int j, uint32_t w32, uint32_t s, uint32_t e, uint32_t m)
{
    uint32_t x = rot32(w32, s); const uint32_t sign = x & kmask(s);
    x = rot32(x, e); const uint32_t E = x & kmask(e);
    x = rot32(x, m); const uint32_t Mt = x & kmask(m);
    if (e) {           /* mini float: plain bit assembly */
        d->isf[j] = ~0u; d->fbits[j] = ((E + 128u - shl32(1u, e - 1u)) << 23) | shl32(Mt, (23u - m) & 63u) | (sign << 31);
        d->iv[j] = 0; d->mxi[j] = 1;
    } else {           /* fixed point: csflt(Mt with the sign as the two's-complement top bit) * 1/(2^m - 1) */
        const uint32_t sh = (31u - m) & 63u;
        d->isf[j] = 0; d->fbits[j] = 0;
        d->iv[j] = Mt | sra32(sign << 31, sh);
        d->mxi[j] = sh < 32u ? 0x7FFFFFFFu >> sh : 0u;
    }
}
static inline __attribute__((always_inline)) u128 dec_float(const int spec, spu_spec_acc* acc, const Dec* d)
{
    const u128 mx = spu_cuflt(VA(d->mxi), 155), mx0 = VEQ0(mx);
    const u128 rc = spu_selb(v_rnr(spec, acc, spu_selb(mx, ONEV, mx0)), S(0), mx0);   /* mx == 0 -> rc = 0 */
    return spu_selb(XFM(spu_csflt(VA(d->iv), 155), rc), VA(d->fbits), VA(d->isf));
}
typedef struct { int Kb, Kt, pf, nf; } Keys;
static inline Keys key_frames(u128i W, u128i mN, u128i mF)
{
    Keys k;
    k.Kt = popc128(W & mN); k.Kb = popc128(W & mF);
    const u128i lo = W & mF;
    const int last = lo ? 127 - ctz128(lo) : -1;
    const u128i hi = (W & mN & ~mF) | ~mN;
    const int first = hi ? clz128(hi) : 128;
    k.pf = last + 1; k.nf = first + 1;
    return k;
}
static inline __attribute__((always_inline)) void v_interp(const int spec, spu_spec_acc* acc, u128 frac, const uint32_t dtA[4], const uint32_t dtB[4], u128* t, u128* omt)
{
    const u128 fA = spu_cuflt(VA(dtA), 155), fB = spu_cuflt(VA(dtB), 155);
    const u128 span = XFA(fA, fB);
    *t = XFM(XFA(frac, fA), v_rnr(spec, acc, span));
    *omt = XFS(ONEV, *t);
}
static inline __attribute__((always_inline)) u128 v_sin(const int spec, spu_spec_acc* acc, u128 x)
{
    const u128 x2 = XFM(x, x);
    return XFM(XFMA(XFMA(XFMA(XFMA(S(S4), x2, S(S3)), x2, S(S2)), x2, S(S1)), x2, ONEV), x);
}
static inline __attribute__((always_inline)) void v_slerp(const int spec, spu_spec_acc* acc, const u128 A[4], const u128 Bq[4], u128 t, u128 omt, u128 out[4])
{
    const u128 d = XFMA(A[3], Bq[3], XFMA(A[2], Bq[2], XFMA(A[1], Bq[1], XFM(A[0], Bq[0]))));
    const u128 sg = spu_selb(ONEV, S(MONE), spu_fcgt_lean(S(0), d));
    const u128 ad = XFM(d, sg);
    const u128 lin = spu_fcgt_lean(ad, S(LIN));
    const u128 adt = spec ? spu_selb(ad, S(HALF), lin) : ad;                /* SANITISE: lin lanes take (omt, t) */
    const u128 p = XFNMS(adt, XFNMS(adt, XFNMS(adt, XFM(adt, S(A3)), S(A2)), S(A1)), S(A0));
    const u128 e = XFS(ONEV, adt);
    const u128 y = spu_fi(e, spu_frsqest(e));
    const u128 th = XFM(p, XFNMS(y, adt, y));
    const u128 sth = v_sin(spec, acc, th), sa = v_sin(spec, acc, XFM(omt, th)), sb = v_sin(spec, acc, XFM(t, th));
    const u128 ri = spu_fi(sth, spu_frest(sth));
    const u128 kA = spu_selb(XFM(sa, ri), omt, lin), kB = spu_selb(XFM(sb, ri), t, lin);
    for (int k = 0; k < 4; k++) out[k] = XFMA(XFM(Bq[k], sg), kB, XFM(A[k], kA));
}
static inline __attribute__((always_inline)) u128 v_lerp(const int spec, spu_spec_acc* acc, u128 a, u128 b, u128 t) { return XFNMS(a, t, XFMA(b, t, a)); }
/* component vectors (k across lanes) -> lane j's quad */
static inline void lanes_out(const u128 v[4], uint32_t out[4][4])
{
    for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) out[j][k] = v[k]._u32[j];
}
static unsigned long long s_fail;
#define RUN2(call) do { spu_spec_acc av_ = SPU_SPEC_INIT(); { const int spec = 1; spu_spec_acc* acc = &av_; (void)spec; call; } \
    if (!SPU_SPEC_OK(av_)) { __atomic_add_fetch(&s_fail, 1, __ATOMIC_RELAXED); const int spec = 0; spu_spec_acc dummy_ = SPU_SPEC_INIT(); spu_spec_acc* acc = &dummy_; (void)spec; (void)acc; call; } } while (0)

/* ---- the leaves ---- */
static inline __attribute__((always_inline)) void g1A570(const int spec, spu_spec_acc* acc, const uint32_t h0[8], const uint32_t w1[8], uint32_t q[8][4])
{
    for (int half = 0; half < 2; half++) {
        u128 o[4]; v_deq48(spec, acc, h0 + 4 * half, w1 + 4 * half, o);
        uint32_t r[4][4]; lanes_out(o, r);
        memcpy(q[4 * half], r, sizeof r);
    }
}
static void c_1A570(uint8_t* L, uint32_t pose, uint32_t data, uint32_t n, uint32_t idx)
{
    static const int order[8] = {3, 6, 1, 7, 5, 0, 4, 2};
    data &= ~15u; idx &= ~15u;
    for (uint32_t g = 0; g < ((n + 7u) >> 3); g++) {
        uint32_t h0[8], w1[8], q[8][4];
        for (uint32_t k = 0; k < 8; k++) read6(L, data + 6u * (8u * g + k), &h0[k], &w1[k]);
        RUN2(g1A570(spec, acc, h0, w1, q));
        for (int kk = 0; kk < 8; kk++) {
            const uint32_t i = 8u * g + (uint32_t)order[kk];
            SQ4(L, pose + 48u * U16(L, idx + 2u * i), q[order[kk]]);
        }
    }
}
typedef struct { u128i mN, mF; uint32_t f, frac, n; } Com;
static inline __attribute__((always_inline)) void g19A20(const int spec, spu_spec_acc* acc, u128 frac, const uint32_t dtA[4], const uint32_t dtB[4],
                                                         const uint32_t ha[4], const uint32_t wa[4], const uint32_t hb[4], const uint32_t wb[4], uint32_t res[4][4])
{
    u128 t, omt, qa[4], qb[4], o[4];
    v_interp(spec, acc, frac, dtA, dtB, &t, &omt);
    v_deq48(spec, acc, ha, wa, qa);
    v_deq48(spec, acc, hb, wb, qb);
    v_slerp(spec, acc, qa, qb, t, omt, o);
    lanes_out(o, res);
}
static void c_19A20(uint8_t* L, const Com* c, const uint32_t cur0[4], uint32_t kbase, const uint32_t st0[4], const uint32_t en0[4],
                    uint32_t n, uint32_t pose, uint32_t idx)
{
    uint32_t cur[4], st[4], en[4];
    memcpy(cur, cur0, 16); memcpy(st, st0, 16); memcpy(en, en0, 16);
    const u128 frac = S(c->frac);
    for (uint32_t g = 0; g < ((n + 3u) >> 2); g++) {
        uint32_t res[4][4], P = 0, dtA[4], dtB[4], ha[4], wa[4], hb[4], wb[4];
        for (int j = 0; j < 4; j++) {
            const Keys k = key_frames(bits128(L, cur[j]), c->mN, c->mF);
            const uint32_t nxt = kbase + 6u * (P + (uint32_t)k.Kb);
            P += (uint32_t)k.Kt;
            const uint32_t pB = k.Kb == k.Kt ? en[j] : nxt;
            const uint32_t pA = k.Kb > 0 ? nxt - 6u : st[j];
            dtA[j] = c->f - (uint32_t)k.pf; dtB[j] = (uint32_t)k.nf - c->f;
            read6(L, pA, &ha[j], &wa[j]); read6(L, pB, &hb[j], &wb[j]);
        }
        RUN2(g19A20(spec, acc, frac, dtA, dtB, ha, wa, hb, wb, res));
        kbase += 6u * P;
        static const int order[4] = {3, 0, 2, 1};
        for (int kk = 0; kk < 4; kk++) SQ4(L, pose + 48u * u16q(L, idx, (uint32_t)order[kk]), res[order[kk]]);
        idx += 8u;
        for (int j = 0; j < 4; j++) { cur[j] += 4u * c->n; st[j] += 24u; en[j] += 24u; }
    }
}
static inline __attribute__((always_inline)) void g1C510(const int spec, spu_spec_acc* acc, const Dec* da, const Dec* db, const Dec* dc, u128 idx, uint32_t res[4][4])
{
    u128 o[4];
    v_reorder(dec_float(spec, acc, da), dec_float(spec, acc, db), dec_float(spec, acc, dc), ONEV, idx, o);
    lanes_out(o, res);
}
static void c_1C510(uint8_t* L, uint32_t data, uint32_t n, uint32_t pose, uint32_t idx, uint32_t dp)
{
    if (n == 0) return;
    const uint32_t D = U32(L, dp);
    Fld F[3]; desc_fields(D, F);
    const uint32_t w0 = F[0].s + F[0].e + F[0].m, w1 = F[1].s + F[1].e + F[1].m, w2 = F[2].s + F[2].e + F[2].m, nb = w0 + w1 + w2;
    const uint32_t bit0 = data << 3;
    for (uint32_t g = 0; g < ((n + 3u) >> 2); g++) {
        Dec da, db, dc; uint32_t res[4][4];
        for (uint32_t j = 0; j < 4; j++) {
            const u128i W = bits128(L, bit0 + (4u * g + j) * nb);
            dec_int(&da, (int)j, top32(W, 0), F[0].s, F[0].e, F[0].m);
            dec_int(&db, (int)j, top32(W, w0), F[1].s, F[1].e, F[1].m);
            dec_int(&dc, (int)j, top32(W, w0 + w1), F[2].s, F[2].e, F[2].m);
        }
        RUN2(g1C510(spec, acc, &da, &db, &dc, S(D), res));
        for (uint32_t j = 0; j < 4; j++) SQ4(L, pose + 48u * u16q(L, idx, j), res[j]);
        idx += 8u;
    }
}
static inline __attribute__((always_inline)) void g1B5F0(const int spec, spu_spec_acc* acc, u128 frac, const uint32_t dtA[4], const uint32_t dtB[4],
                                                         const Dec a[3], const Dec b[3], u128 idx, uint32_t res[4][4])
{
    u128 t, omt, A[4], Bv[4], o[4];
    v_interp(spec, acc, frac, dtA, dtB, &t, &omt);
    const u128 a0 = dec_float(spec, acc, &a[0]), a1 = dec_float(spec, acc, &a[1]), a2 = dec_float(spec, acc, &a[2]);
    const u128 b0 = dec_float(spec, acc, &b[0]), b1 = dec_float(spec, acc, &b[1]), b2 = dec_float(spec, acc, &b[2]);
    v_reorder(a0, a1, a2, v_wfrom(spec, acc, a0, a1, a2, 1), idx, A);       /* key A: clamped */
    v_reorder(b0, b1, b2, v_wfrom(spec, acc, b0, b1, b2, 0), idx, Bv);      /* key B: not */
    for (int k = 0; k < 3; k++) o[k] = v_lerp(spec, acc, A[k], Bv[k], t);
    o[3] = ONEV;
    lanes_out(o, res);
}
static void c_1B5F0(uint8_t* L, const Com* c, const uint32_t cur0[4], uint32_t kbyte, uint32_t sbyte, uint32_t ebyte,
                    uint32_t n, uint32_t pose, uint32_t idx, uint32_t dp)
{
    if (n == 0) return;
    uint32_t cur[4]; memcpy(cur, cur0, 16);
    uint32_t kb = kbyte << 3, sb = sbyte << 3, eb = ebyte << 3;
    const u128 frac = S(c->frac);
    for (uint32_t g = 0; g < ((n + 3u) >> 2); g++) {
        uint32_t res[4][4], P = 0, Wp = 0, dtA[4], dtB[4], Dv[4];
        Dec a[3], b[3];
        for (uint32_t j = 0; j < 4; j++) {
            const uint32_t D = U32(L, dp + 4u * j);
            Fld F[3]; desc_fields(D, F);
            const uint32_t wd0 = F[0].s + F[0].e + F[0].m, wd1 = F[1].s + F[1].e + F[1].m, wd2 = F[2].s + F[2].e + F[2].m;
            const uint32_t w = wd0 + wd1 + wd2;
            const Keys k = key_frames(bits128(L, cur[j]), c->mN, c->mF);
            const uint32_t nxt = kb + (uint32_t)((int32_t)(int16_t)k.Kb * (int32_t)(int16_t)w) + P;
            const uint32_t pA = k.Kb > 0 ? nxt - w : sb + Wp;
            const uint32_t pB = k.Kb == k.Kt ? eb + Wp : nxt;
            P += (uint32_t)((int32_t)(int16_t)k.Kt * (int32_t)(int16_t)w); Wp += w;
            dtA[j] = c->f - (uint32_t)k.pf; dtB[j] = (uint32_t)k.nf - c->f; Dv[j] = D;
            const u128i WA = bits128(L, pA), WB = bits128(L, pB);
            dec_int(&a[0], (int)j, top32(WA, 0), F[0].s, F[0].e, F[0].m);
            dec_int(&a[1], (int)j, top32(WA, wd0), F[1].s, F[1].e, F[1].m);
            dec_int(&a[2], (int)j, top32(WA, wd0 + wd1), F[2].s, F[2].e, F[2].m);
            dec_int(&b[0], (int)j, top32(WB, 0), F[0].s, F[0].e, F[0].m);
            dec_int(&b[1], (int)j, top32(WB, wd0), F[1].s, F[1].e, F[1].m);
            dec_int(&b[2], (int)j, top32(WB, wd0 + wd1), F[2].s, F[2].e, F[2].m);
        }
        RUN2(g1B5F0(spec, acc, frac, dtA, dtB, a, b, VA(Dv), res));
        static const int order[4] = {0, 2, 1, 3};
        for (int kk = 0; kk < 4; kk++) SQ4(L, pose + 48u * u16q(L, idx, (uint32_t)order[kk]), res[order[kk]]);
        idx += 8u; dp += 16u;
        kb += P; sb += Wp; eb += Wp;
        for (int j = 0; j < 4; j++) cur[j] += 4u * c->n;
    }
}
static inline __attribute__((always_inline)) void g1D4D0(const int spec, spu_spec_acc* acc, const Dec* d, uint32_t out[4])
{
    const u128 v = dec_float(spec, acc, d);
    for (int j = 0; j < 4; j++) out[j] = v._u32[j];
}
static void c_1D4D0(uint8_t* L, uint32_t data, uint32_t n, uint32_t out, uint32_t idx, uint32_t dp)
{
    if (n == 0) return;
    Fld F[3]; desc_fields(U32(L, dp), F);
    const uint32_t w = F[0].s + F[0].e + F[0].m, bit0 = data << 3;
    for (uint32_t g = 0; g < ((n + 3u) >> 2); g++) {
        Dec d; uint32_t r[4];
        for (uint32_t j = 0; j < 4; j++) dec_int(&d, (int)j, top32(bits128(L, bit0 + (4u * g + j) * w), 0), F[0].s, F[0].e, F[0].m);
        RUN2(g1D4D0(spec, acc, &d, r));
        for (uint32_t j = 0; j < 4; j++) SW(L, out + 4u * u16q(L, idx, j), r[j]);
        idx += 8u;
    }
}
static inline __attribute__((always_inline)) void g1CBB0(const int spec, spu_spec_acc* acc, u128 frac, const uint32_t dtA[4], const uint32_t dtB[4],
                                                         const Dec* a, const Dec* b, uint32_t out[4])
{
    u128 t, omt;
    v_interp(spec, acc, frac, dtA, dtB, &t, &omt);
    const u128 v = v_lerp(spec, acc, dec_float(spec, acc, a), dec_float(spec, acc, b), t);
    for (int j = 0; j < 4; j++) out[j] = v._u32[j];
}
static void c_1CBB0(uint8_t* L, const Com* c, const uint32_t cur0[4], uint32_t kbyte, uint32_t sbyte, uint32_t ebyte,
                    uint32_t n, uint32_t out, uint32_t idx, uint32_t dp)
{
    if (n == 0) return;
    uint32_t cur[4]; memcpy(cur, cur0, 16);
    uint32_t kb = kbyte << 3, sb = sbyte << 3, eb = ebyte << 3;
    const u128 frac = S(c->frac);
    for (uint32_t g = 0; g < ((n + 3u) >> 2); g++) {
        uint32_t res[4], P = 0, Wp = 0, dtA[4], dtB[4];
        Dec a, b;
        for (uint32_t j = 0; j < 4; j++) {
            Fld F[3]; desc_fields(U32(L, dp + 4u * j), F);
            const uint32_t w = F[0].s + F[0].e + F[0].m;
            const Keys k = key_frames(bits128(L, cur[j]), c->mN, c->mF);
            const uint32_t nxt = kb + (uint32_t)((int32_t)(int16_t)k.Kb * (int32_t)(int16_t)w) + P;
            const uint32_t pA = k.Kb > 0 ? nxt - w : sb + Wp;
            const uint32_t pB = k.Kb == k.Kt ? eb + Wp : nxt;
            P += (uint32_t)((int32_t)(int16_t)k.Kt * (int32_t)(int16_t)w); Wp += w;
            dtA[j] = c->f - (uint32_t)k.pf; dtB[j] = (uint32_t)k.nf - c->f;
            dec_int(&a, (int)j, top32(bits128(L, pA), 0), F[0].s, F[0].e, F[0].m);
            dec_int(&b, (int)j, top32(bits128(L, pB), 0), F[0].s, F[0].e, F[0].m);
        }
        RUN2(g1CBB0(spec, acc, frac, dtA, dtB, &a, &b, res));
        for (uint32_t j = 0; j < 4; j++) SW(L, out + 4u * u16q(L, idx, j), res[j]);
        idx += 8u; dp += 16u;
        kb += P; sb += Wp; eb += Wp;
        for (int j = 0; j < 4; j++) cur[j] += 4u * c->n;
    }
}
static inline u128i hmask128(uint32_t k) { return k == 0 ? 0 : (k >= 128 ? ALL128 : ALL128 << (128u - k)); }

/* ---- the sampler: 1 = done, 0 = declined (nothing written) ---- */
static int clip_core(uint8_t* L, const u128* gpr)
{
    const uint32_t pose = gpr[3]._u32[0], chan = gpr[4]._u32[0], H = gpr[5]._u32[0], C = gpr[6]._u32[0];
    const uint32_t n = gpr[7]._u32[0], f = gpr[8]._u32[0], frac = gpr[9]._u32[0], sp = gpr[1]._u32[0];
    if ((pose & 15u) || f > n || n > 128u || (H & 15u) || (C & 1u)) return 0;
    if (sp < 0x700u || sp + 0x20u > SPU_LS_SIZE || (sp & 15u)) return 0;
    const uint32_t nCR = U16(L, H + 0x16), nCT = U16(L, H + 0x18), nCS = U16(L, H + 0x1A), nCF = U16(L, H + 0x1C);
    const uint32_t nAR = U16(L, H + 0x1E), nAT = U16(L, H + 0x20), nAS = U16(L, H + 0x22), nAF = U16(L, H + 0x24);
    const uint32_t flags = U32(L, H + 0x24) & 0xFu;
    if ((flags & 1u) || !(flags & 2u) || !(flags & 4u) || !(flags & 8u)) return 0;
    if (nCR > 0x1000u || nCT > 0x1000u || nCS > 0x1000u || nCF > 0x1000u ||
        nAR > 0x1000u || nAT > 0x1000u || nAS > 0x1000u || nAF > 0x1000u) return 0;
#define SELFREL(a) (U32(L, (a)) ? (a) + U32(L, (a)) : 0u)
    const uint32_t cR = SELFREL(H + 0x40), cT = SELFREL(H + 0x44), cS = SELFREL(H + 0x48), cF = SELFREL(H + 0x4C);
    const uint32_t dsc = SELFREL(H + 0x50);
#undef SELFREL
    if ((cR & 15u)) return 0;                                   /* 1A570 reads 16-aligned quads (spec 8.3.9) */
    const uint32_t iCR = H + 0x60, iCT = iCR + 2u * ((nCR + 7u) & ~7u), iCS = iCT + 2u * ((nCT + 3u) & ~3u);
    const uint32_t iCF = iCS + 2u * ((nCS + 3u) & ~3u), iAR = iCF + 2u * ((nCF + 3u) & ~3u);
    const uint32_t iAT = iAR + 2u * ((nAR + 3u) & ~3u), iAS = iAT + 2u * ((nAT + 3u) & ~3u), iAF = iAS + 2u * ((nAS + 3u) & ~3u);
    uint32_t h[8];
    for (int k = 0; k < 8; k++) h[k] = U16(L, C + 2u * (uint32_t)k);
    const uint32_t sR = C + 16u, sT = sR + h[0], sS = sT + h[1], sF = sS + h[2], mask = sF + h[3];
    const uint32_t NA = nAR + nAT + nAS + nAF;
    const uint32_t kR = mask + ((n * NA + 7u) >> 3), kT = kR + h[4], kS = kT + h[5], kF = (kS + h[6] + 3u) & ~3u;
    const uint32_t e0 = (kF + h[7] + 15u) & ~15u;
    const uint32_t eR = e0 + 16u, eT = eR + U16(L, e0), eS = eT + U16(L, e0 + 2), eF = eS + U16(L, e0 + 4);
    (void)eF; (void)iAF;
    /* the bones / channels the decoders will store to must be inside the LS (the SPU would wrap) */
    uint32_t maxb = 0, maxc = 0;
    { const uint32_t lists[6][2] = {{iCR, (nCR + 7u) & ~7u}, {iCT, (nCT + 3u) & ~3u}, {iCS, (nCS + 3u) & ~3u},
                                   {iAR, (nAR + 3u) & ~3u}, {iAT, (nAT + 3u) & ~3u}, {iAS, (nAS + 3u) & ~3u}};
      for (int l = 0; l < 6; l++) for (uint32_t i = 0; i < lists[l][1]; i++) { const uint32_t v = U16(L, lists[l][0] + 2u * i); if (v > maxb) maxb = v; }
      const uint32_t fl[2][2] = {{iCF, (nCF + 3u) & ~3u}, {iAF, (nAF + 3u) & ~3u}};
      for (int l = 0; l < 2; l++) for (uint32_t i = 0; i < fl[l][1]; i++) { const uint32_t v = U16(L, fl[l][0] + 2u * i); if (v > maxc) maxc = v; } }
    if (pose + 48u * maxb + 48u > SPU_LS_SIZE || chan + 4u * maxc + 4u > SPU_LS_SIZE) return 0;
    /* what 0x18EC0 writes in its own frame: the caller's lr slot and the r80..r127 save area */
    SQ4(L, sp + 0x10u, gpr[0]._u32);
    for (int k = 80; k < 128; k++) SQ4(L, sp - 16u * (uint32_t)(k - 79), gpr[k]._u32);
    Com c; c.mN = hmask128(n); c.mF = hmask128(f); c.f = f; c.frac = frac; c.n = n;
    const uint32_t nl = n & 0xFFFFu;
    uint32_t b = mask << 3, cuR[4], cuT[4], cuS[4], cuF[4];
    for (uint32_t k = 0; k < 4; k++) cuR[k] = b + nl * k;
    b += n * nAR; for (uint32_t k = 0; k < 4; k++) cuT[k] = b + nl * k;
    b += n * nAT; for (uint32_t k = 0; k < 4; k++) cuS[k] = b + nl * k;
    b += n * nAS; for (uint32_t k = 0; k < 4; k++) cuF[k] = b + nl * k;
    const uint32_t stR[4] = {sR, sR + 6u, sR + 12u, sR + 18u}, enR[4] = {eR, eR + 6u, eR + 12u, eR + 18u};
    c_1A570(L, pose, cR, nCR, iCR);
    c_19A20(L, &c, cuR, kR, stR, enR, nAR, pose, iAR);
    uint32_t d = dsc;
    c_1C510(L, cT, nCT, pose + 16u, iCT, d);
    c_1B5F0(L, &c, cuT, kT, sT, eT, nAT, pose + 16u, iAT, d + 4u);
    d = d + 4u * nAT + 4u;
    c_1C510(L, cS, nCS, pose + 32u, iCS, d);
    c_1B5F0(L, &c, cuS, kS, sS, eS, nAS, pose + 32u, iAS, d + 4u);
    d = d + 4u * nAS + 4u;
    c_1D4D0(L, cF, nCF, chan, iCF, d);
    c_1CBB0(L, &c, cuF, kF, sF, eF, nAF, chan, iAF, d + 4u);
    return 1;
}

static int s_mode = -1, s_check, s_stats;
static unsigned long long s_every = 20000, s_calls, s_decl, s_chk, s_bad;
static _Thread_local int t_bypass;
static _Thread_local uint8_t* t_ls;   /* (deck-merge) SPU_LS_SIZE + 64, made at the first check (module static TLS) */

static int j218_clip(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_J218CLIP");
        s_mode = e ? atoi(e) : 1;
        s_check = getenv("J218CLIP_CHECK") != 0;
        e = getenv("J218CLIP_STATS"); s_stats = e != 0;
        if (e && atoll(e) > 1) s_every = (unsigned long long)atoll(e);
        fprintf(stderr, "[j218clip] native clip sampler (0x18EC0): %s%s\n", s_mode ? "on" : "off (PS3RECOMP_NATIVE_J218CLIP=0)",
                s_mode && s_check ? ", CHECK mode (the lifted result is kept)" : "");
    }
    if (!s_mode || t_bypass || (g_spu_jobrec_mode & SPU_JOBREC_RECORD)) return 0;
    const unsigned long long c = __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);
    const uint32_t sp = ctx->gpr[1]._u32[0] & SPU_LS_MASK;
    int ok;
    if (s_check) {
        if (!t_ls && !(t_ls = (uint8_t*)malloc(SPU_LS_SIZE + 64))) return 0;
        memcpy(t_ls, ctx->ls, SPU_LS_SIZE);
        ok = clip_core(t_ls, ctx->gpr);
        const u128 r0 = ctx->gpr[0];
        t_bypass = 1;
        { const int32_t si = (int32_t)ctx->image_id; ctx->host_depth++;
          jobmod_ab654d30fd90_efe70_spu_func_00018EC0(ctx); spu_drain_call(ctx, r0._u32[0]); ctx->host_depth--;
          spu_img_restore(ctx, si); }
        t_bypass = 0;
        ctx->gpr[0] = r0;
        if (ok) {
            const uint32_t lo = sp - 0x6E0u, hi = sp - 0x300u;      /* the scratch window: not compared */
            const int bad = memcmp(t_ls, ctx->ls, lo) || memcmp(t_ls + hi, ctx->ls + hi, SPU_LS_SIZE - hi);
            __atomic_add_fetch(&s_chk, 1, __ATOMIC_RELAXED);
            if (bad && __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED) <= 8) {
                uint32_t a = 0; while (a < SPU_LS_SIZE && (t_ls[a] == ctx->ls[a] || (a >= lo && a < hi))) a++;
                fprintf(stderr, "[j218clip] CHECK: native differs, first byte 0x%05X (n=%u f=%u)\n", a,
                        ctx->gpr[7]._u32[0], ctx->gpr[8]._u32[0]);
            }
        } else __atomic_add_fetch(&s_decl, 1, __ATOMIC_RELAXED);
        if (s_stats && c % s_every == 0) fprintf(stderr, "[j218clip] %llu calls, %llu declined, check %llu/%llu differ, %llu failed speculative groups\n", c, s_decl, s_bad, s_chk, s_fail);
        return 1;
    }
    ok = clip_core(ctx->ls, ctx->gpr);
    if (!ok) { __atomic_add_fetch(&s_decl, 1, __ATOMIC_RELAXED); return 0; }
    spu_native_dead_ls(sp - 0x6E0u, sp - 0x300u);
    if (s_stats && c % s_every == 0) fprintf(stderr, "[j218clip] %llu calls, %llu declined, %llu failed speculative groups\n", c, s_decl, s_fail);
    return 1;
}

__attribute__((constructor)) static void j218_clip_register(void)
{
    spu_native_register(218, 0x18EC0u, j218_clip);
}
