/* cull_core.h -- what the culling job family (210 scene walker, 208 flat instance culler) shares, natively.
 *
 * Both jobs cull boxes the same way (the same SPU code, at other addresses in each module): the box through a
 * row-vector 4x4 transform (center and |M| extent), an optional height clip, a minimum size, a size-to-distance
 * ratio, then the frame's visibility BSP (four frustum planes, then a tree of occluder planes). Both bucket-sort
 * their visible items on a one-byte key (248 buckets) and write a 512-byte header plus an 8-byte-per-item list
 * at addresses taken from two atomic cursors in guest memory.
 *
 * Read from the lifted modules and from ThiagoFraga761/infamous2-pc's models of the same jobs on the US disc
 * (work/port/jobbc11_native.c, job207_native.c, MIT, Copyright (c) 2026 the infamous2-recomp contributors;
 * rpi/job-bc11-native/RESEARCH.md and rpi/job207-native/RESEARCH.md are the specs). What changed here: the
 * inputs are host pointers into guest memory (no local store, no DMA, no private copies), the outputs go
 * through the wws_njob services, and the checks are this port's job recordings (spu_jobrec.c native pass).
 *
 * Floats. SPU single precision: truncating, denormals as zero, no infinities (exponent 255 is a number). Host
 * SSE under round-toward-zero + FTZ + DAZ gives the same bits for every value below 2^128, so the walk runs on
 * host floats and checks its way out of the rest: an input word with exponent 255 declines (FB_FLOAT) and so
 * does any overflow or invalid flag the walk raised (cull_float_enter / _leave). The SPU's fma is fused with one
 * truncation, as x86 FMA under RZ: the jobs' fma/fms are _mm_fmadd/_mm_fmsub, every other operation stays a
 * separate rounding (the file is built with -ffp-contract=off: no fusing the SPU did not do).
 *
 * Thiago's LICENSE (MIT): Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
 * following conditions: The above copyright notice and this permission notice shall be included in all copies
 * or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND. */
#ifndef CULL_CORE_H
#define CULL_CORE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <immintrin.h>
#include "../wws_njob.h"

/* everything below is built for AVX2 + FMA (the lifted code this exe runs needs them already) */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,fma,sse4.1,bmi,bmi2,popcnt"))), apply_to = function)
#pragma STDC FP_CONTRACT OFF
#endif

#if defined(_WIN32)
#define NJ_TLS __declspec(thread)
#else
#define NJ_TLS __thread
#endif

#define CULL_ITEMS    1792     /* 210: op 7 flushes past this many items; 208: the 1,792nd item flushes */
#define CULL_BSPSTACK 60       /* the BSP walk's stack holds 64 words before it runs into its caller */
#define CULL_BATCHES  32
#define F1 0x3F800000u
#define FH 0x3F000000u

static inline uint32_t R32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline uint32_t R16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline void P32(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }
static inline void P16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

typedef __m128 F4;
/* 16 big-endian bytes = four floats */
static inline F4 LQ(const uint8_t* p)
{
    const __m128i bs = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)p), bs));
}
static inline F4 F4W(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return _mm_castsi128_ps(_mm_setr_epi32((int)a, (int)b, (int)c, (int)d)); }
#define FLANE(v, i) ((uint32_t)_mm_extract_ps((v), (i)))
#define FSPL(v, i) _mm_shuffle_ps((v), (v), _MM_SHUFFLE(i, i, i, i))
#define FSET(v, i, x) _mm_insert_ps((v), _mm_castsi128_ps(_mm_cvtsi32_si128((int)(x))), (i) << 4)
static inline F4 FMUL(F4 a, F4 b) { return _mm_mul_ps(a, b); }
static inline F4 FADD(F4 a, F4 b) { return _mm_add_ps(a, b); }
static inline F4 FSUB(F4 a, F4 b) { return _mm_sub_ps(a, b); }
static inline F4 FMA_(F4 a, F4 b, F4 c) { return _mm_fmadd_ps(a, b, c); }    /* a*b + c, one rounding */
static inline F4 FMS_(F4 a, F4 b, F4 c) { return _mm_fmsub_ps(a, b, c); }    /* a*b - c, one rounding */
static inline F4 FGT(F4 a, F4 b) { return _mm_cmpgt_ps(a, b); }
static inline F4 FSEL(F4 a, F4 b, F4 m) { return _mm_blendv_ps(a, b, m); }  /* selb: m ? b : a */
static inline F4 FABS(F4 a) { return _mm_andnot_ps(_mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u)), a); }
static inline F4 FCVT(int32_t a, int32_t b, int32_t c, int32_t d) { return _mm_cvtepi32_ps(_mm_setr_epi32(a, b, c, d)); }
static inline int GT0(F4 a, F4 b) { return _mm_movemask_ps(_mm_cmpgt_ps(a, b)) & 1; }
/* lanes (bit mask) holding an exponent-255 word */
static inline int FBAD(F4 v, int lanes)
{
    const __m128i e = _mm_set1_epi32(0x7F800000);
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(v), e), e))) & lanes;
}
/* (x + y) + z of lanes 0..2: the jobs' shuffle-and-add horizontal sum, in lane 0 */
static inline F4 HSUM3(F4 v) { return FADD(FADD(v, FSPL(v, 1)), FSPL(v, 2)); }
/* SPU shl: counts of 32..63 give 0 */
static inline int32_t SHL(int32_t v, uint32_t s) { return (s & 63) > 31 ? 0 : (int32_t)((uint32_t)v << (s & 31)); }

/* the walk's MXCSR: round toward zero, FTZ, DAZ, all exceptions masked, flags clear */
static inline unsigned cull_float_enter(void)
{
    const unsigned csr = _mm_getcsr();
    _mm_setcsr((csr & ~0x003Fu) | 0x6000u | 0x8000u | 0x0040u | 0x1F80u);
    return csr;
}
/* 1 = the walk raised overflow or invalid (it must decline) */
static inline int cull_float_leave(unsigned csr)
{
    const unsigned fl = _mm_getcsr();
    _mm_setcsr(csr);
    return (fl & 0x09u) != 0;
}

typedef struct { F4 x[4]; } X4;

/* fallback bits shared by the family (a module adds its own above CF_MODULE) */
enum {
    CF_FLOAT = 1u << 0,    /* an exponent-255 input or a float overflow / invalid operation */
    CF_BSP = 1u << 1,      /* a BSP stack deeper than 60, a looping tree, a node outside the plane block */
    CF_BUCKET = 1u << 2,   /* a bucket key >= 248 (the job would write past its counters) */
    CF_BATCH = 1u << 3,    /* more than 32 batches */
    CF_EA = 1u << 4,       /* an input or a cursor line outside guest memory */
    CF_GET = 1u << 5,      /* a GET the MFC refuses (over 16 KB, misaligned) or rewrites (Edge viewport) */
    CF_LAYOUT = 1u << 6,   /* the descriptor's buffers are not the layout the job assumes */
    CF_RANGE = 1u << 7,    /* a read outside the bytes the job's own GET brought in (stale local store) */
    CF_MEM = 1u << 8,      /* host allocation */
    CF_MODULE = 1u << 9,
};
#define CULL_REASONS_COMMON "FLOAT", "BSP", "BUCKET", "BATCH", "EA", "GET", "LAYOUT", "RANGE", "MEM"

/* The visibility BSP (both jobs' 0x6220 / 0x4ED8). nodes: 32 bytes each, {plane (nx, ny, nz, d); u32 back;
 * u32 front}, offsets in nodes from the node. The first four nodes (node k+1 = node k + w(node k + 0x14)) are
 * frustum planes tested first; then the tree: box entirely in front goes front, entirely behind goes back,
 * straddling pushes back and goes front. A null front is empty space (visible), a null back is solid: try
 * the next pushed node; none left = hidden. Every node read must lie inside the block (nbytes). */
typedef struct {
    const uint8_t* p; uint32_t nbytes, n;
    /* cull_bsp_prepare: the four frustum planes, once per job (their nodes do not depend on the box) */
    int fr_ok;                    /* planes 0..fr_ok-1 readable and good, in the walk's order */
    uint32_t fr_fail;             /* what plane fr_ok (< 4) makes the walk do: CF_BSP or CF_FLOAT */
    uint32_t root;                /* the node after them: the tree */
    F4 fx, fy, fz, fd;            /* plane k in lane k (harmless zeros past fr_ok) */
} cull_bsp;

static inline int bsp_node_ok(const cull_bsp* B, uint32_t off) { return off <= B->nbytes && B->nbytes - off >= 32u; }

static void cull_bsp_prepare(cull_bsp* B)
{
    float x[4] = { 0 }, y[4] = { 0 }, z[4] = { 0 }, d[4] = { 0 };
    uint32_t N = 0;
    B->fr_ok = 4; B->fr_fail = 0;
    for (int k = 0; k < 4; k++) {
        if (!bsp_node_ok(B, N)) { B->fr_ok = k; B->fr_fail = CF_BSP; break; }
        const F4 pl = LQ(B->p + N);
        if (FBAD(pl, 15)) { B->fr_ok = k; B->fr_fail = CF_FLOAT; break; }
        uint32_t w;
        w = FLANE(pl, 0); memcpy(&x[k], &w, 4); w = FLANE(pl, 1); memcpy(&y[k], &w, 4);
        w = FLANE(pl, 2); memcpy(&z[k], &w, 4); w = FLANE(pl, 3); memcpy(&d[k], &w, 4);
        N += R32(B->p + N + 0x14) << 5;
    }
    B->root = N;
    B->fx = _mm_loadu_ps(x); B->fy = _mm_loadu_ps(y); B->fz = _mm_loadu_ps(z); B->fd = _mm_loadu_ps(d);
}

static int bsp_test(const cull_bsp* B, F4 mn, F4 mx, uint32_t* fallback)
{
#ifdef J210_PROFILE
    { extern unsigned long long g_j210_ops[32]; g_j210_ops[29]++; }
#endif
    if (!B->n) return 1;
    /* the frustum planes, all four at once: plane k's test is the walk's (a = n*min, b = n*max per component,
     * hi = a > b ? a : b, (hi.x + hi.y) + hi.z against d), lane k; the first plane in order that rejects returns
     * hidden, one that cannot be read declines -- as the walk, which stops at the first of either */
    {
        const F4 ax = FMUL(B->fx, FSPL(mn, 0)), bx = FMUL(B->fx, FSPL(mx, 0));
        const F4 ay = FMUL(B->fy, FSPL(mn, 1)), by = FMUL(B->fy, FSPL(mx, 1));
        const F4 az = FMUL(B->fz, FSPL(mn, 2)), bz = FMUL(B->fz, FSPL(mx, 2));
        const F4 hx = FSEL(bx, ax, FGT(ax, bx)), hy = FSEL(by, ay, FGT(ay, by)), hz = FSEL(bz, az, FGT(az, bz));
        const F4 sum = FADD(FADD(hx, hy), hz);
        const int rej = _mm_movemask_ps(FGT(B->fd, sum)) & ((1 << B->fr_ok) - 1);
        if (rej) return 0;
        if (B->fr_ok < 4) { *fallback |= B->fr_fail; return 0; }
    }
    uint32_t N = B->root;
    uint32_t stk[CULL_BSPSTACK]; int sp = 0;
    const F4 z = _mm_setzero_ps();
#ifdef J210_PROFILE
    { extern unsigned long long g_j210_ops[32]; g_j210_ops[30]++; }
#endif
    for (uint32_t it = 0; ; it++) {
#ifdef J210_PROFILE
        { extern unsigned long long g_j210_ops[32]; g_j210_ops[31]++; }
#endif
        if (it > 1000000u || !bsp_node_ok(B, N)) { *fallback |= CF_BSP; return 0; }
        const uint8_t* np = B->p + N;
        const F4 pl = LQ(np);
        if (FBAD(pl, 15)) { *fallback |= CF_FLOAT; return 0; }
        const F4 a = FMUL(pl, mn), b = FMUL(pl, mx);
        const F4 g = FGT(a, b);
        const F4 hi = FSEL(b, a, g), lo = FSEL(a, b, g);
        const F4 d = FSPL(pl, 3);
        const F4 mind = FSUB(HSUM3(lo), d), maxd = FSUB(HSUM3(hi), d);
        const uint32_t wback = R32(np + 16), wfront = R32(np + 20);
        int front;
        if (GT0(mind, z)) front = 1;
        else if (GT0(z, maxd)) front = 0;
        else {                                  /* straddles: the back half later */
            if (wback) {
                if (sp >= CULL_BSPSTACK) { *fallback |= CF_BSP; return 0; }
                stk[sp++] = N + (wback << 5);
            }
            front = 1;
        }
        if (front) {
            if (!wfront) return 1;
            N += wfront << 5;
        } else {
            N += wback << 5;
            if (!wback) {
                if (!sp) return 0;
                N = stk[--sp];
            }
        }
    }
}

/* box through the 4x4 row-vector transform X (center and |X| extent): 210 0x6650, 208 0x5270 */
static inline void box_xform(F4 mn, F4 mx, const X4* X, F4* wmn, F4* wmx)
{
    const F4 h = F4W(FH, FH, FH, FH);
    const F4 c = FMUL(FADD(mn, mx), h);
    const F4 e = FSUB(mx, c);
    F4 w = FMUL(X->x[0], FSPL(c, 0));
    w = FMA_(X->x[1], FSPL(c, 1), w);
    const F4 r10 = FMUL(FABS(X->x[1]), FSPL(e, 1));
    w = FMA_(X->x[2], FSPL(c, 2), w);
    F4 r5 = FMA_(FSPL(e, 0), FABS(X->x[0]), r10);
    w = FADD(w, X->x[3]);
    r5 = FMA_(FSPL(e, 2), FABS(X->x[2]), r5);
    *wmx = FADD(w, r5); *wmn = FSUB(w, r5);
}

/* the per-view cull parameters (210: header +0x70.., 208: +0x38..) */
typedef struct {
    F4 cam;                       /* camera position (lanes 0..2) */
    uint32_t hscale, minsize2, ratio, cliph;   /* float bits */
    int clip;                     /* height clip on */
    cull_bsp bsp;
} cull_view;

/* 210 0x4030, 208 0x445C..0x47D4: height clip, size and distance thresholds, then the BSP */
static int cull_box(const cull_view* V, F4 mn, F4 mx, const X4* X, uint32_t* fallback)
{
    F4 wmn, wmx;
    box_xform(mn, mx, X, &wmn, &wmx);
    if (V->clip) {
        const F4 t = F4W(V->cliph, V->cliph, V->cliph, V->cliph);
        if (GT0(t, FSPL(wmx, 2))) return 0;
        if (GT0(t, FSPL(wmn, 2))) wmn = FSET(wmn, 2, V->cliph);
    }
    const F4 e = FSUB(wmx, wmn);
    const F4 s = HSUM3(FMUL(e, e));
    if (GT0(F4W(V->minsize2, 0, 0, 0), FSPL(s, 0))) return 0;
    if (GT0(F4W(V->ratio, 0, 0, 0), _mm_setzero_ps())) {
        F4 d = FMS_(FADD(wmn, wmx), F4W(FH, FH, FH, FH), V->cam);
        d = FSET(d, 2, FLANE(FMUL(FSPL(d, 2), F4W(V->hscale, 0, 0, 0)), 0));   /* height scaled */
        const F4 dist = HSUM3(FMUL(d, d));
        if (GT0(FMUL(FSPL(dist, 0), F4W(V->ratio, 0, 0, 0)), FSPL(s, 0))) return 0;
    }
    return bsp_test(&V->bsp, wmn, wmx, fallback) & 255;
}

/* ---- items, batches, the bucket sort (210 0x6050, 208 0x4D08) ------------------------------------ */
typedef struct {
    uint32_t n, add, list_size;   /* items; cursor-2 increment (208: 8x the list, 0x1C000 at a flush); bytes PUT */
    uint8_t hdr[512];             /* w0, w1, w2 = list EA (at publish), byte 12, byte 13 stale, 14-15 = 0, 248 u16 ends */
    uint8_t* list;                /* CULL_ITEMS * 8 */
} cull_batch;

typedef struct {
    uint32_t n, total;
    uint8_t ib[CULL_ITEMS];
    uint32_t iw2[CULL_ITEMS], iw3[CULL_ITEMS];
    uint32_t nbatch;
    cull_batch batch[CULL_BATCHES];
    uint8_t hdr0[16];             /* the batch header's fixed words */
} cull_out;

static void cull_out_reset(cull_out* o)
{
    o->n = 0; o->total = 0; o->nbatch = 0;
}

/* the sort into batch b: stable within a bucket; the header's ends are the inclusive prefix sums after the scatter */
static void cull_close_batch(cull_out* o, int flush208, uint32_t* fallback)
{
    if (!o->n) return;
    if (o->nbatch == CULL_BATCHES) { *fallback |= CF_BATCH; return; }
    cull_batch* b = &o->batch[o->nbatch];
    if (!b->list) b->list = (uint8_t*)malloc(CULL_ITEMS * 8);
    if (!b->list) { *fallback |= CF_MEM; return; }
    uint16_t cnt[248], off[248];
    memset(cnt, 0, sizeof cnt);
    for (uint32_t i = 0; i < o->n; i++) {
        if (o->ib[i] >= 248) { *fallback |= CF_BUCKET; return; }
        cnt[o->ib[i]]++;
    }
    uint16_t acc = 0;
    for (int k = 0; k < 248; k++) { off[k] = acc; acc = (uint16_t)(acc + cnt[k]); }
    for (uint32_t i = 0; i < o->n; i++) {
        const uint32_t idx = off[o->ib[i]]++;
        P32(b->list + 8 * idx, o->iw2[i]);
        P32(b->list + 8 * idx + 4, o->iw3[i]);
    }
    memcpy(b->hdr, o->hdr0, 16);
    for (int k = 0; k < 248; k++) P16(b->hdr + 0x10 + 2 * k, off[k]);
    b->n = o->n;
    if (flush208 == 1) { b->list_size = 14336; b->add = 0x1C000; }                 /* 208 0x4A30, 0x4AC0 */
    else {
        b->list_size = (o->n * 8 + 127) & ~127u;
        b->add = flush208 == 2 ? b->list_size << 3 : b->list_size;                 /* 208 0x4548: 8x */
    }
    memset(b->list + 8 * o->n, 0, b->list_size - 8 * o->n);    /* the padding: stale local store on the PS3 */
    o->nbatch++;
    o->total += o->n;
    o->n = 0;
}

/* The cursors and the PUTs, as both jobs do them after their walk (per batch: 512 bytes from cursor 1; if that
 * is 0 or past its limit the batch is dropped; then `add` from cursor 2; 0 or past its limit = no list, the
 * header's bucket ends zeroed; the header PUT; the list PUT in 16 KB elements). */
typedef struct { uint32_t cur1, lim1, cur2, lim2; } cull_cursors;

/* PS3_NATIVE_CULL_BREAK=1: the checker's self-test: one bit of every first list item flipped (every native pass
 * that writes a list must then fail) */
static int cull_break(void)
{
    static int s = -1;
    if (s < 0) { const char* e = getenv("PS3_NATIVE_CULL_BREAK"); s = e && *e == '1'; }
    return s;
}

static void cull_publish(wws_njob* nj, cull_out* o, const cull_cursors* c)
{
    if (nj->capture) return;                      /* verify mode: the batches stay as built (verify() reads them) */
    if (cull_break()) for (uint32_t k = 0; k < o->nbatch; k++) if (o->batch[k].n) o->batch[k].list[7] ^= 1;
    uint8_t dc[512];
    memset(dc, 0, sizeof dc);
    dc[13] = 1;                                   /* header byte 13: never written by the job */
    uint8_t dcl[16384];                           /* list padding: dontcare, built per element */
    for (uint32_t k = 0; k < o->nbatch; k++) {
        cull_batch* b = &o->batch[k];
        const uint32_t hea = nj->atomic_add(nj, c->cur1, 512);
        if (hea == 0 || hea + 512 > c->lim1) continue;                  /* dropped, as the job drops it */
        uint32_t lea = nj->atomic_add(nj, c->cur2, b->add);
        if (lea + b->add > c->lim2) lea = 0;
        if (!lea) memset(b->hdr + 0x10, 0, 496);
        P32(b->hdr + 8, lea);
        nj->put(nj, hea, b->hdr, 512, dc);
        if (lea) {
            const uint32_t used = 8 * b->n;
            for (uint32_t off = 0; off < b->list_size; off += 16384) {
                const uint32_t n = b->list_size - off > 16384 ? 16384 : b->list_size - off;
                const uint8_t* d = NULL;
                if (off + n > used) {                                   /* this element holds padding */
                    uint8_t* q = (uint8_t*)dcl;
                    const uint32_t from = used > off ? used - off : 0;
                    memset(q, 0, from); memset(q + from, 1, n - from);
                    d = q;
                }
                nj->put(nj, lea + off, b->list + off, n, d);
            }
        }
    }
}

#if defined(__clang__)
#pragma clang attribute pop
#endif

#endif /* CULL_CORE_H */
