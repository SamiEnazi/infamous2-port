/* From the Nintendo Switch fork github.com/4evry1/infamous2-port-NX (branch switch-nx @ 49fc23333713,
 * switch/natives/job201_native.c, MIT licence of the project), unchanged except this note: there it builds for
 * the Switch only; here it is part of the PC build (natives/spu). The SPU modules and image ids are the same. */
/* job201_native.c -- inFAMOUS 2 (BCES01143): the sort of SPU job 201 (jobmod_14e77a6441c8_e6ce0, 0x6D28) natively.
 * Switch build only (switch/natives/: the PC build compiles every C file of natives/spu); written for this port, not
 * derived from another project.
 *
 * What the job does here. The job keeps a list of 36-byte records (the first three words of each a position, x y z) and,
 * before it builds its output, sorts the list by squared distance from a point, the farthest first. The point is the
 * quadword the caller (0x8C34) stores at LS 0xB9A0 just before the call. The sort is a recursive quicksort in two lifted
 * functions, 0x6D28 and 0x5DF8:
 *     0x6D28 (r3 = n, r4 = list, r5 = C, r6 a byte it does not use)
 *         while n > C: p = 0x5DF8(n, list); the part of p records before the split and the part of n - p - 1 after it
 *         are sorted by the same function, the smaller by a call, the larger by the loop (so the depth is at most
 *         log2 n); the record at the split is where the partition put the pivot. The caller passes C = 1.
 *     0x5DF8 (r3 = n, r4 = list): one partition. n == 2: the two records are swapped unless the first is strictly
 *         farther, and 0 is returned. n >= 3: the pivot is the median of the first, middle (index n / 2) and last
 *         record by key (the tests are strict, in the order the lifted code makes them: with keys a, b, c of the first,
 *         middle and last, b > a gives the first when a > c, else the middle when c > b, else the last; not b > a gives
 *         the first when c > a, else the middle when b > c, else the last); the pivot goes to a spare place and its
 *         slot takes the last record; i = 0, j = n - 1; while j > i: a record i farther than the pivot makes i grow;
 *         otherwise the record j - 1 is looked at: if it is not nearer than the pivot the records i and j - 1 are
 *         swapped and i grows and j shrinks, else j shrinks; then record i goes to the last place and the pivot to
 *         record i, and i is returned.
 *     The key of a record is the SPU's single precision (((x - cx)^2 + (y - cy)^2) + (z - cz)^2), compared with the
 *     SPU's float compare (a sign-magnitude order with denormals as zero); a swap or move copies the first 34 bytes
 *     of a record (eight words and a halfword), not the last two.
 * The module's shuffle patterns at LS 0xB020 and 0xB030 (the two rotations of the squares) are what the model builds in.
 *
 * What the native does. One call of 0x6D28 does the whole sort: the n records are read into a private array, each with
 * its key worked out once, the same quicksort runs on it (the same tests in the same order, so records of equal keys
 * end where the lifted code leaves them), and the 34 bytes of each record go back to the local store. Besides the list
 * the lifted code writes only its stack frames (below sp: dead after the return) and r0 at sp + 0x10 (the link slot of
 * the caller's frame, written at the call's entry whether or not there is anything to sort); the native writes that
 * quadword too and leaves every register as the call found it (r0, r1 and the callee-saved r80..r127 are what a caller
 * can see: it returns nothing, and no caller reads r2..r79 after it: the call at 0x8C6C of 0x8C34, the only one from
 * outside the sort, keeps what it needs across the call in r80 and above, and a liveness pass over the module's code
 * finds no register of r2..r79 read after 0x8C70 before it is written; the sort's own calls read r80..r86 only).
 *
 * What it declines (before it writes; the lifted function then runs, and its own recursive calls are offered to the
 * native again): a stack pointer that is not quadword aligned or leaves the local store, a C below 1, a list whose
 * address is not a multiple of 4 or that reaches past the local store or overlaps the stack (the 4 KB under sp and the
 * link slot) or the constants (the point at 0xB9A0, the patterns at 0xB020 / 0xB030), a stack that reaches those
 * constants (the lifted frames would write over them), patterns that are not the ones built in, a FPCR other than
 * round-toward-zero + flush-to-zero.
 *
 * Floats. A record whose three coordinates and the point's are below 2^59 in magnitude has its key computed on plain
 * floats under FPCR round-toward-zero + flush-to-zero (the SPU threads' mode; no operation can reach FLT_MAX: the squares
 * are below 2^120); a record with a larger or exponent-255 float takes the engine's float helpers (spu_fs_lean, spu_fm_lean,
 * spu_fa_lean), exact for every input. Both give the lifted code's bits.
 *
 * PS3_NATIVE_J201: 1 (default) on, 0 off (the lifted code), 2 a check run: every call the native takes runs on a copy
 * of the local store, then the lifted function runs on the real one and the two stores are compared whole (but for the
 * dead stack under sp), with r0, r1 and r80..r127; the lifted result is what stays. PS3_SPU_NATIVE=0 turns every
 * native rewrite off. The stats line (every 10 s) counts the calls taken and declined (by reason), the records
 * sorted and, in the check run, the calls compared and the ones that differ. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "spu_context.h"
#include "spu_helpers.h"

int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);

#define J201_IMAGE 201                  /* jobmod_14e77a6441c8_e6ce0 */
#define J201_SORT  0x6D28u
#define J201_CAM   0xB9A0u              /* the point: a quadword the caller stores before the call */
#define J201_PAT1  0xB020u              /* the module's shuffle patterns: the squares rotated by one word, by two */
#define J201_PAT2  0xB030u
#define J201_REC   36u                  /* a record */
#define J201_COPY  34u                  /* the bytes of it a move copies */
#define J201_DEAD  0x1000u              /* the stack the lifted sort can write below sp: at most 13 levels of 160 + 192 */

#ifndef J201_COV
#define J201_COV(n) ((void)0)       /* the host test counts which branches its inputs reach */
#endif

enum { DECL_NONE = -1, DECL_ARGS = 0, DECL_OVERLAP, DECL_CONST, DECL_FPCR, DECL_N };

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

/* ---------------------------------------------------------------------------------- the store */
static inline uint32_t R32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void W32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 SPLAT(uint32_t w) { return V4(w, w, w, w); }
static inline int ovl(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 < b1 && b0 < a1; }

/* ---------------------------------------------------------------------------------- the key */
#if defined(__aarch64__)
#define OPAQUE_F(f) __asm__("" : "+w"(f))
#else
#define OPAQUE_F(f) __asm__("" : "+x"(f))
#endif
static inline float FF(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static inline uint32_t FU(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
static inline int small_w(uint32_t w) { return ((w >> 23) & 0xFFu) < 0xBAu; }       /* |x| < 2^59 (or a zero / denormal) */
/* spu_fcgt_lean's key: a zero (denormals too) made +0, the sign-magnitude word as a two's-complement integer */
static inline int32_t fkey(uint32_t x)
{
    const uint32_t f = (x & 0x7F800000u) ? x : 0u;
    return (int32_t)(f ^ ((uint32_t)((int32_t)f >> 31) >> 1));
}
/* the key on plain floats: ((x - cx)^2 + (y - cy)^2) + (z - cz)^2, each operation one truncation (FPCR RZ + FZ). The
 * empty asm on one operand of each keeps the compiler from folding or reassociating, which would round to nearest. */
static inline uint32_t key_fast(const uint32_t* w, const float* c)
{
    float x = FF(w[0]), y = FF(w[1]), z = FF(w[2]);
    OPAQUE_F(x); OPAQUE_F(y); OPAQUE_F(z);
    float dx = x - c[0], dy = y - c[1], dz = z - c[2];
    OPAQUE_F(dx); OPAQUE_F(dy); OPAQUE_F(dz);
    float sx = dx * dx, sy = dy * dy, sz = dz * dz;
    OPAQUE_F(sx); OPAQUE_F(sy); OPAQUE_F(sz);
    float s = sx + sy;
    OPAQUE_F(s);
    s = s + sz;
    return FU(s);
}
/* the same on the engine's float helpers (one lane of each): exact for every input including the extended range */
static inline uint32_t sx_fs(uint32_t a, uint32_t b) { return spu_fs_lean(SPLAT(a), SPLAT(b))._u32[0]; }
static inline uint32_t sx_fm(uint32_t a, uint32_t b) { return spu_fm_lean(SPLAT(a), SPLAT(b))._u32[0]; }
static inline uint32_t sx_fa(uint32_t a, uint32_t b) { return spu_fa_lean(SPLAT(a), SPLAT(b))._u32[0]; }
static uint32_t key_exact(const uint32_t* w, const uint32_t* c)
{
    uint32_t s[3];
    for (int k = 0; k < 3; k++) { const uint32_t d = sx_fs(w[k], c[k]); s[k] = sx_fm(d, d); }
    return sx_fa(sx_fa(s[0], s[1]), s[2]);
}

/* ---------------------------------------------------------------------------------- the sort */
typedef struct { int32_t k; uint8_t p[J201_REC]; } Rec;     /* the key as the compare sees it, the record's 36 bytes */

static inline void swap_rec(Rec* a, Rec* b) { const Rec t = *a; *a = *b; *b = t; }

/* 0x5DF8: one partition of the n >= 2 records at a; the split index */
static int32_t part(Rec* a, int32_t n)
{
    if (n == 2) {
        if (!(a[0].k > a[1].k)) { swap_rec(&a[0], &a[1]); J201_COV(1); } else J201_COV(0);
        return 0;
    }
    const int32_t last = n - 1, mid = n >> 1;
    const int32_t k0 = a[0].k, km = a[mid].k, kl = a[last].k;
    int32_t piv;
    if (km > k0) { if (k0 > kl) { piv = 0; J201_COV(2); } else if (kl > km) { piv = mid; J201_COV(3); } else { piv = last; J201_COV(4); } }
    else         { if (kl > k0) { piv = 0; J201_COV(5); } else if (km > kl) { piv = mid; J201_COV(6); } else { piv = last; J201_COV(7); } }
    const Rec pv = a[piv];
    a[piv] = a[last];
    const int32_t kp = pv.k;
    int32_t i = 0, j = last;
    for (;;) {
        if (a[i].k > kp) { i++; J201_COV(8); if (!(j > i)) { J201_COV(11); break; } continue; }
        for (;;) {                                          /* record i is not farther than the pivot */
            if (a[j - 1].k >= kp) { swap_rec(&a[i], &a[j - 1]); j--; i++; J201_COV(9); break; }
            j--; J201_COV(10);
            if (!(j > i)) { J201_COV(12); goto done; }
        }
        if (!(j > i)) { J201_COV(13); break; }
    }
done:
    a[last] = a[i];
    a[i] = pv;
    return i;
}

/* 0x6D28: while n > C, partition; the smaller side by a call, the larger by the loop */
static void qsort_rec(Rec* a, int32_t n, int32_t C)
{
    while (n > C) {
        const int32_t p = part(a, n);
        if (n - p > p) { J201_COV(14); qsort_rec(a, p, C); a += p + 1; n = n - p - 1; }
        else           { J201_COV(15); qsort_rec(a + p + 1, n - p - 1, C); n = p; }
    }
}

/* ---------------------------------------------------------------------------------- the model */
typedef struct { Rec* buf; size_t cap; uint8_t* ls_copy; } Tls;
static _Thread_local Tls* t_tls;
static pthread_key_t s_key;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static int s_key_ok;
static void tls_free(void* p) { Tls* t = (Tls*)p; if (t) { free(t->buf); free(t->ls_copy); free(t); } }
static void key_make(void) { s_key_ok = pthread_key_create(&s_key, tls_free) == 0; }
static Tls* tls_get(void)                                   /* this thread's buffers (kept for the thread's life), or null */
{
    if (t_tls) return t_tls;
    pthread_once(&s_once, key_make);
    if (!s_key_ok) return 0;
    Tls* t = (Tls*)calloc(1, sizeof *t);
    if (!t) return 0;
    if (pthread_setspecific(s_key, t)) { free(t); return 0; }
    return t_tls = t;
}

static const uint8_t k_pat[32] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                   8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 };

/* the call on (gpr, ls): 1 = done, 0 = declined and nothing written */
static int j201_core(u128* gpr, uint8_t* ls, int* why)
{
    const uint32_t sp = gpr[1]._u32[0], base = gpr[4]._u32[0];
    const int32_t n = (int32_t)gpr[3]._u32[0], C = (int32_t)gpr[5]._u32[0];
    if ((sp & 15u) || sp < J201_DEAD || (uint64_t)sp + 0x20u > SPU_LS_SIZE) { *why = DECL_ARGS; return 0; }
    const int work = n > C;
    if (work) {
        if (C < 1 || (base & 3u)) { *why = DECL_ARGS; return 0; }
        if ((uint64_t)base + (uint64_t)n * J201_REC > SPU_LS_SIZE) { *why = DECL_ARGS; return 0; }
        const uint64_t e = (uint64_t)base + (uint64_t)n * J201_REC;
        if (ovl(base, e, sp - J201_DEAD, (uint64_t)sp + 0x20u) || ovl(base, e, J201_CAM, J201_CAM + 16u) ||
            ovl(base, e, J201_PAT1, J201_PAT1 + 32u) ||
            ovl(sp - J201_DEAD, sp, J201_CAM, J201_CAM + 16u) || ovl(sp - J201_DEAD, sp, J201_PAT1, J201_PAT1 + 32u)) {
            *why = DECL_OVERLAP; return 0;           /* the lifted frames would write over the point or the patterns */
        }
        if (memcmp(ls + J201_PAT1, k_pat, 32)) { *why = DECL_CONST; return 0; }
#if SPU_A64_FPCR
        if ((spu__fpcr_get() & SPU_FPCR_RZFZ) != SPU_FPCR_RZFZ) { *why = DECL_FPCR; return 0; }
#endif
        Tls* t = tls_get();
        if (!t) { *why = DECL_ARGS; return 0; }
        if (t->cap < (size_t)n) {
            Rec* nb = (Rec*)realloc(t->buf, (size_t)n * sizeof(Rec));
            if (!nb) { *why = DECL_ARGS; return 0; }
            t->buf = nb; t->cap = (size_t)n;
        }
        Rec* a = t->buf;
        uint32_t cw[3];
        float cf[3];
        for (int k = 0; k < 3; k++) { cw[k] = R32(ls + J201_CAM + 4 * k); cf[k] = FF(cw[k]); }
        const int cam_small = small_w(cw[0]) && small_w(cw[1]) && small_w(cw[2]);
        const uint8_t* src = ls + base;
        for (int32_t i = 0; i < n; i++, src += J201_REC) {
            uint32_t w[3] = { R32(src), R32(src + 4), R32(src + 8) };
            memcpy(a[i].p, src, J201_REC);
            if (cam_small && small_w(w[0]) && small_w(w[1]) && small_w(w[2])) { a[i].k = fkey(key_fast(w, cf)); J201_COV(16); }
            else { a[i].k = fkey(key_exact(w, cw)); J201_COV(17); }
        }
        qsort_rec(a, n, C);
        uint8_t* dst = ls + base;
        for (int32_t i = 0; i < n; i++, dst += J201_REC) memcpy(dst, a[i].p, J201_COPY);
    }
    else J201_COV(18);
    for (int k = 0; k < 4; k++) W32(ls + sp + 0x10u + 4u * (uint32_t)k, gpr[0]._u32[k]);     /* r0 into the link slot */
    return 1;
}

/* ---------------------------------------------------------------------------------- entry, check run, stats */
typedef struct { unsigned long long calls, native, declined[DECL_N], recs, cmp, bad, regbad; } Stat;
static Stat s_st;
static int s_mode = -1;                                    /* PS3_NATIVE_J201 */
static unsigned long long s_calls;
static _Thread_local int t_bypass;                         /* the lifted function runs inside a check run */

#define ADD(f, n) __atomic_fetch_add(&(f), (n), __ATOMIC_RELAXED)

static void stats(unsigned long long n)
{
    fprintf(stderr, "[j201] %llu calls: native %llu, declined %llu (args %llu, overlap %llu, const %llu, fpcr %llu), records %llu",
            n, s_st.native, s_st.declined[0] + s_st.declined[1] + s_st.declined[2] + s_st.declined[3], s_st.declined[DECL_ARGS],
            s_st.declined[DECL_OVERLAP], s_st.declined[DECL_CONST], s_st.declined[DECL_FPCR], s_st.recs);
    if (s_mode == 2) fprintf(stderr, ", compared %llu, differ %llu (%llu in registers)", s_st.cmp, s_st.bad, s_st.regbad);
    fputc('\n', stderr);
}

/* the stats line, at most once in 10 s (the clock is read on every 64th call) */
static void stats_tick(unsigned long long n)
{
    static uint64_t next;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    const uint64_t now = (uint64_t)t.tv_sec;
    uint64_t w = __atomic_load_n(&next, __ATOMIC_RELAXED);
    if (now < w || !__atomic_compare_exchange_n(&next, &w, now + 10, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    if (w) stats(n);
}

/* mode 2: the model on a copy, the lifted function on the real store, then the comparison: the store whole but for the
 * dead stack under sp, and r0, r1 and the callee-saved r80..r127 (the sort returns nothing and no caller reads r2..r79) */
static int j201_check(spu_context* ctx)
{
    void (*lifted)(spu_context*) = spu_lookup(J201_SORT, J201_IMAGE);
    if (!lifted) return 0;
    Tls* t = tls_get();
    if (!t) return 0;
    if (!t->ls_copy && !(t->ls_copy = (uint8_t*)malloc(SPU_LS_SIZE))) return 0;
    uint8_t* const cp = t->ls_copy;
    memcpy(cp, SPU_LS_BASE(ctx), SPU_LS_SIZE);
    int why = DECL_NONE;
    u128 g[128];
    memcpy(g, ctx->gpr, sizeof g);
    const int32_t n = (int32_t)g[3]._u32[0];
    const uint32_t sp = g[1]._u32[0], base = g[4]._u32[0];
    if (!j201_core(g, cp, &why)) { ADD(s_st.declined[why], 1); return 0; }
    const int32_t si = (int32_t)ctx->image_id;
    const uint32_t r0 = ctx->gpr[0]._u32[0];
    t_bypass = 1;
    ctx->host_depth++; lifted(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_bypass = 0;
    uint32_t first = 0, bytes = 0;
    const uint8_t* const ls = SPU_LS_BASE(ctx);
    for (uint32_t i = 0; i < SPU_LS_SIZE; i += 16) {
        if (i + 16 > sp - J201_DEAD && i < sp) continue;
        if (!memcmp(ls + i, cp + i, 16)) continue;
        for (uint32_t k = i; k < i + 16; k++)
            if (ls[k] != cp[k]) { if (!bytes) first = k; bytes++; }
    }
    int reg = -1;
    for (int r = 0; r < 128 && reg < 0; r++)
        if ((r < 2 || r >= 80) && memcmp(&ctx->gpr[r], &g[r], 16)) reg = r;
    ADD(s_st.cmp, 1);
    if (reg >= 0) ADD(s_st.regbad, 1);
    if (bytes || reg >= 0) {
        const unsigned long long nb = __atomic_add_fetch(&s_st.bad, 1, __ATOMIC_RELAXED);
        if (nb <= 12)
            fprintf(stderr, "[j201] check DIFFER #%llu: n=%d list=%05X C=%d sp=%05X: %u bytes differ (first at %05X, lifted %02X native %02X), register r%d\n",
                    nb, (int)n, base, (int)(int32_t)g[5]._u32[0], sp, bytes, first, bytes ? ls[first] : 0, bytes ? cp[first] : 0, reg);
    }
    ADD(s_st.recs, n > 0 ? (unsigned long long)n : 0);
    ctx->pc = ctx->gpr[0]._u32[0];
    return 1;
}

static int j201_entry(spu_context* ctx)
{
    if (t_bypass) return 0;
    if (__builtin_expect(__atomic_load_n(&s_mode, __ATOMIC_ACQUIRE) < 0, 0)) {
        const char* e = getenv("PS3_NATIVE_J201");
        const int m = e && e[0] ? (e[0] == '2' ? 2 : e[0] != '0') : 1;
        int unset = -1;
        if (__atomic_compare_exchange_n(&s_mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))   /* the first thread says so */
            fprintf(stderr, "[j201] native depth sort (image %d, 0x%X): %s\n", J201_IMAGE, J201_SORT,
                    m == 2 ? "check run against the lifted code" : m ? "on" : "off");
    }
    if (!s_mode) return 0;
    const unsigned long long n = __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);
    if (!(n & 63)) stats_tick(n);
    ADD(s_st.calls, 1);
    if (s_mode == 2) {
        const int r = j201_check(ctx);
        if (r) ADD(s_st.native, 1);
        return r;
    }
    int why = DECL_NONE;
    if (!j201_core(ctx->gpr, SPU_LS_BASE(ctx), &why)) { ADD(s_st.declined[why], 1); return 0; }
    ADD(s_st.native, 1);
    { const int32_t cnt = (int32_t)ctx->gpr[3]._u32[0]; if (cnt > 0) ADD(s_st.recs, (unsigned long long)cnt); }
    return 1;
}

__attribute__((constructor)) static void j201_register(void)
{
    spu_native_register(J201_IMAGE, J201_SORT, j201_entry);
}
