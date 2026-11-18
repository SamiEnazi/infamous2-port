/* job209.c -- job module 209 (jobmod_867e48f161e0_e10e8), the LOD crossfade states (208's sibling), natively.
 *
 * Kicked from 0x1205D8 (0x2ECF74 in the world update). Its entries are 208's (28 bytes: +8 the transform EA,
 * +12 the item base, +16 block 1 = boxes and the item table, +20 block 2 = an 8-byte state record per box,
 * +24 its box count, +26 block 1's bytes). No visibility here: per box, the distance from the camera to the
 * transformed box picks a LOD band (the header's thresholds), and the box's record runs a crossfade state
 * machine: {+0 state (0xFF new, 0 stable, 1 fading), +1 the band faded from, +2 the band, +4 the frame the fade
 * started}. A box whose record changed (or is fading) gets its items updated in guest memory: each item (16
 * bytes at item base + 16 x index) is gathered by a DMA list, gets byte 0 bit 0 = "fading out" and byte 2 = its
 * alpha (0..64 over 720 frames), and is written back by a DMA list. The record block is written back per entry.
 * (The name "light render list" in the job catalogue came from the kicker's label.)
 *
 * Read from the lifted module (worker 0x4030, box transform 0x4FC8; main 0x4DA0 takes the two buffers and two
 * DMA tags). The worker double-buffers the entries (slot = buffer 2 + 0x8A40 x (entry & 1); the next entry's
 * three GETs go out before an entry is worked on), gathers into its slot and works in place. This model keeps
 * the job's program order of guest reads and writes: a private copy of each slot, the GETs at the same points,
 * every write logged in order (and seen by the job's later reads, as the DMA engine runs them in order), and
 * the log PUT to guest memory only when the whole job ran on a modelled path.
 *
 * Floats: the box transform and the distance squared on host floats under round-toward-zero + FTZ + DAZ (as
 * cull_core.h); the square root (frsqest, fi and a Newton step) on the SPU's own exact helpers
 * (runtime/spu/spu_helpers.h) under the default MXCSR; the fade time (csflt, fm, cflts) on host floats.
 * Fallbacks (nothing written, the lifted job runs): CF_* of cull_core.h, BAND (the threshold scan runs past the
 * header and the entries the job could read), ITEMS (a box with more than 64 items: the job's list would run
 * into its own gather buffer), ALIGN (an item EA not 16-aligned: the gather would place it elsewhere), LOG (more
 * writes than the model keeps). */
#include "cull_core.h"
#include <stdio.h>

extern int g_spu_edge_vp_on;

enum { FB_BAND = CF_MODULE, FB_ITEMS = CF_MODULE << 1, FB_ALIGN = CF_MODULE << 2, FB_LOG = CF_MODULE << 3 };
static const char* const s_reasons[] = { CULL_REASONS_COMMON, "BAND", "ITEMS", "ALIGN", "LOG", NULL };

#define SLOT_BYTES 0x8A40u
#define LOG_BYTES  (256u * 1024u)
#define LOG_MAX    4096u

typedef struct { uint32_t ea, size, off; } wlog;
typedef struct {
    uint8_t g[SLOT_BYTES];        /* the slot: block 1 +0, block 2 +0x4000, block 3 +0x8000, items gathered +0x8640 */
    uint32_t sz1, sz2, sz3;       /* the bytes the blocks' GETs brought (a read past them is stale local store) */
} slot_t;

typedef struct J {
    wws_njob* nj;
    uint32_t fallback;
    const uint8_t *hdr, *recs; uint32_t recs_len;
    slot_t slot[2];
    wlog log[LOG_MAX]; uint32_t nlog, logbytes;
    uint8_t logb[LOG_BYTES];
} J;

/* frsqest and fi: the SPU's estimate and interpolation, exact integer forms (runtime/spu/spu_helpers.h) */
#include "../../../runtime/spu/spu_helpers.h"
static uint32_t rsq_estimate(uint32_t d2)
{
    u128 v; v._u32[0] = v._u32[1] = v._u32[2] = v._u32[3] = d2;
    return spu_fi(v, spu_frsqest(v))._u32[0];
}

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,fma,sse4.1,bmi,bmi2,popcnt"))), apply_to = function)
#endif

/* the distance d from d2 (0x45A4..0x45BC): y = fi(frsqest(d2)), one Newton step (fm, fnms, fma: host floats
 * under the walk's round-toward-zero, fused where the SPU fuses), d = y' * d2 */
static uint32_t dist_from_d2(uint32_t d2)
{
    const F4 v = F4W(d2, 0, 0, 0);
    const uint32_t yw = rsq_estimate(d2);
    const F4 y = F4W(yw, 0, 0, 0);
    const F4 r19 = FMUL(y, y);
    const F4 r18 = FMUL(y, F4W(FH, 0, 0, 0));
    const F4 r16 = _mm_fnmadd_ps(v, r19, F4W(F1, 0, 0, 0));   /* 1 - d2 * y^2, one rounding */
    const F4 r14 = FMA_(r16, r18, y);
    return FLANE(FMUL(r14, v), 0);
}

static void fb(J* j, uint32_t why, uint32_t ea, uint32_t size)
{
    if (!j->fallback) { j->nj->bad_ea = ea; j->nj->bad_size = size; }
    j->fallback |= why;
}

/* buffer 1 as the job sees it: the 96-byte header, the entries right after it */
static int b1_ok(const J* j, uint32_t off, uint32_t n) { return (uint64_t)off + n <= 0x60u + j->recs_len; }
static inline const uint8_t* b1(const J* j, uint32_t off) { return off < 0x60u ? j->hdr + off : j->recs + (off - 0x60u); }

/* guest bytes as the job's GET sees them: guest memory, then every write of the job so far, in order */
static int read_guest(J* j, uint32_t ea, uint32_t size, uint8_t* dst)
{
    const uint8_t* g = j->nj->read(j->nj, ea, size);
    if (!g) { fb(j, CF_EA, ea, size); return 0; }
    memcpy(dst, g, size);
    for (uint32_t i = 0; i < j->nlog; i++) {
        const wlog* w = &j->log[i];
        const uint32_t lo = w->ea > ea ? w->ea : ea, hi = (w->ea + w->size < ea + size) ? w->ea + w->size : ea + size;
        if (lo < hi) memcpy(dst + (lo - ea), j->logb + w->off + (lo - w->ea), hi - lo);
    }
    return 1;
}
static void put_log(J* j, uint32_t ea, const uint8_t* src, uint32_t size)
{
    if (!size) return;
    if (j->nlog == LOG_MAX || j->logbytes + size > LOG_BYTES) { j->fallback |= FB_LOG; return; }
    wlog* w = &j->log[j->nlog++];
    w->ea = ea; w->size = size; w->off = j->logbytes;
    memcpy(j->logb + j->logbytes, src, size);
    j->logbytes += size;
}

/* the entry's three GETs into its slot (0x4118.. / 0x42F8..): what the MFC refuses or rewrites declines */
static int get_block(J* j, uint8_t* dst, uint32_t ea, uint32_t sz, uint32_t* got)
{
    *got = 0;
    if (!sz) return 1;                                  /* no GET: the slot keeps stale bytes (every read of them declines) */
    if (sz > 0x4000u || (ea & 15)) { fb(j, CF_GET, ea, sz); return 0; }
    if (!read_guest(j, ea, sz, dst)) return 0;
    static const uint8_t edge_sig[16] = { 0, 0, 0, 0, 0x05, 0x00, 0x02, 0xD0, 0xBF, 0x80, 0, 0, 0x3F, 0x80, 0, 0 };
    if (sz == 0x80 && __atomic_load_n(&g_spu_edge_vp_on, __ATOMIC_RELAXED) && !memcmp(dst, edge_sig, 16)) { fb(j, CF_GET, ea, sz); return 0; }
    *got = sz;
    return 1;
}
static int get_entry(J* j, uint32_t e, int s)
{
    if (!b1_ok(j, 0x60u + 28u * e, 28)) { fb(j, CF_RANGE, 0, 0); return 0; }
    const uint8_t* E = b1(j, 0x60u + 28u * e);
    slot_t* S = &j->slot[s];
    return get_block(j, S->g, R32(E + 16), (R16(E + 26) + 15) & ~15u, &S->sz1) &&
           get_block(j, S->g + 0x4000, R32(E + 20), ((R16(E + 24) << 3) + 15) & ~15u, &S->sz2) &&
           get_block(j, S->g + 0x8000, R32(E + 8), 64, &S->sz3);
}

static inline int fgt(uint32_t a, uint32_t b) { return GT0(F4W(a, 0, 0, 0), F4W(b, 0, 0, 0)); }   /* SPU fcgt, one lane */
static inline int bad_f(uint32_t w) { return (w & 0x7F800000u) == 0x7F800000u; }
static inline int32_t SX4(uint32_t v) { return (int32_t)(v << 28) >> 28; }

/* one entry in slot S (0x4420..0x4B90) */
static void entry(J* j, uint32_t e, slot_t* S)
{
    const uint8_t* E = b1(j, 0x60u + 28u * e);
    uint8_t* G = S->g;
    if (S->sz1 < 8) { fb(j, CF_RANGE, R32(E + 16), S->sz1); return; }
    const uint32_t nb = R32(G), total = R32(G + 4);
    const uint32_t w12 = R32(E + 12);
    const uint64_t T = 8 + 28ull * nb;                    /* the item table after the boxes */
    if (nb) {
        if (S->sz3 < 64) { fb(j, CF_RANGE, R32(E + 8), S->sz3); return; }
    }
    X4 M;
    for (int r = 0; r < 4; r++) M.x[r] = LQ(G + 0x8000 + 16 * r);
    const F4 cam = LQ(j->hdr);
    const uint32_t thr0 = R32(j->hdr + 0x30), frame = R32(j->hdr + 0x50), hdr54 = j->hdr[0x54];
    for (uint32_t i = 0; i < nb && !j->fallback; i++) {
        const uint64_t B = 8 + 28ull * i;
        if (B + 28 > S->sz1) { fb(j, CF_RANGE, R32(E + 16), S->sz1); return; }
        const uint8_t* bp = G + B;
        const F4 mn = F4W(R32(bp), R32(bp + 4), R32(bp + 8), R32(bp + 8));
        const F4 mx = F4W(R32(bp + 12), R32(bp + 16), R32(bp + 20), R32(bp + 20));
        if (FBAD(mn, 7) || FBAD(mx, 7) || FBAD(M.x[0], 7) || FBAD(M.x[1], 7) || FBAD(M.x[2], 7) || FBAD(M.x[3], 7) ||
            FBAD(cam, 7) || bad_f(thr0)) { j->fallback |= CF_FLOAT; return; }
        F4 wmn, wmx;
        box_xform(mn, mx, &M, &wmn, &wmx);                /* 0x4FC8: the same transform as 210's 0x6650 */
        /* the camera clamped into the box, the distance squared to it */
        const F4 c1 = FSEL(cam, wmx, FGT(cam, wmx));
        const F4 c2 = FSEL(wmn, c1, FGT(c1, wmn));
        const F4 v = FSUB(cam, c2);
        const F4 s = FMUL(v, v);
        const uint32_t d2 = FLANE(HSUM3(s), 0);
        uint32_t d = 0;
        if (fgt(d2, 0x00800000u)) d = dist_from_d2(d2);
        if (bad_f(d)) { j->fallback |= CF_FLOAT; return; }
        /* the band: the first threshold (header +0x30, +0x34, ...) the distance does not pass */
        uint32_t band = 0;
        if (fgt(d, thr0)) {
            for (band = 1; ; band++) {
                const uint32_t off = 0x30u + 4u * band;
                if (!b1_ok(j, off, 4) || band > 255) { fb(j, FB_BAND, off, 4); return; }
                const uint32_t t = R32(b1(j, off));
                if (bad_f(t)) { j->fallback |= CF_FLOAT; return; }
                if (!fgt(d, t)) break;
            }
        }
        /* the group mask (header +0x10.., 128 bits a quad): a box whose bit is clear (index >= 0) gets band 6 */
        const int32_t sidx = (int16_t)R16(bp + 24);
        int r2 = 1;
        if (sidx >= 0) {
            const uint32_t q = 0x10u + 16u * ((uint32_t)sidx >> 7), idx = (uint32_t)sidx & 127;
            if (!b1_ok(j, q, 16)) { fb(j, CF_RANGE, q, 16); return; }
            const uint8_t mb = *b1(j, q + 15 - (idx >> 3));
            if (!((mb >> (idx & 7)) & 1)) { band = 6; r2 = 0; }
        }
        /* the box's state record (block 2 + 8 i) */
        if (8ull * i + 8 > S->sz2) { fb(j, CF_RANGE, R32(E + 20), S->sz2); return; }
        uint8_t* P8 = G + 0x4000 + 8 * i;
        const uint8_t old0 = P8[0], old1 = P8[1], old2 = P8[2];
        const uint8_t bb = (uint8_t)band;
        if (hdr54 != 0 || r2 == 0 || old0 == 0xFF) {
            P8[0] = 0; P8[2] = bb; P8[1] = bb;
        } else {
            int chk = 0;                                   /* 0x4BC8 */
            if (old0 == 1) {
                if ((int32_t)frame > (int32_t)(R32(P8 + 4) + 720u)) { P8[0] = 0; P8[1] = old2; chk = 1; }
            } else if (old0 == 0) chk = 1;
            if (chk && (int32_t)(int8_t)old2 != (int32_t)band) { P8[0] = 1; P32(P8 + 4, frame); P8[2] = bb; }
        }
        /* items, when the record is or was fading, or its bands changed (0x4708, 0x4C38) */
        const int work = old0 != 0 || P8[0] != 0 || (int8_t)P8[2] != (int8_t)old2 || (int8_t)P8[1] != (int8_t)old1;
        if (!work) continue;
        const uint32_t start = (uint32_t)(int32_t)(int16_t)R16(bp + 26);
        uint32_t end = total;
        if (nb > i + 1) { if (B + 56 > S->sz1) { fb(j, CF_RANGE, R32(E + 16), S->sz1); return; } end = (uint32_t)(int32_t)(int16_t)R16(bp + 54); }
        const int32_t count = (int32_t)(end - start);
        if (count < 0 || count > 64) { fb(j, FB_ITEMS, start, end); return; }
        if (!count) continue;                              /* GETL / PUTL of size 0: nothing moves */
        /* the gather: item k's 16 bytes at w12 + 16 x s16(item k +0) */
        uint8_t* D = G + 0x8640;
        uint32_t iea[64];
        for (int32_t q = 0; q < count; q++) {
            const uint64_t I = T + 16ull * (start + (uint32_t)q);
            if (I + 16 > S->sz1) { fb(j, CF_RANGE, R32(E + 16), S->sz1); return; }
            iea[q] = w12 + ((uint32_t)(int32_t)(int16_t)R16(G + I) << 4);
            if (iea[q] & 15) { fb(j, FB_ALIGN, iea[q], 16); return; }
            if (!read_guest(j, iea[q], 16, D + 16 * q)) return;
        }
        /* each item's fade flag and alpha (0x48C0) */
        const uint32_t stamp = R32(P8 + 4);
        const int32_t lvl1 = (int8_t)P8[1], lvl2 = (int8_t)P8[2];
        const F4 t = FMUL(_mm_cvtsi32_ss(_mm_setzero_ps(), (int32_t)(frame - stamp)), F4W(0x3AB60B60u, 0, 0, 0));
        const F4 t1 = FSEL(t, F4W(F1, 0, 0, 0), FGT(t, F4W(F1, 0, 0, 0)));
        const F4 t2 = FSEL(t1, _mm_setzero_ps(), FGT(_mm_setzero_ps(), t1));
        const int32_t a64 = _mm_cvttss_si32(FMUL(t2, F4W(0x42800000u, 0, 0, 0)));   /* cflts 167: x 64, truncated */
        for (int32_t q = 0; q < count; q++) {
            const uint8_t lb = G[T + 16ull * (start + (uint32_t)q) + 2];
            const int32_t lo = SX4(lb), hi = SX4((uint32_t)lb >> 4);
            const int in1 = hi <= lvl1 && lvl1 <= lo, in2 = hi <= lvl2 && lvl2 <= lo;
            const uint8_t o0 = (uint8_t)(in1 && !in2);
            const uint8_t o2 = (uint8_t)(in1 != in2 ? a64 : (in2 ? 64 : 0));
            uint8_t* it = D + 16 * q;
            it[0] = (uint8_t)((it[0] & 0xFE) | o0);
            it[2] = o2;
        }
        for (int32_t q = 0; q < count; q++) put_log(j, iea[q], D + 16 * q, 16);   /* the PUTL */
    }
    if (j->fallback) return;
    /* the record block back (0x4ABC): the 16-aligned part, then the tail */
    const uint32_t n2 = R16(E + 24) * 8u, a = n2 & 0x7FFF0u, rest = n2 - a;
    if (n2 > 0x4000u || n2 > S->sz2) { fb(j, CF_RANGE, R32(E + 20), n2); return; }
    if (a) put_log(j, R32(E + 20), G + 0x4000, a);
    if (rest) put_log(j, R32(E + 20) + a, G + 0x4000 + a, rest);
}

static NJ_TLS J* t_j;

static int run209(wws_njob* nj)
{
    if (!t_j) { t_j = (J*)calloc(1, sizeof(J)); if (!t_j) { nj->fallback = CF_MEM; return 0; } }
    J* j = t_j;
    j->nj = nj; j->fallback = 0; j->nlog = 0; j->logbytes = 0; j->hdr = NULL;
    const wws_nbuf* b1b = &nj->buf[1];
    const wws_nbuf* b2b = &nj->buf[2];
    if (!b1b->used || !b1b->list || b1b->size < 16 || b1b->size > 64 || !b2b->used || b2b->pages * 1024u < 2u * SLOT_BYTES) {
        nj->fallback = CF_LAYOUT; return 0;
    }
    const uint8_t* L = nj->read(nj, b1b->ea, b1b->size);
    if (!L) { nj->fallback = CF_EA; nj->bad_ea = b1b->ea; nj->bad_size = b1b->size; return 0; }
    uint32_t esz[2], eea[2]; int ne = 0;
    for (uint32_t o = 0; o + 8 <= b1b->size; o += 8) {
        const uint32_t sz = R32(L + o) & 0x7FFF, ea = R32(L + o + 4);
        if (!sz) continue;
        if (ne == 2) { nj->fallback = CF_LAYOUT; return 0; }
        esz[ne] = sz; eea[ne] = ea; ne++;
    }
    if (ne < 1 || esz[0] != 0x60 || (eea[0] & 15)) { nj->fallback = CF_LAYOUT; return 0; }
    const uint8_t* hdr = nj->read(nj, eea[0], 0x60);
    if (!hdr) { nj->fallback = CF_EA; nj->bad_ea = eea[0]; nj->bad_size = 0x60; return 0; }
    const uint32_t n = R32(hdr + 0x4C);
    if (!n) return 1;                                     /* only entry 0's GETs: nothing written */
    if (ne != 2 || (eea[1] & 15) || 28ull * n > esz[1]) { nj->fallback = CF_LAYOUT; return 0; }
    j->recs = nj->read(nj, eea[1], esz[1]); j->recs_len = esz[1];
    if (!j->recs) { nj->fallback = CF_EA; nj->bad_ea = eea[1]; nj->bad_size = esz[1]; return 0; }
    j->hdr = hdr;
    const unsigned csr = cull_float_enter();
    if (get_entry(j, 0, 0)) {
        for (uint32_t e = 0; e < n && !j->fallback; e++) {
            if (e + 1 < n && !get_entry(j, e + 1, (int)((e + 1) & 1))) break;   /* the prefetch, before the work */
            entry(j, e, &j->slot[e & 1]);
        }
    }
    if (cull_float_leave(csr)) j->fallback |= CF_FLOAT;
    if (j->fallback) { nj->fallback = j->fallback; return 0; }
    if (nj->capture) return 1;
    for (uint32_t i = 0; i < j->nlog; i++) nj->put(nj, j->log[i].ea, j->logb + j->log[i].off, j->log[i].size, NULL);
    return 1;
}

/* verify mode: the last entry's record block, as the lifted job left it in its slot, against the model's */
static int verify209(wws_njob* nj, const uint8_t* ls, const uint32_t buf_ls[WWS_NJOB_SETS])
{
    J* j = t_j;
    (void)nj;
    if (!j || !j->hdr) return 0;
    const uint32_t n = R32(j->hdr + 0x4C);
    if (!n || !buf_ls[2]) return 0;
    const uint32_t e = n - 1, s = e & 1;
    const uint8_t* E = b1(j, 0x60u + 28u * e);
    const uint32_t n2 = R16(E + 24) * 8u;
    const uint32_t at = buf_ls[2] + SLOT_BYTES * s + 0x4000;
    if (n2 > 0x4000u || at + n2 > 0x40000u) return 9;
    return memcmp(ls + at, j->slot[s].g + 0x4000, n2) ? 5 : 0;
}

const wws_nmod wws_nmod_209 = { 209, "LOD crossfade states", run209, verify209, s_reasons, 1u << 1 };

#if defined(__clang__)
#pragma clang attribute pop
#endif
