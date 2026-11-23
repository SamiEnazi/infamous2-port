/* inFAMOUS 2: the render-list builder (job 204, jobmod_3af9ed8fe0d4_e14ff0), natively.
 *
 * The same job as Thiago's ec5f (overlay 225 of the US disc: ThiagoFraga761/infamous2-pc
 * work/port/ec5f_native.c, MIT, Copyright (c) 2026 the infamous2-recomp contributors; his
 * rpi/ec5f-native/RESEARCH.md is the spec), a different build of the same source: the algorithm,
 * the inputs and the command words are the same, the code layout and some field uses differ, so
 * everything here is checked against OUR job's recordings (job replay, agent/native204).
 *
 * It replaces main (LS 0x12C60: queue selection, the per-object loop, the type handlers, the
 * final block). It reads the job's inputs from guest memory where the job would have DMA'd them,
 * builds the command blocks, takes their ring slots with the job's own atomic add and PUTs them
 * through the MFC engine; the 32-byte record is left at the output buffer for the wrapper's PUT.
 * Any branch the model lacks, any unexpected input: nothing has been written, the native declines
 * and the lifted main runs. The local store is not left as the lifted code leaves it (callee
 * frames, scratch): the ranges it dirties are declared dead to the job replay (spu_jobrec.h).
 *
 * (deck) Job types 1 and 3 too (type 4 still declines): their objects carry lists of sub-draws (0x69B0), each
 * reserving a region of the command stream for a later writer and PUTting a 32-byte record of it to a slot that
 * an allocator atomic (0x4740) hands out; a full pool drops the region again, so that atomic's outcome shapes the
 * stream. Such a job is built twice: a dry build that writes nothing and assumes every slot comes (any decline
 * still happens here), then the committing build, which takes the ring slots, the record slots and the matrix
 * slots (0x19138) in the job's own order as it goes (the 0x14210 callback's order) and PUTs the records. Every
 * block is staged in the LS buffer the job would have built it in (out+0x80 even, out+0x1080 odd), so the stale
 * bytes past its JUMP/RETURN and inside the reserved regions are the job's own; out+0x20..0x6F (the record and
 * matrix staging) is left as the job leaves it (a later job reads out+0x28 stale).
 *
 * PS3RECOMP_NATIVE_J204=0: off (the lifted job). PS3_SPU_NATIVE=0: every rewrite off. Off while the job recorder
 * records (the recording must hold the lifted job's own operations).
 *
 * Thiago's LICENSE (MIT): Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"), to deal in the
 * Software without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions: The
 * above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
 * ANY KIND. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "native_float.h"   /* spu_fm, spu_fma ... = the *_lean forms (PS3_NATIVE_FLOAT_LEAN=0: the reference forms) */
#include "spu_dma.h"
#include "spu_jobrec.h"
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif

extern uint8_t* vm_base;
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));


#define EC5F_IMAGE 204
#define EC5F_MAIN  0x12C60u
#define F1 0x3F800000u

static inline uint32_t R32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline uint16_t R16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline int16_t S16(const uint8_t* p) { return (int16_t)R16(p); }
static inline void P32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline void P16(uint8_t* p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 LQ(const uint8_t* p) { return V4(R32(p), R32(p + 4), R32(p + 8), R32(p + 12)); }
static inline u128 SPL(u128 v, int i) { return V4(v._u32[i], v._u32[i], v._u32[i], v._u32[i]); }
static inline u128 SPLW(uint32_t x) { return V4(x, x, x, x); }

/* Fallback reasons (bits of J.fallback). KIND2, KIND10, KIND11 and IN1_9 are
 * modelled now (section 11) and no longer raised; the bits keep their values. */
enum { FB_SRGB = 1, FB_CONST = 2, FB_OP11B = 4, FB_OP4 = 8, FB_OP = 16, FB_KIND2 = 32, FB_KIND10 = 64,
       FB_KIND11 = 128, FB_SIDE = 256, FB_IN1_9 = 512, FB_FPUPLOAD = 1024, FB_OP5 = 2048,
       FB_TYPE = 4096, FB_EA = 8192, FB_MEM = 16384, FB_STALE = 32768, FB_RAND = 65536 };
enum { FB_NBITS = 17 };
static unsigned s_types = 0xFu;   /* (deck) J204_TYPES=<mask>: job types the native takes (bit t = type t; default 0xF = 0..3, 0x5 = only 0 and 2 as before 2026-11-18) */

typedef struct {
    uint32_t ea, used;
    uint8_t data[4096];
} Block;

typedef struct {
    /* reset for every job */
    spu_context* ctx;
    uint32_t out, nitems, nrange, lastcnt, lt, wo, novf, nq;
    const uint8_t *IN, *IN1, *IN2;
    int fallback;
    uint32_t bad_ea, bad_size;
    uint32_t matsz;
    uint32_t rec[8];
    /* command buffer (0x14160 and the emitters' room checks) */
    uint32_t cursor, has_block, ring_ea, delta, limit, done, nblocks;
    uint8_t* cb;
    /* job scratch (LS 0x27000) */
    u128 prevM[4], op5cache, tex[16], box[2];
    /* The type-0 handler's (0x11580) stack quad sp+0x30 = main's sp - 0x210: slot 12 rewrites only its x, so y..w
     * are what 0x7C58's prologue saved there just before (main's r112, see r112_at_7C58) */
    u128 stk;
    u128 r112;         /* (deck) main's r112 in the current range (r112_at_7C58) */
    uint8_t texon[16], need_mat;
    uint32_t vfmt, streams[3], stencil, tex194;
    /* (deck) types 1 and 3 */
    int live;          /* the committing build: atomics taken and records PUT as the job goes (else nothing is written) */
    int need_live;     /* the dry build met an atomic whose outcome shapes the stream */
    int wt;            /* the tag register of the current object's last wait (0: its data tag; 8, 10) */
    uint32_t ctr198;   /* scratch+0x198, the record serial (0 at job start) */
    uint32_t half;     /* 0x18F10 of IN+0x30C (type 3) or 0 */
    uint8_t img_on, img[0x50];   /* LS out+0x20..0x6F as the job leaves it (read at the first sub-draw list) */
    /* kept */
    Block* blocks; uint32_t cap;
    uint8_t spill[4096];
} J;

/* ---------------- guest memory ---------------- */
static inline int ea_ok(uint32_t ea, uint32_t size) { return spu_native_ea_ok(ea, size); }
static const uint8_t* G(J* j, uint32_t ea, uint32_t size)
{
    static const uint8_t zero[0x10000];     /* covers every read but a range's items, checked at the call */
    if (__builtin_expect(!ea_ok(ea, size), 0)) {
        if (!(j->fallback & FB_EA)) { j->bad_ea = ea; j->bad_size = size; }
        j->fallback |= FB_EA; return zero;
    }
    return vm_base + ea;
}

/* A PUT as the job does it: from where the job stages the bytes in the local store (its own output
 * buffers), through the MFC engine's transfer (coherency, the gcm hook), cmd 0x20. */
static void put_ls(J* j, uint32_t lsa, uint32_t ea, uint32_t size)
{
    spu_context* ctx = j->ctx;
    lsa &= SPU_LS_MASK;
    /* (deck) the lean transfer (spu_dma.h: the same checks, copy, coherence and PUT hook without the ~50
     * diagnostic gates) in play; the raw one while the job recorder is on, as before */
    if (!g_spu_jobrec_mode && spu_dma_fast()) mfc_do_transfer_lean(ctx, lsa, ea, size, 0x20);
    else mfc_do_transfer_raw(ctx, lsa, ea, size, 0x20);
}
/* the LS buffer block k is built in (0x14108: out+0x80 and out+0x1080, alternating) */
static inline uint8_t* blk_ls(J* j, uint32_t k) { return j->ctx->ls + ((j->out + 0x80u + 0x1000u * (k & 1)) & SPU_LS_MASK); }

/* ---------------- command buffer ---------------- */
static void W(J* j, uint32_t v) { P32(j->cb + j->cursor, v); j->cursor += 4; }
/* 0x14160, without the ring: publish() places the blocks once the job is done */
static void next_block(J* j)
{
    if (j->has_block && j->nblocks) j->blocks[j->nblocks - 1].used = j->cursor;
    if (j->has_block) { j->novf++; j->wt = 10; }     /* the job waits its output tag before PUTting the block it leaves */
    j->done += 4096; j->cursor = 0; j->has_block = 1;
    if (j->nblocks == j->cap) {
        uint32_t nc = j->cap ? 2 * j->cap : 16;
        Block* nb = (Block*)realloc(j->blocks, nc * sizeof *nb);
        if (!nb) { j->fallback |= FB_MEM; j->cb = j->spill; return; }
        j->blocks = nb; j->cap = nc;
    }
    const uint32_t k = j->nblocks++;
    Block* b = &j->blocks[k];
    j->cb = b->data;
    if (j->live) {
        /* (deck) as the job's 0x14210 does it here: the ring slot (clamped to the limit), the block it leaves ends
         * in NOPs up to 4096 when this one follows it in memory, else in a JUMP to it; this block starts as its LS
         * buffer is (the block two back, or what the LS held before the job) */
        uint32_t off = spu_native_atomic_add32(j->ctx, j->ring_ea, 4096);
        if (off + 4096 > j->limit) off = j->limit - 4096;
        b->ea = off;
        if (k) {
            Block* p = &j->blocks[k - 1];
            if (off == p->ea + 4096) memset(p->data + p->used, 0, 4096 - p->used);
            else P32(p->data + p->used, 0x20000000u | (off + j->delta));
        }
        memcpy(b->data, k < 2 ? blk_ls(j, k) : j->blocks[k - 2].data, 4096);
    }
}
/* Dry build: the ring addresses, as the job's allocations would return them one by one (clamped to the
 * limit), the links, and the writes. Block k ends in a JUMP to block k+1, or in NOPs when k+1 follows it
 * in memory. Each block goes to the LS buffer the job builds it in, over the bytes the block two back left
 * there (what the job PUTs past the JUMP/RETURN), and the job PUTs whole 4096-byte blocks.
 * Committing build: the slots and links are in place, every block is a whole image of its LS buffer. */
static void publish(J* j)
{
    const uint32_t n = j->nblocks;
    if (!j->live)
        for (uint32_t k = 0; k < n; k++) {
            uint32_t off = spu_native_atomic_add32(j->ctx, j->ring_ea, 4096);
            if (off + 4096 > j->limit) off = j->limit - 4096;
            j->blocks[k].ea = off;
        }
    for (uint32_t k = 0; k < n; k++) {
        Block* b = &j->blocks[k];
        uint32_t size = 4096;
        if (!j->live) {
            size = b->used;
            if (k + 1 < n) {
                if (j->blocks[k + 1].ea == b->ea + 4096) { memset(b->data + b->used, 0, 4096 - b->used); size = 4096; }
                else { P32(b->data + b->used, 0x20000000u | (j->blocks[k + 1].ea + j->delta)); size = b->used + 4; }
            }
        }
        uint8_t* ls = blk_ls(j, k);
        memcpy(ls, b->data, size);
        if (!ea_ok(b->ea, size)) continue;
        put_ls(j, (uint32_t)(ls - j->ctx->ls), b->ea, 4096);
    }
    j->rec[0] = j->blocks[0].ea;
}
/* every emitter's room check: cursor + bytes > end (end = base + 4092; 0 before the first block) */
static inline void room(J* j, uint32_t bytes)
{
    if (j->has_block ? j->cursor + bytes > 4092 : bytes > 0) next_block(j);
}

#define IN  (j->IN)
#define IN1 (j->IN1)
#define IN2 (j->IN2)
#define SLOT(kind) S16(IN1 + 0x110 + 2 * (kind))

static void emit_vec(J* j, int slot, const uint32_t* w, int nvec)   /* 0x1EFC burst */
{
    room(j, 8 + 16 * nvec);
    W(j, ((1 + 4 * nvec) << 18) | 0x1EFC); W(j, (uint32_t)slot);
    for (int i = 0; i < 4 * nvec; i++) W(j, w[i]);
}
static void emit_v(J* j, int slot, u128 v)
{
    uint32_t w[4] = { v._u32[0], v._u32[1], v._u32[2], v._u32[3] };
    emit_vec(j, slot, w, 1);
}
/* Is [p, p+n) inside the material the job fetched? Past it the SPU reads
 * stale local store, which no native pass can reproduce: the job falls back. */
static inline int inmat(J* j, const uint8_t* mat, const uint8_t* p, uint32_t n)
{
    if (p >= mat && (uint64_t)(p - mat) + n <= j->matsz) return 1;
    j->fallback |= FB_STALE;
    return 0;
}

/* 0x143F0: textures */
static void f_143F0(J* j, const uint8_t* mat)
{
    const uint8_t* D = IN1 + 0x1C;
    uint32_t n = R32(D + 0x38);
    if (n > 14) n = 14;
    for (uint32_t i = 0; i < n; i++) {
        int unit = S16(D + 4 * i) & 15; uint16_t ti = R16(D + 4 * i + 2);
        const uint8_t* t = mat + 8 + 44 * (uint32_t)ti;
        if (!inmat(j, mat, t, 36)) return;
        uint32_t w0 = R32(t), w4 = R32(t + 4), w8 = R32(t + 8), w12 = R32(t + 12), w16 = R32(t + 16),
                 w20 = R32(t + 20), w24 = R32(t + 24), w28 = R32(t + 28), w32 = R32(t + 32);
        u128* c = &j->tex[unit];
        if (c->_u32[0] == w0 && c->_u32[1] == w8 && c->_u32[2] == w12 && c->_u32[3] == w20) continue;
        *c = V4(w0, w8, w12, w20);
        if (!j->texon[unit]) { room(j, 8); W(j, 0x00040B00 + 4 * unit); W(j, 0x2DC4); j->texon[unit] = 1; }
        room(j, 40);
        W(j, 0x001C1A00 + 0x20 * unit); W(j, w0); W(j, w4); W(j, w8); W(j, w12); W(j, w16); W(j, w20); W(j, w24);
        W(j, 0x00041840 + 4 * unit); W(j, w32);
        if ((w8 & 15) == 4 || ((w8 >> 8) & 15) == 4 || ((w8 >> 16) & 15) == 4) {
            room(j, 8); W(j, 0x00041A1C + 0x20 * unit); W(j, w28);
        }
    }
}

/* 0x15E78: material constants */
static void f_15E78(J* j, const uint8_t* mat, const uint8_t* old)
{
    uint32_t n = R32(IN1 + 0x9C), ntex = R32(IN1 + 0xB4);
    if (n > 17) n = 17;
    if (ntex > 16) { j->fallback |= FB_STALE; return; }
    const uint8_t* nb = mat + 8 + 44 * ntex; const uint8_t* ob = old ? old + 8 + 44 * ntex : 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = R32(IN1 + 0x58 + 4 * i); int slot = (int16_t)(e >> 16);
        uint32_t type = (e >> 10) & 31, off = e & 1023;
        const uint8_t* np = nb + 4 * off; const uint8_t* op = ob ? ob + 4 * off : 0;
        if (type > 11) continue;
        if (!inmat(j, mat, np, 16)) return;
        if (type <= 3) {                                /* skipped when words 0..type are unchanged */
            if (op) { u128 m = spu_fceq(LQ(np), LQ(op)); uint32_t same = ~0u;
                for (uint32_t q = 0; q <= type; q++) same &= m._u32[q];
                if (same) continue; }
            if ((type == 0 || type == 2) && (e & 0x8000)) { j->fallback |= FB_SRGB; continue; }
            uint32_t w[4] = { R32(np), R32(np + 4), R32(np + 8), R32(np + 12) };
            emit_vec(j, slot, w, 1);
        } else if (type == 4) {
            if (op) { u128 m = spu_fceq(LQ(np), LQ(op));
                if (m._u32[0] && m._u32[1] && m._u32[2] && m._u32[3]) continue; }
            uint32_t w[8] = { R32(np), R32(np + 4), 0, F1, R32(np + 8), R32(np + 12), 0, F1 };
            emit_vec(j, slot, w, 2);
        } else j->fallback |= FB_CONST;
    }
}

/* Scalar float helpers for ops 0 and 12, lane 0 of splats. dv: n/d with
 * frest/fi and one Newton step; fl: floor via cflts (|q| < 2^23 only). */
static inline u128 dv(u128 n, u128 d)
{
    u128 e = spu_fi(d, spu_frest(d)), q = spu_fm(n, e);
    return spu_fma(spu_fnms(q, d, n), e, q);
}
static inline int GT(u128 a, u128 b) { return spu_fcgt(a, b)._u32[0] != 0; }
static inline u128 IFL(u128 v, int i) { return spu_csflt(SPLW(spu_cflts(v, 173)._u32[0] + (uint32_t)i), 155); }
/* 0x47E8: one animation axis, a hold/loop timeline of a frames, b rate, c, d length */
static u128 f_47E8(u128 a, u128 b, u128 c, u128 d)
{
    const int run = !(GT(SPLW(0x41F00000u), d) || GT(SPLW(0), b));     /* d >= 30 and b >= 0 */
    u128 s = spu_fa(run ? SPLW(0) : dv(c, d), b), q = dv(a, s);
    const int small = spu_fcmgt(SPLW(0x4B000000u), q)._u32[0] != 0;
    u128 t = small ? IFL(q, 0) : q, t1 = small ? IFL(q, -1) : q;
    u128 f = GT(t, q) ? t1 : t;
    u128 hi = spu_fm(f, c), lo = run ? SPLW(0) : spu_fm(d, spu_fma(b, spu_fs(SPLW(0xBF800000u), f), a));
    return GT(hi, lo) ? hi : lo;
}
/* 0x48C8: both axes of a two-phase timeline */
static void f_48C8(u128 a, u128 b, u128 c, u128 d, u128 e, u128 f, u128 g, u128* A, u128* B)
{
    u128 p = spu_fa(dv(c, d), b), r = spu_fa(dv(f, g), e), per = spu_fa(p, r);
    u128 n = IFL(dv(a, per), 0), n1 = IFL(dv(a, per), 1);
    u128 pn = spu_fm(per, n);
    const int late = GT(spu_fa(p, pn), a), early = GT(spu_fm(per, n1), a);
    *A = (late || !early) ? f_47E8(spu_fnms(r, n, a), b, c, d) : spu_fm(c, n1);
    *B = (!GT(pn, a) && late) ? spu_fm(f, n) : f_47E8(spu_fnms(p, n1, a), e, f, g);
}
/* 7C18 op 0 (0x8098, 0x4A60): scrolling UV offset from 6 material floats,
 * E[1], E[2] and the time at IN+0x160; the result is wrapped to (-1, 1) */
static void op0_uv(J* j, const uint8_t* E, const uint8_t* d, uint32_t* ox, uint32_t* oy)
{
    const u128 z = SPLW(0), one = SPLW(F1), sm = SPLW(0x80000000u);
    u128 d0 = SPLW(R32(d)), d1 = SPLW(R32(d + 4)), d3 = SPLW(R32(d + 12)), d4 = SPLW(R32(d + 16)), d5 = SPLW(R32(d + 20));
    const int n0 = GT(z, d0), n1 = GT(z, d1), fl = R32(d + 8) & 1;
    u128 a0 = spu_andc(d0, sm), a1 = spu_andc(d1, sm);
    const uint32_t e1 = E[1];
    u128 s0 = fl ? spu_csflt(SPLW(e1), 147) : z, s1 = fl ? spu_csflt(SPLW((e1 << 4) + (e1 >> 4)), 147) : z;
    u128 tt = E[2] ? SPLW(R32(IN + 0x160)) : z;
    u128 u0 = spu_fceq(a0, z)._u32[0] ? z : dv(spu_fm(s0, spu_fa(d3, dv(one, a0))), d4);
    u128 v0 = spu_fceq(a1, z)._u32[0] ? z : dv(spu_fm(s1, spu_fa(d3, dv(one, a1))), d5);
    u128 u = spu_fa(u0, tt), v = spu_fa(v0, tt), x, y;
    if (spu_fceq(d3, z)._u32[0]) { x = spu_fm(a0, u); y = spu_fm(a1, v); }
    else if (spu_fceq(a0, z)._u32[0]) { x = z; y = f_47E8(v, d3, d5, a1); }
    else if (spu_fceq(a1, z)._u32[0]) { x = f_47E8(u, d3, d4, a0); y = z; }
    else f_48C8(spu_fa(v0, u), d3, d4, a0, d3, d5, a1, &x, &y);
    if (n0) x = spu_xor(x, sm);
    if (n1) y = spu_xor(y, sm);
    *ox = spu_fs(x, IFL(x, 0))._u32[0]; *oy = spu_fs(y, IFL(y, 0))._u32[0];
}
/* 7C18 op 12 (0x9F38): x = rate * time + E[1]/256, less its nearest integer */
static uint32_t op12_phase(J* j, const uint8_t* E, uint32_t rate)
{
    const u128 sm = SPLW(0x80000000u);
    u128 x = spu_fma(SPLW(rate), SPLW(R32(IN + 0x160)), spu_csflt(SPLW(E[1]), 147));
    u128 ax = spu_andc(x, sm), x2 = spu_fa(ax, ax);
    u128 y = spu_fm(x, SPLW(0x3F7FF800u)); y = spu_fma(y, SPLW(0x39002000u), y);
    const int ok = spu_fcmgt(SPLW(0x7F800000u), x)._u32[0] && spu_fcmgt(SPLW(0x49800000u), y)._u32[0];
    u128 ay = spu_andc(y, sm), n = IFL(ay, 0), np = IFL(ay, 1), nm = IFL(ay, -1);
    u128 r = GT(ax, spu_fs(x2, n)) ? nm : n;
    if (!GT(ax, spu_fs(x2, np))) r = np;
    r = spu_or(spu_andc(r, sm), spu_and(y, sm));
    return ok ? spu_fs(x, r)._u32[0] : 0;
}
/* 0x7C18: per-object constants */
static void f_7C18(J* j, const uint8_t* mat, const uint8_t* E, const uint8_t* extra, uint32_t nx)
{
    uint32_t nops = R32(IN1 + 0xC0);
    if (nops > 20) nops = 20;
    if (R32(IN1 + 0xB4) > 16 || R32(IN1 + 0xB8) > 0x100) { j->fallback |= FB_STALE; return; }
    const uint8_t* d = mat + 8 + 44 * R32(IN1 + 0xB4) + 4 * R32(IN1 + 0xB8);
    for (uint32_t i = 0; i < nops; i++) {
        uint32_t op = R32(IN1 + 0xC4 + 4 * i);
        if (op > 14) continue;
        if (op == 0) {
            if (!inmat(j, mat, d, 24)) return;
            const uint8_t* dd = d; d += 24;
            if (R16(IN1 + 0xA4) == 0xFFFF) continue;    /* only -1 skips */
            uint32_t w[4] = { 0, 0, 0, 0 };
            op0_uv(j, E, dd, &w[0], &w[1]);
            emit_vec(j, S16(IN1 + 0xA4), w, 1);
        } else if (op == 12) {
            if (!inmat(j, mat, d, 4)) return;
            const uint32_t rate = R32(d); d += 4;
            if (S16(IN1 + 0xAC) < 0) continue;
            uint32_t w[4] = { op12_phase(j, E, rate), 0, 0, 0 };
            emit_vec(j, S16(IN1 + 0xAC), w, 1);
        } else if (op == 5) {
            if (!inmat(j, mat, d, 8)) return;
            int slot = S16(IN1 + 0xA8); int32_t a = (int32_t)R32(d), b = (int32_t)R32(d + 4); d += 8;
            if (slot < 0) continue;
            int32_t k = E[1], ab = (int32_t)((uint32_t)a * (uint32_t)b);
            if (!ab || !a) { j->fallback |= FB_OP5; continue; }
            int32_t frame = k % ab, row = frame / a, col = frame % a;
            u128 fa = spu_csflt(SPLW((uint32_t)a), 155), fb = spu_csflt(SPLW((uint32_t)b), 155);
            u128 ra = spu_fi(fa, spu_frest(fa)), rb = spu_fi(fb, spu_frest(fb));
            u128 fc = spu_csflt(SPLW((uint32_t)col), 155), fr = spu_csflt(SPLW((uint32_t)row), 155);
            u128 q1 = spu_fm(fc, ra); u128 x = spu_fma(spu_fnms(q1, fa, fc), ra, q1);
            u128 q2 = spu_fm(fr, rb); u128 y = spu_fma(spu_fnms(q2, fb, fr), rb, q2);
            uint32_t w[4] = { x._u32[0], y._u32[0], y._u32[0], y._u32[0] };
            u128 c = j->op5cache;
            if (spu_fceq(SPLW(w[0]), SPLW(c._u32[0]))._u32[0] && spu_fceq(SPLW(w[1]), SPLW(c._u32[1]))._u32[0]) continue;
            j->op5cache = V4(w[0], w[1], w[2], w[3]);
            emit_vec(j, slot, w, 1);
        } else if (op == 11) {
            if (!inmat(j, mat, d, 40)) return;
            int s3 = S16(IN1 + 0xAE), sB0 = S16(IN1 + 0xB0); uint32_t flag = R32(d); const uint8_t* dd = d; d += 40;
            if (s3 < 0 && sB0 < 0) continue;
            uint32_t m[12];
            const uint8_t* src = dd;
            if (flag != 0) {
                uint32_t key = R32(mat); const uint8_t* hit = 0;
                for (uint32_t q = 0; q < nx && extra; q++) {
                    uint32_t k0 = R32(extra + 64 * q);
                    if (k0 == key) { hit = extra + 64 * q; break; }
                    if (k0 == 0xFFFFFFFFu) hit = extra + 64 * q;
                }
                src = hit;
            }
            if (!src) { const uint32_t I[12] = { F1, 0, 0, 0, 0, F1, 0, 0, 0, 0, F1, 0 }; memcpy(m, I, sizeof m); }
            else for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) m[4 * r + c] = R32(src + 4 + 12 * r + 4 * c);
                m[4 * r + 3] = 0;
            }
            if (s3 >= 0) emit_vec(j, s3, m, 3);
            if (sB0 >= 0) j->fallback |= FB_OP11B;
        } else if (op == 4 || op == 14) {                 /* 0x9BA0: bind texture idx of IN+0x24C to unit A6 */
            if (!inmat(j, mat, d, 4)) return;
            const uint32_t idx = R32(d); d += 4;
            const int unit = S16(IN1 + 0xA6);
            if (unit < 0 || j->tex194 == idx) continue;   /* scr+0x194, -1 at job start */
            j->tex194 = idx;
            if (idx > 6) { j->fallback |= FB_OP4; return; }   /* past the 7 descriptors in IN */
            const uint8_t* t = IN + 0x24C + 44 * idx;
            uint32_t a = R32(t + 8);
            if (op == 14) a = (a & 0xFF0FFFFFu) | 0x00700000u;
            room(j, 60);
            W(j, 0x00201A00 + 32 * unit); W(j, R32(t)); W(j, R32(t + 4)); W(j, a);
            for (int q = 3; q < 8; q++) W(j, R32(t + 4 * q));
            W(j, 0x00041840 + 4 * unit); W(j, R32(t + 32)); W(j, 0x00040B00 + 4 * unit); W(j, R32(t + 36));
            W(j, 0x00041D00 + 4 * unit); W(j, R32(t + 40));
            if (S16(IN1 + 0xAC) >= 0) { uint32_t w[4] = { R32(IN + 0x2FC + 4 * idx) == 1 ? F1 : 0, 0, 0, 0 }; emit_vec(j, S16(IN1 + 0xAC), w, 1); }
        } else j->fallback |= FB_OP;
    }
}

/* object matrix (main 0x136E0..0x13924, 0x13CA8) */
static void object_matrix(J* j, u128 M[4], const uint8_t* rec, const uint8_t* T, const uint8_t* S)
{
    u128 T0 = LQ(T), T1 = LQ(T + 16), T2 = LQ(T + 32);
    u128 S0 = LQ(S), S1 = LQ(S + 16), S2 = LQ(S + 32), S3 = LQ(S + 48);
    u128 A[4];
    for (int c = 0; c < 4; c++)
        A[c] = spu_fma(S2, SPLW(T2._u32[c]), spu_fma(S1, SPLW(T1._u32[c]), spu_fm(S0, SPLW(T0._u32[c]))));
    A[3] = spu_fa(A[3], S3);
    u128 p = V4(R32(rec + 0x28), R32(rec + 0x2C), R32(rec + 0x30), 0);
    u128 sk = spu_fm(SPLW(R32(rec + 0x34)), LQ(IN + 0x340));
    u128 D[3] = { spu_fm(sk, V4(F1, 0, 0, 0)), spu_fm(sk, V4(0, F1, 0, 0)), spu_fm(sk, V4(0, 0, F1, 0)) };
    for (int r = 0; r < 3; r++)
        M[r] = spu_fma(A[2], SPLW(D[r]._u32[2]), spu_fma(A[1], SPLW(D[r]._u32[1]), spu_fm(A[0], SPLW(D[r]._u32[0]))));
    u128 t = spu_fma(A[2], SPL(p, 2), spu_fma(A[1], SPL(p, 1), spu_fm(A[0], SPL(p, 0))));
    u128 m3 = spu_fa(A[3], t); m3._u32[3] = F1;
    if (IN1[0x178]) { m3 = spu_fs(m3, LQ(IN + 0xC0)); m3._u32[3] = F1; }
    M[3] = m3;
}
static int m_changed(J* j, const u128 M[4])     /* xyz of the 4 rows against scr+0 */
{
    for (int r = 0; r < 4; r++) {
        u128 a = M[r], b = j->prevM[r]; a._u32[3] = a._u32[2]; b._u32[3] = b._u32[2];
        u128 m = spu_fceq(a, b);
        if (!(m._u32[0] && m._u32[1] && m._u32[2] && m._u32[3])) return 1;
    }
    return 0;
}
static void mvp(J* j, uint32_t out[16], const u128 M[4])
{
    u128 VP0 = LQ(IN + 0x80), VP1 = LQ(IN + 0x90), VP2 = LQ(IN + 0xA0), VP3 = LQ(IN + 0xB0);
    for (int r = 0; r < 4; r++) {
        u128 m = M[r]; uint32_t w = r == 3 ? F1 : 0;
        u128 acc = spu_fm(VP0, SPLW(m._u32[0]));
        acc = spu_fma(VP1, SPLW(m._u32[1]), acc);
        acc = spu_fma(VP2, SPLW(m._u32[2]), acc);
        acc = spu_fma(VP3, SPLW(w), acc);
        for (int i = 0; i < 4; i++) out[4 * r + i] = acc._u32[i];
    }
}
static inline u128 YZX(u128 v) { return V4(v._u32[1], v._u32[2], v._u32[0], v._u32[0]); }
static inline u128 CROSS(u128 a, u128 b) { return YZX(spu_fnms(b, YZX(a), spu_fm(a, YZX(b)))); }
/* The inverse of M's 3x3 (BDD8 prologue 0xBDD8..0xBFD4, B010 0xB510..): c[k] =
 * lane k of the cross products (M1 x M2, M2 x M0, M0 x M1); iv3 = 1/det in xyz.
 * The rows of the inverse are fm(c[k], iv3). */
static void minv(const u128 M[4], u128 c[3], u128* iv3)
{
    const u128 A = CROSS(M[1], M[2]), B = CROSS(M[2], M[0]), C = CROSS(M[0], M[1]), P = spu_fm(C, M[2]);
    for (int k = 0; k < 3; k++) c[k] = V4(A._u32[k], B._u32[k], C._u32[k], 0);
    const u128 det = spu_fa(spu_fa(SPLW(P._u32[0]), SPLW(P._u32[1])), SPLW(P._u32[2]));
    const u128 es = spu_fi(det, spu_frest(det)), iv = spu_fma(spu_fnms(es, det, SPLW(F1)), es, es);
    *iv3 = V4(iv._u32[0], iv._u32[0], iv._u32[0], 0);
}
/* Matrix kind 11 (B010 0xB510, BDD8 0xCB20): the rows of IN+0xD0 (3 directions)
 * in object space, normalized (0 below 0x0DA2425F), rows 0 and 1 times -1 if
 * IN1[0x179]. Type 0 negates rows 0 and 1 first. */
static void f_kind11(J* j, const u128 c[3], u128 iv3, int neg01)
{
    const u128 i0 = spu_fm(c[0], iv3), i1 = spu_fm(c[1], iv3), i2 = spu_fm(c[2], iv3);
    const u128 sgn = SPLW(IN1[0x179] ? 0xBF800000u : F1), sm = SPLW(0x80000000u);
    uint32_t w[12];
    for (int r = 0; r < 3; r++) {
        u128 Wr = LQ(IN + 0xD0 + 16 * r);
        u128 q = spu_fma(i2, SPL(Wr, 2), spu_fma(i1, SPL(Wr, 1), spu_fm(i0, SPL(Wr, 0))));
        if (neg01 && r < 2) q = spu_xor(q, sm);
        u128 sq = spu_fm(q, q), l = SPLW(spu_fa(spu_fa(SPLW(sq._u32[0]), SPLW(sq._u32[1])), SPLW(sq._u32[2]))._u32[0]);
        u128 rs = spu_fi(l, spu_frsqest(l));
        rs = spu_fma(spu_fnms(l, spu_fm(rs, rs), SPLW(F1)), spu_fm(rs, SPLW(0x3F000000u)), rs);
        u128 n = spu_fcgt(l, SPLW(0x0DA2425Fu))._u32[0] ? spu_fm(q, rs) : SPLW(0);
        if (r < 2) n = spu_fm(sgn, n);
        w[4 * r] = n._u32[0]; w[4 * r + 1] = n._u32[1]; w[4 * r + 2] = n._u32[2]; w[4 * r + 3] = 0;
    }
    emit_vec(j, SLOT(11), w, 3);
}
/* 0xBDD8: type-0 matrices */
static void f_BDD8(J* j, const u128 M[4])
{
    if (!m_changed(j, M)) return;
    for (int r = 0; r < 4; r++) j->prevM[r] = M[r];
    if (SLOT(0) >= 0) {
        uint32_t w[16];
        for (int r = 0; r < 4; r++) for (int i = 0; i < 4; i++) w[4 * r + i] = M[r]._u32[i];
        emit_vec(j, SLOT(0), w, 4);
    }
    if (SLOT(1) >= 0) { uint32_t w[16]; mvp(j, w, M); emit_vec(j, SLOT(1), w, 4); }
    u128 c[3], iv3; minv(M, c, &iv3);
    const u128 c0 = c[0], c1 = c[1], c2 = c[2];
    if (SLOT(2) >= 0) {                                 /* 0xC4E8: rows (A, B, C) / det, w a signed zero */
        const u128 nz = SPLW(0x80000000u);
        u128 t = spu_fma(spu_fma(c2, nz, spu_fma(c1, nz, spu_fm(c0, nz))), iv3, V4(0, 0, 0, F1));
        u128 r0 = spu_fm(c0, iv3), r1 = spu_fm(c1, iv3), r2 = spu_fm(c2, iv3);
        uint32_t w[12];
        for (int r = 0; r < 3; r++) {
            w[4 * r] = r0._u32[r]; w[4 * r + 1] = r1._u32[r]; w[4 * r + 2] = r2._u32[r]; w[4 * r + 3] = t._u32[r];
        }
        emit_vec(j, SLOT(2), w, 3);
    }
    if (SLOT(3) >= 0) {
        uint32_t w[12];
        for (int r = 0; r < 3; r++) {
            u128 v = M[r]; v._u32[3] = 0;
            u128 sq = spu_fm(v, v);
            u128 l = spu_fa(spu_fa(sq, V4(sq._u32[1], sq._u32[2], sq._u32[3], 0)), V4(sq._u32[2], sq._u32[3], 0, 0));
            l = SPL(l, 0);
            u128 rs = spu_fi(l, spu_frsqest(l));
            u128 nr = spu_fma(spu_fnms(l, spu_fm(rs, rs), SPLW(F1)), spu_fm(rs, SPLW(0x3F000000u)), rs);
            u128 o = spu_fm(v, nr); o._u32[3] = 0;
            for (int i = 0; i < 4; i++) w[4 * r + i] = o._u32[i];
        }
        emit_vec(j, SLOT(3), w, 3);
    }
    if (SLOT(10) >= 0) {                                /* 0xCA08: the point IN+0x110 in object space */
        const u128 nT = spu_xor(M[3], SPLW(0x80000000u));
        u128 r3 = spu_fma(spu_fma(c2, SPL(nT, 2), spu_fma(c1, SPL(nT, 1), spu_fm(c0, SPL(nT, 0)))), iv3, V4(0, 0, 0, F1));
        u128 p = LQ(IN + 0x110);
        u128 v = spu_fa(spu_fma(spu_fm(c2, iv3), SPL(p, 2), spu_fma(spu_fm(c1, iv3), SPL(p, 1), spu_fm(spu_fm(c0, iv3), SPL(p, 0)))), r3);
        emit_v(j, SLOT(10), v);
    }
    if (SLOT(11) >= 0) f_kind11(j, c, iv3, 1);
}
/* 0xB010: type-2 matrix and polygon offset; returns whether the offset is on */
static int f_B010(J* j, const u128 M[4], const uint8_t* E)
{
    if (SLOT(11) >= 0 || SLOT(1) >= 0) {
        if (m_changed(j, M)) {
            for (int r = 0; r < 4; r++) j->prevM[r] = M[r];
            if (SLOT(1) >= 0) { uint32_t w[16]; mvp(j, w, M); emit_vec(j, SLOT(1), w, 4); }
            if (SLOT(11) >= 0) { u128 c[3], iv3; minv(M, c, &iv3); f_kind11(j, c, iv3, 0); }
        }
    }
    if ((E[0] & IN1[0x10F] & 4) && IN[0x153] == 0) {
        room(j, 8); W(j, 0x00040A68); W(j, 1);
        room(j, 12); W(j, 0x00080A78); W(j, F1); W(j, 0x44FA0000);
        return 1;
    }
    return 0;
}
/* 0x5488: wait for idle, then method 0x1FD8 = 1 */
static void f_5488(J* j)
{
    room(j, 8); W(j, 0x00040110); W(j, 0);
    room(j, 8); W(j, 0x00041FD8); W(j, 1);
}
/* D6D8 0xEAF8 with 0x186C0, when IN1[9]: the record's box (+0x10 min, +0x1C
 * max) through M, against the previous object's (scr+0x40, zero at job
 * start). A strict overlap on x, y and z gets the barrier, counted in rec[6]. */
static void f_overlap(J* j, const u128 M[4], const uint8_t* rec)
{
    u128 lo = V4(R32(rec + 0x10), R32(rec + 0x14), R32(rec + 0x18), R32(rec + 0x18));
    u128 hi = V4(R32(rec + 0x1C), R32(rec + 0x20), R32(rec + 0x24), R32(rec + 0x24));
    u128 c = spu_fm(spu_fa(lo, hi), SPLW(0x3F000000u)), h = spu_fs(hi, c);
    const u128 sm = SPLW(0x80000000u);
    u128 wc = spu_fa(spu_fma(M[2], SPL(c, 2), spu_fma(M[1], SPL(c, 1), spu_fm(M[0], SPL(c, 0)))), M[3]);
    u128 e = spu_fma(SPL(h, 2), spu_andc(M[2], sm), spu_fma(SPL(h, 0), spu_andc(M[0], sm), spu_fm(SPL(h, 1), spu_andc(M[1], sm))));
    u128 mn = spu_fs(wc, e), mx = spu_fa(wc, e);
    u128 a = mx, b = j->box[0], q = j->box[1], d = mn;
    a._u32[3] = a._u32[2]; b._u32[3] = b._u32[2]; q._u32[3] = q._u32[2]; d._u32[3] = d._u32[2];
    u128 t1 = spu_fcgt(a, b), t2 = spu_fcgt(q, d);
    if (t1._u32[0] & t1._u32[1] & t1._u32[2] & t1._u32[3] & t2._u32[0] & t2._u32[1] & t2._u32[2] & t2._u32[3]) {
        f_5488(j); j->rec[6]++;
    }
    j->box[0] = mn; j->box[1] = mx;
}
/* D6D8 0xDAB0..0xE0B0: per-object vectors from E, H and IN by slots 12, 4, 17, 13, 14 */
static void f_side(J* j, const uint8_t* E, const uint8_t* H)
{
    if (SLOT(12) >= 0) {                               /* E[2]/64 in x; y..w left in the stack quad (j->stk, set before f_7C18) */
        j->stk._u32[0] = spu_csflt(SPLW(E[2]), 149)._u32[0];
        emit_v(j, SLOT(12), j->stk);
    }
    if (SLOT(4) >= 0) {
        const uint32_t f = spu_csflt(SPLW(E[2]), 149)._u32[0], nf = f ^ 0x80000000u;
        u128 v = (E[0] & 1) ? V4(F1, nf, nf, nf)
               : spu_fceq(SPLW(f), SPLW(F1))._u32[0] ? V4(0, F1, 0, 0) : V4(0xBF800000u, f, f, f);
        emit_v(j, SLOT(4), v);
    }
    if (SLOT(17) >= 0) emit_v(j, SLOT(17), SPLW(spu_csflt(SPLW(E[1]), 147)._u32[0]));   /* E[1]/256 */
    if (SLOT(13) >= 0) {                               /* sprite-sheet cell of frame H[8] in an a x b grid */
        const uint32_t a = R32(IN + 0x240), b = R32(IN + 0x244), ab = a * b;
        if (!ab) { j->fallback |= FB_SIDE; return; }   /* the SPU halts (heqi) on a zero divisor */
        const uint32_t fr = R32(H + 8) % ab, row = fr / a, col = fr % a;
        const u128 one = SPLW(F1), fa = spu_csflt(SPLW(a), 155), fb = spu_csflt(SPLW(b), 155);
        u128 ea = spu_fi(fa, spu_frest(fa)), eb = spu_fi(fb, spu_frest(fb));
        u128 qa = spu_fm(one, ea), qb = spu_fm(one, eb);
        u128 ia = spu_fma(spu_fnms(qa, fa, one), ea, qa), ib = spu_fma(spu_fnms(qb, fb, one), eb, qb);
        u128 x = spu_fm(ia, spu_cuflt(SPLW(col), 155)), y = spu_fm(ib, spu_cuflt(SPLW(row), 155));
        emit_v(j, SLOT(13), V4(x._u32[0], y._u32[0], ia._u32[0], ib._u32[0]));
        if (SLOT(14) >= 0) emit_v(j, SLOT(14), V4(R32(IN + 0x248), R32(H + 4), 0, 0));
    }
}
/* 0x5B60: vertex attribute 0 format */
static void f_5B60(J* j, uint32_t id)
{
    uint32_t v;
    switch (id) {
    case 0: case 2: v = 0x0C32; break;
    case 1: case 3: case 5: v = 0x0630 | id; break;
    default: return;
    }
    room(j, 8); W(j, 0x00041740); W(j, v);
}
/* 0x5C98: streams and the draw */
static void f_5C98(J* j, const uint8_t* rec)
{
    const uint8_t* L = IN1 + 0xD8;
    if (j->vfmt != R32(rec + 0x4C)) { f_5B60(j, R32(rec + 0x4C)); j->vfmt = R32(rec + 0x4C); }
    if (j->streams[0] != R32(rec + 0x38) || j->streams[1] != R32(rec + 0x3C) || j->streams[2] != R32(rec + 0x40)) {
        uint32_t n = L[0];
        if (n > 16) n = 16;
        room(j, 8 * n);
        for (uint32_t q = 0; q < n; q++) {
            uint32_t base = R32(rec + 0x38 + 4 * (L[17 + q] & 3));
            W(j, 0x00040000 | (0x1680 + 4 * L[1 + q])); W(j, base + L[33 + q]);
        }
        for (int i = 0; i < 3; i++) j->streams[i] = R32(rec + 0x38 + 4 * i);
    }
    uint32_t nidx = 3 * R16(rec + 4);
    if (nidx) {
        uint32_t nw = (nidx + 255) >> 8, ix = R32(rec + 0x44);
        room(j, 4 * (nw + 8));
        W(j, 0x0008181C); W(j, ix & 0x7FFFFFFF); W(j, 0x10 | (ix >> 31));
        W(j, 0x00041808); W(j, 5); W(j, 0x40001824 | (nw << 18));
        uint32_t full = nidx >> 8, i;
        for (i = 0; i < full; i++) W(j, 0xFF000000u + 256 * i);
        if (nidx & 255) W(j, (((nidx & 255) - 1) << 24) | ((256 * full) & 0x00FFFF00));
        W(j, 0x00041808); W(j, 0);
    }
}
/* 0x6FE0: stencil reference */
static void f_6FE0(J* j, const uint8_t* E)
{
    if ((IN1[0x10F] & 0xE0) == 0) return;
    uint32_t ref = E[0] & 0xE0;
    if (j->stencil == ref) return;
    room(j, 32);
    W(j, 0x000C0330); W(j, 0x207); W(j, ref); W(j, 0xFF);
    W(j, 0x000C0350); W(j, 0x207); W(j, ref); W(j, 0xFF);
    j->stencil = ref;
}

/* (deck) 0x18F10: float to half as the job does it (no rounding; |x| < 2^-15: +0; |x| >= 65536: 0x7BFF) */
static uint32_t f2h(uint32_t x)
{
    const uint32_t a = (x & 0x7FFFFFFFu) >> 16;
    if (a < 0x3800) return 0;
    const uint32_t v = a >= 0x4780 ? 0x7BFFu : (((x - 0x38000000u) << 3) >> 16) & 0x7FFFu;
    return v | ((x >> 16) & 0x8000u);
}
/* (deck) 0x4740 on its GETLLAR line: u16 count +0, u16 high mark +2, u16 flag +6, u16 capacity +8, u32 base +12.
 * Room: count + 1 (the mark too while the flag is 0), the slot base + 32 * count; full: 0, the line unchanged. */
static uint32_t slot_fn(uint8_t* l, void* arg)
{
    (void)arg;
    const uint32_t count = R16(l);
    if (!(R16(l + 8) > count)) return 0;
    const uint32_t c1 = (count + 1) & 0xFFFF;
    P16(l, c1);
    if (R16(l + 6) == 0) P16(l + 2, c1);
    return R32(l + 12) + (count << 5);
}
/* (deck) 0x69B0: the object's sub-draw list (types 1 and 3). n = rec+0x3C items of 80 bytes at rec+0x38. Each:
 * the object's stream bases from the item (L = IN1+0xD8), then a region of 16 * item[1] bytes reserved in the
 * stream (16-aligned, a JUMP-to-self at its start and at every 128-byte-aligned EA in it, the rest left as the
 * LS buffer is) and a 32-byte record of it PUT to a slot of the IN+0x14 pool; no slot: the region is dropped
 * (the cursor goes back). rec+4 bit 0x100 clear: first the transposed matrix to a 128-byte slot of the IN+0x20
 * counter (none in range: no items). The records are staged at out+0x20, the matrix at out+0x40 (j->img). */
static void f_69B0(J* j, const u128 M[4], const uint8_t* rec, const uint8_t* H)
{
    spu_context* ctx = j->ctx;
    if (!j->img_on) { memcpy(j->img, ctx->ls + ((j->out + 0x20u) & SPU_LS_MASK), sizeof j->img); j->img_on = 1; }
    uint8_t* const o = j->img - 0x20;                  /* o + x = out + x */
    const uint32_t list = R32(rec + 0x38);
    const int32_t n = (int32_t)R32(rec + 0x3C);
    j->wt = 10;                                        /* 0x6AC8: entry waits on tag r116 */
    if (!(R32(rec + 4) & 0x100)) {                     /* 0x7170 */
        for (int c = 0; c < 3; c++) for (int r = 0; r < 4; r++) P32(o + 0x40 + 16 * c + 4 * r, M[r]._u32[c]);
        const uint32_t ctr = R32(IN + 0x20), lim = R32(IN + 0x28);
        uint32_t m = 0;                                /* the dry build: assumed in range (its value is the committing build's) */
        if (!j->live) { if ((ctr & 3) || !ea_ok(ctr, 4)) { j->fallback |= FB_EA; return; } j->need_live = 1; }
        else { m = spu_native_atomic_add32(ctx, ctr, 128); if (m + 128 > lim || !m) return; }
        P32(o + 0x2C, m);
        if (j->live) {
            memcpy(ctx->ls + ((j->out + 0x40u) & SPU_LS_MASK), o + 0x40, 48);
            if (ea_ok(m, 48)) put_ls(j, j->out + 0x40u, m, 48);
        }
        j->rec[3] += 1; j->rec[4] += 128;
    } else { P32(o + 0x28, R32(H + 4)); P32(o + 0x2C, 0); }
    o[0x3A] = (R32(IN1 + 0x14) & 1) ? 1 : IN[0x150] ? 3 : 2;
    P32(o + 0x34, R32(IN + 0x310));
    o[0x3B] = H[1];
    P16(o + 0x38, j->half);
    P32(o + 0x30, R32(IN1 + 0x18));
    if (n <= 0) return;
    const uint8_t* L = IN1 + 0xD8;
    const uint32_t nl = L[0], pool = R32(IN + 0x14);
    if (!j->live) {                                    /* past what the model covers: the job reads stale LS */
        if (nl > 16 || n > 0x10000 / 80) { j->fallback |= FB_STALE; return; }
        for (uint32_t q = 0; q < nl; q++) if (L[17 + q] > 12) { j->fallback |= FB_STALE; return; }
        if ((list & 15) || (pool & 127) || !ea_ok(pool, 16)) { j->fallback |= FB_EA; return; }
        j->need_live = 1;
    }
    const uint8_t* items = G(j, list, 80u * (uint32_t)n);
    if (j->fallback) return;
    for (int32_t i = 0; i < n; i++) {
        const uint8_t* it = items + 80 * i;
        j->wt = 8;                                     /* 0x6C60: item i's GET, tag r108 */
        j->rec[2] += 1; j->rec[5] += R16(it + 10) / 3;
        if (nl) {
            room(j, 8 * nl);
            for (uint32_t q = 0; q < nl; q++) { W(j, 0x00040000 | (0x1680 + 4 * L[1 + q])); W(j, R32(it + 28 + 4 * L[17 + q]) + L[33 + q]); }
        }
        if (i > 0) j->wt = 10;                         /* 0x6EB0: the previous record's PUT, tag r116 */
        const uint32_t rsz = 16u * it[1];              /* 0x5068 */
        room(j, rsz + 12);
        if (j->fallback) return;
        const uint32_t saved = j->cursor;
        while (j->cursor & 15) W(j, 0);
        const uint32_t rea = j->blocks[j->nblocks - 1].ea + j->cursor;
        if (j->live && rsz)
            for (uint32_t e = rea; ; ) {
                P32(j->cb + j->cursor + ((e - rea) & ~3u), 0x20000000u | (j->delta + e));
                e = (e + 128) & ~127u;
                if (!(rea + rsz > e)) break;
            }
        j->cursor += rsz;
        P32(o + 0x20, rea);
        P16(o + 0x3C, R32(IN2 + 8));
        P16(o + 0x3E, ++j->ctr198);
        P32(o + 0x24, list + 80u * (uint32_t)i);
        const uint32_t slot = j->live ? spu_native_atomic_line(ctx, pool, slot_fn, 0) : 1;
        if (!slot) { j->cursor = saved; continue; }
        if (j->live) {
            memcpy(ctx->ls + ((j->out + 0x20u) & SPU_LS_MASK), o + 0x20, 32);
            if (ea_ok(slot, 32)) put_ls(j, j->out + 0x20u, slot, 32);
        }
    }
}

/* (deck) 0x18F50: half to float as the job does it (no denormal / inf / NaN cases) */
static uint32_t h2f(uint32_t h)
{
    const uint32_t w = h << 16;
    uint32_t r = (uint32_t)((int32_t)w >> 3) & ~0x70000000u;
    if (w & 0x7FFFFFFFu) r += 0x38000000u;
    return r;
}
/* (deck) 0x7AA0's length: x, y, z squared and summed as (x + y) + z, times the frsqest + Newton rsqrt of
 * it; 0 unless the sum is above 2^-126 */
static u128 len3(u128 v)
{
    const u128 sq = spu_fm(v, v);
    const u128 s = SPL(spu_fa(spu_fa(sq, V4(sq._u32[1], sq._u32[2], sq._u32[3], sq._u32[0])), V4(sq._u32[2], sq._u32[3], sq._u32[0], sq._u32[1])), 0);
    if (!spu_fcgt(s, SPLW(0x00800000u))._u32[0]) return SPLW(0);
    const u128 r = spu_fi(s, spu_frsqest(s));
    return spu_fm(spu_fma(spu_fnms(s, spu_fm(r, r), SPLW(F1)), spu_fm(r, SPLW(0x3F000000u)), r), s);
}
/* (deck) 0x7AA0 (type 1, slots 5..7): the object's distance. rec+4 bit 0x100: from IN+0x100 + IN+0xC0 to
 * the translation column of the 3x4 matrix at H+4 + 48 * H[2] (a GET on tag r108); else |M[3].xyz|. */
static u128 f_7AA0(J* j, const u128 M[4], const uint8_t* rec, const uint8_t* H)
{
    if (R32(rec + 4) & 0x100) {
        const uint32_t ea = R32(H + 4) + 48u * H[2];
        if (ea & 15) { j->fallback |= FB_EA; return SPLW(0); }
        const uint8_t* X = G(j, ea, 48);
        j->wt = 8;
        const u128 P = spu_fa(LQ(IN + 0x100), LQ(IN + 0xC0));
        return len3(spu_fs(P, V4(R32(X + 12), R32(X + 28), R32(X + 44), F1)));
    }
    u128 v = M[3]; v._u32[3] = F1;
    return len3(v);
}
/* (deck) 0x5F38 (type 1): slot 5's colour and slot 7's fade at distance d. c = (d - near) / (far - near)
 * clamped to [0, 1] (near / far = the halves at H+0x10 / H+0x12, or IN+0x230 / IN+0x234 when equal;
 * far - near < 1e-4: a step at near); slot 5 = lerp(H[8..11], H[12..15], c) / 255 * (4 * H[3] / 255),
 * slot 7.x = its w * H[3] / 255. H+8 and H+0xC both 0: zeros. */
static void f_5F38(J* j, const uint8_t* H, u128 d, u128* q5, uint32_t* x7)
{
    if (!(R32(H + 8) | R32(H + 0xC))) { *q5 = SPLW(0); *x7 = 0; return; }
    const u128 one = SPLW(F1), zero = SPLW(0), inv255 = SPLW(0x3B808080u);
    u128 nr = SPLW(h2f(R16(H + 0x10))), fr = SPLW(h2f(R16(H + 0x12)));
    if (spu_fceq(spu_fs(fr, nr), zero)._u32[0]) { nr = SPLW(R32(IN + 0x230)); fr = SPLW(R32(IN + 0x234)); }
    const u128 h3 = spu_fm(spu_csflt(SPLW(H[3]), 155), inv255);
    u128 A, B;
    for (int i = 0; i < 4; i++) {
        A._u32[i] = spu_cuflt(SPLW(H[8 + i]), 155)._u32[0];
        B._u32[i] = spu_cuflt(SPLW(H[12 + i]), 155)._u32[0];
    }
    const u128 dd = SPL(d, 0), span = spu_fs(fr, nr);
    const u128 step = spu_fcgt(dd, nr)._u32[0] ? one : zero;
    const u128 e = spu_fi(span, spu_frest(span));
    const u128 t = spu_fm(spu_fma(spu_fnms(e, span, one), e, e), spu_fs(dd, nr));
    u128 c = spu_fcgt(SPLW(0x38D1B717u), span)._u32[0] ? step : t;
    if (spu_fcgt(c, one)._u32[0]) c = one;
    if (spu_fcgt(zero, c)._u32[0]) c = zero;
    const u128 a = spu_fm(A, inv255);
    const u128 lerp = spu_fma(spu_fms(B, inv255, a), SPL(c, 0), a);
    *q5 = spu_fm(lerp, SPL(spu_fm(h3, SPLW(0x40800000u)), 0));
    *x7 = spu_fm(SPL(lerp, 3), SPL(h3, 0))._u32[0];
    (void)j;
}

/* The branches the model lacks that IN1 alone decides (section 10.2),
 * checked at the first object that passes the filter: before it nothing is
 * emitted, and a job none passes writes only its record, whatever the type
 * (11.1). Conservative: a job that might never reach one of them still goes
 * to the SPU. */
static void precheck(J* j, uint32_t type, int mat_on)
{
    if (mat_on) {
        uint32_t n = R32(IN1 + 0x9C);
        if (n > 17) n = 17;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t e = R32(IN1 + 0x58 + 4 * i), t = (e >> 10) & 31;
            if (t > 11) continue;
            if ((t == 0 || t == 2) && (e & 0x8000)) j->fallback |= FB_SRGB;
            else if (t > 4) j->fallback |= FB_CONST;
        }
        if ((int32_t)R32(IN1 + 0xC0) > 0) {
            uint32_t nops = R32(IN1 + 0xC0);
            if (nops > 20) nops = 20;
            for (uint32_t i = 0; i < nops; i++) {
                uint32_t op = R32(IN1 + 0xC4 + 4 * i);
                if (op > 14 || op == 5 || op == 0 || op == 12) continue;
                if (op == 11) { if (S16(IN1 + 0xB0) >= 0) j->fallback |= FB_OP11B; }
                else if (op == 4 || op == 14) continue;
                else j->fallback |= FB_OP;
            }
        }
    }
    if (type == 0) {
        static const int ks[] = { 43, 45, 7, 6, 5, 9, 8 };
        for (unsigned i = 0; i < sizeof ks / sizeof *ks; i++) if (SLOT(ks[i]) >= 0) j->fallback |= FB_SIDE;
        if (IN1[0xD4] ? 1 : IN1[0x17A]) j->fallback |= FB_FPUPLOAD;
        /* (deck) slot 12's y..w are main's r112 as 0x7C58's prologue saved it for this object; without that call
         * (no material ops) they are whatever an earlier callee left on the stack there: not modelled */
        if (SLOT(12) >= 0 && !(mat_on && (int32_t)R32(IN1 + 0xC0) > 0)) j->fallback |= FB_SIDE;
    }
    if (type == 1) {      /* (deck) 0x10E48: slot 6 (0x11080) and 8, 9 (0xA4A8) unmodelled, 0x62F0 as in type 0 */
        if (SLOT(6) >= 0 || SLOT(8) >= 0 || SLOT(9) >= 0) j->fallback |= FB_SIDE;
        if (IN1[0xD4] ? 1 : IN1[0x17A]) j->fallback |= FB_FPUPLOAD;
    }
}

/* main's per-item filter (0x13240..0x13368): 1 = the item is skipped */
static inline int item_reject(J* j, const uint8_t* E, const uint8_t* H)
{
    const uint32_t ew0 = R32(E), hw0 = R32(H);
    return ((hw0 & 0x40000000) && IN[0x151]) || (hw0 & 0x80000000) || (ew0 & 0x08000000) ||
           (IN1[0x0A] && ((ew0 >> 5) & 7) != R32(IN2 + 4));
}
/* (deck) An item's 16-byte E record as main holds it in r112 once it fetched the item's material (0x1346C, rotqbyi
 * 12: E+12 (the material EA), E+0, E+4, E+8). */
static inline u128 rot12(const uint8_t* E) { return V4(R32(E + 12), R32(E), R32(E + 4), R32(E + 8)); }
/* (deck) j->stk = main's r112 when it calls 0x7C58 for item k, which that call's prologue saves at main's sp - 0x210,
 * the type-0 handler's slot 12 quad. r112 is zero at the range's start (0x1311C) and becomes rot12(E) of each item whose
 * material main fetches (a passing item whose material EA differs from r112's x, 0x133A8). main's loop is pipelined:
 * it filters item k+1 and fetches its material (0x13130..0x13584) before it processes item k, so item k+1 counts. */
static __attribute__((noinline)) void r112_at_7C58(J* j, const uint8_t* items, uint32_t k, uint32_t cnt)
{
    j->stk = j->r112;
    if (k + 1 >= cnt) return;
    const uint8_t* E1 = G(j, R32(items + 8 * (k + 1)), 16);
    const uint8_t* H1 = G(j, R32(items + 8 * (k + 1) + 4), 48);
    if (!item_reject(j, E1, H1) && R32(E1 + 12) != j->r112._u32[0]) j->stk = rot12(E1);
}

/* The job's main, 0x12C50. */
static void run(J* j)
{
    uint32_t type = R32(IN1 + 4);
    const uint8_t* D = IN1 + 0x1C;
    const int mat_on = R32(D + 0x38) || R32(D + 0x80) || S16(IN1 + 0xA4) >= 0 || S16(IN1 + 0xA6) >= 0 ||
                       S16(IN1 + 0xA8) >= 0 || S16(IN1 + 0xAC) >= 0 || S16(IN1 + 0xAA) >= 0 ||
                       S16(IN1 + 0xAE) >= 0 || S16(IN1 + 0xB0) >= 0;

    /* (deck) IN2+0xC nonzero: main also drops each item with probability IN2[0xC] / 1025 (0x13CFC: the LCG at 0x19490,
     * its state in the module's LS); not modelled */
    if (R32(IN2 + 0xC)) { j->fallback |= FB_RAND; return; }
    /* 0x4328: range selection */
    uint32_t cur = R32(G(j, R32(IN + 4), 4));
    uint32_t n = (uint32_t)((int32_t)(cur - R32(IN + 0)) >> 9);
    if (n > 100) n = 100;
    j->nq = n;
    const uint8_t* Q = n ? G(j, R32(IN + 0), n * 512) : 0;
    if (j->fallback) return;
    uint32_t rb[100], re[100]; int front = 0, back = (int)n;
    uint32_t b = R32(IN1 + 0) & 0xFF;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t* r = Q + 512 * k;
        if (R32(r) != R32(IN + 0x2C) || R32(r + 4) != R32(IN + 0x30)) continue;
        int32_t s = R16(r + 0x0E + 2 * b), e = R16(r + 0x10 + 2 * b);
        if (IN1[0x0A] == 0) {
            int32_t N = (int32_t)R32(IN2), i = (int32_t)R32(IN2 + 4);
            if (N <= 0) { j->fallback |= FB_TYPE; return; }
            int32_t c = (e - s + N - 1) / N; int32_t s2 = (int32_t)((uint32_t)s + (uint32_t)c * (uint32_t)i); if (!(e > s2)) s2 = e;
            int32_t e2 = (int32_t)((uint32_t)s2 + (uint32_t)c); if (!(e > e2)) e2 = e; s = s2; e = e2;
        }
        if (s == e) continue;
        int idx = r[12] ? --back : front++;
        rb[idx] = R32(r + 8) + 8 * s; re[idx] = R32(r + 8) + 8 * e;
    }
    int nr = front;
    for (int i = back; i < (int)n; i++) { rb[nr] = rb[i]; re[nr] = re[i]; nr++; }
    if (!nr) return;

    j->ring_ea = R32(IN + 8); j->delta = R32(IN + 0xC); j->limit = R32(IN + 0x10);
    j->op5cache = SPLW(F1); j->streams[0] = j->streams[1] = j->streams[2] = 0x7FFFFFFF; j->stencil = 0xFFFFFFFF; j->tex194 = 0xFFFFFFFF;
    j->need_mat = SLOT(0) >= 0 || SLOT(1) >= 0 || SLOT(2) >= 0 || SLOT(3) >= 0 || SLOT(10) >= 0 || SLOT(11) >= 0;
    const uint32_t matsz = (R32(IN1 + 0xBC) + 15) & ~15u;
    j->matsz = matsz;
    int started = 0;

    for (int r = 0; r < nr; r++) {
        if (re[r] < rb[r] || re[r] - rb[r] > 0x100000) { j->bad_ea = rb[r]; j->bad_size = 0xF3; j->fallback |= FB_EA; return; }
        const uint8_t* items = G(j, rb[r], re[r] - rb[r]);
        if (j->fallback) return;                /* the zero stand-in is smaller than a range can be */
        uint32_t cnt = (re[r] - rb[r]) >> 3; j->nitems += cnt; j->nrange++; j->lastcnt = cnt;
        uint32_t last_mat = 0; const uint8_t* old_mat = 0; const uint8_t* mat = 0;
        j->r112 = SPLW(0);                      /* (deck) main's r112: rot12 of the last item whose material it fetched */
        int pend = -1, pend_t = 0; j->lt = 0; j->wo = 0;       /* the tag of the last wait of the lifted pipeline (see j204_main) */
        for (uint32_t k = 0; k < cnt; k++) {
            const uint8_t* E = G(j, R32(items + 8 * k), 16);
            const uint8_t* H = G(j, R32(items + 8 * k + 4), 48);
            const int reject = item_reject(j, E, H);
            /* the lifted loop waits item k's instance tag, then the data tag of the previous passing item */
            j->lt = k % 3; j->wo = 0;
            if (pend >= 0) { j->lt = pend % 3; j->wo = pend_t; }
            pend = reject ? -1 : (int)k;
            j->wt = 0;      /* (deck) the object's own last wait: 10 = the output tag (a block left, a record PUT), 8 = a sub-draw GET */
            if (reject) continue;
            if (!started) {
                started = 1;
                if (type > 3 || !(s_types >> type & 1)) { j->fallback |= FB_TYPE; return; }
                j->half = f2h(type == 3 ? R32(IN + 0x30C) : 0);
                precheck(j, type, mat_on);
                if (matsz > 0x300) { j->bad_ea = matsz; j->bad_size = 0xF2; j->fallback |= FB_EA; }
                if (j->fallback) return;
            }
            int changed = 0;
            if (mat_on && R32(E + 12) != last_mat) { mat = G(j, R32(E + 12), matsz); last_mat = R32(E + 12); changed = 1; j->r112 = rot12(E); }
            const uint8_t* rec = G(j, R32(E + 4) & ~15u, 0xB0);
            const uint8_t* T = G(j, R32(H + 0x14) + 48 * R16(E + 8), 48);
            const uint8_t* S = G(j, R32(H + 0x1C) + 64 * R16(E + 10), 64);
            uint32_t nx = R32(H + 0x20);
            if (nx > 16) { j->bad_ea = nx; j->bad_size = 0xF4; j->fallback |= FB_EA; return; }
            const uint8_t* extra = nx ? G(j, R32(H + 0x24), 64 * nx) : 0;
            if (j->fallback) return;
            if (mat_on) {
                if (changed) { f_143F0(j, mat); f_15E78(j, mat, old_mat); old_mat = mat; }
                if ((int32_t)R32(IN1 + 0xC0) > 0) {
                    /* (deck) no material fetched in this range yet (its EA 0): 0x7C58 reads a stale LS buffer */
                    if (!mat) { j->fallback |= FB_STALE; return; }
                    if (type == 0 && SLOT(12) >= 0) r112_at_7C58(j, items, k, cnt);
                    f_7C18(j, mat, E, extra, nx);
                }
            }
            u128 M[4]; object_matrix(j, M, rec, T, S);
            if (type == 0) {
                if (j->need_mat) f_BDD8(j, M);
                if (IN1[9]) f_overlap(j, M, rec);
                f_side(j, E, H);
                f_6FE0(j, E);
                f_5C98(j, rec);
            } else if (type == 1) {              /* (deck) 0x10E48 */
                if (j->need_mat) f_BDD8(j, M);
                if (SLOT(5) >= 0 || SLOT(7) >= 0) {   /* (slot 6 declined) */
                    u128 q5; uint32_t x7;
                    f_5F38(j, H, f_7AA0(j, M, rec, H), &q5, &x7);
                    if (j->fallback) return;
                    if (SLOT(5) >= 0) emit_v(j, SLOT(5), q5);
                    if (SLOT(7) >= 0) emit_v(j, SLOT(7), V4(x7, 0, 0, 0));
                }
                f_6FE0(j, E);
                f_69B0(j, M, rec, H);
            } else {                             /* 0xBCD0 (type 2), 0xBB60 (type 3, deck) */
                int po = f_B010(j, M, E);
                if (type == 2) f_5C98(j, rec); else f_69B0(j, M, rec, H);
                if (po) { room(j, 8); W(j, 0x00040A68); W(j, 0); }
            }
            if (type == 0 || type == 2) { j->rec[2] += 1; j->rec[5] += R16(rec + 4); }   /* types 1, 3: per sub-draw */
            pend_t = j->wt;
            if (j->fallback) return;
        }
        if (pend >= 0) { j->lt = pend % 3; j->wo = pend_t; }
    }
    j->rec[1] = j->has_block ? j->done + j->cursor - 4096 : 0xFFFFFFFCu;
    if (j->nblocks) {
        const uint32_t ov1 = j->novf;
        if (j->vfmt) f_5B60(j, 0);
        if (j->novf != ov1) j->wo = 10;
        W(j, 0x00020000);
        j->blocks[j->nblocks - 1].used = j->cursor;
    }
}
#undef IN
#undef IN1
#undef IN2

static void job_init(J* j, spu_context* ctx)
{
    memset(j, 0, offsetof(J, blocks));
    j->ctx = ctx;
    j->out = ctx->gpr[9]._u32[0] & SPU_LS_MASK & ~15u;
    if (j->out > SPU_LS_SIZE - 0x2080) j->fallback |= FB_EA;   /* (deck) the record, its staging and the two block buffers */
    uint32_t in = ctx->gpr[3]._u32[0] & SPU_LS_MASK & ~15u;
    if (in > SPU_LS_SIZE - 0x520) { j->fallback |= FB_EA; in = 0; }
    j->IN = ctx->ls + in; j->IN1 = j->IN + 0x380; j->IN2 = j->IN + 0x500;
}

static _Thread_local J* t_j;
static _Thread_local int t_bypass;
typedef void (*j204_fn)(spu_context*);
extern j204_fn spu_lookup(uint32_t addr, int image_id);

/* PS3RECOMP_NATIVE_J204=2 (the live check): the native builds its blocks without taking a ring slot or
 * writing anything, the lifted main then runs, and the blocks it PUT are compared with the native
 * ones (through each block's first JUMP/RETURN: the RSX never reads past it) and its 32-byte record
 * with the native's (the first block's address aside). Differences are logged; every job runs lifted. */
static unsigned long long s_cmp, s_bad;
static int j204_compare(J* j, spu_context* ctx, const uint8_t* rec_ls)
{
    uint32_t rec[8];
    for (int i = 0; i < 8; i++) rec[i] = R32(rec_ls + 4 * i);
    if (memcmp(rec + 1, j->rec + 1, 7 * 4) != 0 || (!rec[0]) != (!j->nblocks)) return 1;
    uint32_t ea = rec[0];
    for (uint32_t k = 0; k < j->nblocks; k++) {
        const Block* b = &j->blocks[k];
        if (!ea_ok(ea, 4096)) return 2;
        const uint8_t* g = vm_base + ea;
        if (memcmp(g, b->data, b->used) != 0) return 3;
        if (k + 1 == j->nblocks) break;
        const uint32_t tail = R32(g + b->used);
        ea = (tail & 0xE0000000u) == 0x20000000u ? (tail & 0x1FFFFFFFu) - j->delta : ea + 4096;
    }
    (void)ctx;
    return 0;
}
static int s_mode = -1, s_dbg, s_break;
static int s_stats_on;   /* (deck) J204_STATS set: the native's own cycles too */

static int j204_main(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_J204");
        s_mode = e ? atoi(e) : 1;
        s_dbg = getenv("J204_DEBUG") != 0;
        { const char* t = getenv("J204_TYPES"); if (t && *t) s_types = (unsigned)strtoul(t, 0, 0) & 0xFu; }
        s_break = getenv("PS3RECOMP_J204_BREAK") ? atoi(getenv("PS3RECOMP_J204_BREAK")) : 0;   /* checker self-test: a wrong block / record, every replay must fail */
        fprintf(stderr, "[j204] native render-list builder: %s\n", s_mode ? "on" : "off (PS3RECOMP_NATIVE_J204=0)");
    }
    if (!s_mode || t_bypass || (g_spu_jobrec_mode & SPU_JOBREC_RECORD)) return 0;   /* (deck) a recording holds the lifted job */
    if (!t_j) { t_j = (J*)calloc(1, sizeof(J)); if (!t_j) return 0; }
    J* j = t_j;
    const int replay = spu_native_replay_begin(ctx);
    if (s_dbg) { fprintf(stderr, "[j204] regs r3..r10 ="); for (int q = 3; q <= 10; q++) fprintf(stderr, " %08X", ctx->gpr[q]._u32[0]); fprintf(stderr, " sp %05X\n", ctx->gpr[1]._u32[0]); }
    static unsigned long long s_tsc_run, s_tsc_all, s_tsc_n, s_tsc_ty[4], s_acc_ty[4];
    const unsigned long long t_a = s_stats_on ? __builtin_ia32_rdtsc() : 0;
    job_init(j, ctx);
    run(j);
    const unsigned long long t_b = s_stats_on ? __builtin_ia32_rdtsc() : 0;
    if (s_dbg) fprintf(stderr, "[j204] D draws %u items %u ranges %u lastcnt %u type %u fb %X s12 %d nops %d rnd %u\n", j->rec[2], j->nitems, j->nrange, j->lastcnt, R32(j->IN1 + 4), j->fallback,
                       S16(j->IN1 + 0x110 + 2 * 12), (int32_t)R32(j->IN1 + 0xC0), R32(j->IN2 + 0xC));
    /* (deck) J204_STATS=<n>: every n calls, how many the native declined and why (the FB_* bits) */
    static int s_stats = -1;
    static unsigned s_why[FB_NBITS], s_calls, s_decl;
    if (__builtin_expect(s_stats < 0, 0)) { const char* e = getenv("J204_STATS"); s_stats = e ? atoi(e) : 0; s_stats_on = s_stats > 0; }
    if (s_stats) {
        const unsigned n = __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);
        static unsigned s_type[34];   /* FB_TYPE declines by job type (33: N <= 0 of a split range) */
        static unsigned s_dty[5][FB_NBITS], s_cty[5];   /* (deck) calls and declines by reason, per job type (4: 4 and up) */
        { const uint32_t ty = R32(j->IN1 + 4) < 4 ? R32(j->IN1 + 4) : 4;
          __atomic_add_fetch(&s_cty[ty], 1, __ATOMIC_RELAXED);
          for (int i = 0; i < FB_NBITS; i++) if (j->fallback >> i & 1) __atomic_add_fetch(&s_dty[ty][i], 1, __ATOMIC_RELAXED); }
        if (n % (unsigned)s_stats == 0)
            for (int t = 0; t < 5; t++) {
                fprintf(stderr, "[j204] type %d%s: %u calls, declined by reason:", t, t == 4 ? "+" : "", s_cty[t]);
                for (int i = 0; i < FB_NBITS; i++) if (s_dty[t][i]) fprintf(stderr, " bit%d=%u", i, s_dty[t][i]);
                fputc('\n', stderr);
            }
        if (j->fallback) {
            __atomic_add_fetch(&s_decl, 1, __ATOMIC_RELAXED);
            for (int i = 0; i < FB_NBITS; i++) if (j->fallback >> i & 1) __atomic_add_fetch(&s_why[i], 1, __ATOMIC_RELAXED);
            if (j->fallback & FB_TYPE) {
                const uint32_t ty = R32(j->IN1 + 4);
                __atomic_add_fetch(&s_type[ty == 0 || ty == 2 ? 33 : ty < 32 ? ty : 32], 1, __ATOMIC_RELAXED);
            }
        }
        if (n % (unsigned)s_stats == 0) {
            fprintf(stderr, "[j204] FB_TYPE by type:");
            for (int i = 0; i < 34; i++) if (s_type[i]) fprintf(stderr, " %d=%u", i, s_type[i]);
            fputc('\n', stderr);
        }
        /* (deck) a sample that crossed cores can come out negative (unsigned: huge): such samples are dropped */
        const int t_ok = t_a && t_b >= t_a && t_b - t_a < 100000000ull;
        if (!j->fallback && t_ok) { __atomic_add_fetch(&s_tsc_run, t_b - t_a, __ATOMIC_RELAXED); __atomic_add_fetch(&s_tsc_n, 1, __ATOMIC_RELAXED); }
        if (!j->fallback && t_ok && R32(j->IN1 + 4) < 4) {   /* (deck) by job type (the committing build of types 1/3 is added below) */
            __atomic_add_fetch(&s_tsc_ty[R32(j->IN1 + 4)], t_b - t_a, __ATOMIC_RELAXED); __atomic_add_fetch(&s_acc_ty[R32(j->IN1 + 4)], 1, __ATOMIC_RELAXED);
        }
        if (n % (unsigned)s_stats == 0) {
            fprintf(stderr, "[j204] accepted by type (calls, kcycles each):");
            for (int i = 0; i < 4; i++) fprintf(stderr, " %d=%llu/%.1f", i, s_acc_ty[i], s_acc_ty[i] ? s_tsc_ty[i] / 1000.0 / s_acc_ty[i] : 0.0);
            fputc('\n', stderr);
            fprintf(stderr, "[j204] native run: %.1f kcycles per accepted call; ", s_tsc_n ? s_tsc_run / 1000.0 / s_tsc_n : 0.0);
            (void)s_tsc_all;
            fprintf(stderr, "[j204] %u calls, declined %u:", n, s_decl);
            for (int i = 0; i < FB_NBITS; i++) if (s_why[i]) fprintf(stderr, " bit%d=%u", i, s_why[i]);
            fputc('\n', stderr);
        }
    }
    if (j->fallback) {
        static int nl;
        if ((j->fallback & FB_EA) && nl++ < 6)
            fprintf(stderr, "[j204] EA fallback: 0x%08X size 0x%X (type %u)\n", j->bad_ea, j->bad_size, R32(j->IN1 + 4));
        if (s_dbg && nl++ < 40) fprintf(stderr, "[j204] declined: reasons 0x%X type %u\n", j->fallback, R32(j->IN1 + 4));
        return 0;
    }
    const uint32_t out = j->out;
    if (out > SPU_LS_SIZE - 32) return 0;
    if (j->nblocks && (!ea_ok(j->ring_ea, 4) || j->limit < 4096)) return 0;   /* nothing written yet: the lifted job runs */
    if (j->need_live) {
        /* (deck) the committing build (types 1/3 with sub-draws): the same inputs, so no decline can come now */
        if (s_mode == 2) return 0;   /* the live check compares dry builds only */
        const unsigned long long t_c = s_stats_on ? __builtin_ia32_rdtsc() : 0;
        job_init(j, ctx); j->live = 1;
        run(j);
        if (__builtin_expect(j->fallback, 0)) {   /* the inputs changed between the builds (another processor wrote them) */
            static int nw;
            if (nw++ < 8) fprintf(stderr, "[j204] committing build met reasons 0x%X (type %u); its output stands\n", j->fallback, R32(j->IN1 + 4));
            if (j->nblocks && j->cb != j->spill) {   /* run() stopped early: the stream still ends in a RETURN */
                if (j->cursor > 4092) j->cursor = 4092;
                W(j, 0x00020000); j->blocks[j->nblocks - 1].used = j->cursor;
                j->rec[1] = j->done + j->cursor - 4096;
            }
        }
        const unsigned long long t_d = s_stats_on ? __builtin_ia32_rdtsc() : 0;
        if (t_c && t_d >= t_c && t_d - t_c < 100000000ull && t_a && t_b >= t_a && t_b - t_a < 100000000ull) {
            const unsigned long long d = t_d - t_c;
            __atomic_add_fetch(&s_tsc_run, d, __ATOMIC_RELAXED);
            if (R32(j->IN1 + 4) < 4) __atomic_add_fetch(&s_tsc_ty[R32(j->IN1 + 4)], d, __ATOMIC_RELAXED);
        }
    }
    if (s_mode == 2) {
        j204_fn fn = spu_lookup(EC5F_MAIN, ctx->resident_ovl);
        if (!fn) return 0;
        const int32_t si = (int32_t)ctx->image_id;
        const uint32_t r0 = ctx->gpr[0]._u32[0];
        t_bypass = 1;
        ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
        spu_img_restore(ctx, si);
        t_bypass = 0;
        const int bad = j204_compare(j, ctx, ctx->ls + out);
        s_cmp++;
        if (bad) {
            s_bad++;
            if (s_bad <= 8) fprintf(stderr, "[j204] job differs (%d, type %u): lifted record %08X %08X %08X, native %08X %08X %08X, %u blocks\n",
                                    bad, R32(j->IN1 + 4), R32(ctx->ls + out), R32(ctx->ls + out + 4), R32(ctx->ls + out + 8), j->rec[0], j->rec[1], j->rec[2], j->nblocks);
        }
        if ((s_cmp & 1023) == 0) fprintf(stderr, "[j204] compared %llu jobs, %llu differ\n", s_cmp, s_bad);
        ctx->pc = ctx->gpr[0]._u32[0];
        return 1;
    }
    if (s_break && j->nblocks) j->blocks[0].data[8] ^= 1;
    { const unsigned long long t_p = s_stats_on ? __builtin_ia32_rdtsc() : 0;
      if (j->nblocks) publish(j);
      if (s_stats_on) { static unsigned long long s_tsc_pub, s_pub_n, s_blk; s_tsc_pub += __builtin_ia32_rdtsc() - t_p; s_blk += j->nblocks;
          if (!(++s_pub_n & 0x3FFFF)) fprintf(stderr, "[j204] publish: %.1f kcycles per call, %.2f blocks per call\n", s_tsc_pub / 1000.0 / s_pub_n, (double)s_blk / s_pub_n); } }
    if (s_break == 2) j->rec[2] ^= 1;
    for (int i = 0; i < 8; i++) P32(ctx->ls + out + 4 * i, j->rec[i]);
    if (j->img_on) memcpy(ctx->ls + ((out + 0x20u) & SPU_LS_MASK), j->img, sizeof j->img);   /* (deck) out+0x20..0x6F as the job leaves it */
    /* the tag wait the lifted main ends in: the tag of its last object's slot (the job's two tags r5/r6 alternate), r4 when it fetched none.
     * The wrapper's later tag-status read depends on the mask it leaves. */
    {
        if (s_dbg) fprintf(stderr, "[j204] draws %u items %u ranges %u lastcnt %u type %u\n", j->rec[2], j->nitems, j->nrange, j->lastcnt, R32(j->IN1 + 4));
        /* the job's three tags r4, r5, r6 rotate over the items; wo: the object's last wait was on r10 (the output tag) or r8 (a sub-draw GET) */
        const uint32_t tg = j->wo ? ctx->gpr[j->wo]._u32[0] : ctx->gpr[4 + (j->nrange ? j->lt : 0)]._u32[0];
        const uint32_t mask = 1u << (tg & 31);
        if (replay) { spu_wrch_raw(ctx, MFC_WrTagMask, mask); spu_wrch_raw(ctx, MFC_WrTagUpdate, 1); (void)spu_rdch_raw(ctx, MFC_RdTagStat); }
        else { u128 m = {0}, one = {0}; m._u32[0] = mask; one._u32[0] = 1; spu_wrch(ctx, MFC_WrTagMask, m); spu_wrch(ctx, MFC_WrTagUpdate, one); (void)spu_rdch(ctx, MFC_RdTagStat); }
    }
    /* what the lifted main leaves in the local store and no later code reads: its callees' frames,
     * the scratch buffers, the atomic line. (deck) out+0x20..0x7F is left as the job leaves it (j->img) and
     * compared, and so are the block buffers out+0x80..0x2080 for every type (each block is staged in the LS
     * buffer the job builds it in). */
    if (replay) {
        const uint32_t sp = ctx->gpr[1]._u32[0] & SPU_LS_MASK;
        spu_native_dead_ls(sp - 0x1800, sp);
        spu_native_dead_ls(0x80, 0x100);
        spu_native_dead_ls(0x19AC0, 0x19AE0);   /* the staging quads of the two-vector constant burst (0x16A54) */
        spu_native_dead_ls(0x27000, 0x28800 + 512 * j->nq + 0x400);
        spu_native_dead_ls(0x2C000, 0x2C800);
        spu_native_replay_end(ctx, 1);
    }
    return 1;
}

__attribute__((constructor)) static void j204_register(void)
{
    spu_native_register(EC5F_IMAGE, EC5F_MAIN, j204_main);
}
