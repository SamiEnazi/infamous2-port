/* From the Nintendo Switch fork github.com/4evry1/infamous2-port-NX (branch switch-nx @ 49fc23333713,
 * switch/natives/job209_native.c, MIT licence of the project), unchanged except this note: there it builds for
 * the Switch only; here it is part of the PC build (natives/spu). The SPU modules and image ids are the same. */
/* job209_native.c -- inFAMOUS 2 (BCES01143): the worker of SPU job 209 (jobmod_867e48f161e0_e10e8, LS 0x4030)
 * natively. Switch build only (switch/natives/: the PC build compiles every C file of natives/spu); written for this
 * port, not derived from another project.
 *
 * What the job is. The detail-level and fade updater that sits beside the flat instance culler (job 208): it takes the
 * same flat list of 28-byte entries and walks the boxes of each, per box moving a detail level and, for the items of the
 * box, a fade value. The module's crt0 and wrapper (0x50E8, 0x4DA0) get three buffers from the job manager and call the
 * worker 0x4030 with
 *     r3 = P  a header: +0x00 camera (xyz), +0x10 .. +0x2F two 128-bit class masks, +0x30 the distance thresholds T0, T1, T2 ...
 *             (floats; T0 at +0x30, T1 at +0x34 ...), +0x4C the entry count N, +0x50 the frame time (word) and +0x54 a flag byte
 *     r4 = B  the entry list (P + 0x60), 28 bytes an entry
 *     r5 = C  the gather area: two slots of 0x8A40 bytes (an entry uses slot (index & 1), so one entry is fetched while
 *             the previous one is worked on)
 *     r6 = D  the two DMA tags, one a slot (the native has no use for them)
 * An entry: +8 transform EA (a 4x4 row matrix, 64 bytes), +12 item base EA, +16 EA of block 1, +20 EA of block 2, +24
 * u16 number of boxes' records in block 2, +26 u16 size of block 1. Per entry the job GETs the three blocks into its
 * slot (block 1 at +0 up to 16 KB, block 2 at +0x4000, the transform at +0x8000; sizes rounded up to 16):
 *     block 1: u32 n boxes, u32 item total, n boxes of 28 bytes (min xyz, max xyz as floats, +24 a halfword: the low 7
 *              bits are a class and the upper nine, signed, select a class mask (a negative halfword: no mask), +26 s16 first item), then the items, 16 bytes
 *              (+0 s16 index into the item base, +2 a byte: low nibble = upper level, high nibble = lower level, signed)
 *     block 2: per box 8 bytes: +0 flag, +1 older level, +2 level, +4 u32 time of the last change
 * and PUTs block 2 back at the end of the entry. Per box, in order:
 *   - the box through the entry's transform (the same code as job 208's 0x5270 and job 210's: centre, |M| * extent),
 *     its distance from the camera (the clamped point, the squared distance summed as (x + y) + z, a reciprocal square
 *     root estimate with one Newton step, times the squared distance), and from that the level L: the number of
 *     thresholds below it (0 when it is not above T0, else the first i >= 1 with distance <= T_i);
 *   - the box's class (halfword +24): when it is negative, or its bit is set in the class mask the halfword selects,
 *     the level stands and the box is "kept"; when the bit is clear the level is 6 and the box is "not kept";
 *   - the record: a box that is not kept, or any box when the header's flag byte (+0x54) is set, takes (flag 0, older
 *     level L, level L); a kept box goes through a hysteresis: flag 0xFF as that; flag 1 ("changing") returns to 0 with
 *     older level := level once the frame time is more than 720 past the record's time; then with flag 0 a level that
 *     differs from L becomes flag 1, time := frame time, level := L; a flag of anything else is left alone;
 *   - when the flag was or is now nonzero, or the older level or the level changed, the items of the box (first item up
 *     to the next box's first item, or the item total) are brought in by one DMA list (16 bytes at base + 16 * index
 *     each), each gets bit 0 of its flag byte set to "in the range of the level and not in that of the older level" and
 *     byte 2 set to the fade, clamp((now - time) / 720, 0, 1) * 64 truncated, while exactly one of the two ranges holds
 *     it, else 64 when both do and 0 when neither; the records go back by a list PUT.
 * The worker does no tag wait the model needs and returns nothing; the callee-saved registers are what it found.
 *
 * What the native does. It reads each entry's three blocks from guest memory where the job's GETs would (same sizes, same
 * addresses, in the same order: the next entry's before the work on this one), works on private copies of them, reads
 * the items' records from guest memory where the job's list GETs would, and queues every PUT the job would do (block 2
 * of each entry, the item records of each box) in order; a read sees the PUTs queued before it laid over the guest's bytes,
 * as the job's GET sees the PUTs it did. Only when the whole job was modelled are the PUTs done, in
 * order, with the coherency notices the MFC gives them; until then nothing outside the native's own buffers is
 * written, so a call that ends in a decline (the lifted job then runs from the start) changes nothing. The local store
 * is not written: the two slots, the item scratch and the stack the lifted worker spills into are dead after it, and
 * the callee-saved registers (r0, r1, r80..r127) are the ones it found. What it declines, before it writes:
 *   - an argument the layout does not fit: P, B or C not quadword aligned, sp not quadword aligned, the entry list or
 *     header reaching the slots, the stack or the module's code, more than 4096 entries;
 *   - a block the job's GET would skip or turn into something else (the MFC refuses a size over 16 KB or a size of zero
 *     and one whose EA is not 16-aligned like the local-store address, and skips an EA that is not committed memory; a
 *     128-byte transfer that matches the Edge viewport signature is rewritten), an entry whose boxes or items reach past the
 *     bytes its GET fetched, more than 64 items in a box (the list would run over the next area), a first item
 *     that is negative or past the end, an item record whose EA is not 16-aligned or committed;
 *   - the module's constants not what the model has built in (the two shuffle patterns at LS 0x5230 / 0x5240), a read of
 *     the local store that would land in the slots or the stack, more than 4096 queued PUTs or 4 MB of them,
 *     the job recorder on.
 *
 * Floats. The arithmetic is the SPU's single precision. For the data this job really sees (every float of the box, the
 * matrix and the camera below 2^24 in magnitude: then no operation can reach FLT_MAX, and the largest value met is about
 * 2^108) the distance of a box runs on plain floats under FPCR round-toward-zero + flush-to-zero, the mode of the SPU
 * threads, with the helpers' own estimate tables and steps; the fade the same. A box with a larger float, a thread in
 * another floating-point mode, or PS3_NATIVE_J209_SLOW=1, takes the engine's float helpers (spu_fm_lean, spu_frsqest,
 * spu_fi, spu_csflt, spu_cflts ...) with the lifted code's operands, exact for every input including the extended range.
 *
 * PS3_NATIVE_J209: 1 (default) on, 0 off (the lifted worker), 2 a check run: the model runs without doing its PUTs, then
 * the lifted worker runs on the real context, and the two are compared: the PUTs the model queued against the guest
 * memory the lifted worker left (the later of two overlapping PUTs wins; a PUT the lifted worker makes that the model
 * leaves out is not seen by this comparison), r0, r1 and r80..r127 against what the call
 * found, and the local store outside the slots and the stack against the copy taken before; the lifted result is what
 * stays. A job whose guest blocks changed between the model's read and the lifted worker's own (another job writes them)
 * can differ without an error in the model; the stats line counts the compared and the differing ones.
 * PS3_SPU_NATIVE=0 turns every native rewrite off. The stats line (every 10 s) counts
 * the calls the native took and declined (by reason), the entries, boxes and items it worked on. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include "spu_context.h"
#include "spu_helpers.h"

extern uint8_t* vm_base;
extern volatile int g_spu_jobrec_mode;
extern uint32_t g_spu_put_rsx_lo, g_spu_put_rsx_hi;
extern int g_spu_put_rsx_exact;
extern int (*g_spu_put_rsx_hook)(uint32_t ea, uint32_t size);
extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
int spu_native_ea_ok(uint32_t ea, uint32_t size);
int spu_coh_is_reserved(uint32_t addr);
void spu_coh_notify_write_except(uint32_t ea, const void* self);
uint64_t spu_lockline_mask_ea(uint32_t ea);
uint64_t spu_lockline_mask_range(uint32_t ea, uint32_t len);
void spu_lockline_lock_mask(uint64_t mask);
void spu_lockline_unlock_mask(uint64_t mask);

#ifndef J209_TRACE
#define J209_TRACE(...) ((void)0)
#endif
#ifndef J209_T
#define J209_T(n) ((void)0)         /* the host test times the sections of the model */
#endif
#ifndef J209_COV
#define J209_COV(n) ((void)0)       /* the host test counts which branches its inputs reach */
#endif
#define J209_IMAGE  209
#define J209_WORKER 0x4030u

#define J209_SLOT      0x8A40u     /* the gather area: two slots of this many bytes */
#define J209_MAXITEMS  64u         /* the item scratch of a slot: 0x400 bytes of records, 0x200 of list elements */
#define J209_MAXENT    4096u
#define J209_MAXPW     4096u
#define J209_ARENA_MAX (4u << 20)
#define J209_BITS      16384u

enum { FB_LS = 1, FB_GET = 2, FB_EA = 4, FB_RANGE = 8, FB_ITEMS = 16, FB_CONST = 32, FB_BIG = 64 };
#define J209_NREASON 7

/* ---------------------------------------------------------------------------------- small helpers */
static inline uint32_t R32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t R16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline void W32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 SPLAT(uint32_t w) { return V4(w, w, w, w); }
static inline u128 LQ(const uint8_t* q) { return V4(R32(q), R32(q + 4), R32(q + 8), R32(q + 12)); }   /* the quadword at q, as spu_ls_read128 */
static inline int ovl(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1 && a0 < a1 && b0 < b1; }
static inline int ea_ok(uint32_t ea, uint32_t size) { return ea >= 0x10000u && spu_native_ea_ok(ea, size); }

/* ---------------------------------------------------------------------------------- the state of one call */
typedef struct { uint32_t ea, size, off; } PW;              /* a queued PUT: the guest range and where its bytes are in the arena */

typedef struct {
    uint32_t ea1, ea2, ea3, base;                           /* the entry's four words */
    uint32_t sz1, sz2;                                      /* bytes fetched of block 1 / 2 (0 = the GET did not happen) */
    uint32_t n2;                                            /* u16 at +24: records block 2 holds (and the PUT writes) */
} Ent;

typedef struct {
    spu_context* ctx;
    uint8_t* ls;
    int fallback;
    uint32_t bad_ea, bad_size;
    uint32_t P, B, C, sp, N;
    int fast;                                               /* the thread runs under round-toward-zero + flush-to-zero */
    uint32_t dead_lo[2], dead_hi[2];                        /* LS ranges the lifted worker writes: slots, stack */
    Ent ent[2];                                             /* the two slots' entries */
    uint32_t npw, cappw, pw_lo, pw_hi;                      /* the queued PUTs and the hull of their ranges */
    PW* pw;
    uint64_t pwbits[J209_BITS / 64];                        /* the lines the queued PUTs touch (hashed) */
    uint8_t* arena; uint32_t arena_n, arena_cap;
    uint64_t n_ent, n_box, n_item;
    uint8_t b1[2][0x4000];
    uint8_t b2[2][0x4000];
    uint8_t b3[2][64];
} J;

static int s_fast = 1;                                       /* PS3_NATIVE_J209_SLOW=1: always the helpers */
static _Thread_local J* t_j;
static _Thread_local uint8_t* t_lsb;                        /* the check run's copy of the store */
static _Thread_local int t_bypass;

/* ---------------------------------------------------------------------------------- guest memory */
/* A read of guest memory as the job's GET would see it: what the guest holds, then every PUT queued earlier in this call
 * laid over it in order (the job does its PUTs as it goes; the native does them at the end). A bitmap of the 16-byte
 * lines the queued PUTs touch (hashed to 16384 bits) answers "none" for nearly every item record; a hit, and every
 * larger read, goes through the queue. */
static inline uint32_t lbit(uint32_t line) { return (line ^ (line >> 14)) & (J209_BITS - 1); }
static void gread(const J* j, uint8_t* dst, uint32_t ea, uint32_t size)
{
    memcpy(dst, vm_base + ea, size);
    if (!j->npw || ea + size <= j->pw_lo || ea >= j->pw_hi) return;
    if (size <= 16) {                                       /* at most two lines */
        const uint32_t l0 = ea >> 4, l1 = (ea + size - 1) >> 4;
        const uint32_t b0 = lbit(l0), b1 = lbit(l1);
        if (!(j->pwbits[b0 >> 6] >> (b0 & 63) & 1) && !(j->pwbits[b1 >> 6] >> (b1 & 63) & 1)) return;
    }
    for (uint32_t i = 0; i < j->npw; i++) {
        const PW* w = &j->pw[i];
        const uint32_t lo = ea > w->ea ? ea : w->ea, hi = ea + size < w->ea + w->size ? ea + size : w->ea + w->size;
        if (lo < hi) memcpy(dst + (lo - ea), j->arena + w->off + (lo - w->ea), hi - lo);
    }
}

/* the checks of a GET the MFC would do as asked (mfc_do_transfer_lean); 0 = decline with the reason set */
static int get_ok(J* j, uint32_t ea, uint32_t size, int is_blk)
{
    if (!size) return 1;                                    /* the caller handles "no transfer" */
    if (size > 0x4000u || (size >= 16 && (size & 15)) || (ea & 15)) { j->fallback |= FB_GET; return 0; }
    if (!ea_ok(ea, size)) {
        if (!(j->fallback & FB_EA)) { j->bad_ea = ea; j->bad_size = size; }
        j->fallback |= FB_EA; return 0;
    }
    static const uint8_t edge_sig[16] = { 0, 0, 0, 0, 0x05, 0x00, 0x02, 0xD0, 0xBF, 0x80, 0, 0, 0x3F, 0x80, 0, 0 };
    if (is_blk && size == 0x80) { uint8_t h[16]; gread(j, h, ea, 16); if (!memcmp(h, edge_sig, 16)) { j->fallback |= FB_GET; return 0; } }
    return 1;
}

static int pw_add(J* j, uint32_t ea, const uint8_t* data, uint32_t size)
{
    if (!size) return 1;
    if (j->npw >= J209_MAXPW) { j->fallback |= FB_BIG; return 0; }
    if (j->arena_n + size > j->arena_cap) {
        uint32_t nc = j->arena_cap ? j->arena_cap * 2 : 65536u;
        while (nc < j->arena_n + size) nc *= 2;
        if (nc > J209_ARENA_MAX) { j->fallback |= FB_BIG; return 0; }
        uint8_t* na = (uint8_t*)realloc(j->arena, nc);
        if (!na) { j->fallback |= FB_BIG; return 0; }
        j->arena = na; j->arena_cap = nc;
    }
    if (j->npw == j->cappw) {
        const uint32_t nc = j->cappw ? j->cappw * 2 : 256u;
        PW* np = (PW*)realloc(j->pw, nc * sizeof(PW));
        if (!np) { j->fallback |= FB_BIG; return 0; }
        j->pw = np; j->cappw = nc;
    }
    for (uint32_t l = ea >> 4, e = (ea + size - 1) >> 4; l <= e; l++) { const uint32_t b = lbit(l); j->pwbits[b >> 6] |= 1ull << (b & 63); }
    memcpy(j->arena + j->arena_n, data, size);
    j->pw[j->npw].ea = ea; j->pw[j->npw].size = size; j->pw[j->npw].off = j->arena_n;
    j->arena_n += size;
    if (!j->npw) { j->pw_lo = ea; j->pw_hi = ea + size; }
    else { if (ea < j->pw_lo) j->pw_lo = ea; if (ea + size > j->pw_hi) j->pw_hi = ea + size; }
    j->npw++;
    return 1;
}

/* spu_put_is_rsx and spu_put_copy of the MFC (spu_dma.h): a PUT of more than 4 bytes into memory the RSX can see (IO-mapped:
 * the table data of this job lives there too) is stored from its highest address down, each store a release, so that a
 * reader of the word it waits on sees everything above it new; any other PUT is an ordinary copy. */
static int put_is_rsx(uint32_t ea, uint32_t n)
{
    const uint32_t hi = __atomic_load_n(&g_spu_put_rsx_hi, __ATOMIC_RELAXED), lo = __atomic_load_n(&g_spu_put_rsx_lo, __ATOMIC_RELAXED);
    if (ea >= hi || (uint64_t)ea + n <= (uint64_t)lo) return 0;
    if (__atomic_load_n(&g_spu_put_rsx_exact, __ATOMIC_RELAXED)) return 1;
    int (*h)(uint32_t, uint32_t) = g_spu_put_rsx_hook;
    return h && h(ea, n);
}
static void put_copy(uint32_t ea, const uint8_t* s, uint32_t n)
{
    uint8_t* g = vm_base + ea;
    if (n > 4u && put_is_rsx(ea, n)) {
        size_t i = n;
        for (; i > 0 && (((uintptr_t)(g + i)) & 7u); i--) __atomic_store_n(g + i - 1, s[i - 1], __ATOMIC_RELEASE);
        for (; i >= 8; i -= 8) { uint64_t v; memcpy(&v, s + i - 8, 8); __atomic_store_n((uint64_t*)(void*)(g + i - 8), v, __ATOMIC_RELEASE); }
        for (; i > 0; i--) __atomic_store_n(g + i - 1, s[i - 1], __ATOMIC_RELEASE);
    } else memcpy(g, s, n);
}

/* one PUT as the MFC does it: the copy, then the notices for reserved lines (mfc_do_transfer_lean) */
static void guest_put(spu_context* ctx, uint32_t ea, const uint8_t* src, uint32_t size)
{
    const uint32_t a0 = ea & ~127u, a1 = (ea + size - 1u) & ~127u;
    int reserved = 0;
    for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) { reserved = 1; break; } if (a == a1) break; }
    if (reserved) {
        const uint64_t m = spu_lockline_mask_range(ea, size);
        spu_lockline_lock_mask(m);
        put_copy(ea, src, size);
        for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) spu_coh_notify_write_except(a, ctx); if (a == a1) break; }
        spu_lockline_unlock_mask(m);
    } else {
        put_copy(ea, src, size);
        for (uint32_t a = a0; ; a += 128u) {
            if (spu_coh_is_reserved(a)) {
                const uint64_t m = spu_lockline_mask_ea(a);
                spu_lockline_lock_mask(m);
                spu_coh_notify_write_except(a, ctx);
                spu_lockline_unlock_mask(m);
            }
            if (a == a1) break;
        }
    }
    if (g_spu_put_hook) g_spu_put_hook(ea, size);
}

static void publish(J* j)
{
    for (uint32_t i = 0; i < j->npw; i++) guest_put(j->ctx, j->pw[i].ea, j->arena + j->pw[i].off, j->pw[i].size);
}

/* ---------------------------------------------------------------------------------- local store reads */
/* 1 when the LS bytes [a, a+n) are ones the lifted worker does not write (not the slots, not its stack) */
static inline int ls_stable(const J* j, uint32_t a, uint32_t n)
{
    if ((uint64_t)a + n > SPU_LS_SIZE) return 0;
    return !ovl(a, a + n, j->dead_lo[0], j->dead_hi[0]) && !ovl(a, a + n, j->dead_lo[1], j->dead_hi[1]);
}

/* ---------------------------------------------------------------------------------- the scalar float form */
/* The job's float code through the engine's helpers (below) is exact for every input but runs four lanes of every
 * operation and checks each one for the extended range. For the data this job really sees (coordinates, a matrix,
 * a camera: every float below 2^24 in magnitude) the same arithmetic on plain floats gives the same bits, when the
 * thread runs under FPCR (MXCSR) round-toward-zero + flush-to-zero, which the SPU threads do: no operation can reach
 * FLT_MAX or see exponent 255 (the largest value met is about 2^108), a denormal operand or result is a zero (flush),
 * and the one thing the helpers do beyond that, making every zero +0, shows in no result here (a zero's sign decides
 * no compare below, which read through fkey, and no value that is stored). The fused multiply-adds are fmaf. Each
 * box's own floats are tested; a box, or a call, that fails the test or runs under another mode takes the helpers. */
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
static inline float FF(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static inline uint32_t FU(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
/* spu_fcgt_lean's key: a zero (denormals too) made +0, the sign-magnitude word as a two's-complement integer */
static inline int32_t fkey(uint32_t x)
{
    const uint32_t f = (x & 0x7F800000u) ? x : 0u;
    return (int32_t)(f ^ ((uint32_t)((int32_t)f >> 31) >> 1));
}
static inline int fgt_(uint32_t a, uint32_t b) { return fkey(a) > fkey(b); }
static inline int small_w(uint32_t w) { return ((w >> 23) & 0xFFu) < 0x97u; }      /* |x| < 2^24 (or a zero / denormal) */
static inline int fp_rzfz(void)
{
#if defined(__aarch64__)
    uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return (v & 0x01C00000u) == 0x01C00000u;                /* RMode = round toward zero, FZ */
#elif defined(__x86_64__) || defined(__i386__)
    return (_mm_getcsr() & 0xE040u) == 0xE040u; /* RC = toward zero, FTZ, DAZ */
#else
    return 0;
#endif
}
/* spu_frsqest and spu_fi on one lane, the helpers' own tables and steps */
static inline uint32_t rsqrt_est(uint32_t bits)
{
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t rexp = exp == 0u ? 0xFFu : 190u - (exp + 1u) / 2u;
    return (rexp << 23) | (spu_frsqest_fraction_lut[(bits >> 18) & 0x3Fu] & 0x7FFFFFu);
}
static inline uint32_t interp(uint32_t a, uint32_t est)
{
    const uint32_t y = a & 0x7FFFFu, hi = ((est >> 10) & 0x1FFFu) << 19, p = (est & 0x3FFu) * y;
    const uint32_t d = hi - p;
    const int brw = hi < p;
    const uint32_t frac = (brw ? d >> 8 : d >> 9) & 0x7FFFFFu;
    const uint32_t e = (est >> 23) & 0xFFu, e2 = e - (uint32_t)brw;
    if (e == 0 || e2 == 0) return 0;
    return (est & 0x80000000u) | (e2 << 23) | frac;
}
/* the box [mn, mx] (x, y, z as float words) through the matrix m (rows of x, y, z), its distance from cam: the bits of
 * the float the job's 0x4538 code leaves in word 0 */
static uint32_t dist_fast(const uint32_t* mn, const uint32_t* mx, const float (*m)[3], const uint32_t* cam)
{
    float c[3], h[3], lo[3], hi[3];
    for (int k = 0; k < 3; k++) { c[k] = (FF(mn[k]) + FF(mx[k])) * 0.5f; h[k] = FF(mx[k]) - c[k]; }
    for (int k = 0; k < 3; k++) {
        const float r11 = m[0][k] * c[0];
        const float r13 = __builtin_fmaf(m[1][k], c[1], r11);
        const float r10 = h[1] * __builtin_fabsf(m[1][k]);
        const float r7 = __builtin_fmaf(m[2][k], c[2], r13);
        const float r5 = __builtin_fmaf(h[0], __builtin_fabsf(m[0][k]), r10);
        const float r4 = r7 + m[3][k];
        const float r6 = __builtin_fmaf(h[2], __builtin_fabsf(m[2][k]), r5);
        hi[k] = r4 + r6; lo[k] = r4 - r6;
    }
    float d2[3];
    for (int k = 0; k < 3; k++) {
        const uint32_t hb = FU(hi[k]), lb = FU(lo[k]);
        const uint32_t t35 = fgt_(cam[k], hb) ? hb : cam[k];
        const uint32_t t9 = fgt_(t35, lb) ? t35 : lb;
        const float d = FF(cam[k]) - FF(t9);
        d2[k] = d * d;
    }
    const float s = (d2[0] + d2[1]) + d2[2];
    const uint32_t sb = FU(s);
    if (!(sb > 0x00800000u)) return 0;                      /* s > FLT_MIN (the sum of squares is never negative) */
    const uint32_t eb = rsqrt_est(sb);
    const float r = FF(interp(sb, eb));
    const float r2 = r * r, rh = r * 0.5f;
    const float e = __builtin_fmaf(-s, r2, 1.0f);
    const float y = __builtin_fmaf(e, rh, r);
    return FU(y * s);
}

/* ---------------------------------------------------------------------------------- the job's float code */
/* 0x4FC8: a box through the 4x4 row-vector transform M (rows in m[0..3]): the centre through it, |M| times the extent */
static void box_xform(u128 mn, u128 mx, const u128* m, u128* lo, u128* hi)
{
    const u128 half = SPLAT(0x3F000000u), one = SPLAT(0x3F800000u), sg = SPLAT(0x80000000u);
    const u128 t = spu_fa_lean(mn, mx);
    const u128 a0 = spu_andc(m[0], sg), a1 = spu_andc(m[1], sg), a2 = spu_andc(m[2], sg);
    const u128 c = spu_fm_lean(t, half);
    const u128 h = spu_fs_lean(mx, c);
    const u128 cc = spu_selb(c, one, V4(0, 0, 0, 0xFFFFFFFFu));
    const u128 cx = SPLAT(cc._u32[0]), cy = SPLAT(cc._u32[1]), cz = SPLAT(cc._u32[2]);
    const u128 hx = SPLAT(h._u32[0]), hy = SPLAT(h._u32[1]), hz = SPLAT(h._u32[2]);
    const u128 r11 = spu_fm_lean(m[0], cx);
    const u128 r13 = spu_fma_lean(m[1], cy, r11);
    const u128 r10 = spu_fm_lean(hy, a1);
    const u128 r7 = spu_fma_lean(m[2], cz, r13);
    const u128 r5 = spu_fma_lean(hx, a0, r10);
    const u128 r4 = spu_fa_lean(r7, m[3]);
    const u128 r6 = spu_fma_lean(hz, a2, r5);
    *hi = spu_fa_lean(r4, r6);
    *lo = spu_fs_lean(r4, r6);
}

/* the distance of the camera from the box [lo, hi] (the job's 0x4538..0x45D8): clamp, squared sum (x + y) + z, root */
static u128 box_dist(u128 cam, u128 lo, u128 hi)
{
    const u128 half = SPLAT(0x3F000000u), one = SPLAT(0x3F800000u);
    const u128 g1 = spu_fcgt_lean(cam, hi);
    const u128 t35 = spu_selb(cam, hi, g1);
    const u128 g2 = spu_fcgt_lean(t35, lo);
    const u128 t9 = spu_selb(lo, t35, g2);
    const u128 d = spu_fs_lean(cam, t9);
    const u128 d2 = spu_fm_lean(d, d);
    const u128 sh1 = V4(d2._u32[1], d2._u32[2], d2._u32[3], d2._u32[0]);       /* the pattern at LS 0x5230 */
    const u128 sh2 = V4(d2._u32[2], d2._u32[3], d2._u32[0], d2._u32[1]);       /* the pattern at LS 0x5240 */
    const u128 s1 = spu_fa_lean(d2, sh1);
    const u128 s2 = spu_fa_lean(s1, sh2);
    const u128 s = SPLAT(s2._u32[0]);
    const u128 big = spu_fcgt_lean(s, SPLAT(0x00800000u));
    const u128 est = spu_frsqest(s);
    const u128 r = spu_fi(s, est);
    const u128 r2 = spu_fm_lean(r, r);
    const u128 rh = spu_fm_lean(r, half);
    const u128 e = spu_fnms_lean(s, r2, one);
    const u128 y = spu_fma_lean(e, rh, r);
    const u128 sq = spu_fm_lean(y, s);
    return spu_selb(V4(0, 0, 0, 0), sq, big);
}

/* ---------------------------------------------------------------------------------- the entries */
/* The three GETs of entry e into slot s, as the job does them (0x4118.., 0x42F0..): 0 = declined */
static int fetch(J* j, uint32_t e, uint32_t s)
{
    const uint8_t* E = j->ls + j->B + 28u * e;
    Ent* t = &j->ent[s];
    t->ea3 = R32(E + 8); t->base = R32(E + 12); t->ea1 = R32(E + 16); t->ea2 = R32(E + 20);
    t->n2 = R16(E + 24);
    const uint32_t s1 = R16(E + 26);
    const uint32_t sz1 = (s1 + 15u) & ~15u, sz2 = ((t->n2 << 3) + 15u) & ~15u;
    t->sz1 = t->sz2 = 0;
    if (!get_ok(j, t->ea1, sz1, 1)) return 0;
    if (!get_ok(j, t->ea2, sz2, 1)) return 0;
    if (!get_ok(j, t->ea3, 64, 0)) return 0;
    if (sz1) { gread(j, j->b1[s], t->ea1, sz1); t->sz1 = sz1; }
    if (sz2) { gread(j, j->b2[s], t->ea2, sz2); t->sz2 = sz2; }
    gread(j, j->b3[s], t->ea3, 64);
    return 1;
}

/* One entry: the boxes in order, then block 2's PUT. */
static int work_entry(J* j, uint32_t k, uint32_t s)
{
    (void)k;
    const Ent* t = &j->ent[s];
    uint8_t* b1 = j->b1[s];
    uint8_t* b2 = j->b2[s];
    const uint8_t* ls = j->ls;
    const uint32_t P = j->P;
    if (t->sz1 < 8) { j->fallback |= FB_RANGE; return 0; }
    const uint32_t n = R32(b1), itot = R32(b1 + 4);
    j->n_ent++;
    if (n) {
        if (n > 0x10000u || 8u + 28u * (uint64_t)n > t->sz1 || 8u * (uint64_t)n > t->sz2) { j->fallback |= FB_RANGE; return 0; }
        const uint32_t T = 8u + 28u * n;                   /* the item table in block 1 */
        u128 M[4];
        for (int r = 0; r < 4; r++) M[r] = LQ(j->b3[s] + 16 * r);
        const u128 cam = LQ(ls + P);
        const uint32_t thr0 = R32(ls + P + 0x30);
        const uint32_t now = R32(ls + P + 0x50);
        const int pflag = ls[P + 0x54] != 0;
        /* the scalar form needs a camera and a matrix of ordinary size */
        float mf[4][3];
        int fastm = j->fast;
        for (int r = 0; r < 4; r++) for (int k = 0; k < 3; k++) { fastm &= small_w(M[r]._u32[k]); mf[r][k] = FF(M[r]._u32[k]); }
        for (int k = 0; k < 3; k++) fastm &= small_w(cam._u32[k]);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t Bx = 8u + 28u * i;
            uint8_t* rec = b2 + 8u * i;
            /* the box through the transform, its distance, its level */
            const uint32_t w0 = R32(b1 + Bx), w1 = R32(b1 + Bx + 4), w2 = R32(b1 + Bx + 8);
            const uint32_t w3 = R32(b1 + Bx + 12), w4 = R32(b1 + Bx + 16), w5 = R32(b1 + Bx + 20);
            J209_T(1);
            uint32_t distb;
            if (fastm && small_w(w0) && small_w(w1) && small_w(w2) && small_w(w3) && small_w(w4) && small_w(w5)) {
                const uint32_t mn[3] = { w0, w1, w2 }, mx[3] = { w3, w4, w5 };
                distb = dist_fast(mn, mx, (const float (*)[3])mf, cam._u32);
            } else {
                u128 lo, hi;
                box_xform(V4(w0, w1, w2, w2), V4(w3, w4, w5, w5), M, &lo, &hi);
                distb = box_dist(cam, lo, hi)._u32[0];
            }
            J209_T(3);
            int32_t L = 0;
            if (fgt_(distb, thr0)) {
                uint32_t a = P + 0x34u;
                int32_t it = 0;
                for (;;) {
                    if ((uint64_t)a + 4u > SPU_LS_SIZE || !ls_stable(j, a & ~15u, 16)) { j->fallback |= FB_LS; return 0; }
                    it++;
                    if (!fgt_(distb, R32(ls + a))) break;
                    a += 4;
                    if (it > 256) { j->fallback |= FB_LS; return 0; }
                }
                L = it;
            }
            J209_T(4);
            /* the class of the box: the halfword at +24 */
            const int32_t cw = (int16_t)R16(b1 + Bx + 24);
            int kept;
            if (cw < 0) { kept = 1; J209_COV(0); }
            else {
                const uint32_t v7 = (uint32_t)cw & 127u;
                const int32_t idx = cw >> 7;
                const uint32_t ra = (uint32_t)(P + ((uint32_t)idx << 4) + 0x10u) & 0x3FFF0u;
                if (!ls_stable(j, ra, 16)) { j->fallback |= FB_LS; return 0; }
                kept = (ls[ra + 15u - (v7 >> 3)] >> (v7 & 7)) & 1;
                if (!kept) L = 6;
                J209_COV(kept ? 1 : 2);
            }
            J209_TRACE("box %u Bx %u: cw %08X kept %d L %d dist %08X\n", i, Bx, (unsigned)cw, kept, L, distb);
            J209_T(5);
            /* the record */
            const uint32_t flag0 = rec[0], lev0 = rec[2], old1 = rec[1];
            int path = 0;                                   /* 1 reset (0x46C8), 2 changing (0x4CA0), 3 settled (0x4BC8), 0 leave */
            if (pflag || !kept) { path = 1; J209_COV(3); }
            else if (flag0 == 0xFFu) { path = 1; J209_COV(4); }
            else if (flag0 == 1u) path = 2;
            else path = flag0 == 0 ? 3 : 0;
            J209_COV(5 + path);
            uint32_t fl = flag0;                            /* the flag the "settled" test sees */
            if (path == 2) {
                const uint32_t tm = R32(rec + 4);
                if ((int32_t)now > (int32_t)(tm + 720u)) { rec[0] = 0; rec[1] = rec[2]; fl = 0; path = 3; J209_COV(9); }
                else { path = 0; J209_COV(10); }
            }
            if (path == 1) { rec[0] = 0; rec[2] = (uint8_t)L; rec[1] = (uint8_t)L; }
            else if (path == 3 && fl == 0) {
                if ((int32_t)(int8_t)lev0 != L) { rec[0] = 1; W32(rec + 4, now); rec[2] = (uint8_t)L; J209_COV(11); }
                else J209_COV(12);
            }
            /* the items: only for a box that was or is now changing, or whose level or older level changed */
            int work = flag0 != 0 || rec[0] != 0 || (int8_t)rec[2] != (int8_t)lev0 || (int8_t)rec[1] != (int8_t)old1;
            j->n_box++;
            J209_T(6);
            J209_COV(L > 6 ? 13 : 14 + L);
            if (!work) { J209_COV(21); continue; }
            const int32_t first = (int16_t)R16(b1 + Bx + 26);
            uint32_t end;
            if (i + 1 < n) end = (uint32_t)(int32_t)(int16_t)R16(b1 + Bx + 28 + 26); else end = itot;
            if (first < 0 || end >= 0x10000u || end < (uint32_t)first) { j->fallback |= FB_ITEMS; return 0; }
            const uint32_t c = end - (uint32_t)first;
            if (!c) { J209_COV(22); continue; }
            J209_COV(23);
            if (c > J209_MAXITEMS || T + 16u * end > t->sz1) { j->fallback |= FB_ITEMS; return 0; }
            J209_T(7);
            /* the list GET: 16 bytes each at base + 16 * index; the lifted job's elements, then the records */
            uint32_t eas[J209_MAXITEMS];
            uint8_t irec[J209_MAXITEMS][16];
            for (uint32_t m = 0; m < c; m++) {
                const uint8_t* it = b1 + T + 16u * ((uint32_t)first + m);
                const uint32_t ea = t->base + ((uint32_t)(int32_t)(int16_t)R16(it) << 4);
                if (!get_ok(j, ea, 16, 0)) return 0;
                eas[m] = ea;
                gread(j, irec[m], ea, 16);
            }
            J209_T(8);
            /* the fade of the box: clamp((now - time) / 720, 0, 1) * 64, truncated */
            const uint32_t age = now - R32(rec + 4);
            uint32_t q;
            if (j->fast) {
                uint32_t fb = FU((float)(int32_t)age * FF(0x3AB60B60u));
                if (fgt_(fb, 0x3F800000u)) fb = 0x3F800000u;
                if (fgt_(0u, fb)) fb = 0u;
                q = (uint32_t)(int32_t)(FF(fb) * 64.0f);
            } else {
                u128 f = spu_csflt(SPLAT(age), 155);
                f = spu_fm_lean(f, SPLAT(0x3AB60B60u));
                const u128 one = SPLAT(0x3F800000u), zero = V4(0, 0, 0, 0);
                f = spu_selb(f, one, spu_fcgt_lean(f, one));
                f = spu_selb(f, zero, spu_fcgt_lean(zero, f));
                q = spu_cflts(f, 167)._u32[0];
            }
            const int32_t A1 = (int8_t)rec[1], A2 = (int8_t)rec[2];
            for (uint32_t m = 0; m < c; m++) {
                const uint32_t lv = b1[T + 16u * ((uint32_t)first + m) + 2];
                const int32_t up = (int32_t)(lv << 28) >> 28, lw = (int32_t)(lv << 24) >> 28;
                const int in1 = A1 <= up && lw <= A1, in2 = A2 <= up && lw <= A2;
                J209_COV(24 + in1 + 2 * in2);
                const uint32_t out0 = in1 && !in2;
                uint32_t v;
                if (in1 != in2) { v = q & 0xFFFFu; J209_COV(28 + (q == 0 ? 0 : q == 64 ? 2 : 1)); } else v = in2 ? 64u : 0u;
                irec[m][0] = (uint8_t)((irec[m][0] & 0xFEu) | out0);
                irec[m][2] = (uint8_t)v;
            }
            J209_T(9);
            for (uint32_t m = 0; m < c; m++) {
                j->n_item++;
                if (!pw_add(j, eas[m], irec[m], 16)) return 0;
            }
            J209_T(10);
        }
    }
    /* block 2 back: the aligned part, then the 8-byte tail */
    const uint32_t bytes = t->n2 << 3, al = bytes & 0x7FFF0u, tail = bytes - al;
    if (al || tail) {
        if (!ea_ok(t->ea2, bytes) || t->ea2 < 0x10000u) { j->fallback |= FB_EA; return 0; }
        if (al && !pw_add(j, t->ea2, b2, al)) return 0;
        if (tail && !pw_add(j, t->ea2 + al, b2 + al, tail)) return 0;
    }
    return 1;
}

static int model(J* j)
{
    spu_context* ctx = j->ctx;
    j->ls = SPU_LS_BASE(ctx);
    j->npw = 0; j->arena_n = 0; j->pw_lo = 0; j->pw_hi = 0;
    memset(j->pwbits, 0, sizeof j->pwbits);
    j->n_ent = j->n_box = j->n_item = 0;
    j->fallback = 0; j->bad_ea = j->bad_size = 0;
    j->fast = s_fast && fp_rzfz();
    if (g_spu_jobrec_mode) { j->fallback |= FB_CONST; return 0; }
    j->P = ctx->gpr[3]._u32[0]; j->B = ctx->gpr[4]._u32[0]; j->C = ctx->gpr[5]._u32[0]; j->sp = ctx->gpr[1]._u32[0];
    if ((j->P | j->B | j->C | j->sp) & 15u || (uint64_t)j->P + 0x60u > SPU_LS_SIZE || (uint64_t)j->B + 28u > SPU_LS_SIZE ||
        (uint64_t)j->C + 2u * J209_SLOT > SPU_LS_SIZE || j->sp < 0x400u || (uint64_t)j->sp + 0x40u > SPU_LS_SIZE) { j->fallback |= FB_LS; return 0; }
    const uint8_t* ls = j->ls;
    /* the constants the model has built in: the shuffle patterns of the squared-distance sum */
    static const uint8_t pat[32] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                     8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 };
    if (memcmp(ls + 0x5230, pat, 32)) { j->fallback |= FB_CONST; return 0; }
    j->dead_lo[0] = j->C; j->dead_hi[0] = j->C + 2u * J209_SLOT;
    j->dead_lo[1] = j->sp - 0x300u; j->dead_hi[1] = j->sp + 0x20u;
    j->N = R32(ls + j->P + 0x4C);
    if (j->N > J209_MAXENT) { j->fallback |= FB_LS; return 0; }
    if (!j->N) return 1;
    /* the header and the entry list must not be anything the lifted worker writes */
    if (!ls_stable(j, j->P, 0x60) || !ls_stable(j, j->B, 28u * j->N) || ovl(j->P, j->P + 0x60u, 0x4000u, 0x5400u) ||
        ovl(j->B, j->B + 28u * j->N, 0x4000u, 0x5400u)) { j->fallback |= FB_LS; return 0; }
    if (!fetch(j, 0, 0)) return 0;
    for (uint32_t k = 0; k < j->N; k++) {
        if (k + 1 < j->N && !fetch(j, k + 1, (k + 1) & 1u)) return 0;
        if (!work_entry(j, k, k & 1u)) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------------- entry, check run, stats */
typedef struct { unsigned long long calls, native, declined, why[J209_NREASON], ent, box, item, put, cmp, bad; } Stat;
static Stat s_st;
static int s_mode = -1;                                      /* PS3_NATIVE_J209 */
static unsigned long long s_ncalls;

#define ADD(f, n) __atomic_fetch_add(&(f), (n), __ATOMIC_RELAXED)

static void stats(unsigned long long n)
{
    static const char* const name[J209_NREASON] = { "LS", "GET", "EA", "RANGE", "ITEMS", "CONST", "BIG" };
    char why[200] = "", line[600]; int w = 0, l;
    for (int i = 0; i < J209_NREASON; i++)
        if (s_st.why[i]) w += snprintf(why + w, sizeof why - w, " %s:%llu", name[i], s_st.why[i]);
    l = snprintf(line, sizeof line, "[j209] %llu calls: native %llu, SPU %llu (fallbacks by reason:%s); entries %llu, boxes %llu, items %llu, puts %llu",
                 n, s_st.native, s_st.declined, w ? why : " none", s_st.ent, s_st.box, s_st.item, s_st.put);
    if (s_mode == 2 && l > 0 && l < (int)sizeof line) snprintf(line + l, sizeof line - l, "; compared %llu, differ %llu", s_st.cmp, s_st.bad);
    fprintf(stderr, "%s\n", line);       /* one write: other threads print too */
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

static J* thread_j(void)
{
    if (!t_j) t_j = (J*)calloc(1, sizeof(J));
    return t_j;
}

/* mode 2: the lifted worker after the model, then the comparison. Returns 1. */
static int j209_check(spu_context* ctx, J* j)
{
    if (!t_lsb && !(t_lsb = (uint8_t*)malloc(SPU_LS_SIZE))) return 0;
    uint8_t* const ls = SPU_LS_BASE(ctx);
    memcpy(t_lsb, ls, SPU_LS_SIZE);
    u128 g0[128];
    memcpy(g0, ctx->gpr, sizeof g0);
    const uint32_t dl0 = j->dead_lo[0], dh0 = j->dead_hi[0], dl1 = j->dead_lo[1], dh1 = j->dead_hi[1];
    void (*fn)(spu_context*) = spu_lookup(J209_WORKER, ctx->resident_ovl);
    if (!fn) return 0;
    const int32_t si = (int32_t)ctx->image_id;
    const uint32_t r0 = ctx->gpr[0]._u32[0];
    t_bypass = 1;
    ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_bypass = 0;
    int bad = 0;
    unsigned why = 0;
    /* the queued PUTs against the guest memory the lifted worker left (the later of two overlapping PUTs wins) */
    for (uint32_t i = 0; i < j->npw && !bad; i++) {
        uint8_t tmp[0x4000];
        const PW* p = &j->pw[i];
        if (p->size > sizeof tmp) continue;
        memcpy(tmp, j->arena + p->off, p->size);
        for (uint32_t v = i + 1; v < j->npw; v++) {
            const PW* q = &j->pw[v];
            const uint32_t lo = p->ea > q->ea ? p->ea : q->ea, hi = (p->ea + p->size) < (q->ea + q->size) ? (p->ea + p->size) : (q->ea + q->size);
            if (lo < hi) memcpy(tmp + (lo - p->ea), j->arena + q->off + (lo - q->ea), hi - lo);
        }
        if (memcmp(vm_base + p->ea, tmp, p->size)) {
            bad = 1; why = 1;
            uint32_t k = 0; while (vm_base[p->ea + k] == tmp[k]) k++;
            const unsigned long long nb = s_st.bad + 1;       /* the first differences, in detail */
            if (nb <= 6) {
                fprintf(stderr, "[j209] PUT #%u of %u: EA %08X size %u differs at +%u: job %02X%02X%02X%02X%02X%02X%02X%02X, native %02X%02X%02X%02X%02X%02X%02X%02X\n",
                        i, j->npw, p->ea, p->size, k, vm_base[p->ea + (k & ~7u)], vm_base[p->ea + (k & ~7u) + 1], vm_base[p->ea + (k & ~7u) + 2],
                        vm_base[p->ea + (k & ~7u) + 3], vm_base[p->ea + (k & ~7u) + 4], vm_base[p->ea + (k & ~7u) + 5],
                        vm_base[p->ea + (k & ~7u) + 6], vm_base[p->ea + (k & ~7u) + 7], tmp[k & ~7u], tmp[(k & ~7u) + 1], tmp[(k & ~7u) + 2],
                        tmp[(k & ~7u) + 3], tmp[(k & ~7u) + 4], tmp[(k & ~7u) + 5], tmp[(k & ~7u) + 6], tmp[(k & ~7u) + 7]);
            }
        }
    }
    for (int r = 0; r < 128 && !bad; r++)
        if ((r == 0 || r == 1 || r >= 80) && memcmp(&ctx->gpr[r], &g0[r], 16)) { bad = 1; why = 2 + (unsigned)r; }
    for (uint32_t a = 0; a < SPU_LS_SIZE && !bad; a += 16) {
        if ((a + 16 > dl0 && a < dh0) || (a + 16 > dl1 && a < dh1)) continue;
        if (memcmp(ls + a, t_lsb + a, 16)) { bad = 1; why = 0x1000u + a; }
    }
    ADD(s_st.cmp, 1);
    if (bad) {
        const unsigned long long nb = __atomic_add_fetch(&s_st.bad, 1, __ATOMIC_RELAXED);
        if (nb <= 12)
            fprintf(stderr, "[j209] check DIFFER #%llu (%s %x): P=%05X B=%05X C=%05X sp=%05X N=%u, %u PUTs, %llu boxes, %llu items\n",
                    nb, why == 1 ? "guest PUT" : why < 130 ? "register" : "local store", why, j->P, j->B, j->C, j->sp, j->N, j->npw,
                    (unsigned long long)j->n_box, (unsigned long long)j->n_item);
    }
    return 1;
}

static int j209_worker(spu_context* ctx)
{
    if (t_bypass) return 0;
    if (__builtin_expect(__atomic_load_n(&s_mode, __ATOMIC_ACQUIRE) < 0, 0)) {
        const char* e = getenv("PS3_NATIVE_J209");
        { const char* sl = getenv("PS3_NATIVE_J209_SLOW"); if (sl && sl[0] == '1') s_fast = 0; }
        int m = e && e[0] ? (e[0] == '2' ? 2 : e[0] != '0') : 1, unset = -1;
        if (__atomic_compare_exchange_n(&s_mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            fprintf(stderr, "[j209] native LOD / fade updater (image %d, 0x%X): %s%s\n", J209_IMAGE, J209_WORKER,
                    m == 2 ? "check run against the lifted worker" : m ? "on" : "off", s_fast ? "" : ", float helpers only");
    }
    if (!s_mode) return 0;
    J* j = thread_j();
    if (!j) return 0;
    j->ctx = ctx;
    const unsigned long long n = __atomic_add_fetch(&s_ncalls, 1, __ATOMIC_RELAXED);
    ADD(s_st.calls, 1);
    if (!(n & 63)) stats_tick(n);
    if (!model(j)) {
        ADD(s_st.declined, 1);
        for (int i = 0; i < J209_NREASON; i++) if (j->fallback >> i & 1) ADD(s_st.why[i], 1);
        if (j->fallback & FB_EA) { static int nl; if (nl++ < 6) fprintf(stderr, "[j209] EA fallback: 0x%08X size 0x%X\n", j->bad_ea, j->bad_size); }
        return 0;
    }
    ADD(s_st.ent, j->n_ent); ADD(s_st.box, j->n_box); ADD(s_st.item, j->n_item); ADD(s_st.put, j->npw);
    if (s_mode == 2) return j209_check(ctx, j);
    publish(j);
    ADD(s_st.native, 1);
    return 1;
}

__attribute__((constructor)) static void j209_register(void)
{
    spu_native_register(J209_IMAGE, J209_WORKER, j209_worker);
}
