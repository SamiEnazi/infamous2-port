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
static int aabb_visible(J* j, const uint8_t* p, uint32_t mlsa)
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
static int lod_inside(J* j, uint32_t idx, u128 pos)
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
        if (aabb_visible(j, cur + 4, j->F)) return cur + 12;
        return cur + 12 + R16(cur + 2);
    case 2:
        if (aabb_visible(j, cur + 4, j->F)) push(j, R16(cur + 2), R32(cur + 12), j->a, j->b, j->M, lod);
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
        if (cur[1] != 0) {
            put_matrix(j, j->F + 0x40, N);
            if (!aabb_visible(j, cur + 64, j->F + 0x40)) return cur + 72;
        }
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
    put_matrix(j, j->F, j->M);
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
        put_matrix(j, j->F, j->M);
        ev_add(j, EV_WAIT, 1u << (s->tag & 31), 0, 0, 0);
        cur = vm_base + s->ea;
        end = cur + s->size;
        cur_slot = other;
    }
    if (j->nrec > j->batch0) ev_add(j, EV_FLUSH, j->batch0, j->nrec, 1, 0);
    return !j->fallback;
}

/* ---------------- phase 2 ---------------- */
static void latch(J* j, int replay, uint32_t ch, uint32_t v)
{
    (void)replay;
    spu_wrch_raw(j->ctx, ch, v);
}

static void tag_wait(J* j, int replay, uint32_t mask)
{
    latch(j, replay, MFC_WrTagMask, mask);
    latch(j, replay, MFC_WrTagUpdate, 1);
    (void)spu_rdch_raw(j->ctx, MFC_RdTagStat);
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
    const uint32_t d0 = replay ? spu_native_replay_dec(ctx, 1) : spu_rdch_raw(ctx, SPU_RdDec);
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
    const uint32_t d1 = replay ? spu_native_replay_dec(ctx, 2) : spu_rdch_raw(ctx, SPU_RdDec);
    P32(ls + j->buf2, R32(ls + j->buf2) + (~d1 - (~d0 + 2u)));
}

static int j210_walker(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_J210");
        s_mode = e ? atoi(e) : 1;
        s_dbg = getenv("J210_DEBUG") != 0;
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
