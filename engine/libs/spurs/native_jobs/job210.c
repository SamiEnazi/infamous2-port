/* job210.c -- job module 210 (jobmod_867e5d2348b3_e2898), the per-view scene walker / visibility culler, natively.
 *
 * One job per view of a frame (main camera, shadow views, a reflection-like view with a height clip). It walks a
 * tree of command streams in guest memory: group nodes with quantized boxes, LOD switches, instances with their
 * own 3x4 transform, draw items. Every box is culled (cull_core.h); every LOD switch tests the distance to the
 * camera against one of seven radii. The visible draw items, each (batch EA + 16 x index, instance EA), are
 * bucket-sorted on a byte key and written as a 512-byte header + list for 204 (the render-list builder).
 *
 * The same job as ThiagoFraga761/infamous2-pc's bc11 (overlay 217 of the US disc; work/port/jobbc11_native.c,
 * MIT, Copyright (c) 2026 the infamous2-recomp contributors; rpi/job-bc11-native/RESEARCH.md is the spec): our
 * module is the same code at the same addresses (worker 0x4358, wrapper 0x5BC8, BSP 0x6220, the opcode table at
 * LS 0x6BE0). His model replaced the SPU worker and kept the local store; this one is the whole job on guest
 * memory (no SPU at all), checked against this port's recordings (spu_jobrec.c native pass).
 *
 * Inputs (the descriptor): set 1 = a DMA list of three elements: the 144-byte header, the visibility BSP
 * (32 bytes x count), the root command stream; sets 2 and 3 = the job's output and item scratch (local store
 * only). The worker assumes buffer 1 holds them back to back (header at +0, BSP at +0x90, root at
 * +0x90 + 32 x count) and its two 16 KB child-stream slots at +0x4000 / +0x8000: anything else declines.
 * Child streams are read where the job GETs them (EA & ~15, align16((EA & 15) + size), at most 16 KB).
 * Fallbacks (nothing written, the lifted job runs): cull_core.h's CF_* plus OP (an opcode above 20), SKIP0
 * (op 0 of size 0: the job would loop), ALIGN (a record not 4-aligned: the job's quadword loads wrap). */
#include "cull_core.h"
#include <stdio.h>

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,fma,sse4.1,bmi,bmi2,popcnt"))), apply_to = function)
#endif

extern int g_spu_edge_vp_on;

#define QMAX 384
#ifdef J210_PROFILE
#include <intrin.h>
extern unsigned long long g_j210_t[8];
#define PT(k, expr) ({ const unsigned long long _t0 = __rdtsc(); __typeof__(expr) _v = (expr); g_j210_t[k] += __rdtsc() - _t0; _v; })
#else
#define PT(k, expr) (expr)
#endif                  /* 0x4278: the queue refuses a push at count > 383 */

enum { FB_OP = CF_MODULE, FB_SKIP0 = CF_MODULE << 1, FB_ALIGN = CF_MODULE << 2 };
static const char* const s_reasons[] = { CULL_REASONS_COMMON, "OP", "SKIP0", "ALIGN", NULL };

typedef struct { uint32_t ctx0, ctx1, ea; uint16_t size; uint8_t lod; X4 xf; } QE;
typedef struct { X4 xf; uint32_t ctx0, ctx1, size; uint8_t lod; const uint8_t* p; const uint8_t* lim; } Slot;

typedef struct J {
    wws_njob* nj;
    uint32_t fallback;
    const uint8_t* hdr;
    cull_view V;
    F4 cam;
    uint32_t hscale;
    X4 C;                         /* current transform */
    uint32_t ctx0, ctx1;          /* item base EA, instance EA */
    QE q[QMAX]; uint32_t qn;
    Slot slot[2];
    cull_out out;
} J;

static void fb(J* j, uint32_t why, uint32_t ea, uint32_t size)
{
    if (!j->fallback) { j->nj->bad_ea = ea; j->nj->bad_size = size; }
    j->fallback |= why;
}

/* 0x66F8: quantized local box. byte 0 = center shift, bytes 1-3 signed center, byte 4 = extent shift,
 * bytes 5-7 unsigned extent */
static inline void box_q(const uint8_t* p, F4* mn, F4* mx)
{
    const uint32_t s0 = p[0], s1 = p[4];
    const int32_t c1 = (int8_t)p[1], c2 = (int8_t)p[2], c3 = (int8_t)p[3];
    const int32_t e1 = p[5], e2 = p[6], e3 = p[7];
    const F4 c = FCVT(SHL(c1, s0), SHL(c2, s0), SHL(c3, s0), SHL(c3, s0));
    const F4 e = FCVT(SHL(e1, s1), SHL(e2, s1), SHL(e3, s1), SHL(e3, s1));
    *mx = FADD(c, e); *mn = FSUB(c, e);
}

/* 0x5E20: LOD band b. Out of band when lod > b and the distance (height scaled by hdr+0x70) reaches the
 * band's radius hdr+0x54[b] */
static int lod_in(J* j, uint32_t b, uint32_t lod, F4 c, const X4* X)
{
    if (!((lod & 255) > (b & 255))) return 1;
    F4 w = FMUL(X->x[0], FSPL(c, 0));
    w = FMA_(X->x[1], FSPL(c, 1), w);
    w = FMA_(X->x[2], FSPL(c, 2), w);
    w = FADD(w, X->x[3]);
    F4 d = FSUB(w, j->cam);
    const uint32_t tw = R32(j->hdr + 0x54 + ((b & 255) << 2));
    if ((tw & 0x7F800000u) == 0x7F800000u) { j->fallback |= CF_FLOAT; return 0; }
    d = FSET(d, 2, FLANE(FMUL(FSPL(d, 2), F4W(j->hscale, 0, 0, 0)), 0));
    const F4 dist = HSUM3(FMUL(d, d));
    /* |t| >= 2^64: t*t is at least 2^128 on the SPU, above every distance the host computed without overflowing;
     * one that overflowed raised the flag that declines, so dist must be computed: the test on its bits keeps it */
    if ((tw & 0x7F800000u) >= (191u << 23)) return FLANE(dist, 0) != 0xFFFFFFFFu;
    const F4 t = F4W(tw, tw, tw, tw);
    return GT0(FMUL(t, t), FSPL(dist, 0));
}

/* op 19 (0x5258): the center within the radius (+8)? lod_in's distance without the band precondition */
static int sphere_in(J* j, uint32_t rw, F4 c, const X4* X)
{
    F4 w = FMUL(X->x[0], FSPL(c, 0));
    w = FMA_(X->x[1], FSPL(c, 1), w);
    w = FMA_(X->x[2], FSPL(c, 2), w);
    w = FADD(w, X->x[3]);
    F4 d = FSUB(w, j->cam);
    d = FSET(d, 2, FLANE(FMUL(FSPL(d, 2), F4W(j->hscale, 0, 0, 0)), 0));
    const F4 dist = HSUM3(FMUL(d, d));
    if ((rw & 0x7F800000u) >= (191u << 23)) return FLANE(dist, 0) != 0xFFFFFFFFu;
    const F4 t = F4W(rw, rw, rw, rw);
    return GT0(FMUL(t, t), FSPL(dist, 0));
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
/* op 10 (0x5570): a plane in the node's local space (+8: n, d) to world space with the adjugate of the current
 * transform over its determinant, normalized, tested against the camera; with header +0x38 bit 5 set, a camera
 * behind it culls the child. frest / fi / frsqest leave no room for host floats: the SPU's own helpers (the
 * exact forms of runtime/spu/spu_helpers.h), as literally as the listing, under the default MXCSR.
 * (Thiago's op10_visible.) */
#include "../../../runtime/spu/spu_helpers.h"
static u128 U4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static u128 F2U(F4 v) { u128 u; _mm_storeu_si128((__m128i*)u._u32, _mm_castps_si128(v)); return u; }
static u128 PRM(u128 v) { return U4(v._u32[1], v._u32[2], v._u32[0], v._u32[0]); }     /* the 0x6D00 shuffle */
static u128 ROT1(u128 v) { return U4(v._u32[1], v._u32[2], v._u32[3], v._u32[0]); }
static u128 ROT2(u128 v) { return U4(v._u32[2], v._u32[3], v._u32[0], v._u32[1]); }
static u128 SPLW(u128 v, int i) { return U4(v._u32[i], v._u32[i], v._u32[i], v._u32[i]); }
static u128 ULANE(u128 v, int i, uint32_t x) { v._u32[i] = x; return v; }
static int op10_visible(const X4* Cx, F4 camf, const uint8_t* r, uint32_t hdr38, unsigned csr_helpers)
{
    const unsigned in = _mm_getcsr();
    _mm_setcsr(csr_helpers);                         /* the exact helpers assume round to nearest, no FTZ / DAZ */
    const u128 C0 = F2U(Cx->x[0]), C1 = F2U(Cx->x[1]), C2 = F2U(Cx->x[2]), C3 = F2U(Cx->x[3]);
    const u128 one = U4(F1, F1, F1, F1), m3 = U4(~0u, ~0u, ~0u, 0), sgn = U4(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u);
    const u128 c1p = PRM(C1), c0p = PRM(C0), c2p = PRM(C2);
    const u128 r25 = spu_fm(C0, c1p), r13 = spu_fm(C2, c0p), r18 = spu_fm(C1, c2p);
    const u128 r78 = spu_xor(C3, sgn);
    const u128 r77 = spu_fnms(C1, c0p, r25), r76 = spu_fnms(C0, c2p, r13), r74 = spu_fnms(C2, c1p, r18);
    const u128 r92 = PRM(r77), r67 = PRM(r76), r71 = PRM(r74);
    const u128 r61 = spu_fm(r92, C2);
    const u128 r5  = U4(r71._u32[0], r67._u32[0], r92._u32[0], 0);
    const u128 r36 = U4(r71._u32[1], r67._u32[1], r92._u32[1], 0);
    const u128 r26 = U4(r71._u32[2], r67._u32[2], r92._u32[2], 0);
    const u128 det = spu_fa(spu_fa(r61, ROT1(r61)), ROT2(r61));
    u128 r27 = spu_fm(r5, SPLW(r78, 0));
    r27 = spu_fma(r36, SPLW(r78, 1), r27);
    r27 = spu_fma(r26, SPLW(r78, 2), r27);
    const u128 r32 = SPLW(det, 0);
    const u128 r31 = spu_fi(r32, spu_frest(r32));
    const u128 r28 = spu_fnms(r31, r32, one);
    const u128 inv = spu_and(spu_fma(r28, r31, r31), m3);
    const u128 r20 = spu_and(spu_fm(r36, inv), m3);
    const u128 r13b = ULANE(spu_fma(r27, inv, U4(0, 0, 0, F1)), 3, F1);
    const u128 r14 = spu_and(spu_fm(r5, inv), m3), r23 = spu_and(spu_fm(r26, inv), m3);
    const u128 r74b = U4(r14._u32[0], r20._u32[0], r23._u32[0], r13b._u32[0]);
    const u128 r92b = U4(r14._u32[1], r20._u32[1], r23._u32[1], r13b._u32[1]);
    const u128 r69 = U4(r14._u32[2], r20._u32[2], r23._u32[2], r13b._u32[2]);
    /* the record's plane: +8 nx, +12 ny, +16 nz (FW(r+16, r): the quad at r+16 rotated by r: aligned, the word), +20 d */
    const uint32_t w8 = R32(r + 8), w12 = R32(r + 12), w16 = R32(r + 16), w20 = R32(r + 20);
    u128 r58 = spu_fm(r74b, U4(w8, w8, w8, w8));
    r58 = spu_fma(r92b, U4(w12, w12, w12, w12), r58);
    r58 = spu_fma(r69, U4(w16, w16, w16, w16), r58);
    const u128 r66 = ULANE(C3, 3, F1);
    const u128 r64 = spu_fm(r66, spu_and(r58, m3));
    const u128 r61b = spu_fa(spu_fa(r64, ROT1(r64)), ROT2(r64));
    const u128 r80 = spu_fa(U4(w20, w20, w20, w20), SPLW(r61b, 0));
    const u128 r15 = ULANE(r58, 3, r80._u32[3]);
    const u128 r56 = spu_fm(r15, r15);
    const u128 r22 = SPLW(spu_fa(spu_fa(r56, ROT1(r56)), ROT2(r56)), 0);
    const u128 r19 = spu_fcgt(r22, U4(0x0DA2425Fu, 0x0DA2425Fu, 0x0DA2425Fu, 0x0DA2425Fu));
    const u128 r17 = spu_fi(r22, spu_frsqest(r22));
    const u128 r21 = spu_fm(r17, r17), r49 = spu_fm(r17, U4(FH, FH, FH, FH));
    const u128 r16 = spu_fnms(r22, r21, one);
    const u128 r48 = spu_fma(r16, r49, r17);
    const u128 r46 = spu_and(r19, spu_fm(r15, r48));
    const u128 r44 = spu_selb(r15, r46, r19);
    const u128 r38 = spu_fm(r44, F2U(camf));
    const u128 r30 = SPLW(spu_fa(spu_fa(r38, ROT1(r38)), ROT2(r38)), 0);
    const u128 r31b = spu_fs(r30, SPLW(r44, 3));
    const int behind = spu_fcgt(U4(0, 0, 0, 0), r31b)._u32[0] != 0;
    _mm_setcsr(in);
    return !(behind && (hdr38 & 32));
}
#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,fma,sse4.1,bmi,bmi2,popcnt"))), apply_to = function)
#endif

/* ---- the queue (0x4278 push, 0x5EE8 pop and GET) ------------------------------------------------- */
static inline void push(J* j, uint32_t size, uint32_t ea, uint32_t c0, uint32_t c1, const X4* X, uint32_t lod)
{
    if (!size || j->qn > QMAX - 1) return;
    QE* e = &j->q[j->qn++];
    e->ctx0 = c0; e->ctx1 = c1; e->ea = ea; e->size = (uint16_t)size; e->lod = (uint8_t)lod; e->xf = *X;
}
/* pop into slot k: the GET of the child stream (where the job's GET puts it: the stream starts at EA & 15) */
static int pop(J* j, int k)
{
    if (!j->qn) return 0;
    const QE* e = &j->q[--j->qn];
    Slot* s = &j->slot[k];
    s->xf = e->xf; s->ctx0 = e->ctx0; s->ctx1 = e->ctx1; s->size = e->size; s->lod = e->lod;
    const uint32_t ea = e->ea & ~15u, sz = ((e->ea & 15) + e->size + 15) & ~15u;
    s->p = s->lim = NULL;
    if (sz > 0x4000u) { fb(j, CF_GET, ea, sz); return 1; }   /* the MFC would refuse it */
    if (!sz) return 1;
    const uint8_t* g = j->nj->read(j->nj, ea, sz);
    if (!g) { fb(j, CF_EA, ea, sz); return 1; }
    /* a 128-byte GET that starts like Edge's viewport block is rewritten by the MFC under resolution scaling */
    static const uint8_t edge_sig[16] = { 0, 0, 0, 0, 0x05, 0x00, 0x02, 0xD0, 0xBF, 0x80, 0, 0, 0x3F, 0x80, 0, 0 };
    if (sz == 0x80 && __atomic_load_n(&g_spu_edge_vp_on, __ATOMIC_RELAXED) && !memcmp(g, edge_sig, 16)) { fb(j, CF_GET, ea, sz); return 1; }
    s->p = g + (e->ea & 15);
    s->lim = g + sz;
    return 1;
}

static inline void emit(J* j, const uint8_t* r)
{
    cull_out* o = &j->out;
    if (o->n + 1 > CULL_ITEMS) { cull_close_batch(o, 0, &j->fallback); if (j->fallback) return; }
    o->ib[o->n] = r[1];
    o->iw2[o->n] = j->ctx0 + (R16(r + 2) << 4);
    o->iw3[o->n] = j->ctx1;
    o->n++;
}

/* op 6 / op 14: the 256-bit group mask at hdr+0x10, bit (idx & 127) of quad idx >> 7 */
static inline int mask_bit(const J* j, uint32_t mb)
{
    const uint32_t idx = mb & 127;
    const uint8_t* mq = j->hdr + 0x10 + 16 * (mb >> 7);
    return (mq[15 - (idx >> 3)] >> (idx & 7)) & 1;
}

static inline F4 c16x4(const uint8_t* r)   /* int16 center x4: the record's +2..+7 * 4.0 */
{
    const int32_t x = (int16_t)R16(r + 2), y = (int16_t)R16(r + 4), z = (int16_t)R16(r + 6);
    return FMUL(FCVT(x, y, z, 0), F4W(0x40800000u, 0x40800000u, 0x40800000u, 0x40800000u));
}

/* the bytes of a record each handler reads (the skips 8, 9 and 20 read only the opcode); a record that runs
 * past what the stream's GET brought in would read stale local store on the SPU: CF_RANGE */
static const uint8_t s_size[21] = { 4, 12, 16, 10, 16, 24, 24, 4, 1, 1, 24, 72, 20, 28, 28, 12, 8, 16, 8, 24, 1 };

/* ---- the walk (0x4358) ----------------------------------------------------------------------------- */
static void walk(J* j, const uint8_t* root, uint32_t rootlen, const uint8_t* rootlim)
{
    X4 I4;
    I4.x[0] = F4W(F1, 0, 0, 0); I4.x[1] = F4W(0, F1, 0, 0); I4.x[2] = F4W(0, 0, F1, 0); I4.x[3] = F4W(0, 0, 0, F1);
    j->slot[0].xf = j->slot[1].xf = I4;
    j->C = I4; j->ctx0 = j->ctx1 = 0;
    uint32_t lod = 7;
    const uint8_t* r = root;
    const uint8_t* end = root + rootlen;
    const uint8_t* lim = rootlim;
    int cur = 1, pre = 0;
    const uint32_t hdr38 = R32(j->hdr + 0x38);
    for (;;) {
        while (end > r) {
            if (j->fallback) return;
            const uint32_t op = r[0];
            if (op > 20) { j->nj->bad_op = op; j->fallback |= FB_OP; return; }   /* a table skip into .bss */
            if ((uintptr_t)r & 3) { j->fallback |= FB_ALIGN; return; }    /* the job's quadword loads would wrap */
            if ((size_t)(lim - r) < s_size[op]) { fb(j, CF_RANGE, 0, (uint32_t)(lim - r)); return; }
#ifdef J210_PROFILE
            { extern unsigned long long g_j210_ops[32]; g_j210_ops[op]++; }
#endif
            switch (op) {
            case 0: { const uint32_t h = R16(r + 2); if (!h) { j->fallback |= FB_SKIP0; return; } r += h; break; }
            case 1: case 2: {
                F4 mn, mx; box_q(r + 4, &mn, &mx);
                const int vis = PT(0, cull_box(&j->V, mn, mx, &j->C, &j->fallback));
                if (op == 1) r += vis ? 12 : 12 + R16(r + 2);
                else { if (vis) push(j, R16(r + 2), R32(r + 12), j->ctx0, j->ctx1, &j->C, lod); r += 16; }
                break;
            }
            case 3: {
                /* center x: FW(r, r+16) & 0xFFFF = the halfword at +2 */
                const F4 c = c16x4(r);
                if (lod_in(j, r[1], lod, c, &j->C)) r += 12;
                else r += 12 + R16(r + 8);
                break;
            }
            case 4: {   /* in band: push child (+8 size, +12 EA, min lod) */
                const F4 c = c16x4(r);
                const uint32_t b = r[1];
                if (lod_in(j, b, lod, c, &j->C)) { const uint32_t l = lod & 255; push(j, R16(r + 8), R32(r + 12), j->ctx0, j->ctx1, &j->C, l > b ? b : l); }
                r += 16;
                break;
            }
            case 6:     /* op 5 behind a group bit (+22); clear: the far child */
                if (!mask_bit(j, r[22])) { push(j, R16(r + 20), R32(r + 16), j->ctx0, j->ctx1, &j->C, lod); r += 24; break; }
                /* fall through */
            case 5: {   /* LOD switch on an int16 center x4: near (+8 size, +12 EA, min lod) or far (+20 size, +16 EA) */
                const F4 c = c16x4(r);
                const uint32_t b = r[1];
                if (lod_in(j, b, lod, c, &j->C)) { const uint32_t l = lod & 255; push(j, R16(r + 8), R32(r + 12), j->ctx0, j->ctx1, &j->C, l > b ? b : l); }
                else push(j, R16(r + 20), R32(r + 16), j->ctx0, j->ctx1, &j->C, lod);
                r += 24;
                break;
            }
            case 7: emit(j, r); r += 8; break;
            case 10:    /* plane facing test, then push child (+2 size, +4 EA) */
                if (op10_visible(&j->C, j->cam, r, hdr38, 0x1F80u)) push(j, R16(r + 2), R32(r + 4), j->ctx0, j->ctx1, &j->C, lod);
                r += 24;
                break;
            case 15: { const uint32_t ea = R32(r + 8); if (ea) push(j, 512, ea, j->ctx0, j->ctx1, &j->C, 7); r += 12; break; }
            case 16: push(j, R16(r + 2), R32(r + 4), j->ctx0, j->ctx1, &j->C, 7); r += 8; break;
            case 19: {  /* sphere switch: int16 center x4, radius +8; inside: +12 EA / +20 size, else +16 EA / +22 size */
                const F4 c = c16x4(r);
                const uint32_t rw = R32(r + 8);
                if ((rw & 0x7F800000u) == 0x7F800000u) { j->fallback |= CF_FLOAT; return; }
                if (sphere_in(j, rw, c, &j->C)) push(j, R16(r + 20), R32(r + 12), j->ctx0, j->ctx1, &j->C, lod);
                else push(j, R16(r + 22), R32(r + 16), j->ctx0, j->ctx1, &j->C, lod);
                r += 24;
                break;
            }
            case 8: r += 8; break;
            case 9: r += 4; break;
            case 11: {  /* instance: local 3x4 at +16..+63 composed with C (row-vector); byte 1 set: cull its box first */
                /* the local 3x4 (rows at +16, +28, +40, translation +52) as three quadwords: q0 = w16..w28,
                 * q1 = w32..w44, q2 = w48..w60: the same twelve words, splatted from where they sit */
                const F4 q0 = LQ(r + 16), q1 = LQ(r + 32), q2 = LQ(r + 48);
                if (FBAD(q0, 15) || FBAD(q1, 15) || FBAD(q2, 15)) { j->fallback |= CF_FLOAT; return; }
                const X4* Cx = &j->C; X4 M;
                M.x[3] = FADD(FMA_(Cx->x[2], FSPL(q2, 3), FMA_(Cx->x[1], FSPL(q2, 2), FMUL(Cx->x[0], FSPL(q2, 1)))), Cx->x[3]);
                M.x[0] = FMA_(Cx->x[2], FSPL(q0, 2), FMA_(Cx->x[1], FSPL(q0, 1), FMUL(Cx->x[0], FSPL(q0, 0))));
                M.x[1] = FMA_(Cx->x[2], FSPL(q1, 1), FMA_(Cx->x[1], FSPL(q1, 0), FMUL(Cx->x[0], FSPL(q0, 3))));
                M.x[2] = FMA_(Cx->x[2], FSPL(q2, 0), FMA_(Cx->x[1], FSPL(q1, 3), FMUL(Cx->x[0], FSPL(q1, 2))));
                int vis = 1;
                if (r[1]) { F4 mn, mx; box_q(r + 64, &mn, &mx); vis = PT(1, cull_box(&j->V, mn, mx, &M, &j->fallback)); }
                if (vis) push(j, R16(r + 2), R32(r + 4), R32(r + 8), R32(r + 12), &M, lod);
                r += 72;
                break;
            }
            case 12: {
                const F4 c = F4W(R32(r + 8), R32(r + 12), R32(r + 16), R32(r + 16));
                if (FBAD(c, 7)) { j->fallback |= CF_FLOAT; return; }
                const uint32_t b = r[1];
                if (lod_in(j, b, lod, c, &j->C)) { const uint32_t l = lod & 255; push(j, R16(r + 2), R32(r + 4), j->ctx0, j->ctx1, &j->C, l > b ? b : l); }
                r += 20;
                break;
            }
            case 14:
                if (!mask_bit(j, r[26])) { push(j, R16(r + 24), R32(r + 20), j->ctx0, j->ctx1, &j->C, lod); r += 28; break; }
                /* fall through */
            case 13: {
                const F4 c = F4W(R32(r + 8), R32(r + 12), R32(r + 16), R32(r + 16));
                if (FBAD(c, 7)) { j->fallback |= CF_FLOAT; return; }
                const uint32_t b = r[1];
                if (lod_in(j, b, lod, c, &j->C)) { const uint32_t l = lod & 255; push(j, R16(r + 2), R32(r + 4), j->ctx0, j->ctx1, &j->C, l > b ? b : l); }
                else push(j, R16(r + 24), R32(r + 20), j->ctx0, j->ctx1, &j->C, lod);
                r += 28;
                break;
            }
            case 17: {
                const F4 c = F4W(R32(r + 4), R32(r + 8), R32(r + 12), R32(r + 12));
                if (FBAD(c, 7)) { j->fallback |= CF_FLOAT; return; }
                if (lod_in(j, r[1], lod, c, &j->C)) r += 16;
                else r += 16 + R16(r + 2);
                break;
            }
            case 18: {
                const uint32_t ea = R32(r + 4);
                if (ea) push(j, (uint32_t)r[1] * r[3] + 8, ea, j->ctx0, j->ctx1, &j->C, 7);
                r += 8;
                break;
            }
            case 20: r += 8; break;
            }
        }
        if (j->fallback) return;
        /* 0x45D8: the stream is done; take the prefetched one, or pop now */
        if (!pre && !PT(2, pop(j, cur))) break;
        const int other = 1 - cur;
        pre = PT(2, pop(j, other));
        if (j->fallback) return;
        const Slot* s = &j->slot[cur];
        j->C = s->xf; j->ctx0 = s->ctx0; j->ctx1 = s->ctx1;
        r = s->p; end = r + s->size; lim = s->lim; lod = s->lod;
        cur = other;
    }
    PT(3, (cull_close_batch(&j->out, 0, &j->fallback), 0));
}

static NJ_TLS J* t_j;

static int run210(wws_njob* nj)
{
    if (!t_j) { t_j = (J*)calloc(1, sizeof(J)); if (!t_j) { nj->fallback = CF_MEM; return 0; } }
    J* j = t_j;
    j->nj = nj; j->fallback = 0; j->qn = 0;
    cull_out_reset(&j->out);
    /* the layout the worker assumes */
    const wws_nbuf* b1 = &nj->buf[1];
    if (!b1->used || !b1->list || b1->size < 24 || b1->size > 64 || b1->pages < 48) { nj->fallback = CF_LAYOUT; return 0; }
    const uint8_t* L = nj->read(nj, b1->ea, b1->size);
    if (!L) { nj->fallback = CF_EA; nj->bad_ea = b1->ea; nj->bad_size = b1->size; return 0; }
    uint32_t esz[3], eea[3]; int ne = 0;
    for (uint32_t o = 0; o + 8 <= b1->size; o += 8) {
        const uint32_t sz = R32(L + o) & 0x7FFF, ea = R32(L + o + 4);
        if (!sz) continue;
        if (ne == 3) { nj->fallback = CF_LAYOUT; return 0; }
        esz[ne] = sz; eea[ne] = ea; ne++;
    }
    if (ne != 3 || esz[0] != 0x90 || ((eea[0] | eea[1] | eea[2]) & 15)) { nj->fallback = CF_LAYOUT; return 0; }
    const uint8_t* hdr = nj->read(nj, eea[0], 0x90);
    if (!hdr) { nj->fallback = CF_EA; nj->bad_ea = eea[0]; nj->bad_size = 0x90; return 0; }
    const uint32_t nplanes = R32(hdr + 0x30), rootlen = R32(hdr + 0x34);
    if (!nplanes || !rootlen) return 1;                  /* 0x5BC8: the wrapper skips the worker: nothing written */
    if (esz[1] != ((32u * nplanes + 15u) & ~15u) || nplanes > 0x800 || rootlen > esz[2]) { nj->fallback = CF_LAYOUT; return 0; }
    const uint8_t* bsp = nj->read(nj, eea[1], esz[1]);
    const uint8_t* root = nj->read(nj, eea[2], esz[2]);
    if (!bsp || !root) { nj->fallback = CF_EA; nj->bad_ea = bsp ? eea[2] : eea[1]; nj->bad_size = bsp ? esz[2] : esz[1]; return 0; }
    j->hdr = hdr;
    j->cam = LQ(hdr);
    const F4 h70 = LQ(hdr + 0x70);
    if (FBAD(j->cam, 7) || FBAD(h70, 15)) { nj->fallback = CF_FLOAT; return 0; }
    j->hscale = FLANE(h70, 0);
    j->V.cam = j->cam; j->V.hscale = FLANE(h70, 0); j->V.minsize2 = FLANE(h70, 1); j->V.ratio = FLANE(h70, 2);
    j->V.cliph = FLANE(h70, 3); j->V.clip = hdr[0x81] != 0;
    j->V.bsp.p = bsp; j->V.bsp.nbytes = esz[1]; j->V.bsp.n = nplanes;
    cull_bsp_prepare(&j->V.bsp);
    cull_cursors cc = { R32(hdr + 0x44), R32(hdr + 0x48), R32(hdr + 0x4C), R32(hdr + 0x50) };
    /* the output header as 0x4434..0x450C leaves it: w0 = hdr+0x3C, w1 = hdr+0x40, byte 12 = hdr+0x80 */
    memset(j->out.hdr0, 0, 16);
    P32(j->out.hdr0, R32(hdr + 0x3C)); P32(j->out.hdr0 + 4, R32(hdr + 0x40)); j->out.hdr0[12] = hdr[0x80];
    const unsigned csr = cull_float_enter();
    PT(4, (walk(j, root, rootlen, root + esz[2]), 0));
    if (cull_float_leave(csr)) j->fallback |= CF_FLOAT;
    if (j->fallback) { nj->fallback = j->fallback; return 0; }
    if (j->out.nbatch) {
        /* the cursors' lines must be guest memory: the MFC refuses an atomic on anything else (the job would spin) */
        if (!nj->read(nj, cc.cur1 & ~127u, 128) || !nj->read(nj, cc.cur2 & ~127u, 128)) {
            nj->fallback = CF_EA; nj->bad_ea = cc.cur1; nj->bad_size = 128; return 0;
        }
        cull_publish(nj, &j->out, &cc);
    }
    return 1;
}

/* verify mode: the lifted job's buffer 2 (+0x180 the last batch's header, +0x380 its list) against the capture */
static int verify210(wws_njob* nj, const uint8_t* ls, const uint32_t buf_ls[WWS_NJOB_SETS])
{
    (void)nj;
    J* j = t_j;
    if (!j || !j->out.nbatch) return 0;                  /* nothing was captured: nothing to compare */
    const uint32_t b2 = buf_ls[2];
    if (!b2 || b2 + 0x380 + 14336 > 0x40000) return 9;
    const cull_batch* b = &j->out.batch[j->out.nbatch - 1];
    const uint8_t* o = ls + b2;
    if (memcmp(o + 0x180, b->hdr, 8) || o[0x18C] != b->hdr[12] || memcmp(o + 0x18E, b->hdr + 14, 2)) return 2;
    if (R32(o + 0x188) && memcmp(o + 0x190, b->hdr + 0x10, 496)) return 3;
    if (memcmp(o + 0x380, b->list, 8 * b->n)) return 4;
    return 0;
}

const wws_nmod wws_nmod_210 = { 210, "scene walker", run210, verify210, s_reasons, 1u << 1 };

#if defined(__clang__)
#pragma clang attribute pop
#endif
