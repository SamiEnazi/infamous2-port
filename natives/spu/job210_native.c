/* inFAMOUS 2: the culling-tree walker of job 210 (jobmod_867e5d2348b3_e2898, LS 0x4358), natively.
 *
 * The walker interprets byte-coded record streams (21 record types: AABB / LOD / mask tests,
 * transforms, child references) found by pointer-chasing through main memory, and emits type-7
 * records that it counting-sorts into 248 buckets and PUTs to two heaps reserved with atomic adds.
 * The lifted walker fetches every block with a 16-byte-granular DMA GET and dispatches every record
 * through an indirect branch; this rewrite reads the blocks straight from guest memory (as job204's
 * native does) and runs the walk as plain C. Spec: work/bench/job210-analysis.md (dev tree).
 *
 * Two phases. Phase 1 walks the whole tree without writing anything that outlives the job: the
 * records go to a native array, the lifted code's GETs / tag waits / flushes to an event list. Any
 * record type or input it does not model: it declines (returns 0) and the lifted walker runs.
 * Phase 2 commits in the lifted code's order: buffer 2 and its header, every flush (sort, the two
 * atomic reservations, the header PUT and the entry PUTL), buffer 3's records, the profiler word, and
 * the MFC channel latches the lifted code would leave. Stage 1: the AABB test (0x4030 with 0x6650 and
 * 0x6220) and the LOD test (0x5E20) are the lifted functions themselves, called with their operands
 * staged in the dead stack below the walker's frame -- bit-exact by construction.
 *
 * In a job replay (tools/jobreplay.sh) the walker's span of the recording ends at its second RdDec
 * (spu_native_replay_end_dec); the ranges it does not leave as the lifted code does are declared dead:
 * the BSS (0x6D60-0xE620, zeroed by the job's own wrapper at every call), the stack below the walker's
 * caller, the two DMA landing buffers and the atomic line at 0x80.
 *
 * PS3RECOMP_NATIVE_J210=0: off (the lifted walker). PS3_SPU_NATIVE=0: every rewrite off.
 * J210_DEBUG=1: a line per declined call. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "native_float.h"
#include "spu_dma.h"
#include "spu_jobrec.h"
#include <immintrin.h>

extern uint8_t* vm_base;
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));

/* the lifted functions stage 1 calls */
void jobmod_867e5d2348b3_e2898_spu_func_00004030(spu_context* ctx);   /* aabb_visible(aabb, M, cam, buf1, planes) -> r3 */
void jobmod_867e5d2348b3_e2898_spu_func_00005E20(spu_context* ctx);   /* lod_inside(idx, lod, table, M, p, cam, K) -> r3 */

#define J210_IMAGE   210
#define J210_WALKER  0x4358u
#define STACK_MAX    384
#define BATCH        1792
#define SLOT_BYTES   0x4000u

static inline uint32_t R32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint16_t R16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline int16_t S16(const uint8_t* p) { return (int16_t)R16(p); }
static inline void P32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void P16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 LQ(const uint8_t* p) { return V4(R32(p), R32(p + 4), R32(p + 8), R32(p + 12)); }
static inline void SQ(uint8_t* p, u128 v) { P32(p, v._u32[0]); P32(p + 4, v._u32[1]); P32(p + 8, v._u32[2]); P32(p + 12, v._u32[3]); }
static inline u128 SPLAT(u128 v, int i) { return V4(v._u32[i], v._u32[i], v._u32[i], v._u32[i]); }
static inline u128 SPLW(uint32_t x) { return V4(x, x, x, x); }

typedef struct { uint32_t a, b, ea; uint16_t size; uint8_t lod; u128 m[4]; } Ent;     /* the BSS stack entry + its matrix */
typedef struct { u128 m[4]; uint32_t a, b, ea, size, lod, tag, landing; } Slot;         /* a landing slot (walker frame sp+0x150/0x1B0) */
typedef struct { uint32_t w8, w12; uint8_t key; } Rec;                                   /* a type-7 record (buffer 3: key, -, w8, w12) */
enum { EV_GET = 1, EV_WAIT = 2, EV_FLUSH = 3 };
typedef struct { uint8_t kind; uint32_t x0, x1, x2, x3; } Ev;

enum { FB_TYPE = 1, FB_EA = 2, FB_SIZE = 4, FB_KEY = 8, FB_TAIL = 16, FB_ZERO = 32, FB_MEM = 64 };

typedef struct {
    spu_context* ctx;
    uint8_t* ls;
    uint32_t buf1, planes, stream, buf3, buf2, t2, t1, tagA;
    u128 r1, r0, cam, K;
    uint32_t F;                 /* native scratch in the dead stack: F+0 M, +0x40 N, +0x80 AABB, +0xA0 cam */
    int fallback;
    uint32_t bad_ea, bad_size, bad_type;
    /* walk state */
    u128 M[4];
    uint32_t a, b, lod;
    Ent stack[STACK_MAX];
    uint32_t nstack;
    Slot S[2];
    /* outputs */
    Rec* recs; uint32_t nrec, caprec;
    Ev* evs; uint32_t nev, capev;
    uint32_t batch0;            /* first record of the open batch */
} J;

static _Thread_local J* t_j;
static int s_mode = -1, s_dbg;
static unsigned long long s_calls, s_declined, s_why[8];   /* J210_STATS=1: a summary line every 5000 calls */
static int s_stats;
static int s_math = -1;                 /* J210_MATH: 0 = the lifted tests (stage 1), 1 = the native tests; J210_CHECK=1: both, lifted wins */
static unsigned long long s_chk_vis, s_chk_lod, s_bad_vis, s_bad_lod, s_vis_deep, s_planes_slow;
static int s_fastplanes = -1;          /* J210_FASTPLANES=0: the per-op planes walk only */

static void ev_add(J* j, uint8_t kind, uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3)
{
    if (j->nev == j->capev) {
        uint32_t cap = j->capev ? j->capev * 2 : 1024;
        Ev* n = (Ev*)realloc(j->evs, (size_t)cap * sizeof(Ev));
        if (!n) { j->fallback |= FB_MEM; return; }
        j->evs = n; j->capev = cap;
    }
    Ev* e = &j->evs[j->nev++]; e->kind = kind; e->x0 = x0; e->x1 = x1; e->x2 = x2; e->x3 = x3;
}

/* ---------------- calls into the lifted job (stage 1) ---------------- */
static inline void call_lifted(J* j, void (*fn)(spu_context*), uint32_t link)
{
    spu_context* ctx = j->ctx;
    ctx->gpr[0] = V4(link, 0, 0, 0);
    const int32_t si = (int32_t)ctx->image_id;
    ctx->host_depth++; fn(ctx); spu_drain_call(ctx, link); ctx->host_depth--;
    spu_img_restore(ctx, si);
}

static void put_matrix(J* j, uint32_t lsa, const u128* m)
{
    for (int i = 0; i < 4; i++) SQ(j->ls + lsa + 16 * i, m[i]);
}

/* 0x66F8 decode_aabb, lean path (L6010-6081): the packed AABB at p[0..7] -> mn (out), mx (out+16) */
static void decode_aabb(const uint8_t* p, u128* mn, u128* mx)
{
    const u128 sA = SPLW(p[0]), sB = SPLW(p[4]);
    const int32_t cx = (int8_t)p[1], cy = (int8_t)p[2], cz = (int8_t)p[3];
    const u128 c = spu_csflt(spu_shl(V4((uint32_t)cx, (uint32_t)cy, (uint32_t)cz, (uint32_t)cz), sA), 155);
    const u128 e = spu_csflt(spu_shl(V4(p[5], p[6], p[7], p[7]), sB), 155);
    *mx = spu_fa(c, e);
    *mn = spu_fs(c, e);
}

/* type 1/2/11: 0x66F8 then 0x4030 with M (or N) at mlsa; 1 = visible */
static int aabb_visible_lifted(J* j, const uint8_t* p, uint32_t mlsa)
{
    spu_context* ctx = j->ctx;
    u128 mn, mx; decode_aabb(p, &mn, &mx);
    SQ(j->ls + j->F + 0x80, mn); SQ(j->ls + j->F + 0x90, mx);
    ctx->gpr[1] = j->r1;
    ctx->gpr[3] = V4(j->F + 0x80, 0, 0, 0);
    ctx->gpr[4] = V4(mlsa, 0, 0, 0);
    ctx->gpr[5] = V4(j->F + 0xA0, 0, 0, 0);
    ctx->gpr[6] = V4(j->buf1, 0, 0, 0);
    ctx->gpr[7] = V4(j->planes, 0, 0, 0);
    call_lifted(j, jobmod_867e5d2348b3_e2898_spu_func_00004030, 0x4D1Cu);
    return (ctx->gpr[3]._u32[0] & 0xFFu) != 0;
}

/* types 3/4/5/12/17: 0x5E20 with the current M (staged at F); 1 = inside */
static int lod_inside_lifted(J* j, uint32_t idx, u128 pos)
{
    spu_context* ctx = j->ctx;
    ctx->gpr[1] = j->r1;
    ctx->gpr[3] = V4(idx, 0, 0, 0);
    ctx->gpr[4] = V4(j->lod, 0, 0, 0);
    ctx->gpr[5] = V4(j->buf1 + 0x54, 0, 0, 0);
    ctx->gpr[6] = V4(j->F, 0, 0, 0);
    ctx->gpr[7] = pos;
    ctx->gpr[8] = j->cam;
    ctx->gpr[9] = j->K;
    call_lifted(j, jobmod_867e5d2348b3_e2898_spu_func_00005E20, 0x4D90u);
    return ctx->gpr[3]._u32[0] != 0;
}

/* positions: int16 x4.0 (types 3/4/5) or three floats (12 at +8, 17 at +4) */
static inline u128 pos_i16(const uint8_t* q)
{
    const u128 v = spu_csflt(V4((uint32_t)(int32_t)S16(q), (uint32_t)(int32_t)S16(q + 2), (uint32_t)(int32_t)S16(q + 4), 0), 155);
    return spu_fm(v, SPLW(0x40800000u));
}

/* ---------------- stage 2: the tests natively (same helpers, same order as the lifted lean paths) ---------------- */
static inline u128 SEL3(u128 a, u128 b) { a._u32[3] = b._u32[3]; return a; }          /* selb(a, b, fsmbi(0x000F)): lane 3 from b */
static inline u128 SEL2(u128 a, u128 b) { a._u32[2] = b._u32[2]; return a; }          /* selb(a, b, fsmbi(0x00F0)): lane 2 from b */
static inline u128 SEL1(u128 a, u128 b) { a._u32[1] = b._u32[1]; return a; }          /* selb(a, b, fsmbi(0x0F00)): lane 1 from b */
static inline u128 ROT(u128 v, int w) { return V4(v._u32[w & 3], v._u32[(w + 1) & 3], v._u32[(w + 2) & 3], v._u32[(w + 3) & 3]); }   /* rotqbyi(v, 4w) */
static inline u128 HSUM(u128 v) { return spu_fa(spu_fa(v, ROT(v, 1)), ROT(v, 2)); }    /* fa(fa(v, shufb 0x6B60), shufb 0x6B70) */
static inline int GT0(u128 a, u128 b) { return spu_fcgt_lean(a, b)._u32[0] != 0; }    /* lane 0 of fcgt */
#define F_ONE  0x3F800000u
#define F_HALF 0x3F000000u

/* 0x6650 xform_aabb (L5890-5929) */
static void nat_xform(const u128 mnL, const u128 mxL, const u128* R, u128* mn, u128* mx)
{
    const u128 c = spu_fm(spu_fa(mnL, mxL), SPLW(F_HALF));
    const u128 e = spu_fs(mxL, c);
    const u128 c1 = SEL3(c, SPLW(F_ONE));
    const u128 t = spu_fma(R[2], SPLAT(c1, 2), spu_fma(R[1], SPLAT(c1, 1), spu_fm(R[0], SPLAT(c1, 0))));
    const u128 wc = spu_fa(t, R[3]);
    u128 A0 = R[0], A1 = R[1], A2 = R[2];
    for (int i = 0; i < 4; i++) { A0._u32[i] &= 0x7FFFFFFFu; A1._u32[i] &= 0x7FFFFFFFu; A2._u32[i] &= 0x7FFFFFFFu; }
    const u128 we = spu_fma(SPLAT(e, 2), A2, spu_fma(SPLAT(e, 0), A0, spu_fm(SPLAT(e, 1), A1)));
    *mx = spu_fa(wc, we);
    *mn = spu_fs(wc, we);
}

/* 0x6220 planes_visible (L5012-5331, 0x644C loop): 1 = not rejected; -1 = its 64-deep local stack would overflow */
static int nat_planes(J* j, const u128 mn, const u128 mx)
{
    const uint32_t count = R32(j->ls + j->buf1 + 0x30);
    if (count == 0) return 1;
    const uint8_t* base = j->ls;
    const u128 mn1 = SEL3(mn, SPLW(F_ONE)), mx1 = SEL3(mx, SPLW(F_ONE));
    uint32_t n[4];
    n[0] = j->planes;
    for (int k = 1; k < 4; k++) n[k] = (n[k - 1] + 32u * R32(base + ((n[k - 1] + 0x14) & SPU_LS_MASK))) & SPU_LS_MASK;
    for (int k = 0; k < 4; k++) {
        const u128 P = LQ(base + n[k]);
        const u128 a = spu_fm(P, mx1), b = spu_fm(P, mn1);
        const u128 g = spu_fcgt_lean(b, a);
        const u128 hi = spu_selb(a, b, g);
        if (GT0(SPLAT(P, 3), spu_fa(spu_fa(SPLAT(hi, 0), SPLAT(hi, 1)), SPLAT(hi, 2)))) return 0;
    }
    uint32_t node = (n[3] + 32u * R32(base + ((n[3] + 0x14) & SPU_LS_MASK))) & SPU_LS_MASK;
    uint32_t stack[64];
    int sp = 0;
    for (;;) {
        const u128 P = LQ(base + node);
        const u128 a = spu_fm(P, mn), b = spu_fm(P, mx);
        const u128 g = spu_fcgt_lean(a, b);
        const u128 hi = spu_selb(b, a, g), lo = spu_selb(a, b, g);
        const u128 hs = spu_fa(spu_fa(SPLAT(hi, 0), SPLAT(hi, 1)), SPLAT(hi, 2));
        const u128 lsum = spu_fa(spu_fa(SPLAT(lo, 0), SPLAT(lo, 1)), SPLAT(lo, 2));
        const u128 d = spu_fs(SEL1(lsum, hs), SPLAT(P, 3));
        const u128 zero = SPLW(0);
        if (!GT0(SPLAT(d, 0), zero)) {                                   /* not wholly in front */
            const uint32_t c = R32(base + ((node + 0x10) & SPU_LS_MASK));
            if (GT0(zero, SPLAT(d, 1))) {                                /* wholly behind: down, or pop */
                node = (node + 32u * c) & SPU_LS_MASK;
                if (c) continue;
                if (sp == 0) return 0;
                node = stack[--sp];
                continue;
            }
            if (c) { if (sp >= 64) return -1; stack[sp++] = (node + 32u * c) & SPU_LS_MASK; }
        }
        const uint32_t sib = R32(base + ((node + 0x14) & SPU_LS_MASK));
        if (sib == 0) return 1;
        node = (node + 32u * sib) & SPU_LS_MASK;
    }
}

/* The planes walk again, speculatively (spu_helpers.h, "speculative float regions"): SSE loads, the products
 * and sums as plain RZ/FTZ/DAZ operations (the SPU threads' float mode: for operands and results inside the
 * ordinary float range these are the SPU's bits), each result's magnitude folded into one verdict, and the
 * planes' own words too (a plane word with exponent 255 is a big number to the SPU and Inf/NaN to the host).
 * It only decides -- no lane of a product feeds anything but the sums and compares below, and the sign of a
 * zero changes no compare -- so when the verdict says every value stayed in range, its answer is nat_planes'
 * answer. Otherwise (never seen in the recordings) the caller runs nat_planes. Lane 3 of the products is not
 * used (the sums take lanes 0-2) and is left out of the verdict. 1/0/-1 as nat_planes; 2 = verdict failed. */
static inline __m128 PLD(const uint8_t* base, uint32_t a)
{
    const __m128i v = _mm_loadu_si128((const __m128i*)(const void*)(base + (a & SPU_LS_MASK)));
    return _mm_castsi128_ps(_mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
static inline __m128 XV(u128 v) { __m128 r; memcpy(&r, &v, 16); return r; }
#define ACC(acc, v, m) (acc) = _mm_max_epu32((acc), _mm_and_si128(_mm_castps_si128(v), (m)))
static inline __m128 SUM3(__m128 v)                       /* lane 0: (v0 + v1) + v2, as fa(fa(splat0, splat1), splat2) */
{
    return _mm_add_ss(_mm_add_ss(v, _mm_shuffle_ps(v, v, 0x55)), _mm_shuffle_ps(v, v, 0xAA));
}
static int nat_planes_fast(J* j, const u128 mnU, const u128 mxU)
{
    const uint32_t count = R32(j->ls + j->buf1 + 0x30);
    if (count == 0) return 1;
    const uint8_t* base = j->ls;
    const __m128i abs4 = _mm_set1_epi32(0x7FFFFFFF), abs3 = _mm_setr_epi32(0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0);
    __m128i acc = _mm_setzero_si128();
    const __m128 mn = XV(mnU), mx = XV(mxU);
    ACC(acc, mn, abs4); ACC(acc, mx, abs4);
    const __m128 one = _mm_set1_ps(1.0f);
    const __m128 mn1 = _mm_blend_ps(mn, one, 8), mx1 = _mm_blend_ps(mx, one, 8);
    uint32_t n = j->planes;
    int ret = -2;
    for (int k = 0; k < 4 && ret == -2; k++) {
        if (k) n = (n + 32u * R32(base + ((n + 0x14) & SPU_LS_MASK))) & SPU_LS_MASK;
        const __m128 P = PLD(base, n);
        ACC(acc, P, abs4);
        const __m128 a = _mm_mul_ps(P, mx1), b = _mm_mul_ps(P, mn1);
        ACC(acc, a, abs3); ACC(acc, b, abs3);
        const __m128 hi = _mm_blendv_ps(a, b, _mm_cmpgt_ps(b, a));
        const __m128 hs = SUM3(hi);
        ACC(acc, hs, abs4);
        if (_mm_comigt_ss(_mm_shuffle_ps(P, P, 0xFF), hs)) ret = 0;
    }
    if (ret == -2) {
        uint32_t node = (n + 32u * R32(base + ((n + 0x14) & SPU_LS_MASK))) & SPU_LS_MASK;
        uint32_t stack[64];
        int sp = 0;
        for (;;) {
            const __m128 P = PLD(base, node);
            ACC(acc, P, abs4);
            const __m128 a = _mm_mul_ps(P, mn), b = _mm_mul_ps(P, mx);
            ACC(acc, a, abs3); ACC(acc, b, abs3);
            const __m128 g = _mm_cmpgt_ps(a, b);
            const __m128 hi = _mm_blendv_ps(b, a, g), lo = _mm_blendv_ps(a, b, g);
            const __m128 p3 = _mm_shuffle_ps(P, P, 0xFF);
            const __m128 d0 = _mm_sub_ss(SUM3(lo), p3), d1 = _mm_sub_ss(SUM3(hi), p3);
            ACC(acc, d0, abs4); ACC(acc, d1, abs4);
            if (!_mm_comigt_ss(d0, _mm_setzero_ps())) {                 /* not wholly in front */
                const uint32_t c = R32(base + ((node + 0x10) & SPU_LS_MASK));
                if (_mm_comigt_ss(_mm_setzero_ps(), d1)) {               /* wholly behind: down, or pop */
                    node = (node + 32u * c) & SPU_LS_MASK;
                    if (c) continue;
                    if (sp == 0) { ret = 0; break; }
                    node = stack[--sp];
                    continue;
                }
                if (c) { if (sp >= 64) { ret = -1; break; } stack[sp++] = (node + 32u * c) & SPU_LS_MASK; }
            }
            const uint32_t sib = R32(base + ((node + 0x14) & SPU_LS_MASK));
            if (sib == 0) { ret = 1; break; }
            node = (node + 32u * sib) & SPU_LS_MASK;
        }
    }
    /* SPU_SPEC_OK's test: every magnitude below 0x7F7FFFFF (FLT_MAX and Inf/NaN fail) */
    return _mm_testz_si128(_mm_cmpgt_epi32(acc, _mm_set1_epi32(0x7F7FFFFE)), _mm_set1_epi32(-1)) ? ret : 2;
}

/* (deck-perf) The box transform, the size / distance pre-tests and the LOD test speculatively (spu_helpers.h
 * "speculative float regions"): every checked op its fast form, one range verdict at the end; all three are pure
 * functions of their inputs, so a failed verdict just runs the per-op form (nat_xform / below / nat_lod) instead.
 * A decision taken on in-range values is the per-op form's decision (the compares are exact on them). */
#define XFM(a, b)      (spec ? spu_fm_spec((a), (b), acc) : spu_fm((a), (b)))
#define XFA(a, b)      (spec ? spu_fa_spec((a), (b), acc) : spu_fa((a), (b)))
#define XFS(a, b)      (spec ? spu_fs_spec((a), (b), acc) : spu_fs((a), (b)))
#define XFMA(a, b, c)  (spec ? spu_fma_spec((a), (b), (c), acc) : spu_fma((a), (b), (c)))
#define XFMS(a, b, c)  (spec ? spu_fms_spec((a), (b), (c), acc) : spu_fms((a), (b), (c)))
static inline __attribute__((always_inline)) u128 x_hsum(const int spec, spu_spec_acc* acc, u128 v)
{
    return XFA(XFA(v, ROT(v, 1)), ROT(v, 2));
}
static inline __attribute__((always_inline)) void x_xform(const int spec, spu_spec_acc* acc, const u128 mnL, const u128 mxL,
                                                          const u128* R, u128* mn, u128* mx)
{
    const u128 c = XFM(XFA(mnL, mxL), SPLW(F_HALF));
    const u128 e = XFS(mxL, c);
    const u128 c1 = SEL3(c, SPLW(F_ONE));
    const u128 t = XFMA(R[2], SPLAT(c1, 2), XFMA(R[1], SPLAT(c1, 1), XFM(R[0], SPLAT(c1, 0))));
    const u128 wc = XFA(t, R[3]);
    u128 A0 = R[0], A1 = R[1], A2 = R[2];
    for (int i = 0; i < 4; i++) { A0._u32[i] &= 0x7FFFFFFFu; A1._u32[i] &= 0x7FFFFFFFu; A2._u32[i] &= 0x7FFFFFFFu; }
    const u128 we = XFMA(SPLAT(e, 2), A2, XFMA(SPLAT(e, 0), A0, XFM(SPLAT(e, 1), A1)));
    *mx = XFA(wc, we);
    *mn = XFS(wc, we);
}
/* 0 culled by a pre-test, 3 = go on to the plane tree with *mn, *mx */
static inline __attribute__((always_inline)) int x_vis_pre(const int spec, spu_spec_acc* acc, J* j, const u128 mnL, const u128 mxL,
                                                            const u128* M, u128* mno, u128* mxo)
{
    u128 mn, mx;
    x_xform(spec, acc, mnL, mxL, M, &mn, &mx);
    const u128 K = j->K;
    if (j->ls[j->buf1 + 0x81] != 0) {
        const u128 thr = SPLAT(K, 3);
        if (GT0(thr, SPLAT(mx, 2))) return 0;
        const u128 z = GT0(thr, SPLAT(mn, 2)) ? thr : SPLAT(mn, 2);
        mn = SEL2(mn, z);
    }
    const u128 d = XFS(mx, mn);
    const u128 S2 = SPLAT(x_hsum(spec, acc, XFM(d, d)), 0);
    if (GT0(ROT(K, 1), S2)) return 0;
    const u128 c2 = XFA(mn, mx);
    const u128 K2 = ROT(K, 2);
    if (GT0(K2, SPLW(0))) {
        u128 v = XFMS(c2, SPLW(F_HALF), j->cam);
        v = SEL2(v, XFM(SPLAT(v, 2), SPLAT(K, 0)));
        const u128 D = SPLAT(x_hsum(spec, acc, XFM(v, v)), 0);
        if (GT0(XFM(D, K2), S2)) return 0;
    }
    *mno = mn; *mxo = mx;
    return 3;
}
static inline __attribute__((always_inline)) int x_lod(const int spec, spu_spec_acc* acc, J* j, uint32_t R, const u128 p)
{
    const u128* M = j->M;
    const u128 t = XFMA(M[2], SPLAT(p, 2), XFMA(M[1], SPLAT(p, 1), XFM(M[0], SPLAT(p, 0))));
    u128 v = XFS(XFA(t, M[3]), j->cam);
    v = SEL2(v, XFM(SPLAT(v, 2), SPLAT(j->K, 0)));
    const u128 D = SPLAT(x_hsum(spec, acc, XFM(v, v)), 0);
    return GT0(XFM(SPLW(R), SPLW(R)), D);
}
static unsigned long long s_pre_slow, s_lod_slow;

/* 0x4030 aabb_visible lean path (L18-165): 1 = visible, 0 = culled, -1 = not modelled (the lifted test decides) */
static int nat_visible(J* j, const u128 mnL, const u128 mxL, const u128* M)
{
    u128 mn, mx;
    {
        spu_spec_acc av = SPU_SPEC_INIT();
        const int r = x_vis_pre(1, &av, j, mnL, mxL, M, &mn, &mx);
        if (__builtin_expect(SPU_SPEC_OK(av), 1)) {
            if (r != 3) return r;
            goto planes;
        }
        __atomic_add_fetch(&s_pre_slow, 1, __ATOMIC_RELAXED);
    }
    nat_xform(mnL, mxL, M, &mn, &mx);
    const u128 K = j->K;
    if (j->ls[j->buf1 + 0x81] != 0) {
        const u128 thr = SPLAT(K, 3);
        if (GT0(thr, SPLAT(mx, 2))) return 0;
        const u128 z = GT0(thr, SPLAT(mn, 2)) ? thr : SPLAT(mn, 2);
        mn = SEL2(mn, z);
    }
    const u128 d = spu_fs(mx, mn);
    const u128 S2 = SPLAT(HSUM(spu_fm(d, d)), 0);
    if (GT0(ROT(K, 1), S2)) return 0;
    const u128 c2 = spu_fa(mn, mx);
    const u128 K2 = ROT(K, 2);
    if (GT0(K2, SPLW(0))) {
        u128 v = spu_fms(c2, SPLW(F_HALF), j->cam);
        v = SEL2(v, spu_fm(SPLAT(v, 2), SPLAT(K, 0)));
        const u128 D = SPLAT(HSUM(spu_fm(v, v)), 0);
        if (GT0(spu_fm(D, K2), S2)) return 0;
    }
planes:
    if (__builtin_expect(s_fastplanes < 0, 0)) { const char* e = getenv("J210_FASTPLANES"); s_fastplanes = !(e && e[0] == '0'); }
    int r = s_fastplanes ? nat_planes_fast(j, mn, mx) : 2;
    if (__builtin_expect(r == 2, 0)) {
        if (s_fastplanes) __atomic_add_fetch(&s_planes_slow, 1, __ATOMIC_RELAXED);
        r = nat_planes(j, mn, mx);
    }
    return r < 0 ? -1 : (r & 0xFF) != 0;
}

/* 0x5E20 lod_inside (L4580-4668): 1 = inside */
static int nat_lod(J* j, uint32_t idx, const u128 p)
{
    if (!((j->lod & 255u) > (idx & 255u))) return 1;
    const uint32_t R = R32(j->ls + ((j->buf1 + 0x54 + 4u * (idx & 255u)) & SPU_LS_MASK));
    {
        spu_spec_acc av = SPU_SPEC_INIT();
        const int r = x_lod(1, &av, j, R, p);
        if (__builtin_expect(SPU_SPEC_OK(av), 1)) return r;
        __atomic_add_fetch(&s_lod_slow, 1, __ATOMIC_RELAXED);
    }
    const u128* M = j->M;
    const u128 t = spu_fma(M[2], SPLAT(p, 2), spu_fma(M[1], SPLAT(p, 1), spu_fm(M[0], SPLAT(p, 0))));
    u128 v = spu_fs(spu_fa(t, M[3]), j->cam);
    v = SEL2(v, spu_fm(SPLAT(v, 2), SPLAT(j->K, 0)));
    const u128 D = SPLAT(HSUM(spu_fm(v, v)), 0);
    return GT0(spu_fm(SPLW(R), SPLW(R)), D);
}

/* the tests as the walker calls them: M (or N) as rows, mlsa where the lifted test finds it */
static int aabb_visible(J* j, const uint8_t* p, const u128* Mrows, uint32_t mlsa)
{
    if (s_math == 1) {
        u128 mn, mx; decode_aabb(p, &mn, &mx);
        const int v = nat_visible(j, mn, mx, Mrows);
        if (__builtin_expect(v >= 0, 1)) return v;
        __atomic_add_fetch(&s_vis_deep, 1, __ATOMIC_RELAXED);
        put_matrix(j, mlsa, Mrows);                                   /* a plane tree deeper than 64: the lifted test */
        return aabb_visible_lifted(j, p, mlsa);
    }
    if (mlsa != j->F) put_matrix(j, mlsa, Mrows);
    const int l = aabb_visible_lifted(j, p, mlsa);
    if (s_math == 2) {
        u128 mn, mx; decode_aabb(p, &mn, &mx);
        const int v = nat_visible(j, mn, mx, Mrows);
        __atomic_add_fetch(&s_chk_vis, 1, __ATOMIC_RELAXED);
        if (v >= 0 && v != l && __atomic_add_fetch(&s_bad_vis, 1, __ATOMIC_RELAXED) <= 8)
            fprintf(stderr, "[j210] CHECK aabb: native %d lifted %d (rec %02X%02X%02X%02X %02X%02X%02X%02X)\n",
                    v, l, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }
    return l;
}

static int lod_inside(J* j, uint32_t idx, u128 pos)
{
    if (s_math == 1) return nat_lod(j, idx, pos);
    const int l = lod_inside_lifted(j, idx, pos);
    if (s_math == 2) {
        const int v = nat_lod(j, idx, pos);
        __atomic_add_fetch(&s_chk_lod, 1, __ATOMIC_RELAXED);
        if (v != l && __atomic_add_fetch(&s_bad_lod, 1, __ATOMIC_RELAXED) <= 8)
            fprintf(stderr, "[j210] CHECK lod: native %d lifted %d (idx %u lod %u pos %08X %08X %08X)\n",
                    v, l, idx, j->lod, pos._u32[0], pos._u32[1], pos._u32[2]);
    }
    return l;
}

/* ---------------- the stack (0x4278 push, 0x5EE8 pop + GET) ---------------- */
static void push(J* j, uint32_t size, uint32_t ea, uint32_t a, uint32_t b, const u128* m, uint32_t lod)
{
    size &= 0xFFFFu;
    if (size == 0) return;
    const uint32_t n = j->nstack;
    if ((int32_t)n > STACK_MAX - 1) return;                 /* silent drop, as L387/L395 */
    Ent* e = &j->stack[n];
    e->a = a; e->b = b; e->ea = ea; e->size = (uint16_t)size; e->lod = (uint8_t)lod;
    memcpy(e->m, m, sizeof e->m);
    j->nstack = n + 1;
}

static int pop_get(J* j, Slot* s)
{
    if (j->nstack == 0) return 0;
    const Ent* e = &j->stack[--j->nstack];
    s->ea = e->ea; s->size = e->size; s->lod = e->lod; s->a = e->a; s->b = e->b;
    memcpy(s->m, e->m, sizeof s->m);
    const uint32_t lsa = s->landing, eal = e->ea & ~15u, sz = ((e->ea & 15u) + e->size + 15u) & ~15u;
    if (sz > SLOT_BYTES) { j->fallback |= FB_SIZE; j->bad_size = sz; return 1; }
    if (!spu_native_ea_ok(eal, sz)) { if (!(j->fallback & FB_EA)) { j->bad_ea = eal; j->bad_size = sz; } j->fallback |= FB_EA; return 1; }
    ev_add(j, EV_GET, lsa, eal, sz, s->tag);
    return 1;
}

/* ---------------- records ---------------- */
static void emit(J* j, uint8_t key, uint32_t w8, uint32_t w12)
{
    if (key >= 248) { j->fallback |= FB_KEY; return; }
    if (j->nrec - j->batch0 + 1 > BATCH) {                  /* the 1793rd: flush the 1792 first (L3540-3545) */
        ev_add(j, EV_FLUSH, j->batch0, j->nrec, 0, 0);
        j->batch0 = j->nrec;
    }
    if (j->nrec == j->caprec) {
        uint32_t cap = j->caprec ? j->caprec * 2 : 4096;
        Rec* n = (Rec*)realloc(j->recs, (size_t)cap * sizeof(Rec));
        if (!n) { j->fallback |= FB_MEM; return; }
        j->recs = n; j->caprec = cap;
    }
    Rec* r = &j->recs[j->nrec++]; r->key = key; r->w8 = w8; r->w12 = w12;
}

/* one record at cur (end: the block's end); returns the next cursor, or NULL to decline */
static const uint8_t* record(J* j, const uint8_t* cur, const uint8_t* end)
{
    static const uint8_t need[21] = { 4, 12, 16, 12, 16, 24, 24, 8, 8, 4, 24, 72, 20, 28, 28, 12, 8, 16, 8, 24, 8 };
    const uint8_t t = cur[0];
    if (t > 20) { j->fallback |= FB_TYPE; j->bad_type = t; return NULL; }
    if (cur + need[t] > end) { j->fallback |= FB_TAIL; return NULL; }   /* the lifted code would read stale LS past the block */
    const uint32_t lod = j->lod, idx = cur[1], minlod = idx < lod ? idx : lod;
    switch (t) {
    case 0: {
        const uint32_t s = R16(cur + 2);
        if (!s) { j->fallback |= FB_ZERO; return NULL; }
        return cur + s;
    }
    case 1:
        if (aabb_visible(j, cur + 4, j->M, j->F)) return cur + 12;
        return cur + 12 + R16(cur + 2);
    case 2:
        if (aabb_visible(j, cur + 4, j->M, j->F)) push(j, R16(cur + 2), R32(cur + 12), j->a, j->b, j->M, lod);
        return cur + 16;
    case 3:
        if (lod_inside(j, idx, pos_i16(cur + 2))) return cur + 12;
        return cur + 12 + R16(cur + 8);
    case 4:
        if (lod_inside(j, idx, pos_i16(cur + 2))) push(j, R16(cur + 8), R32(cur + 12), j->a, j->b, j->M, minlod);
        return cur + 16;
    case 5:
        if (lod_inside(j, idx, pos_i16(cur + 2))) push(j, R16(cur + 8), R32(cur + 12), j->a, j->b, j->M, minlod);
        else push(j, R16(cur + 20), R32(cur + 16), j->a, j->b, j->M, lod);
        return cur + 24;
    case 7:
        emit(j, cur[1], j->a + 16u * R16(cur + 2), j->b);
        return cur + 8;
    case 11: {
        /* 0x4F10 lean (L3104-3221): N = local 3x4 (m0..m11 at +16) x M */
        const u128 *P = j->M;
        u128 m[12];
        for (int i = 0; i < 12; i++) m[i] = SPLW(R32(cur + 16 + 4 * i));
        u128 N[4];
        N[0] = spu_fma(P[2], m[2], spu_fma(P[1], m[1], spu_fm(P[0], m[0])));
        N[1] = spu_fma(P[2], m[5], spu_fma(P[1], m[4], spu_fm(P[0], m[3])));
        N[2] = spu_fma(P[2], m[8], spu_fma(P[1], m[7], spu_fm(P[0], m[6])));
        N[3] = spu_fa(spu_fma(P[2], m[11], spu_fma(P[1], m[10], spu_fm(P[0], m[9]))), P[3]);
        if (cur[1] != 0 && !aabb_visible(j, cur + 64, N, j->F + 0x40)) return cur + 72;
        push(j, R16(cur + 2), R32(cur + 4), R32(cur + 8), R32(cur + 12), N, lod);
        return cur + 72;
    }
    case 12: {
        const u128 p = V4(R32(cur + 8), R32(cur + 12), R32(cur + 16), R32(cur + 16));
        if (lod_inside(j, idx, p)) push(j, R16(cur + 2), R32(cur + 4), j->a, j->b, j->M, minlod);
        return cur + 20;
    }
    case 17: {
        const u128 p = V4(R32(cur + 4), R32(cur + 8), R32(cur + 12), R32(cur + 12));
        if (lod_inside(j, idx, p)) return cur + 16;
        return cur + 16 + R16(cur + 2);
    }
    case 18:
        if (R32(cur + 4)) push(j, (uint32_t)cur[1] * cur[3] + 8u, R32(cur + 4), j->a, j->b, j->M, 7);
        return cur + 8;
    default:
        j->fallback |= FB_TYPE; j->bad_type = t;
        return NULL;
    }
}

static int walk(J* j)
{
    static const u128 ident[4] = { { ._u32 = { 0x3F800000u, 0, 0, 0 } }, { ._u32 = { 0, 0x3F800000u, 0, 0 } },
                                   { ._u32 = { 0, 0, 0x3F800000u, 0 } }, { ._u32 = { 0, 0, 0, 0x3F800000u } } };
    memcpy(j->M, ident, sizeof j->M);
    j->a = j->b = 0; j->lod = 7;
    j->nstack = 0; j->nrec = 0; j->batch0 = 0; j->nev = 0;
    j->S[0].landing = j->buf1 + 0x4000u; j->S[0].tag = j->t1;
    j->S[1].landing = j->buf1 + 0x8000u; j->S[1].tag = j->tagA;
    if (s_math != 1) put_matrix(j, j->F, j->M);
    const uint32_t L = R32(j->ls + j->buf1 + 0x34);
    if ((uint64_t)j->stream + L > SPU_LS_SIZE) { j->fallback |= FB_SIZE; return 0; }
    const uint8_t* cur = j->ls + j->stream;
    const uint8_t* end = cur + L;
    int pending = 0, cur_slot = 1;
    for (;;) {
        while (end > cur) {
            cur = record(j, cur, end);
            if (!cur || j->fallback) return 0;
        }
        if (!pending) { if (!pop_get(j, &j->S[cur_slot])) break; }
        const int other = 1 - cur_slot;
        pending = pop_get(j, &j->S[other]);
        if (j->fallback) return 0;
        const Slot* s = &j->S[cur_slot];
        j->a = s->a; j->b = s->b; j->lod = s->lod;
        memcpy(j->M, s->m, sizeof j->M);
        if (s_math != 1) put_matrix(j, j->F, j->M);
        ev_add(j, EV_WAIT, 1u << (s->tag & 31), 0, 0, 0);
        cur = vm_base + s->ea;
        end = cur + s->size;
        cur_slot = other;
    }
    if (j->nrec > j->batch0) ev_add(j, EV_FLUSH, j->batch0, j->nrec, 1, 0);
    return !j->fallback;
}

/* ---------------- phase 2 ---------------- */
/* Channel operations: in a replay the unhooked forms (spu_*_raw mark the operation as not part of the
 * recording -- they need the replay's per-thread state, so never outside a replay); in the game the
 * ordinary channel functions, as the lifted code issues them. */
static void latch(J* j, int replay, uint32_t ch, uint32_t v)
{
    if (replay) { spu_wrch_raw(j->ctx, ch, v); return; }
    u128 x; memset(&x, 0, sizeof x); x._u32[0] = v;
    spu_wrch(j->ctx, ch, x);
}

static uint32_t rdch(J* j, int replay, uint32_t ch)
{
    if (replay) return spu_rdch_raw(j->ctx, ch);
    return spu_rdch(j->ctx, ch)._u32[0];
}

static void tag_wait(J* j, int replay, uint32_t mask)
{
    latch(j, replay, MFC_WrTagMask, mask);
    latch(j, replay, MFC_WrTagUpdate, 1);
    (void)rdch(j, replay, MFC_RdTagStat);
}

/* 0x6050 counting sort of records [r0, r1) into H = buf2+0x180 (table at +16) and OUT = buf2+0x380 */
static void sort_batch(J* j, uint32_t r0, uint32_t r1)
{
    uint8_t* H = j->ls + j->buf2 + 0x180;
    uint8_t* OUT = j->ls + j->buf2 + 0x380;
    uint16_t hist[248];
    memset(hist, 0, sizeof hist);
    for (uint32_t i = r0; i < r1; i++) hist[j->recs[i].key]++;
    uint16_t s = 0;
    for (int k = 0; k < 248; k++) { P16(H + 16 + 2 * k, s); s = (uint16_t)(s + hist[k]); }
    for (uint32_t i = r0; i < r1; i++) {
        const Rec* r = &j->recs[i];
        uint8_t* h = H + 16 + 2 * r->key;
        const uint16_t p = R16(h);
        P16(h, (uint16_t)(p + 1));
        P32(OUT + 8u * p, r->w8);
        P32(OUT + 8u * p + 4, r->w12);
    }
}

static void flush(J* j, int replay, uint32_t r0, uint32_t r1, int final)
{
    spu_context* ctx = j->ctx;
    uint8_t* ls = j->ls;
    const uint32_t n = r1 - r0;
    tag_wait(j, replay, 1u << (j->t2 & 31));
    /* buffer 3 holds the batch as the lifted code wrote it (key, bytes 1..7 untouched, w8, w12) */
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* p = ls + ((j->buf3 + 16u * i) & SPU_LS_MASK);
        p[0] = j->recs[r0 + i].key; P32(p + 8, j->recs[r0 + i].w8); P32(p + 12, j->recs[r0 + i].w12);
    }
    sort_batch(j, r0, r1);
    const uint32_t hdr = spu_native_atomic_add32(ctx, R32(ls + j->buf1 + 0x44), 512);
    if (hdr + 512u > R32(ls + j->buf1 + 0x48) || hdr == 0) { (void)final; return; }   /* the batch is dropped */
    const uint32_t bytes = (n * 8u + 127u) & ~127u;
    uint32_t ent = spu_native_atomic_add32(ctx, R32(ls + j->buf1 + 0x4C), bytes);
    if (ent + bytes > R32(ls + j->buf1 + 0x50)) ent = 0;
    if (ent == 0) memset(ls + j->buf2 + 0x190, 0, 496);
    P32(ls + j->buf2 + 0x188, ent);
    /* the header PUT (0x5B60): LSA buf2+0x180, EA hdr, 512 bytes, tag t2 */
    latch(j, replay, MFC_LSA, j->buf2 + 0x180); latch(j, replay, MFC_EAL, hdr);
    latch(j, replay, MFC_Size, 512); latch(j, replay, MFC_TagID, j->t2);
    mfc_do_transfer_raw(ctx, j->buf2 + 0x180, hdr, 512, 0x20);
    if (ent) {
        /* the entry PUTL (0x6500): list at buf2+0xE0, one element per 16 KB */
        uint32_t rem = bytes, ea = ent, k = 0, lsa = j->buf2 + 0x380;
        while (rem) {
            const uint32_t c = rem > 16384u ? 16384u : rem;
            P32(ls + j->buf2 + 0xE0 + 8 * k, c); P32(ls + j->buf2 + 0xE0 + 8 * k + 4, ea);
            mfc_do_transfer_raw(ctx, lsa, ea, c, 0x20);
            ea += c; lsa += c; rem -= c; k++;
        }
        latch(j, replay, MFC_LSA, j->buf2 + 0x380); latch(j, replay, MFC_EAL, j->buf2 + 0xE0);
        latch(j, replay, MFC_Size, 8 * k); latch(j, replay, MFC_TagID, j->t2);
    }
}

static void commit(J* j, int replay)
{
    spu_context* ctx = j->ctx;
    uint8_t* ls = j->ls;
    memset(ls + j->buf2, 0, 384);
    const uint32_t d0 = replay ? spu_native_replay_dec(ctx, 1) : rdch(j, 0, SPU_RdDec);
    P32(ls + j->buf2 + 0x180, R32(ls + j->buf1 + 0x3C));
    P32(ls + j->buf2 + 0x184, R32(ls + j->buf1 + 0x40));
    ls[j->buf2 + 0x18C] = ls[j->buf1 + 0x80];
    P16(ls + j->buf2 + 0x18E, 0);
    for (uint32_t i = 0; i < j->nev; i++) {
        const Ev* e = &j->evs[i];
        if (e->kind == EV_GET) {
            latch(j, replay, MFC_LSA, e->x0); latch(j, replay, MFC_EAL, e->x1);
            latch(j, replay, MFC_Size, e->x2); latch(j, replay, MFC_TagID, e->x3);
        } else if (e->kind == EV_WAIT) {
            tag_wait(j, replay, e->x0);
        } else {
            flush(j, replay, e->x0, e->x1, (int)e->x2);
        }
    }
    const uint32_t d1 = replay ? spu_native_replay_dec(ctx, 2) : rdch(j, 0, SPU_RdDec);
    P32(ls + j->buf2, R32(ls + j->buf2) + (~d1 - (~d0 + 2u)));
}

static int j210_walker(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_J210");
        s_mode = e ? atoi(e) : 1;
        s_dbg = getenv("J210_DEBUG") != 0;
        s_stats = getenv("J210_STATS") != 0;
        { const char* m = getenv("J210_MATH"); s_math = m ? (atoi(m) != 0) : 1; }
        if (getenv("J210_CHECK")) s_math = 2;
        fprintf(stderr, "[j210] native culling-tree walker: %s\n", s_mode ? "on" : "off (PS3RECOMP_NATIVE_J210=0)");
    }
    if (!s_mode || (g_spu_jobrec_mode & SPU_JOBREC_RECORD)) return 0;
    if (!t_j) { t_j = (J*)calloc(1, sizeof(J)); if (!t_j) return 0; }
    J* j = t_j;
    j->ctx = ctx; j->ls = ctx->ls;
    j->buf1 = ctx->gpr[3]._u32[0] & SPU_LS_MASK; j->planes = ctx->gpr[4]._u32[0] & SPU_LS_MASK;
    j->stream = ctx->gpr[5]._u32[0] & SPU_LS_MASK; j->buf3 = ctx->gpr[6]._u32[0] & SPU_LS_MASK;
    j->buf2 = ctx->gpr[7]._u32[0] & SPU_LS_MASK; j->t2 = ctx->gpr[8]._u32[0];
    j->t1 = ctx->gpr[10]._u32[0]; j->tagA = ctx->gpr[11]._u32[0];
    j->fallback = 0; j->bad_type = 0;
    /* phase 1 passes the lifted tests their operands in r3..r9 and they clobber r2..r79: on a decline the
     * lifted walker must start from its own arguments, so the register file is kept */
    static _Thread_local u128 saved[128];
    memcpy(saved, ctx->gpr, sizeof saved);
    const u128 r0 = ctx->gpr[0], r1 = ctx->gpr[1];
    const uint32_t sp = r1._u32[0] & SPU_LS_MASK;
    /* far below everything the lifted walker and its callees touch (walker frame 928 B, then 0x4030 112 B +
     * 0x6220 288 B or 0x6050 592 B): on a decline the lifted walker must find its frame region as it was --
     * its profiler (0x4C08) reads one never-written word of it (sp'+0x3C) */
    j->F = (sp - 0x1000u) & ~15u;
    /* the lifted callees' frames go below the scratch; 0x4030 stores its link at its caller's sp+0x10
     * (the walker's own frame does the same), so that word must not be part of the staging */
    j->r1 = spu_ai(r1, -0x1040);
    j->cam = LQ(j->ls + j->buf1);
    j->K = LQ(j->ls + j->buf1 + 0x70);
    SQ(j->ls + j->F + 0xA0, j->cam);
    const int replay = spu_native_replay_begin(ctx);
    const int ok = walk(j);
    ctx->gpr[0] = r0; ctx->gpr[1] = r1;
    if (replay) {   /* both ways the stack below the caller is dead (the native's staging, the callees' frames) */
        spu_native_dead_ls(sp - 0x1800u, sp);
    }
    if (s_stats) {
        const unsigned long long c = __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);
        if (!ok) {
            __atomic_add_fetch(&s_declined, 1, __ATOMIC_RELAXED);
            for (int i = 0; i < 8; i++) if (j->fallback >> i & 1) __atomic_add_fetch(&s_why[i], 1, __ATOMIC_RELAXED);
        }
        if (c % 5000 == 0)
            fprintf(stderr, "[j210] %llu calls, %llu declined (type %llu, ea %llu, size %llu, key %llu, tail %llu, zero %llu);"
                    " check: aabb %llu/%llu lod %llu/%llu differ, deep %llu, planes slow %llu, pre slow %llu, lod slow %llu\n",
                    c, s_declined, s_why[0], s_why[1], s_why[2], s_why[3], s_why[4], s_why[5],
                    s_bad_vis, s_chk_vis, s_bad_lod, s_chk_lod, s_vis_deep, s_planes_slow, s_pre_slow, s_lod_slow);
    }
    if (!ok) {
        memcpy(ctx->gpr, saved, sizeof saved);
        if (s_dbg) {
            static int nl;
            if (nl++ < 40) fprintf(stderr, "[j210] declined: reasons 0x%X type %u ea 0x%08X size 0x%X\n",
                                   j->fallback, j->bad_type, j->bad_ea, j->bad_size);
        }
        return 0;
    }
    commit(j, replay);
    if (replay) {
        spu_native_dead_ls(0x6D60, 0xE620);                         /* the BSS: zeroed by the wrapper every call */
        spu_native_dead_ls(j->buf1 + 0x4000u, j->buf1 + 0xC000u);   /* the two DMA landing buffers */
        spu_native_dead_ls(0x80, 0x100);                            /* the atomic line */
        spu_native_replay_end_dec(ctx, 1, 2);
    }
    return 1;
}

__attribute__((constructor)) static void j210_register(void)
{
    spu_native_register(J210_IMAGE, J210_WALKER, j210_walker);
}
