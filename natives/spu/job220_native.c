/* inFAMOUS 2: job 220 (jobmod_cbb4b5c85d75_e59f8, the batched ray / segment collision queries), its BSP
 * traversal + hit sort / merge 0x7470, natively.
 *
 * Spec: work/bench/job220-spec.md; a line-for-line port of the verified reference work/bench/emu220/ref7470.py
 * (100/100 recorded calls byte-exact, native simulation 100/100 jobs, 1 460 synthetic cases 0 mismatches).
 * 0x7470 clips the job's 1-4 segments through each candidate instance's BSP tree (a node queue of 256, a fragment
 * ring of 1024, plane / node windows filled by GETs), then quicksorts and merges the hits. Its only channel work is
 * the synchronous GET 0x91B8: the native calls the LIFTED 0x91B8 at the same points with the same (ea, size, lsa)
 * and link, so the job's event stream is the lifted one; everything else is LS in / LS out. Every float op is the
 * engine's exact SPU helper in the reference's order.
 *
 * Paths the reference does not model (an instance without a BSP, a full plane list, a full fragment ring or hit
 * list) are found only after GETs: every LS quad the native writes (the GET landings included) is journaled
 * first, and such a call restores them and returns 0 -> the lifted 0x7470 runs from the entry state (repeating
 * its GETs: correct live, static geometry; no recorded call takes such a path).
 * The callee frames [sp-0xB00, sp) are declared dead to the job replay.
 *
 * Status 2026-11-17: exact (the 100 recorded jobs replay bit for bit) but NOT faster: replay 107 us per job
 * either way (the lifted code already runs its float statements as speculative regions, while this port uses
 * the per-op exact helpers), so it is OFF by default. To gain, the per-node math (plane_xform + box test) and
 * the per-fragment distances would have to run as one speculative region each (spu_*_spec, exact rerun).
 *
 * PS3RECOMP_NATIVE_J220=1: on (default off). J220_DEBUG=1: print every GET. J220_CHECK=1 (live only): the native, then the LS restored and the lifted
 * function, every LS byte outside the dead window and r3 compared (the lifted result is kept). J220_STATS=N. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "native_float.h"
#include "spu_jobrec.h"

int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void jobmod_cbb4b5c85d75_e59f8_spu_func_000091B8(spu_context* ctx);   /* the synchronous GET */
void jobmod_cbb4b5c85d75_e59f8_spu_func_00007470(spu_context* ctx);   /* the lifted function (check mode) */

#define ONE  0x3F800000u
#define HALF 0x3F000000u
#define M32  0xFFFFFFFFu
#define THR6140 0x0DA2425Fu

typedef struct {
    spu_context* ctx;
    uint8_t* L;
    uint32_t sp, spf;
    u128 r0, r1;        /* the entry link and sp registers */
    int decline;        /* set by an unmodelled path */
} J;

/* ---------------- the journal: the first write to a quad saves it ---------------- */
typedef struct { uint32_t q; uint8_t b[16]; } JQ;
static _Thread_local uint8_t t_dirty[16384 / 8];
static _Thread_local JQ* t_log;
static _Thread_local uint32_t t_nlog, t_caplog;
static void jsave_q(J* j, uint32_t q)
{
    q &= 16383u;
    if (t_dirty[q >> 3] & (1u << (q & 7))) return;
    t_dirty[q >> 3] |= (uint8_t)(1u << (q & 7));
    if (t_nlog == t_caplog) {
        t_caplog = t_caplog ? t_caplog * 2 : 4096;
        t_log = (JQ*)realloc(t_log, t_caplog * sizeof *t_log);
    }
    t_log[t_nlog].q = q; memcpy(t_log[t_nlog].b, j->L + 16u * q, 16); t_nlog++;
}
static inline void jsave(J* j, uint32_t a, uint32_t n)
{
    if (!n) return;
    const uint32_t q0 = (a & 0x3FFFFu) >> 4, q1 = ((a + n - 1u) & 0x3FFFFu) >> 4;
    if (q0 <= q1) { for (uint32_t q = q0; q <= q1; q++) jsave_q(j, q); }
    else { for (uint32_t q = q0; q < 16384u; q++) jsave_q(j, q); for (uint32_t q = 0; q <= q1; q++) jsave_q(j, q); }
}
static void jdone(int undo, uint8_t* L)
{
    for (uint32_t i = t_nlog; i-- > 0;) {
        if (undo) memcpy(L + 16u * t_log[i].q, t_log[i].b, 16);
        t_dirty[t_log[i].q >> 3] = 0;
    }
    t_nlog = 0;
}

/* ---------------- LS access (big endian, the SPU's wrap), stores journaled ---------------- */
static inline uint32_t lw(J* j, uint32_t a) { const uint8_t* p = j->L + (a & 0x3FFFCu); return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t lh(J* j, uint32_t a) { const uint8_t* p = j->L + (a & 0x3FFFEu); return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t lh_q(J* j, uint32_t a) { const uint32_t q = a & 0x3FFF0u, o = a & 15u; return (uint32_t)j->L[q + o] << 8 | j->L[q + ((o + 1u) & 15u)]; }
static inline uint32_t lb(J* j, uint32_t a) { return j->L[a & 0x3FFFFu]; }
static inline u128 lq(J* j, uint32_t a)
{
    const uint8_t* p = j->L + (a & 0x3FFF0u); u128 v;
    for (int i = 0; i < 4; i++) v._u32[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    return v;
}
static inline void sw(J* j, uint32_t a, uint32_t v) { a &= 0x3FFFCu; jsave(j, a, 4); uint8_t* p = j->L + a; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void sh(J* j, uint32_t a, uint32_t v) { a &= 0x3FFFEu; jsave(j, a, 2); uint8_t* p = j->L + a; p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void sb(J* j, uint32_t a, uint32_t v) { a &= 0x3FFFFu; jsave(j, a, 1); j->L[a] = (uint8_t)v; }
static inline void sq(J* j, uint32_t a, u128 v)
{
    a &= 0x3FFF0u; jsave(j, a, 16); uint8_t* p = j->L + a;
    for (int i = 0; i < 4; i++) { p[4 * i] = (uint8_t)(v._u32[i] >> 24); p[4 * i + 1] = (uint8_t)(v._u32[i] >> 16); p[4 * i + 2] = (uint8_t)(v._u32[i] >> 8); p[4 * i + 3] = (uint8_t)v._u32[i]; }
}
static inline int32_t i32(uint32_t v) { return (int32_t)v; }
static inline int32_t i16(uint32_t v) { return (int16_t)(uint16_t)v; }
static inline uint32_t cmod(uint32_t x, uint32_t m)       /* C signed x % m (m a power of two), as compiled */
{
    const int32_t sx = (int32_t)x; const uint32_t r = (uint32_t)(sx < 0 ? -sx : sx) & (m - 1u);
    return sx < 0 ? (uint32_t)(-(int32_t)r) : r;
}

/* ---------------- 4-lane ops (exact SPU lanes) ---------------- */
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 SPL(uint32_t x) { return V4(x, x, x, x); }
static inline u128 splat(u128 v, int i) { return SPL(v._u32[i]); }
static inline u128 vfm(u128 a, u128 b) { return spu_fm(a, b); }
static inline u128 vfa(u128 a, u128 b) { return spu_fa(a, b); }
static inline u128 vfs(u128 a, u128 b) { return spu_fs(a, b); }
static inline u128 vfma(u128 a, u128 b, u128 c) { return spu_fma(a, b, c); }        /* a*b + c */
static inline u128 vfnms(u128 a, u128 b, u128 c) { return spu_fnms(a, b, c); }      /* c - a*b */
static inline u128 vfcgt(u128 a, u128 b) { return spu_fcgt_lean(a, b); }
static inline u128 vsel(u128 a, u128 b, u128 m) { return spu_selb(a, b, m); }
static inline u128 vand(u128 a, u128 b) { return V4(a._u32[0] & b._u32[0], a._u32[1] & b._u32[1], a._u32[2] & b._u32[2], a._u32[3] & b._u32[3]); }
static inline u128 yzxx(u128 v) { return V4(v._u32[1], v._u32[2], v._u32[0], v._u32[0]); }
static inline u128 P70(u128 a, u128 b) { return V4(a._u32[1], a._u32[2], a._u32[3], b._u32[0]); }
static inline u128 P80(u128 a, u128 b) { return V4(a._u32[2], a._u32[3], b._u32[0], b._u32[1]); }
static inline u128 P90(u128 a, u128 b) { return V4(a._u32[0], b._u32[0], a._u32[1], b._u32[1]); }
static inline u128 PA0(u128 a, u128 b) { return V4(a._u32[2], b._u32[2], a._u32[3], b._u32[3]); }
static inline u128 xneg(u128 v) { return V4(v._u32[0] ^ 0x80000000u, v._u32[1] ^ 0x80000000u, v._u32[2] ^ 0x80000000u, v._u32[3] ^ 0x80000000u); }
#define LANE3   V4(0, 0, 0, M32)
#define LANE012 V4(M32, M32, M32, 0)
#define SPL1    SPL(ONE)
#define Z4      V4(0, 0, 0, 0)
/* scalar lanes */
static inline uint32_t fa1(uint32_t a, uint32_t b) { return spu_fa(SPL(a), SPL(b))._u32[0]; }
static inline uint32_t fs1(uint32_t a, uint32_t b) { return spu_fs(SPL(a), SPL(b))._u32[0]; }
static inline uint32_t fm1(uint32_t a, uint32_t b) { return spu_fm(SPL(a), SPL(b))._u32[0]; }
static inline uint32_t fma1(uint32_t a, uint32_t b, uint32_t c) { return spu_fma(SPL(a), SPL(b), SPL(c))._u32[0]; }
static inline uint32_t fnms1(uint32_t a, uint32_t b, uint32_t c) { return spu_fnms(SPL(a), SPL(b), SPL(c))._u32[0]; }
static inline int gt1(uint32_t a, uint32_t b) { return spu_fcgt_lean(SPL(a), SPL(b))._u32[0] != 0; }   /* key1(a) > key1(b) */

/* ---------------- leaves ---------------- */
static void inv_affine(const u128 R[4], u128 out[4])           /* 0x6A98 */
{
    const u128 r0 = R[0], r1 = R[1], r2 = R[2], r3 = R[3];
    const u128 a1 = yzxx(r1), a0 = yzxx(r0), a2 = yzxx(r2);
    const u128 r59 = vfm(r0, a1), r56 = vfm(r2, a0), r53 = vfm(r1, a2);
    const u128 n3 = xneg(r3);
    const u128 r48 = vfnms(r1, a0, r59), r47 = vfnms(r0, a2, r56), r45 = vfnms(r2, a1, r53);
    const u128 c01 = yzxx(r48), c20 = yzxx(r47), c12 = yzxx(r45);
    const u128 r29 = vfm(c01, r2);
    const u128 r36 = P90(c20, Z4), r35 = P90(c12, c01), r31 = PA0(c12, c01), r32 = PA0(c20, Z4);
    const u128 r19 = P90(r35, r36), r17 = PA0(r35, r36), r13 = P90(r31, r32);
    const u128 det = splat(vfa(vfa(r29, P70(r29, r29)), P80(r29, r29)), 0);
    const u128 r8 = vfma(r13, splat(n3, 2), vfma(r17, splat(n3, 1), vfm(r19, splat(n3, 0))));
    const u128 y = spu_fi(det, spu_frest(det));
    const u128 inv = vand(vfma(vfnms(y, det, SPL1), y, y), LANE012);
    out[0] = vfm(r19, inv); out[1] = vfm(r17, inv); out[2] = vfm(r13, inv); out[3] = vfma(r8, inv, V4(0, 0, 0, ONE));
}
static u128 plane_xform(u128 p, const u128 Mx[4])                /* 0x6140 */
{
    const u128 n = vand(p, LANE012);
    const u128 n2 = vfma(Mx[2], splat(n, 2), vfma(Mx[1], splat(n, 1), vfm(Mx[0], splat(n, 0))));
    const u128 t = vfm(vsel(Mx[3], SPL1, LANE3), vand(n2, LANE012));
    const u128 s = vfa(vfa(t, P70(t, t)), P80(t, t));
    const u128 w = vfa(splat(p, 3), splat(s, 0));
    const u128 r6 = vsel(n2, w, LANE3);
    const u128 q2 = vfm(r6, r6);
    const u128 l2 = splat(vfa(vfa(q2, P70(q2, q2)), P80(q2, q2)), 0);
    const u128 big = vfcgt(l2, SPL(THR6140));
    const u128 y = spu_fi(l2, spu_frsqest(l2));
    const u128 r = vfma(vfnms(l2, vfm(y, y), SPL1), vfm(y, SPL(HALF)), y);
    return vsel(r6, vfm(r6, r), big);
}
static int cmp4D00(u128 a, u128 b)
{
    const uint32_t d = a._u32[0] - b._u32[0];
    if (d) return i32(d) < 0 ? -1 : 1;   /* only the sign is used (the reference returns the difference) */
    const uint32_t f = fs1(a._u32[1], b._u32[1]);
    if (gt1(0, f)) return -1;
    return gt1(f, 0) ? 1 : 0;
}
static uint32_t part5350(J* j, uint32_t n, uint32_t base)
{
#define QX(i) lq(j, base + 16u * (uint32_t)(i))
    if (n == 2) {
        if (cmp4D00(QX(0), QX(1)) > -1) { const u128 a = QX(0), b = QX(1); sq(j, base, b); sq(j, base + 16u, a); }
        return 0;
    }
    uint32_t jj = n - 1u;
    const int32_t sn = i32(n);
    const uint32_t h = (uint32_t)((sn + (sn < 0 ? 1 : 0)) >> 1);
    const uint32_t m = h, last = n - 1u;
    uint32_t piv;
    if (cmp4D00(QX(m), QX(0)) > -1) {
        if (cmp4D00(QX(last), QX(0)) < 0) piv = 0;
        else if (cmp4D00(QX(m), QX(last)) < 0) piv = m;
        else piv = last;
    } else {
        if (cmp4D00(QX(0), QX(last)) < 0) piv = 0;
        else if (cmp4D00(QX(last), QX(m)) > -1) piv = last;
        else piv = m;
    }
    const u128 P = QX(piv);
    sq(j, base + 16u * piv, QX(last));
    uint32_t i = 0;
    if (i32(jj) > 0) {
        for (;;) {
            if (cmp4D00(QX(i), P) > -1) {
                if (cmp4D00(QX(jj - 1u), P) > 0) jj -= 1u;
                else { const u128 a = QX(i), b = QX(jj - 1u); sq(j, base + 16u * i, b); sq(j, base + 16u * (jj - 1u), a); i += 1u; jj -= 1u; }
                if (!(i32(jj) > i32(i))) break;
            } else {
                i += 1u;
                if (!(i32(jj) > i32(i))) break;
            }
        }
    }
    sq(j, base + 16u * (n - 1u), QX(i));
    sq(j, base + 16u * i, P);
    return i;
#undef QX
}
static void qsort55C0(J* j, uint32_t n, uint32_t base, uint32_t thr)
{
    for (;;) {
        if (!(i32(n) > i32(thr))) return;
        const uint32_t p = part5350(j, n, base);
        if (i32(n - p) > i32(p)) { qsort55C0(j, p, base, thr); n = n - p - 1u; base = base + 16u * p + 16u; }
        else { qsort55C0(j, n - p - 1u, base + 16u * p + 16u, thr); n = p; }
        if (!(i32(n) > i32(thr))) return;
    }
}
static uint32_t compact5210(J* j, uint32_t W, uint32_t b, uint32_t e)
{
#define FLAG(k) ((lb(j, W + 0x1000u + 16u * (k) + 14u) >> 3) & 1u)
    if (b == e) return e;
    uint32_t k = b, jj = e;
    for (;;) {
        if (FLAG(k)) k = cmod(k + 1u, 1024);
        else {
            for (;;) { jj = cmod(jj + 1023u, 1024); if (jj == k) return k; if (FLAG(jj)) break; }
            const uint32_t a1 = W + 0x1000u + 16u * jj, a2 = W + 0x1000u + 16u * k;
            const u128 x = lq(j, a1), y = lq(j, a2);
            sq(j, a2, x); sq(j, a1, y);
            k = cmod(k + 1u, 1024);
        }
        if (k == jj) return k;
    }
#undef FLAG
}

/* ---------------- the GET (the lifted 0x91B8, from the frame, with the original link) ---------------- */
static void get(J* j, uint32_t ea, uint32_t size, uint32_t lsa, uint32_t link)
{
    jsave(j, lsa, size);
    spu_context* ctx = j->ctx;
    static int dbg = -1; if (dbg < 0) dbg = getenv("J220_DEBUG") != 0;
    if (dbg) fprintf(stderr, "[j220] get ea %08X size %X lsa %05X link %04X\n", ea, size, lsa, link);
    /* 0x91B8 is a leaf (r2-r8, the MFC channels, LS 0xA150): it reads only word 0 of r3-r5, no stack */
    ctx->gpr[3] = SPL(ea); ctx->gpr[4] = SPL(size); ctx->gpr[5] = SPL(lsa);
    ctx->gpr[0] = V4(link, 0, 0, 0);
    const int32_t si = (int32_t)ctx->image_id;
    ctx->host_depth++; jobmod_cbb4b5c85d75_e59f8_spu_func_000091B8(ctx); spu_drain_call(ctx, link); ctx->host_depth--;
    spu_img_restore(ctx, si);
}

/* ---------------- one instance (0x7F10 .. 0x8B40) ---------------- */
static void instance(J* j, uint32_t W, uint32_t Q, uint32_t M, uint32_t P, uint32_t ea, uint32_t root)
{
    const uint32_t spf = j->spf;
    u128 R[4], I[4], B[4], T[4];
    for (int k = 0; k < 4; k++) R[k] = lq(j, Q + 16u * (uint32_t)k);
    inv_affine(R, I);
    for (int k = 0; k < 4; k++) B[k] = lq(j, spf + 0x200u + 16u * (uint32_t)k);
    for (int k = 0; k < 4; k++) {
        const u128 b = B[k];
        u128 t = vfma(I[2], splat(b, 2), vfma(I[1], splat(b, 1), vfm(I[0], splat(b, 0))));
        if (k == 3) t = vfa(t, I[3]);
        T[k] = t;
    }
    const u128 T0 = T[0], T1 = T[1], T2 = T[2], T3 = T[3];
    const u128 c01 = yzxx(vfnms(T1, yzxx(T0), vfm(T0, yzxx(T1))));
    const u128 c20 = yzxx(vfnms(T0, yzxx(T2), vfm(T2, yzxx(T0))));
    const u128 c12 = yzxx(vfnms(T2, yzxx(T1), vfm(T1, yzxx(T2))));
    const u128 r7 = vfm(c01, T2);
    const u128 det = splat(vfa(vfa(r7, P70(r7, r7)), P80(r7, r7)), 0);
    const u128 r8 = P90(c12, c01), r13 = P90(c20, Z4), r73 = PA0(c12, c01), r74 = PA0(c20, Z4);
    const u128 r63 = P90(r8, r13), r62 = PA0(r8, r13), r61 = P90(r73, r74);
    const u128 nT3 = xneg(T3);
    const u128 r59 = vfma(r61, splat(nT3, 2), vfma(r62, splat(nT3, 1), vfm(r63, splat(nT3, 0))));
    const u128 y = spu_fi(det, spu_frest(det));
    const u128 inv = vand(vfma(vfnms(y, det, SPL1), y, y), LANE012);
    const u128 r99 = vand(vfm(r63, inv), LANE012), r97 = vand(vfm(r62, inv), LANE012), r100 = vand(vfm(r61, inv), LANE012);
    const u128 r98 = vsel(vfma(r59, inv, V4(0, 0, 0, ONE)), SPL1, LANE3);
    const u128 r91 = P90(r99, r100), r92 = P90(r97, r98), r96 = PA0(r99, r100), r95 = PA0(r97, r98);
    const u128 MP[4] = { P90(r91, r92), PA0(r91, r92), P90(r96, r95), vsel(T3, SPL1, LANE3) };
    /* 0x6088 with the identity rows: the AABB of the 2m end points */
    u128 bmin = SPL(0x7FFFFFFFu), bmax = SPL(M32);
    if (i32(2u * M) > 0) {
        const u128 ID[4] = { V4(ONE, 0, 0, 0), V4(0, ONE, 0, 0), V4(0, 0, ONE, 0), V4(0, 0, 0, ONE) };
        for (uint32_t k = 0; k < 2u * M; k++) {
            const u128 p = vsel(lq(j, P + 16u * k), SPL1, LANE3);
            const u128 v = vfa(vfma(ID[2], splat(p, 2), vfma(ID[1], splat(p, 1), vfm(ID[0], splat(p, 0)))), ID[3]);
            bmin = vsel(v, bmin, vfcgt(v, bmin));
            bmax = vsel(bmax, v, vfcgt(v, bmax));
        }
    }
    /* 0x5EE0 and the instance's plane-list entry */
    uint32_t pl_n = lw(j, W + 0x5220u);
    if (i32(pl_n + 1u) > 1024) { j->decline = 2; return; }
    sw(j, W + 0x5220u, pl_n + 1u);
    sw(j, W + 0x5224u + 8u * pl_n, ea); sh(j, W + 0x5228u + 8u * pl_n, 0xFFFFu);
    uint32_t c = lw(j, W + 0x7230u); sw(j, W + 0x7230u, c + 1u); sq(j, W + 0x7240u + 16u * c, Z4);
    uint32_t idx = pl_n & 1023u;
    for (int32_t jx = 0; jx < i32(M); jx++)
        sq(j, W + 0x1000u + 16u * (uint32_t)jx, V4(P + 32u * (uint32_t)jx, 0, ONE, idx << 22 | idx << 12));
    /* 0x8258: the root entry, the queue indices */
    sb(j, W, 0); sw(j, W + 4u, root); sw(j, W + 8u, 0); sw(j, W + 12u, M);
    uint32_t qr = 0, qw = 1, fr = 0, fw = M;
    int first = 1;
    for (;;) {
        if (!first && qw == qr) return;
        first = 0;
        const uint32_t E = W + 16u * qr;
        const uint32_t nxt = cmod(qr + 1u, 256);
        const int32_t node = i32(lw(j, E + 4u));
        if (node >= 0) {
            const uint32_t rec = lw(j, 0xA240u) + 6u * (uint32_t)node;
            if (lw(j, W + 0x5208u) > rec || rec + 6u > lw(j, W + 0x520Cu)) {
                const uint32_t st = rec & ~127u;
                sw(j, W + 0x5208u, st); sw(j, W + 0x520Cu, st + lw(j, W + 0x5200u));
                get(j, st, lw(j, W + 0x5200u), lw(j, W + 0x5204u), 0x83E8u);
            }
            const uint32_t ptr = lw(j, W + 0x5204u) + rec - lw(j, W + 0x5208u);
            const uint32_t pea = lw(j, 0xA244u) + 16u * lh_q(j, ptr);
            if (lw(j, W + 0x5218u) > pea || pea + 16u > lw(j, W + 0x521Cu)) {
                const uint32_t st = pea & ~127u;
                sw(j, W + 0x5218u, st); sw(j, W + 0x521Cu, st + lw(j, W + 0x5210u));
                get(j, st, lw(j, W + 0x5210u), lw(j, W + 0x5214u), 0x84C4u);
            }
            const u128 pl = lq(j, lw(j, W + 0x5214u) + pea - lw(j, W + 0x5218u));
            const u128 tp = plane_xform(pl, MP);
            const u128 a = vfm(tp, bmin), b = vfm(tp, bmax);
            const u128 g = vfcgt(a, b);
            const u128 hi = vsel(b, a, g), lo = vsel(a, b, g);
            const uint32_t smin = fa1(fa1(lo._u32[0], lo._u32[1]), lo._u32[2]), smax = fa1(fa1(hi._u32[0], hi._u32[1]), hi._u32[2]);
            const uint32_t dmin = fs1(smin, tp._u32[3]), dmax = fs1(smax, tp._u32[3]);
            if (!gt1(0, dmin)) { sb(j, E, 0); sw(j, E + 4u, (uint32_t)i16(lh_q(j, ptr + 4u))); continue; }
            if (!gt1(dmax, 0)) { sb(j, E, 1); sw(j, E + 4u, (uint32_t)i16(lh_q(j, ptr + 2u))); continue; }
            /* 0x86C8: split the fragments of E at the plane */
            pl_n = lw(j, W + 0x5220u);
            if (i32(pl_n + 1u) > 1024) { j->decline = 2; return; }
            sw(j, W + 0x5220u, pl_n + 1u);
            sw(j, W + 0x5224u + 8u * pl_n, ea); sh(j, W + 0x5228u + 8u * pl_n, lh_q(j, ptr));
            c = lw(j, W + 0x7230u); sw(j, W + 0x7230u, c + 1u); sq(j, W + 0x7240u + 16u * c, tp);
            idx = pl_n & 1023u;
            uint32_t k = lw(j, E + 8u);
            uint32_t wi = fw;
            if (k != lw(j, E + 12u)) {
                for (;;) {
                    const uint32_t F = W + 0x1000u + 16u * k;
                    const u128 fq = lq(j, F);
                    const u128 p0 = lq(j, fq._u32[0]), p1 = lq(j, fq._u32[0] + 16u);
                    const u128 d = vfs(p1, p0);
                    const u128 A = vfma(d, SPL(fq._u32[2]), p0), Bp = vfma(d, SPL(fq._u32[1]), p0);
                    const u128 ma = vfm(tp, A), mb = vfm(tp, Bp);
                    const uint32_t d1 = fs1(fa1(fa1(ma._u32[0], ma._u32[1]), ma._u32[2]), tp._u32[3]);
                    const uint32_t d0 = fs1(fa1(fa1(mb._u32[0], mb._u32[1]), mb._u32[2]), tp._u32[3]);
                    const uint32_t w1 = cmod(wi + 1u, 1024);
                    if (fr == w1) { j->decline = 3; return; }
                    const uint32_t D = W + 0x1000u + 16u * wi;
                    const int pos0 = gt1(d0, 0), pos1 = gt1(d1, 0);
                    if (!pos0 && !pos1) {
                        u128 q = lq(j, F); q._u32[3] |= 0x800u; sq(j, D, q); wi = w1;
                    } else if (!gt1(0, d0) && !gt1(0, d1)) {
                        u128 q = lq(j, F); q._u32[3] &= ~0x800u; sq(j, D, q); wi = w1;
                    } else {
                        const uint32_t w2 = cmod(w1 + 1u, 1024);
                        const uint32_t dd = fs1(d0, d1);
                        const uint32_t yy = spu_fi(SPL(dd), spu_frest(SPL(dd)))._u32[0];
                        const uint32_t rc = fma1(fnms1(yy, dd, ONE), yy, yy);
                        const uint32_t tm = fma1(fs1(fq._u32[2], fq._u32[1]), fm1(rc, d0), fq._u32[1]);
                        if (fr == w2) { j->decline = 3; return; }
                        const uint32_t f0 = pos0 ? 0u : 1u;
                        u128 q = lq(j, F); q._u32[2] = tm;
                        q._u32[3] = (q._u32[3] & ~0x003FF800u) | idx << 12 | f0 << 11;
                        sq(j, D, q);
                        q = lq(j, F); q._u32[1] = tm;
                        q._u32[3] = (q._u32[3] & ~0xFFC00800u) | idx << 22 | (f0 ^ 1u) << 11;
                        sq(j, W + 0x1000u + 16u * w1, q);
                        wi = w2;
                    }
                    k = cmod(k + 1u, 1024);
                    if (k == lw(j, E + 12u)) break;
                }
            }
            const uint32_t mid = compact5210(j, W, fw, wi);
            if (fw != mid) {
                const uint32_t E2 = W + 16u * qw;
                sb(j, E2, 1); qw = cmod(qw + 1u, 256);
                sw(j, E2 + 4u, (uint32_t)i16(lh_q(j, ptr + 2u))); sw(j, E2 + 8u, fw); sw(j, E2 + 12u, mid);
            }
            if (wi != mid) {
                const uint32_t E2 = W + 16u * qw;
                sb(j, E2, 0); qw = cmod(qw + 1u, 256);
                sw(j, E2 + 4u, (uint32_t)i16(lh_q(j, ptr + 4u))); sw(j, E2 + 8u, mid); sw(j, E2 + 12u, wi);
            }
            fw = wi;
            qr = nxt;
            fr = lw(j, E + 12u);
        } else {
            /* 0x85D8: leaf */
            if (lb(j, E) == 0) { qr = nxt; fr = lw(j, E + 12u); continue; }
            uint32_t e = lw(j, E + 12u), k = lw(j, E + 8u);
            if (e != k) {
                const uint32_t H = W + 0xB240u;
                uint32_t n = lw(j, H);
                if (i32(n) > 1023) { j->decline = 4; return; }
                for (;;) {
                    sq(j, H + 16u + 16u * n, lq(j, W + 0x1000u + 16u * k));
                    n = n + 1u; sw(j, H, n);
                    k = cmod(k + 1u, 1024);
                    e = lw(j, E + 12u);
                    if (k == e) break;
                    if (i32(n) > 1023) { j->decline = 4; return; }
                }
            }
            fr = e; qr = nxt;
        }
    }
}

/* ---------------- the function: 1 = done (r3 = the group count), 0 = declined (LS as at entry) ---------------- */
static int core7470(J* j)
{
    spu_context* ctx = j->ctx;
    const uint32_t W = ctx->gpr[3]._u32[0], FL = ctx->gpr[4]._u32[0], Q = ctx->gpr[5]._u32[0], N = ctx->gpr[6]._u32[0];
    const uint32_t IL = ctx->gpr[7]._u32[0], M = ctx->gpr[8]._u32[0], P = ctx->gpr[9]._u32[0];
    const uint32_t sp = ctx->gpr[1]._u32[0];
    /* entry declines (spec 5.7), before anything is written */
    if ((W | Q | P) & 15u || (IL & 3u) || i32(M) < 0 || i32(M) > 0x100 || (i32(N) > 1024) ||
        W > SPU_LS_SIZE - 0xF260u || P + 32u * M > SPU_LS_SIZE || (sp & 15u) || sp < 0xB00u || sp + 0x20u > SPU_LS_SIZE)
        return 0;
    j->sp = sp; j->spf = sp - 0x560u; j->r0 = ctx->gpr[0]; j->r1 = ctx->gpr[1]; j->decline = 0;
    sq(j, sp + 0x10u, ctx->gpr[0]);
    if (i32(N) > 0) {
        for (uint32_t i = 0; i < N && !j->decline; i++) {
            const uint32_t ea = lw(j, IL + 4u * i);
            if (lb(j, 0xA1F0u) == 0) sb(j, 0xA1F0u, 1);
            get(j, ea, 32, 0xA260u, 0x75F8u);
            if (lb(j, 0xA200u) == 0) sb(j, 0xA200u, 1);
            get(j, lw(j, 0xA260u), 80, 0xA210u, 0x7638u);
            get(j, lw(j, 0xA26Cu), 64, j->spf + 0x200u, 0x764Cu);
            const int32_t root = i16(lh(j, 0xA248u));
            if (root < 0) { j->decline = 1; break; }
            instance(j, W, Q, M, P, ea, (uint32_t)root);
        }
        if (j->decline) return 0;
    }
    /* 0x7B3C: sort the hits, merge them per ray */
    const uint32_t H = W + 0xB240u;
    qsort55C0(j, lw(j, H), H + 16u, 1);
    int32_t n = i32(lw(j, H));
    uint32_t groups = 0, prev = 0;
    for (int32_t k = 0; n > k; k++) {
        const uint32_t cur = H + 16u + 16u * (uint32_t)k;
        if (prev == 0) { groups++; prev = cur; }
        else {
            u128 cq = lq(j, cur), pv = lq(j, prev);
            if (pv._u32[0] != cq._u32[0] || gt1(cq._u32[1], pv._u32[2])) { groups++; prev = cur; }
            else {
                int same = 1;
                if (FL & 2u) {
                    const uint32_t ex = (pv._u32[3] >> 12) & 0x3FFu, en = cq._u32[3] >> 22;
                    same = lw(j, W + 0x5220u + 8u * ex + 4u) == lw(j, W + 0x5220u + 8u * en + 4u);
                }
                if (!same) { groups++; prev = cur; }
                else {
                    cq._u32[0] = 0; sq(j, cur, cq);
                    if (gt1(cq._u32[2], pv._u32[2])) {
                        pv._u32[2] = cq._u32[2];
                        pv._u32[3] = (pv._u32[3] & ~0x003FF000u) | (((cq._u32[3] >> 12) & 0x3FFu) << 12);
                        sq(j, prev, pv);
                    }
                }
            }
        }
        n = i32(lw(j, H));
    }
    ctx->gpr[3] = SPL(groups);
    ctx->gpr[0] = j->r0;        /* SPU_RET returns through r0: the GET calls set it to their links */
    return 1;
}

static int s_mode = -1, s_check;
static unsigned long long s_every = 20000, s_calls, s_decl[5], s_chk, s_bad;
static _Thread_local int t_bypass;
static _Thread_local uint8_t* t_snap;

static int j220_entry(spu_context* ctx)
{
    if (__builtin_expect(s_mode < 0, 0)) {
        const char* e = getenv("PS3RECOMP_NATIVE_J220");
        s_mode = e ? atoi(e) : 0;   /* default off: measured no faster than the lifted code (below) */
        s_check = getenv("J220_CHECK") != 0;
        e = getenv("J220_STATS"); if (e && atoll(e) > 1) s_every = (unsigned long long)atoll(e);
        fprintf(stderr, "[j220] native BSP traversal + hit sort (0x7470): %s%s\n", s_mode ? "on" : "off (PS3RECOMP_NATIVE_J220=1 turns it on)",
                s_mode && s_check ? ", CHECK mode (the lifted result is kept)" : "");
    }
    if (!s_mode || t_bypass || (g_spu_jobrec_mode & SPU_JOBREC_RECORD)) return 0;
    const int replay = (g_spu_jobrec_mode & SPU_JOBREC_REPLAY) != 0;
    const unsigned long long c = __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);
    J j; memset(&j, 0, sizeof j); j.ctx = ctx; j.L = ctx->ls;
    if (s_check && !replay) {
        if (!t_snap && !(t_snap = (uint8_t*)malloc(SPU_LS_SIZE))) return 0;
        u128 g[128]; memcpy(g, ctx->gpr, sizeof g);
        memcpy(t_snap, ctx->ls, SPU_LS_SIZE);
        const int ok = core7470(&j);
        jdone(!ok, ctx->ls);
        if (!ok) { memcpy(ctx->gpr, g, sizeof g); __atomic_add_fetch(&s_decl[j.decline], 1, __ATOMIC_RELAXED); return 0; }
        static _Thread_local uint8_t* t_nat;
        if (!t_nat && !(t_nat = (uint8_t*)malloc(SPU_LS_SIZE))) return 0;
        memcpy(t_nat, ctx->ls, SPU_LS_SIZE);
        const uint32_t r3n = ctx->gpr[3]._u32[0];
        memcpy(ctx->ls, t_snap, SPU_LS_SIZE); memcpy(ctx->gpr, g, sizeof g);
        t_bypass = 1;
        { const u128 r0 = ctx->gpr[0]; const int32_t si = (int32_t)ctx->image_id; ctx->host_depth++;
          jobmod_cbb4b5c85d75_e59f8_spu_func_00007470(ctx); spu_drain_call(ctx, r0._u32[0]); ctx->host_depth--; spu_img_restore(ctx, si); }
        t_bypass = 0;
        const uint32_t lo = j.sp - 0xB00u, hi = j.sp;
        const int bad = memcmp(t_nat, ctx->ls, lo) || memcmp(t_nat + hi, ctx->ls + hi, SPU_LS_SIZE - hi) || ctx->gpr[3]._u32[0] != r3n;
        __atomic_add_fetch(&s_chk, 1, __ATOMIC_RELAXED);
        if (bad && __atomic_add_fetch(&s_bad, 1, __ATOMIC_RELAXED) <= 8) {
            uint32_t a = 0; while (a < SPU_LS_SIZE && (t_nat[a] == ctx->ls[a] || (a >= lo && a < hi))) a++;
            fprintf(stderr, "[j220] CHECK: native differs, first byte 0x%05X, r3 %u vs %u\n", a, r3n, ctx->gpr[3]._u32[0]);
        }
        if (c % s_every == 0) fprintf(stderr, "[j220] %llu calls, check %llu/%llu differ, declined %llu/%llu/%llu/%llu\n",
                                     c, s_bad, s_chk, s_decl[1], s_decl[2], s_decl[3], s_decl[4]);
        return 1;
    }
    u128 g[128]; memcpy(g, ctx->gpr, sizeof g);
    const int ok = core7470(&j);
    jdone(!ok, ctx->ls);
    if (!ok) {
        memcpy(ctx->gpr, g, sizeof g);
        __atomic_add_fetch(&s_decl[j.decline], 1, __ATOMIC_RELAXED);
        return 0;
    }
    spu_native_dead_ls(j.sp - 0xB00u, j.sp);
    if (c % s_every == 0) fprintf(stderr, "[j220] %llu calls, declined %llu (no BSP) %llu (plane list) %llu (ring) %llu (hits)\n",
                                 c, s_decl[1], s_decl[2], s_decl[3], s_decl[4]);
    return 1;
}

__attribute__((constructor)) static void j220_register(void)
{
    spu_native_register(220, 0x7470u, j220_entry);
}
