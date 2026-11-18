/* job208.c -- job module 208 (jobmod_7393701cbd39_e1390), the flat instance culler (210's sibling), natively.
 *
 * Kicked per light / view next to 210 (0x12026C from 0x2ED354). A flat list of entries, each naming three guest
 * blocks: a set of boxes with an item range each plus the item table; one 8-byte record per box (its LOD levels
 * and a flag); the entry's 4x4 transform. Every box goes through the transform and is culled exactly as in 210
 * (cull_core.h). For a visible box, each item in its range whose level range admits the box's level goes out as
 * (item base + 16 x index, instance word), keyed by one of four bucket bytes the header's mode picks. The output
 * is 210's: a 512-byte header with 248 bucket ends plus the bucket-sorted 8-byte list, from two atomic cursors.
 *
 * The same job as ThiagoFraga761/infamous2-pc's job207 (US overlay 207; work/port/job207_native.c, MIT, Copyright
 * (c) 2026 the infamous2-recomp contributors; rpi/job207-native/RESEARCH.md is the spec): worker 0x4030 (entered by
 * a tail branch from the wrapper 0x4B48), BSP 0x4ED8, transform 0x5270, sort 0x4D08. Here the whole job runs on
 * guest memory, checked against this port's recordings.
 *
 * Inputs: set 1 = a DMA list: the 0x50-byte header, the BSP (32 x count), the entry list (28 bytes each);
 * set 3 = the gather buffer the three per-entry GETs fill (+0 boxes and items, +0x4000 box records, +0x8000 the
 * transform), item scratch after it; set 2 = the output (local store only). A read the job makes past what its
 * own GET of that block brought in sees stale local store on the SPU: CF_RANGE (declined). */
#include "cull_core.h"
#include <stdio.h>

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,fma,sse4.1,bmi,bmi2,popcnt"))), apply_to = function)
#endif

extern int g_spu_edge_vp_on;

static const char* const s_reasons[] = { CULL_REASONS_COMMON, NULL };

typedef struct J {
    wws_njob* nj;
    uint32_t fallback;
    cull_view V;
    cull_out out;
} J;

static void fb(J* j, uint32_t why, uint32_t ea, uint32_t size)
{
    if (!j->fallback) { j->nj->bad_ea = ea; j->nj->bad_size = size; }
    j->fallback |= why;
}
static inline int32_t SX4(uint32_t v) { return (int32_t)(v << 28) >> 28; }

/* one of the entry's three GETs: what the MFC refuses (the SPU then reads stale LS) or rewrites declines */
static const uint8_t* get_block(J* j, uint32_t ea, uint32_t sz)
{
    if (!sz) return (const uint8_t*)"";                 /* no GET: the slot keeps stale bytes, every read declines */
    if (sz > 0x4000u || (ea & 15)) { fb(j, CF_GET, ea, sz); return NULL; }
    const uint8_t* g = j->nj->read(j->nj, ea, sz);
    if (!g) { fb(j, CF_EA, ea, sz); return NULL; }
    static const uint8_t edge_sig[16] = { 0, 0, 0, 0, 0x05, 0x00, 0x02, 0xD0, 0xBF, 0x80, 0, 0, 0x3F, 0x80, 0, 0 };
    if (sz == 0x80 && __atomic_load_n(&g_spu_edge_vp_on, __ATOMIC_RELAXED) && !memcmp(g, edge_sig, 16)) { fb(j, CF_GET, ea, sz); return NULL; }
    return g;
}

static inline void emit(J* j, uint32_t bucket, uint32_t w2, uint32_t w3)
{
    cull_out* o = &j->out;
    o->ib[o->n] = (uint8_t)bucket; o->iw2[o->n] = w2; o->iw3[o->n] = w3;
    o->n++;
    if (o->n == CULL_ITEMS) cull_close_batch(o, 1, &j->fallback);   /* 0x49B8: right after the 1,792nd */
}

/* the worker 0x4030 */
static void walk(J* j, const uint8_t* H, const uint8_t* ents, uint32_t nent)
{
    const uint32_t mode = R32(H + 0x24);
    for (uint32_t e = 0; e != nent; e++) {
        if (j->fallback) return;
        const uint8_t* E = ents + 28 * e;
        const uint32_t ea1 = R32(E + 16), sz1 = (R16(E + 26) + 15) & ~15u;
        const uint32_t ea2 = R32(E + 20), sz2 = ((R16(E + 24) << 3) + 15) & ~15u;
        const uint32_t ea3 = R32(E + 8);
        const uint8_t* g1 = get_block(j, ea1, sz1);
        const uint8_t* g2 = g1 ? get_block(j, ea2, sz2) : NULL;
        const uint8_t* g3 = g2 ? get_block(j, ea3, 64) : NULL;
        if (!g3) return;
        if (sz1 < 8) { fb(j, CF_RANGE, ea1, sz1); return; }
        const uint32_t nb = R32(g1), total = R32(g1 + 4);
        const uint32_t w12 = R32(E + 12), w4 = R32(E + 4);
        if (!nb) continue;
        const uint64_t T = 8 + 28ull * nb;                    /* the item table after the boxes */
        X4 M;
        for (int r = 0; r < 4; r++) {
            M.x[r] = LQ(g3 + 16 * r);
            if (FBAD(M.x[r], 7)) { j->fallback |= CF_FLOAT; return; }
        }
        for (uint32_t i = 0; nb > i; i++) {
            const uint64_t B = 8 + 28ull * i;
            if (B + 28 > sz1) { fb(j, CF_RANGE, ea1, sz1); return; }
            const uint8_t* bp = g1 + B;
            const F4 mn = F4W(R32(bp), R32(bp + 4), R32(bp + 8), R32(bp + 8));
            const F4 mx = F4W(R32(bp + 12), R32(bp + 16), R32(bp + 20), R32(bp + 20));
            if (FBAD(mn, 7) || FBAD(mx, 7)) { j->fallback |= CF_FLOAT; return; }
            if (!cull_box(&j->V, mn, mx, &M, &j->fallback)) { if (j->fallback) return; continue; }
            if (j->fallback) return;
            /* items [start, end): end is the next box's start, or the total */
            const uint32_t start = (uint32_t)(int32_t)(int16_t)R16(bp + 26);
            uint32_t end = total;
            if (nb > i + 1) { if (B + 56 > sz1) { fb(j, CF_RANGE, ea1, sz1); return; } end = (uint32_t)(int32_t)(int16_t)R16(bp + 54); }
            if (!(end > start)) continue;
            if (8ull * i + 8 > sz2) { fb(j, CF_RANGE, ea2, sz2); return; }
            const uint8_t* P8 = g2 + 8 * i;
            const int32_t v2 = (int8_t)P8[2];                 /* the box's level */
            for (uint32_t k = start; end > k; k++) {
                const uint64_t I = T + 16ull * k;
                if (I + 16 > sz1 || I + 3 + mode >= sz1) { fb(j, CF_RANGE, ea1, sz1); return; }
                const uint8_t* ip = g1 + I;
                const uint32_t bucket = ip[3 + mode];
                if (bucket == 255) continue;
                const uint32_t rb = ip[2];
                const int32_t lo = SX4(rb), hi = SX4(rb >> 4);   /* the item's level range: hi <= v <= lo */
                int take;
                if (mode == 2) take = !(v2 > lo || hi > v2);
                else if (mode == 3) take = (hi <= 6 && lo > 5) || ip[9];
                else if (v2 <= lo && v2 >= hi) take = 1;
                else {
                    const int32_t v1 = (int8_t)P8[1];
                    take = P8[0] == 1 && !(v1 > lo) && !(hi > v1);
                }
                if (!take) continue;
                const int32_t idx = (int16_t)R16(ip);
                emit(j, bucket, w12 + ((uint32_t)idx << 4), w4);
                if (j->fallback) return;
            }
        }
    }
    if (j->fallback) return;
    cull_close_batch(&j->out, 2, &j->fallback);
}

static NJ_TLS J* t_j;

static int run208(wws_njob* nj)
{
    if (!t_j) { t_j = (J*)calloc(1, sizeof(J)); if (!t_j) { nj->fallback = CF_MEM; return 0; } }
    J* j = t_j;
    j->nj = nj; j->fallback = 0;
    cull_out_reset(&j->out);
    const wws_nbuf* b1 = &nj->buf[1];
    const wws_nbuf* b3 = &nj->buf[3];
    if (!b1->used || !b1->list || b1->size < 24 || b1->size > 64 || !b3->used || b3->pages * 1024u < 0x8040u + 16u * CULL_ITEMS) {
        nj->fallback = CF_LAYOUT; return 0;
    }
    const uint8_t* L = nj->read(nj, b1->ea, b1->size);
    if (!L) { nj->fallback = CF_EA; nj->bad_ea = b1->ea; nj->bad_size = b1->size; return 0; }
    uint32_t esz[3], eea[3]; int ne = 0;
    for (uint32_t o = 0; o + 8 <= b1->size; o += 8) {
        const uint32_t sz = R32(L + o) & 0x7FFF, ea = R32(L + o + 4);
        if (!sz) continue;
        if (ne == 3) { nj->fallback = CF_LAYOUT; return 0; }
        esz[ne] = sz; eea[ne] = ea; ne++;
    }
    if (ne < 1 || esz[0] != 0x50 || (eea[0] & 15)) { nj->fallback = CF_LAYOUT; return 0; }
    const uint8_t* H = nj->read(nj, eea[0], 0x50);
    if (!H) { nj->fallback = CF_EA; nj->bad_ea = eea[0]; nj->bad_size = 0x50; return 0; }
    const uint32_t nplanes = R32(H + 0x10), nent = R32(H + 0x14);
    if (!nent) return 1;                                  /* 0x41BC: the output header's words in LS, nothing PUT */
    /* the worker's planes at buffer +0x50, the entry list at align16(+0x50 + 32 x count) */
    if (ne != 3 || esz[1] != 32u * nplanes || !nplanes || nplanes > 0x800 || ((eea[1] | eea[2]) & 15) ||
        28ull * nent > esz[2]) { nj->fallback = CF_LAYOUT; return 0; }
    const uint8_t* bsp = nj->read(nj, eea[1], esz[1]);
    const uint8_t* ents = nj->read(nj, eea[2], esz[2]);
    if (!bsp || !ents) { nj->fallback = CF_EA; nj->bad_ea = bsp ? eea[2] : eea[1]; nj->bad_size = bsp ? esz[2] : esz[1]; return 0; }
    const F4 cam = LQ(H), h30 = LQ(H + 0x30), h40 = LQ(H + 0x40);
    if (FBAD(cam, 7) || FBAD(h30, 12) || FBAD(h40, 3)) { nj->fallback = CF_FLOAT; return 0; }
    j->V.cam = cam; j->V.minsize2 = FLANE(h30, 2); j->V.ratio = FLANE(h30, 3);
    j->V.hscale = FLANE(h40, 0); j->V.cliph = FLANE(h40, 1); j->V.clip = H[0x48] != 0;
    j->V.bsp.p = bsp; j->V.bsp.nbytes = esz[1]; j->V.bsp.n = nplanes;
    cull_bsp_prepare(&j->V.bsp);
    cull_cursors cc = { R32(H + 0x28), R32(H + 0x2C), R32(H + 0x30), R32(H + 0x34) };
    /* the output header as 0x4134..0x418C leaves it: w0 = hdr+0x1C, w1 = hdr+0x20, byte 12 = hdr+0x49 */
    memset(j->out.hdr0, 0, 16);
    P32(j->out.hdr0, R32(H + 0x1C)); P32(j->out.hdr0 + 4, R32(H + 0x20)); j->out.hdr0[12] = H[0x49];
    const unsigned csr = cull_float_enter();
    walk(j, H, ents, nent);
    if (cull_float_leave(csr)) j->fallback |= CF_FLOAT;
    if (j->fallback) { nj->fallback = j->fallback; return 0; }
    if (j->out.nbatch) {
        if (!nj->read(nj, cc.cur1 & ~127u, 128) || !nj->read(nj, cc.cur2 & ~127u, 128)) {
            nj->fallback = CF_EA; nj->bad_ea = cc.cur1; nj->bad_size = 128; return 0;
        }
        cull_publish(nj, &j->out, &cc);
    }
    return 1;
}

/* verify mode: the lifted job's buffer 2 (+0x80 the last batch's header, +0x280 its list) against the capture */
static int verify208(wws_njob* nj, const uint8_t* ls, const uint32_t buf_ls[WWS_NJOB_SETS])
{
    (void)nj;
    J* j = t_j;
    if (!j || !j->out.nbatch) return 0;
    const uint32_t b2 = buf_ls[2];
    if (!b2 || b2 + 0x280 + 14336 > 0x40000) return 9;
    const cull_batch* b = &j->out.batch[j->out.nbatch - 1];
    const uint8_t* o = ls + b2;
    if (memcmp(o + 0x80, b->hdr, 8) || o[0x8C] != b->hdr[12] || memcmp(o + 0x8E, b->hdr + 14, 2)) return 2;
    if (R32(o + 0x88) && memcmp(o + 0x90, b->hdr + 0x10, 496)) return 3;
    if (memcmp(o + 0x280, b->list, 8 * b->n)) return 4;
    return 0;
}

const wws_nmod wws_nmod_208 = { 208, "flat instance culler", run208, verify208, s_reasons, 1u << 1 };

#if defined(__clang__)
#pragma clang attribute pop
#endif
