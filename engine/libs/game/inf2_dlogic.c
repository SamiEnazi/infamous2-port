/*
 * inf2_dlogic.c -- Track D (work/native/TRACK_D.md): parts of inFAMOUS 2's own main-thread logic redone as
 * native code. Title knowledge for the v1.04 EBOOT (PPU-41a7d047...); every guest address below is from
 * that lift. Each part replaces a lifted guest function through its hook slot (cheats/hooks.txt,
 * include/ps3emu/ppu_hooks.h) and keeps the guest-visible behaviour: the same calls into the game, in the
 * same order with the same arguments, and the same bytes written to guest memory (scratch included),
 * unless a part says otherwise.
 *
 *   D_TICK   0x0032E030  the world tick: the two entity lists' parent-before-child order (a DFS with a
 *                         virtual getParent per entity) and the tick loop (vt+8 per entity, retried until
 *                         every entity says 2 = done, children after their parents). Native loops; a
 *                         getParent that is a plain field load (lwz r3,D(r3); blr) is read directly.
 *   D_POLL   (in D_TICK)  the tick loop's re-polls of an entity waiting for its anim job: the game's chain
 *                         0x323D8C -> 0x32F24 -> 0x38ABF8 is a pure "not done yet" (returns 1, writes
 *                         nothing) while the job's token test says 0; the native loop asks 0x38ABF8 itself
 *                         and skips the two wrappers when the answer is "not done".
 *   D_QSORT  0x00629138  qsort(base, n, 4, CD174): the per-view draw-item sort (CE464 / CE35C). The game's
 *                         own introsort (0x628B4C: Bentley-McIlroy 3-way partition, median of 9, insertion
 *                         sort at <= 32, heap sort when the depth budget runs out) run natively on an index
 *                         array with the comparator's keys read once per element: the same order, ties
 *                         included (same algorithm, same comparisons).
 *   D_MEMCPY 0x00626900 / 0x00630538  memcpy / memset: the engine's natives (ppu_native_memcpy/memset,
 *                         ppu3 2026-11-08: decline -> the lifted body), through hook slots. DEFAULT OFF: a win
 *                         on the old lift, but the lean lift's memcpy is already ~1.5 host instructions per
 *                         guest instruction and the hook path adds fixed checks to ~1,300 mostly small copies a
 *                         frame (round 1: the one window that got slower, fight 24-32 s -1.4%, had it on).
 *
 * Switches (read once at start; one "[dlogic]" line lists what is on):
 *   PS3_D=0                  everything off (the lifted functions run; the hooks pass through)
 *   PS3_D_TICK, PS3_D_POLL, PS3_D_QSORT = 0|1   one part (default 1); PS3_D_MEMCPY=1 (default 0)
 *   PS3_D_CHECK=1            every part that has one runs its check mode instead (also PS3_D_<NAME>_CHECK=1):
 *       TICK  the game's own 0x32E030 runs; at its array_copy_resize call (0x6AB340 from 0x32E5C0) the
 *             native parent-before-child order is computed from the same lists and compared with the
 *             game's arrays (output, positions, parent positions): "[dlogic] TICK MISMATCH ..."
 *       QSORT the game's qsort runs on the array; the native sort runs on a copy of the input; outputs
 *             compared: "[dlogic] QSORT MISMATCH ..."; also times both (us saved per frame)
 *   PS3_STATE_DIGEST=1|<file> one line per frame (default file dlogic_digest.txt in the working folder):
 *             frame, the two entity lists (items + their +4 index), every qsort result of the frame, the
 *             progress block 0x00A64E20 (karma/XP/powers), as hashes.
 *   PS3_D_STATS=<s>          the counters line every <s> seconds (default 5, 0 = never)
 */
#include "../../runtime/ppu/ppu_context.h"
#include "../../include/ps3emu/ppu_hooks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define DL_HAVE_TSC 1
#endif

extern uint8_t* vm_base;
void ps3_indirect_call(ppu_context* ctx);
void* ps3_module_ppu_fn(uint32_t guest_addr);
void ppu_lean_st8(uint32_t ea, uint8_t v);
void ppu_lean_st32(uint32_t ea, uint32_t v);
void ppu_lean_st64(uint32_t ea, uint64_t v);
int ppu_native_memcpy(ppu_context* ctx);
int ppu_native_memset(ppu_context* ctx);

#define TOC 0x009DB730u

/* ---- guest memory (big-endian). A store is the lean lift's own store (vm_write32_mm, PPU_ST_PMAP):
 * the page-map byte 1 = a plain store and its re-check, else the careful store out of line (the SPU
 * reservation protocol, the null page, the TOC watch) ---- */
extern unsigned char g_ppu_st_pmap[1u << 20];   /* (the engine: ps3_module_vars.h makes it the module's copy) */
void ppu_store_pm_recheck(uint32_t ea, uint32_t n);
typedef uint64_t __attribute__((aligned(1))) dl_u64u;
typedef uint32_t __attribute__((aligned(1))) dl_u32u;
static inline uint32_t R32(uint32_t ea) { return __builtin_bswap32(*(volatile const dl_u32u*)(vm_base + ea)); }
static inline uint32_t R8(uint32_t ea) { return *(volatile const uint8_t*)(vm_base + ea); }
static inline int pm_fast(uint32_t ea) { return __atomic_load_n(&g_ppu_st_pmap[ea >> 12], __ATOMIC_RELAXED) == 1u; }
static inline void W32(uint32_t ea, uint32_t v)
{
    if (__builtin_expect(pm_fast(ea), 1)) {
        *(volatile dl_u32u*)(vm_base + ea) = __builtin_bswap32(v);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        if (__builtin_expect(!pm_fast(ea), 0)) ppu_store_pm_recheck(ea, 4);
        return;
    }
    ppu_lean_st32(ea, v);
}
static inline void W8(uint32_t ea, uint32_t v)
{
    if (__builtin_expect(pm_fast(ea), 1)) {
        *(volatile uint8_t*)(vm_base + ea) = (uint8_t)v;
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        if (__builtin_expect(!pm_fast(ea), 0)) ppu_store_pm_recheck(ea, 1);
        return;
    }
    ppu_lean_st8(ea, (uint8_t)v);
}
static inline void W64(uint32_t ea, uint64_t v)
{
    if (__builtin_expect(pm_fast(ea), 1)) {
        *(volatile dl_u64u*)(vm_base + ea) = __builtin_bswap64(v);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        if (__builtin_expect(!pm_fast(ea), 0)) ppu_store_pm_recheck(ea, 8);
        return;
    }
    ppu_lean_st64(ea, v);
}

/* ---- clock --------------------------------------------------------------------------------------- */
static double s_tsc_ns = 0.0;   /* ns per tick */
#ifndef _WIN32
static inline uint64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
#endif
#ifdef DL_HAVE_TSC
static inline uint64_t tsc(void) { return __rdtsc(); }
#else
static inline uint64_t tsc(void) { return mono_ns(); }   /* (no TSC: nanoseconds) */
#endif
static void tsc_calibrate(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    const uint64_t t0 = tsc();
    do { QueryPerformanceCounter(&b); } while ((double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart < 0.02);
    const uint64_t t1 = tsc();
    s_tsc_ns = ((double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart) / (double)(t1 - t0);
#elif defined(DL_HAVE_TSC)
    const uint64_t a = mono_ns(), t0 = tsc();
    uint64_t b;
    do { b = mono_ns(); } while (b - a < 20000000ull);
    s_tsc_ns = (double)(b - a) / (double)(tsc() - t0);
#else
    s_tsc_ns = 1.0;
#endif
}

/* ---- configuration ------------------------------------------------------------------------------- */
static int s_tick, s_poll, s_qsort, s_memcpy;          /* parts on */
static int s_tick_chk, s_qsort_chk, s_poll_chk;         /* check modes */
static int s_stats_s = 5;
static FILE* s_digest;
static int env_flag(const char* n, int dflt)
{
    const char* e = getenv(n);
    if (!e || !*e) return dflt;
    return !(e[0] == '0' && e[1] == 0);
}

/* ---- calling the game ---------------------------------------------------------------------------- */
typedef void (*gfn)(ppu_context*);
static gfn gfn_get(uint32_t a)
{
    gfn f = (gfn)ps3_module_ppu_fn(a);
    if (!f) { fprintf(stderr, "[dlogic] FATAL: the game module has no function at 0x%08X\n", a); fflush(stderr); abort(); }
    return f;
}
#define GFN_DECL(name, a) static gfn name(void) { static gfn f; if (!f) f = gfn_get(a); return f; }
GFN_DECL(F_29C23C, 0x0029C23Cu) GFN_DECL(F_4B902C, 0x004B902Cu) GFN_DECL(F_7A1AC, 0x0007A1ACu)
GFN_DECL(F_1DFE78, 0x001DFE78u) GFN_DECL(F_4BB070, 0x004BB070u) GFN_DECL(F_D0F14, 0x000D0F14u)
GFN_DECL(F_30756C, 0x0030756Cu) GFN_DECL(F_4CF5A4, 0x004CF5A4u) GFN_DECL(F_4CF884, 0x004CF884u)
GFN_DECL(F_6AB340, 0x006AB340u) GFN_DECL(F_64C7F0, 0x0064C7F0u) GFN_DECL(F_6AB3E4, 0x006AB3E4u)
GFN_DECL(F_64E2B4, 0x0064E2B4u) GFN_DECL(F_38ABF8, 0x0038ABF8u)

/* a direct call (bl): TOC in r2, the return address the game's call site has */
static inline void gcall(ppu_context* c, gfn f, uint32_t lr)
{
    c->gpr[2] = TOC;
    c->lr = lr;
    ppu_hook_call(c, (ppu_hook_body)f);
}
/* a virtual call through a function descriptor (bctrl): r2 from the descriptor, TOC again after. An EBOOT
 * target is called directly (what the lift's inline dispatch ppu_icall_inl does on a hit), through a small
 * per-thread cache of code address -> lifted function; anything else goes the engine's way. */
#define VC_SLOTS 1024
static _Thread_local struct { uint32_t code; gfn fn; } s_vc[VC_SLOTS];
static inline void gvcall(ppu_context* c, uint32_t desc, uint32_t lr)
{
    const uint32_t code = R32(desc);
    c->ctr = code;
    c->gpr[2] = R32(desc + 4);
    c->lr = lr;
    gfn f = 0;
    if (code && code < 0x30000000u) {
        const uint32_t k = (code >> 2) & (VC_SLOTS - 1);
        if (s_vc[k].code == code) f = s_vc[k].fn;
        else if ((f = (gfn)ps3_module_ppu_fn(code)) != 0) { s_vc[k].code = code; s_vc[k].fn = f; }
    }
    if (f) f(c); else ps3_indirect_call(c);
    c->gpr[2] = TOC;
}

/* ---- statistics ---------------------------------------------------------------------------------- */
typedef struct {
    volatile uint64_t frames, items, passes, ticks, ticks_not2, poll_skip, gp_native, gp_guest;
    volatile uint64_t t_total, t_p1, t_p2, t_p2_calls, t_poll, t_first, t_idle, idle_passes;
    volatile uint64_t chk_lists, chk_bad, chk_anom;
    volatile uint64_t q_calls, q_native, q_pass, q_elems, q_t_native, q_t_orig, q_chk, q_bad;
    volatile uint64_t m_calls, poll_raced, poll_bad;
} dstats;
static dstats S;
static int s_bad_logged;
static void first_warn(const char* what, uint32_t a, uint32_t b)
{
    static uint32_t seen[64]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == a) return;
    if (n < 64) seen[n++] = a;
    fprintf(stderr, "[dlogic] FIRST OCCURRENCE: %s (0x%08X, 0x%08X)\n", what, a, b);
}

/* ---- digest -------------------------------------------------------------------------------------- */
static uint64_t s_dg_q;   /* the frame's qsort results, folded */
static inline uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0xFF51AFD7ED558CCDull; }
static uint64_t hash_mem(uint32_t ea, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, vm_base + ea + i, 8); h = mix(h, v); }
    return h;
}
static void digest_frame(uint32_t a1)
{
    if (!s_digest) return;
    uint64_t hl = 0;
    for (int v6 = 0; v6 <= 1; v6++) {
        const uint32_t L = a1 + 51216 + 20 * v6;
        const int32_t n = (int32_t)R32(L + 8);
        const uint32_t p = R32(L + 4);
        hl = mix(hl, (uint32_t)n);
        for (int32_t k = 0; k < n && k < 100000; k++) {
            const uint32_t it = R32(p + 4 * (uint32_t)k);
            hl = mix(hl, ((uint64_t)it << 32) | (it ? R32(it + 4) : 0));
        }
    }
    fprintf(s_digest, "%llu lists %016llx qsort %016llx prog %016llx\n", (unsigned long long)S.frames,
            (unsigned long long)hl, (unsigned long long)s_dg_q, (unsigned long long)hash_mem(0x00A64E20u, 0x400));
    s_dg_q = 0;
}

/* ============================================================================================
 * D_TICK: 0x0032E030
 * ============================================================================================ */
/* getParent (vt+0xC): a plain field load is read directly; anything else is the game's call */
#define GP_SLOTS 256
static struct { uint32_t vt; int32_t d; } s_gp[GP_SLOTS];   /* d: field offset, or INT32_MIN = call it */
static int32_t gp_kind(uint32_t vt)
{
    const uint32_t k = (vt >> 3) & (GP_SLOTS - 1);
    if (s_gp[k].vt == vt) return s_gp[k].d;
    const uint32_t desc = R32(vt + 0xC), code = R32(desc);
    int32_t d = INT32_MIN;
    if (code && code < 0x30000000u) {
        const uint32_t w0 = R32(code), w1 = R32(code + 4);
        if ((w0 & 0xFFFF0000u) == 0x80630000u && w1 == 0x4E800020u) d = (int16_t)(w0 & 0xFFFF);   /* lwz r3,d(r3); blr */
    }
    if (d == INT32_MIN) first_warn("getParent is not a plain field load (called)", vt, code);
    s_gp[k].vt = vt; s_gp[k].d = d;
    return d;
}
static inline uint32_t get_parent(ppu_context* c, uint32_t obj, uint32_t lr)
{
    const uint32_t vt = R32(obj);
    const int32_t d = gp_kind(vt);
    if (d != INT32_MIN) { S.gp_native++; return R32(obj + (uint32_t)d); }
    S.gp_guest++;
    c->gpr[3] = obj;
    gvcall(c, R32(vt + 0xC), lr);
    return (uint32_t)c->gpr[3];
}

/* the poll short cut: the tick (vt+8) is 0x323D8C or 0x462F38, directly or through a this-adjusting thunk
 * (the entity classes' vtables point at 0x6AA824 / 0x6DA108 = addi r3,r3,-160; b <tick>), with its anim object
 * waiting for a job. Per vtable: the tick's code, which of the two it reaches (0 = neither) and the adjustment. */
#define VT8_SLOTS 256
static struct { uint32_t vt; uint32_t code; int32_t adj; int kind; } s_vt8[VT8_SLOTS];
static inline int tick_kind(uint32_t a) { return a == 0x00323D8Cu ? 1 : a == 0x00462F38u ? 2 : 0; }
static inline int vt8_slot(uint32_t vt)
{
    const uint32_t k = (vt >> 3) & (VT8_SLOTS - 1);
    if (s_vt8[k].vt == vt) return (int)k;
    const uint32_t code = R32(R32(vt + 8));
    int kind = tick_kind(code);
    int32_t adj = 0;
    if (!kind && code && code < 0x30000000u) {
        const uint32_t w0 = R32(code), w1 = R32(code + 4);
        if ((w0 & 0xFFFF0000u) == 0x38630000u && (w1 & 0xFC000003u) == 0x48000000u) {   /* addi r3,r3,d; b t */
            int32_t li = (int32_t)(w1 & 0x03FFFFFCu);
            if (li & 0x02000000) li -= 0x04000000;
            kind = tick_kind(code + 4 + (uint32_t)li);
            if (kind) adj = (int16_t)(w0 & 0xFFFF);
        }
    }
    s_vt8[k].vt = vt; s_vt8[k].code = code; s_vt8[k].adj = adj; s_vt8[k].kind = kind;
    return (int)k;
}
static inline uint32_t vt8_code(uint32_t vt) { return s_vt8[vt8_slot(vt)].code; }
/* which ticks the loop calls, and how often they say "not done" (one line per minute): counters per vtable
 * slot of the cache above (plain increments; the line groups them by code) */
static uint64_t s_vt8_calls[VT8_SLOTS], s_vt8_not2[VT8_SLOTS], s_vt8_skip[VT8_SLOTS];
static inline void th_note(int k, int not2, int skipped)
{
    s_vt8_calls[k]++; s_vt8_not2[k] += (uint64_t)not2; s_vt8_skip[k] += (uint64_t)skipped;
}
static int s_timing;   /* PS3_D_TIMING=1: the us/frame counters (rdtsc around every tick call: not free at ~9,000 calls/frame) */
/* 1 = the game's tick would return 1 and write nothing (its job is still running): skip it.
 * The two entity ticks that hand an anim object to 0x32F24 (anim_update_or_skip), as they test it:
 *   0x323D8C: e+472 == 0 (else its vt+0xB4 path), then e+400;
 *   0x462F38: e+708 > 0 (the other way in, a byte at e+782, is left to the game), then e+400. */
void (*volatile inf2_dlogic_help)(uint32_t mgr, uint32_t token);   /* the runner's seam (null = none) */
volatile uint64_t inf2_dlogic_help_jobs, inf2_dlogic_help_ns, inf2_dlogic_help_left;   /* its counters (the runner's) */
static int poll_not_done(ppu_context* c, uint32_t item, uint32_t vt)
{
    const int k = vt8_slot(vt);
    const int kind = s_vt8[k].kind;
    if (!kind) return 0;
    const uint32_t e = item + (uint32_t)s_vt8[k].adj;      /* the tick's r3 after the thunk */
    if (kind == 1) { if (R32(e + 472) != 0) return 0; }
    else { if ((int32_t)R32(e + 708) <= 0) return 0; }
    const uint32_t a = R32(e + 400);                       /* the anim object (32F24's a1) */
    if (!a || !R8(a + 1644)) return 0;                     /* no job pending: the real update runs */
    const int8_t slot = (int8_t)R8(a + 1552);
    if (slot == -1) return 0;                              /* 32F24 clears the flag and says 2 */
    const uint32_t mgr = (uint32_t)(384 * (int32_t)slot + R32(0x0099F698u)), tok = a + 1552;
    c->gpr[3] = mgr;
    c->gpr[4] = tok;
    const uint64_t t0 = s_timing ? tsc() : 0;
    gcall(c, F_38ABF8(), 0x00032FFCu);                    /* the token test, as 32F24 calls it */
    if (s_timing) S.t_poll += tsc() - t0;
    if ((uint8_t)c->gpr[3] != 0) return 0;
    /* not done: the main thread would only spin here. A native job runner can register inf2_dlogic_help to
     * run that job (or another queued one of its manager) on this thread instead (Track B1 integration; none
     * registered = the poll as before). It must not run guest PPU code. */
    if (inf2_dlogic_help) inf2_dlogic_help(mgr, tok);
    return 1;
}

/* phase 1 + 2 of one list (the code from 0x32E1D0 to 0x32E82C), exactly as the game does it in its
 * frame at sp (the same scratch arrays, the same header words, the same calls) */
static void tick_list(ppu_context* c, uint32_t L, int32_t n, uint32_t sp)
{
    const uint32_t alloc0 = R32(0x009AB1F4u);
    c->gpr[3] = alloc0; gcall(c, F_4CF5A4(), 0x0032E1E8u);           /* push */
    const uint32_t al = R32(0x009AB1F4u);
    const uint32_t n4 = (uint32_t)n << 2;
    /* v61: the parent's new position per output slot ("order") */
    W32(sp + 0x10C, 0); W32(sp + 0x7C, 0); W32(sp + 0x80, 0); W32(sp + 0x108, 1); W32(sp + 0x84, 5);
    W32(sp + 0x88, (uint32_t)n); W32(sp + 0x110, al); W32(sp + 0x118, 16); W32(sp + 0x114, n4);
    W64(sp + 0x28, TOC);
    c->gpr[3] = al; c->gpr[4] = sp + 0x114; gvcall(c, R32(R32(al)), 0x0032E248u);
    W32(sp + 0x7C, (uint32_t)c->gpr[3]);
    c->gpr[3] = alloc0; gcall(c, F_4CF5A4(), 0x0032E258u);           /* push */
    /* v73: each entity's new position ("pos"), -1 = not placed */
    W32(sp + 0xCC, 1); W32(sp + 0xD0, 0); W32(sp + 0xAC, 0); W32(sp + 0xB0, 0); W32(sp + 0xB4, 5);
    W32(sp + 0xB8, (uint32_t)n); W32(sp + 0xD4, al); W32(sp + 0xDC, 16); W32(sp + 0xD8, n4);
    c->gpr[3] = al; c->gpr[4] = sp + 0xD8; gvcall(c, R32(R32(al)), 0x0032E2A8u);
    W32(sp + 0xAC, (uint32_t)c->gpr[3]);
    W32(sp + 0x78, 0xFFFFFFFFu);
    if (n > 0) {   /* 0x68A670(&v73, n, &-1) */
        const uint32_t p = R32(sp + 0xAC), cnt = R32(sp + 0xB0), v = R32(sp + 0x78);
        for (int32_t k = 0; k < n; k++) W32(p + 4 * (cnt + (uint32_t)k), v);
        W32(sp + 0xB0, cnt + (uint32_t)n);
    }
    /* v69: the output list */
    W32(sp + 0xF4, 1); W32(sp + 0xF8, 0); W32(sp + 0x9C, 0); W32(sp + 0xA0, 0); W32(sp + 0x100, n4);
    W32(sp + 0xA4, 5); W32(sp + 0xA8, (uint32_t)n); W32(sp + 0xFC, al); W32(sp + 0x104, 16);
    c->gpr[3] = al; c->gpr[4] = sp + 0x100; gvcall(c, R32(R32(al)), 0x0032E318u);
    W32(sp + 0x9C, (uint32_t)c->gpr[3]);
    /* v65: the DFS stack (32 entries; the game never grows it) */
    W32(sp + 0x98, 0x20); W32(sp + 0xE4, 0); W32(sp + 0xE0, 1); W32(sp + 0x8C, 0); W32(sp + 0x94, 5);
    W32(sp + 0xF0, 16); W32(sp + 0xEC, 0x80); W32(sp + 0x90, 0); W32(sp + 0xE8, al);
    c->gpr[3] = al; c->gpr[4] = sp + 0xEC; gvcall(c, R32(R32(al)), 0x0032E378u);
    W32(sp + 0x8C, (uint32_t)c->gpr[3]);

    const uint64_t t1 = tsc();
    if (n > 0) {
        const uint32_t P = R32(sp + 0xAC), O = R32(sp + 0x7C), U = R32(sp + 0x9C), Sk = R32(sp + 0x8C);
        /* the frame words the loop keeps in memory: 0x74 i, 0x80 order count, 0x90 stack count, 0xA0 out count */
        uint32_t i_ = R32(sp + 0x74), oc = R32(sp + 0x80), sc = R32(sp + 0x90), uc = R32(sp + 0xA0);
        int32_t r23 = 0;
        for (;;) {
            const uint32_t items = R32(L + 4);
            const uint32_t item = R32(items + 4 * (uint32_t)r23);
            if (item == 0 || (int32_t)R32(P + 4 * (uint32_t)r23) >= 0) {
                r23++;
                if (n == r23) break;
                continue;
            }
            int32_t v21 = -1;
            i_ = (uint32_t)r23;
            uint32_t cur = item;
            for (;;) {
                const uint32_t parent = get_parent(c, cur, 0x0032E464u);
                if (parent == 0) break;
                const uint32_t pidx = R32(parent + 4);
                v21 = (int32_t)R32(P + 4 * pidx);
                if (v21 >= 0) break;
                /* 0x6460F8: i already on the stack? (a cycle) */
                int found = -1;
                for (uint32_t k = 0; k < sc; k++) if (R32(Sk + 4 * k) == i_) { found = (int)k; break; }
                if (found >= 0) {
                    const uint32_t top = R32(Sk + 4 * sc - 4);
                    sc = sc - 1;
                    i_ = top;
                    v21 = (int32_t)R32(P + 4 * R32(Sk + 4 * sc - 4));
                    S.chk_anom++;
                    first_warn("TICK: a parent cycle in an entity list", L, i_);
                    break;
                }
                if (sc >= 32) first_warn("TICK: parent chain deeper than the game's 32-entry stack", L, sc);
                W32(Sk + 4 * sc, i_);
                sc++;
                i_ = pidx;
                cur = parent;
            }
            if (sc != 0) {
                const uint32_t its = R32(L + 4);
                do {
                    W32(P + 4 * i_, oc);
                    W32(O + 4 * oc, (uint32_t)v21);
                    v21 = (int32_t)oc;
                    oc++;
                    const uint32_t top = R32(Sk + 4 * sc - 4);
                    const uint32_t val = R32(its + 4 * i_);
                    sc--;
                    W32(U + 4 * uc, val);
                    uc++;
                    i_ = top;
                } while (sc != 0);
            }
            {
                const uint32_t its = R32(L + 4);
                r23++;
                W32(P + 4 * i_, oc);
                W32(O + 4 * oc, (uint32_t)v21);
                oc++;
                W32(U + 4 * uc, R32(its + 4 * i_));
                uc++;
            }
            if (n == r23) break;
        }
        W32(sp + 0x74, i_); W32(sp + 0x80, oc); W32(sp + 0x90, sc); W32(sp + 0xA0, uc);
    }
    c->gpr[3] = L + 4; c->gpr[4] = sp + 0x9C; gcall(c, F_6AB340(), 0x0032E5C0u);   /* the list = the output */
    {   /* each entity's +4 = its index (only stores that change something) */
        const uint32_t p = R32(L + 4);
        for (uint32_t k = 0; (int32_t)R32(L + 8) > (int32_t)k; k++) {
            const uint32_t it = R32(p + 4 * k);
            if (R32(it + 4) != k) W32(it + 4, k);
        }
    }
    S.t_p1 += tsc() - t1;
    c->gpr[3] = sp + 0x8C; gcall(c, F_64C7F0(), 0x0032E604u);
    c->gpr[3] = sp + 0x9C; gcall(c, F_6AB3E4(), 0x0032E610u);
    c->gpr[3] = sp + 0xAC; gcall(c, F_64C7F0(), 0x0032E61Cu);
    c->gpr[3] = alloc0; gcall(c, F_4CF884(), 0x0032E628u);           /* pop */

    /* phase 2: the ticks */
    const int32_t n2 = (int32_t)R32(L + 8);
    W32(sp + 0x11C, 1); W32(sp + 0xC4, 5); W32(sp + 0x120, 0); W32(sp + 0x124, al); W32(sp + 0x12C, 16);
    W32(sp + 0xBC, 0); W32(sp + 0xC0, 0); W32(sp + 0xC8, (uint32_t)n2); W32(sp + 0x128, (uint32_t)n2);
    c->gpr[3] = al; c->gpr[4] = sp + 0x128; gvcall(c, R32(R32(al)), 0x0032E688u);
    W32(sp + 0xBC, (uint32_t)c->gpr[3]);
    W8(sp + 0x70, 0);
    if (n2 > 0) {   /* 0x64E2EC(&v74, n2, &0) */
        const uint32_t cnt = R32(sp + 0xC0), p = R32(sp + 0xBC), v = R8(sp + 0x70);
        for (int32_t k = 0; k < n2; k++) W8(p + cnt + (uint32_t)k, v);
        W32(sp + 0xC0, cnt + (uint32_t)n2);
    }
    const uint64_t t2 = tsc();
    uint64_t tcalls = 0;
    if (n2 > 0) {
        const uint32_t F = R32(sp + 0xBC), O = R32(sp + 0x7C);
        int32_t lo = 0x7FFFFFFF, hi = -1, k = 0, end = n2;
        uint32_t done_pass = 0, npass = 0;
        uint64_t tpass = t2;
        S.passes++;
        for (;;) {
            if (R8(F + (uint32_t)k) != 0) { k++; goto chk; }
            {
                const int32_t p = (int32_t)R32(O + 4 * (uint32_t)k);
                const uint32_t item = R32(R32(L + 4) + 4 * (uint32_t)k);
                if (p >= 0 && R8(F + (uint32_t)p) == 0) goto postpone;
                const uint32_t vt = R32(item);
                int32_t res;
                int skipped = 0;
                if (s_poll && poll_not_done(c, item, vt)) {
                    S.poll_skip++;
                    res = 1;
                    skipped = 1;
                    if (s_poll_chk) {   /* the game's own tick anyway: 1 (or 2: the job finished meanwhile) */
                        c->gpr[3] = item;
                        gvcall(c, R32(vt + 8), 0x0032E764u);
                        res = (int32_t)(uint32_t)c->gpr[3];
                        if (res == 2) S.poll_raced++;
                        else if (res != 1) {
                            S.poll_bad++;
                            if (s_bad_logged++ < 20)
                                fprintf(stderr, "[dlogic] POLL MISMATCH entity 0x%08X: skipped as not done, the game's tick says %d\n", item, res);
                        }
                    }
                } else {
                    const uint64_t tc = s_timing ? tsc() : 0;
                    c->gpr[3] = item;
                    gvcall(c, R32(vt + 8), 0x0032E764u);
                    if (s_timing) tcalls += tsc() - tc;
                    res = (int32_t)(uint32_t)c->gpr[3];
                    S.ticks++;
                }
                if (s_stats_s > 0) th_note(vt8_slot(vt), res != 2, skipped);
                if (res == 2) { W8(F + (uint32_t)k, 1); k++; done_pass++; goto chk; }
                S.ticks_not2++;
                if (lo > k) lo = k;
            }
        postpone:
            { const int32_t k1 = k + 1; if (!(hi > k1)) hi = k1; k = k1; }
        chk:
            if (end > k) continue;
            {   /* a pass ends: one in which nothing finished is waiting (for the anim jobs) */
                if (npass != 0 && done_pass == 0) S.idle_passes++;
                if (s_timing) {
                    const uint64_t tn = tsc();
                    if (npass == 0) S.t_first += tn - tpass;
                    else if (done_pass == 0) S.t_idle += tn - tpass;
                    tpass = tn;
                }
                done_pass = 0; npass++;
            }
            if (!(lo < hi)) break;
            k = lo; end = hi; lo = 0x7FFFFFFF; hi = -1;
            S.passes++;
        }
    }
    S.t_p2 += tsc() - t2;
    S.t_p2_calls += tcalls;
    S.items += (uint32_t)(n2 > 0 ? n2 : 0);
    c->gpr[3] = sp + 0xBC; gcall(c, F_64E2B4(), 0x0032E814u);
    c->gpr[3] = sp + 0x7C; gcall(c, F_64C7F0(), 0x0032E820u);
    c->gpr[3] = alloc0; gcall(c, F_4CF884(), 0x0032E82Cu);           /* pop */
}

static void tick_native(ppu_context* c)
{
    const uint64_t t0 = tsc();
    const uint32_t a1 = (uint32_t)c->gpr[3];
    const int32_t a2 = (int32_t)c->gpr[4];
    const uint64_t r1 = c->gpr[1], lr = c->lr;
    const uint32_t sp = (uint32_t)r1 - 0x1C0u;
    /* the game's frame: back chain, callee-saved registers, CR and LR in the caller's slots */
    W64(sp, r1);
    for (int r = 14; r <= 31; r++) W64(sp + 0x130 + 8 * (uint32_t)(r - 14), c->gpr[r]);
    W32((uint32_t)r1 + 8, c->cr);
    W64((uint32_t)r1 + 0x10, lr);
    c->gpr[1] = sp;
    int v6 = 1;
    if (a2 != 0) {
        v6 = 0;
        const uint32_t w = a1 + 51264;
        c->gpr[3] = w; gcall(c, F_29C23C(), 0x0032E864u);
        c->gpr[3] = w; gcall(c, F_4B902C(), 0x0032E870u);
        c->gpr[3] = a1; gcall(c, F_7A1AC(), 0x0032E87Cu);
        c->gpr[3] = R32(0x009AB1FCu); gcall(c, F_1DFE78(), 0x0032E888u);
    }
    for (; v6 <= 1; v6++) {
        const uint32_t L = a1 + 51216 + 20 * (uint32_t)v6;
        const int32_t n = (int32_t)R32(L + 8);
        if (n != 0) tick_list(c, L, n, sp);
    }
    c->gpr[3] = a1; gcall(c, F_7A1AC(), 0x0032E0E4u);
    if (a2 != 0) {
        const uint32_t w = a1 + 51264;
        c->gpr[3] = w; c->gpr[4] = 1; gcall(c, F_4BB070(), 0x0032E10Cu);
        c->gpr[3] = w; gcall(c, F_29C23C(), 0x0032E118u);
        c->gpr[3] = w; gcall(c, F_4B902C(), 0x0032E124u);
        c->gpr[3] = a1; gcall(c, F_7A1AC(), 0x0032E130u);
        c->gpr[3] = w; c->gpr[4] = 6; gcall(c, F_4BB070(), 0x0032E140u);
        c->gpr[3] = a1 + 444992; gcall(c, F_D0F14(), 0x0032E154u);
        c->gpr[3] = a1 + 445080; gcall(c, F_30756C(), 0x0032E160u);
        c->gpr[3] = a1; gcall(c, F_7A1AC(), 0x0032E16Cu);
    }
    c->gpr[1] = r1;
    c->gpr[2] = TOC;
    c->lr = lr;
    S.t_total += tsc() - t0;
}

static void hook_tick(ppu_context* c, ppu_hook_body body)
{
    const uint32_t a1 = (uint32_t)c->gpr[3];
    S.frames++;
    if (s_tick && !s_tick_chk) tick_native(c);
    else {
        const uint64_t t0 = tsc();
        ppu_hook_call(c, body);
        S.t_total += tsc() - t0;
    }
    digest_frame(a1);
}

/* ---- TICK check: the game's phase-1 arrays at its array_copy_resize(L+4, &out) call vs a native
 * (independent, host-array) parent-before-child order of the same list ---- */
static void hook_copy_resize(ppu_context* c, ppu_hook_body body)
{
    if (!s_tick_chk || (uint32_t)c->lr != 0x0032E5C0u) { ppu_hook_call(c, body); return; }
    const uint32_t L = (uint32_t)c->gpr[3] - 4, uh = (uint32_t)c->gpr[4], sp = (uint32_t)c->gpr[1];
    static ppu_context saved;          /* the getParent calls below use the registers: the call's own back after */
    memcpy(&saved, c, sizeof saved);
    const int32_t n = (int32_t)R32(L + 8);
    const uint32_t items = R32(L + 4);
    S.chk_lists++;
    if (n > 0 && n < 65536) {
        int32_t* pos = (int32_t*)malloc(sizeof(int32_t) * (size_t)n * 3 + 8);
        int32_t* ord = pos + n; uint32_t* out = (uint32_t*)(ord + n);
        int32_t* stk = (int32_t*)malloc(sizeof(int32_t) * (size_t)n + 8);
        for (int32_t k = 0; k < n; k++) pos[k] = -1;
        int32_t oc = 0, anomaly = 0;
        for (int32_t i = 0; i < n && !anomaly; i++) {
            const uint32_t it = R32(items + 4 * (uint32_t)i);
            if (!it || pos[i] >= 0) continue;
            /* walk up to a placed ancestor or a root */
            int32_t sn = 0, cur = i, par = -1;
            uint32_t o = it;
            for (;;) {
                const uint32_t p = get_parent(c, o, 0x0032E464u);
                if (!p) break;
                const uint32_t pi = R32(p + 4);
                if (pi >= (uint32_t)n) { anomaly = 1; break; }
                if (pos[pi] >= 0) { par = pos[pi]; break; }
                for (int32_t q = 0; q < sn; q++) if (stk[q] == cur) anomaly = 1;
                if (anomaly || sn >= n) { anomaly = 1; break; }
                stk[sn++] = cur;
                cur = (int32_t)pi; o = p;
            }
            if (anomaly) break;
            /* emit from the top ancestor down */
            pos[cur] = oc; ord[oc] = par; out[oc] = R32(items + 4 * (uint32_t)cur); par = oc; oc++;
            while (sn > 0) { const int32_t x = stk[--sn]; pos[x] = oc; ord[oc] = par; out[oc] = R32(items + 4 * (uint32_t)x); par = oc; oc++; }
        }
        if (anomaly) {
            S.chk_anom++;
            first_warn("TICK check: list with an out-of-range parent index or a cycle (not compared)", L, (uint32_t)n);
        } else {
            const uint32_t gu = R32(uh), guc = R32(uh + 4), go = R32(sp + 0x7C), gp = R32(sp + 0xAC);
            int bad = (int32_t)guc != oc;
            int at = -1;
            for (int32_t k = 0; k < oc && !bad; k++)
                if (R32(gu + 4 * (uint32_t)k) != out[k] || (int32_t)R32(go + 4 * (uint32_t)k) != ord[k]) { bad = 1; at = k; }
            for (int32_t k = 0; k < n && !bad; k++)
                if ((int32_t)R32(gp + 4 * (uint32_t)k) != pos[k]) { bad = 1; at = 100000 + k; }
            if (bad) {
                S.chk_bad++;
                if (s_bad_logged++ < 20)
                    fprintf(stderr, "[dlogic] TICK MISMATCH list 0x%08X n %d: game out %u native %d, first difference at %d\n",
                            L, n, guc, oc, at);
            }
        }
        free(stk); free(pos);
    }
    memcpy(c, &saved, sizeof saved);
    ppu_hook_call(c, body);
}

/* ============================================================================================
 * D_QSORT: 0x00629138 qsort(base, n, size 4, the CD174 descriptor)
 * ============================================================================================ */
typedef struct { uint32_t item, k0, k1, k2, k4; int32_t k3; } qkey;   /* k3 < 0: not read yet */
static qkey* s_qk; static int32_t* s_qa; static int32_t* s_qtmp; static uint32_t s_qcap;
static inline uint32_t qk3(qkey* k)
{
    if (k->k3 < 0) {
        const uint32_t obj = R32(k->item);
        k->k3 = (int32_t)(R32(R32(R32(obj + 8) + 0x24) + 8) & 0x80u);
    }
    return (uint32_t)k->k3;
}
/* the comparator 0x000CD174 on elements x, y (indices into the key table): the sign of its result */
static inline int32_t qcmp(int32_t x, int32_t y)
{
    qkey* a = &s_qk[x]; qkey* b = &s_qk[y];
    int32_t d = (int32_t)(a->k0 - b->k0); if (d) return d;
    d = (int32_t)(a->k1 - b->k1); if (d) return d;
    d = (int32_t)(a->k2 - b->k2); if (d) return d;
    d = (int32_t)(qk3(a) - qk3(b)); if (d) return d;
    return (int32_t)(a->k4 - b->k4);
}
/* the game's helpers on the element array A (positions) */
#define QSW(i, j) do { const int32_t _t = A[i]; A[i] = A[j]; A[j] = _t; } while (0)                 /* 0x6285E4 */
#define QSW3(a, b, c) do { const int32_t _t = A[a]; A[a] = A[c]; A[c] = A[b]; A[b] = _t; } while (0)  /* 0x628458: a<-c, c<-b, b<-a */
static inline void qrot(int32_t* A, int32_t first, int32_t last)                                       /* 0x628A50 */
{
    const int32_t t = A[last];
    memmove(A + first + 1, A + first, sizeof(int32_t) * (size_t)(last - first));
    A[first] = t;
}
static inline void qmed3(int32_t* A, int32_t a, int32_t b, int32_t c)                                  /* 0x6288F8 */
{
    if (qcmp(A[b], A[a]) < 0) QSW(a, b);
    if (qcmp(A[c], A[b]) < 0) QSW(b, c);
    if (qcmp(A[b], A[a]) < 0) QSW(a, b);
}
static void qadjust(int32_t* A, int32_t first, uint32_t hole, uint32_t bottom)                       /* 0x628728 */
{
    const uint32_t top = hole;
    uint32_t idx = 2 * hole + 2;
    if (idx > bottom) return;
    for (;;) {
        if (bottom != idx && qcmp(A[first + idx], A[first + idx - 1]) >= 0) {
            QSW(first + hole, first + idx);
            hole = idx;
        } else {
            QSW(first + hole, first + idx - 1);
            idx = idx - 1;
            hole = idx;
        }
        const uint32_t nx = 2 * idx + 2;
        if (bottom < nx) break;
        idx = nx;
    }
    while (top < hole) {
        const uint32_t par = (hole - 1) >> 1;
        if (qcmp(A[first + hole], A[first + par]) > 0) { QSW(first + par, first + hole); hole = par; }
        else break;
    }
}
static void qsort_rec(int32_t* A, int32_t first, uint32_t n, uint32_t ideal)                         /* 0x628B4C */
{
    if (ideal != 0 && n > 32) {
        for (;;) {
            const int32_t last = first + (int32_t)n;
            const int32_t mid = first + (int32_t)(n >> 1);
            const int32_t l1 = last - 1;
            if ((uint32_t)(l1 - first) > 40u) {
                const int32_t step = (int32_t)(((uint32_t)(l1 - first) >> 3) + 1);
                qmed3(A, first, first + step, first + 2 * step);
                qmed3(A, mid - step, mid, mid + step);
                qmed3(A, l1 - 2 * step, l1 - step, l1);
                qmed3(A, first + step, mid, l1 - step);
            } else {
                qmed3(A, first, mid, l1);
            }
            int32_t pf = mid, pl = mid + 1;
            while (first < pf && qcmp(A[pf - 1], A[pf]) == 0) pf--;
            while (pl < last && qcmp(A[pl], A[pf]) == 0) pl++;
            int32_t gf = pl, gl = pf;
            for (;;) {
                for (; gf < last; gf++) {
                    const int32_t r = qcmp(A[pf], A[gf]);
                    if (r < 0) continue;
                    if (r > 0) break;
                    QSW(pl, gf); pl++;
                }
                for (; first < gl; gl--) {
                    const int32_t r = qcmp(A[gl - 1], A[pf]);
                    if (r < 0) continue;
                    if (r > 0) break;
                    pf--; QSW(pf, gl - 1);
                }
                if (gl == first && gf == last) break;
                if (gl == first) {
                    if (pl == gf) QSW(pl, pf); else QSW3(gf, pf, pl);
                    gf++; pf++; pl++;
                } else if (gf == last) {
                    gl--; pf--; pl--;
                    if (gl == pf) QSW(pf, pl); else QSW3(gl, pl, pf);
                } else {
                    gl--; QSW(gf, gl); gf++;
                }
            }
            const uint32_t nl = (uint32_t)(pf - first), nr = (uint32_t)(last - pl);
            ideal = (ideal >> 1) + (ideal >> 2);
            if (nl > nr) { qsort_rec(A, pl, nr, ideal); n = nl; }
            else { qsort_rec(A, first, nl, ideal); first = pl; n = nr; }
            if (ideal == 0) break;
            if (n <= 32) goto isort;
        }
        /* ideal ran out: */
        if (n > 32) goto hsort;
        goto isort;
    }
    if (ideal == 0 && n > 32) goto hsort;
isort:
    if (n > 1) {
        int32_t nx = first;
        for (uint32_t m = n - 1; m != 0; m--) {
            nx++;
            if (qcmp(A[nx], A[first]) < 0) { qrot(A, first, nx); continue; }
            int32_t hole = nx, p = nx;
            for (;;) { p--; if (qcmp(A[nx], A[p]) < 0) hole = p; else break; }
            if (hole != nx) qrot(A, hole, nx);
        }
    }
    return;
hsort:
    for (uint32_t i = n >> 1; i != 0; ) { i--; qadjust(A, first, i, n); }
    for (uint32_t m = n; m != 1; ) {
        m--;
        QSW(first, first + (int32_t)m);
        qadjust(A, first, 0, m);
    }
}

static int q_prepare(uint32_t base, uint32_t n)
{
    if (n + 1 > s_qcap) {
        const uint32_t cap = (n + 1) * 2;
        qkey* k = (qkey*)realloc(s_qk, sizeof(qkey) * cap);
        int32_t* a = (int32_t*)realloc(s_qa, sizeof(int32_t) * cap);
        int32_t* t = (int32_t*)realloc(s_qtmp, sizeof(int32_t) * cap);
        if (!k || !a || !t) return 0;
        s_qk = k; s_qa = a; s_qtmp = t; s_qcap = cap;
    }
    for (uint32_t i = 0; i < n; i++) {
        qkey* k = &s_qk[i];
        const uint32_t it = R32(base + 4 * i), obj = R32(it);
        k->item = it;
        k->k0 = R32(obj + 0x5C); k->k1 = R32(it + 4); k->k2 = R32(obj + 0x60); k->k4 = R32(obj + 0x64); k->k3 = -1;
        s_qa[i] = (int32_t)i;
    }
    return 1;
}
static void q_write(uint32_t base, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t v = s_qk[s_qa[i]].item;
        if (R32(base + 4 * i) != v) W32(base + 4 * i, v);
    }
}
static void q_digest(uint32_t base, uint32_t n)
{
    if (!s_digest) return;
    uint64_t h = mix(s_dg_q, ((uint64_t)base << 32) | n);
    for (uint32_t i = 0; i < n; i++) h = mix(h, R32(base + 4 * i));
    s_dg_q = h;
}

static void hook_qsort(ppu_context* c, ppu_hook_body body)
{
    const uint32_t base = (uint32_t)c->gpr[3], n = (uint32_t)c->gpr[4], size = (uint32_t)c->gpr[5], cd = (uint32_t)c->gpr[6];
    S.q_calls++;
    /* the key buffers are one thread's: the render gather's (the first thread that sorts with CD174) */
    static uintptr_t s_q_tid;              /* the thread: the address of a thread-local (unique per thread) */
    static _Thread_local char t_me;
    int other = 0;
    if (size == 4 && R32(cd) == 0x000CD174u) {
        const uintptr_t me = (uintptr_t)&t_me;
        uintptr_t z = 0;
        __atomic_compare_exchange_n(&s_q_tid, &z, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s_q_tid, __ATOMIC_ACQUIRE) != me) {
            other = 1; first_warn("QSORT: a CD174 sort on a second thread (the game's sort runs there)", (uint32_t)me, 0);
        }
    }
    if (!(s_qsort || s_qsort_chk) || other || size != 4 || R32(cd) != 0x000CD174u || n >= 0x01000000u ||
        (uint64_t)c->gpr[4] != n || base < 0x10000u) {
        S.q_pass++;
        ppu_hook_call(c, body);
        return;
    }
    if (s_qsort_chk) {
        /* the native side's key reads run first, cold, and count for it; the game's sort then finds those
         * lines in the cache: "saves" is on the low side */
        const uint64_t tp = tsc();
        if (!q_prepare(base, n)) { ppu_hook_call(c, body); return; }
        const uint64_t t0 = tsc();
        ppu_hook_call(c, body);
        const uint64_t t1 = tsc();
        qsort_rec(s_qa, 0, n, n);
        const uint64_t t2 = tsc();
        S.q_t_orig += t1 - t0; S.q_t_native += (t2 - t1) + (t0 - tp); S.q_chk++; S.q_elems += n;
        for (uint32_t i = 0; i < n; i++)
            if (R32(base + 4 * i) != s_qk[s_qa[i]].item) {
                S.q_bad++;
                if (s_bad_logged++ < 20)
                    fprintf(stderr, "[dlogic] QSORT MISMATCH base 0x%08X n %u: first difference at %u (game 0x%08X native 0x%08X)\n",
                            base, n, i, R32(base + 4 * i), s_qk[s_qa[i]].item);
                break;
            }
        q_digest(base, n);
        return;
    }
    const uint64_t t0 = tsc();
    if (!q_prepare(base, n)) { S.q_pass++; ppu_hook_call(c, body); return; }
    qsort_rec(s_qa, 0, n, n);
    q_write(base, n);
    S.q_t_native += tsc() - t0;
    S.q_native++; S.q_elems += n;
    q_digest(base, n);
}

/* ============================================================================================
 * D_MEMCPY: 0x00626900 memcpy, 0x00630538 memset (the engine's natives; a decline runs the body)
 * ============================================================================================ */
static void hook_memcpy(ppu_context* c, ppu_hook_body body)
{
    if (s_memcpy && ppu_native_memcpy(c)) { S.m_calls++; return; }
    ppu_hook_call(c, body);
}
static void hook_memset(ppu_context* c, ppu_hook_body body)
{
    if (s_memcpy && ppu_native_memset(c)) { S.m_calls++; return; }
    ppu_hook_call(c, body);
}

/* ============================================================================================
 * counters, install
 * ============================================================================================ */
static void stats_sleep(int s)
{
#ifdef _WIN32
    Sleep((DWORD)s * 1000);
#else
    sleep((unsigned)s);
#endif
}
static void stats_loop(void)
{
    dstats last; memset(&last, 0, sizeof last);
    for (;;) {
        stats_sleep(s_stats_s);
        dstats now; memcpy(&now, (const void*)&S, sizeof now);
        const double f = (double)(now.frames - last.frames);
        if (f <= 0) { last = now; continue; }
#define PF(x) ((double)(now.x - last.x) / f)
#define US(x) ((double)(now.x - last.x) * s_tsc_ns / 1e3 / f)
        fprintf(stderr, "[dlogic] %.0f frames/s | TICK %s: %.0f items, %.1f passes, %.0f ticks (%.0f not done), %.0f polls skipped, "
                        "getParent %.0f read / %.1f called | us/frame: all %.1f, order %.1f, tick loop %.1f (of it in the game's ticks %.1f, "
                        "token tests %.1f; first pass %.1f, %.1f passes where nothing finished %.1f)",
                f / s_stats_s, s_tick_chk ? "check" : s_tick ? "native" : "game", PF(items), PF(passes), PF(ticks), PF(ticks_not2),
                PF(poll_skip), PF(gp_native), PF(gp_guest), US(t_total), US(t_p1), US(t_p2), US(t_p2_calls), US(t_poll),
                US(t_first), PF(idle_passes), US(t_idle));
        if (s_tick_chk) fprintf(stderr, " | check %.1f lists/frame, %llu bad, %llu anomalies", PF(chk_lists),
                                (unsigned long long)now.chk_bad, (unsigned long long)now.chk_anom);
        if (s_poll_chk) fprintf(stderr, " | poll check: %llu raced (finished meanwhile), %llu bad",
                                (unsigned long long)now.poll_raced, (unsigned long long)now.poll_bad);
        fprintf(stderr, " | QSORT %s: %.1f calls (%.1f native, %.1f game), %.0f elements, native %.1f us",
                s_qsort_chk ? "check" : s_qsort ? "native" : "game", PF(q_calls), PF(q_native), PF(q_pass), PF(q_elems), US(q_t_native));
        if (s_qsort_chk) fprintf(stderr, " vs game %.1f us (saves %.1f us/frame), %llu checked, %llu bad", US(q_t_orig),
                                 US(q_t_orig) - US(q_t_native), (unsigned long long)now.q_chk, (unsigned long long)now.q_bad);
        {   /* the job runner's help in the tick loop (wws_runner.c dlogic_help, PS3_D_HELP) */
            static uint64_t lj, lns, ll;
            const uint64_t j = inf2_dlogic_help_jobs, ns = inf2_dlogic_help_ns, l = inf2_dlogic_help_left;
            if (inf2_dlogic_help)
                fprintf(stderr, " | HELP %.1f jobs/frame, %.1f us/frame in them, %.0f polls left with nothing to run",
                        (double)(j - lj) / f, (double)(ns - lns) / 1e3 / f, (double)(l - ll) / f);
            lj = j; lns = ns; ll = l;
        }
        fprintf(stderr, " | MEMCPY %.0f native calls/frame\n", PF(m_calls));
        if (s_digest) fflush(s_digest);
        static int nlines;
        if ((++nlines % 12) == 4) {   /* the ticks by function, since start: who is polled */
            struct { uint32_t code; uint64_t calls, not2, skip; } th[VT8_SLOTS];
            int m = 0;
            for (int k = 0; k < VT8_SLOTS; k++) {   /* group the vtables by their tick's code */
                if (!s_vt8_calls[k]) continue;
                int j = 0;
                while (j < m && th[j].code != s_vt8[k].code) j++;
                if (j == m) { th[m].code = s_vt8[k].code; th[m].calls = th[m].not2 = th[m].skip = 0; m++; }
                th[j].calls += s_vt8_calls[k]; th[j].not2 += s_vt8_not2[k]; th[j].skip += s_vt8_skip[k];
            }
            for (int a = 0; a < m; a++) for (int b = a + 1; b < m; b++)
                if (th[b].not2 > th[a].not2) { const __typeof__(th[0]) t = th[a]; th[a] = th[b]; th[b] = t; }
            fprintf(stderr, "[dlogic] TICK by function (calls / not done / skipped by POLL):");
            for (int a = 0; a < m && a < 10; a++)
                fprintf(stderr, " %06X %llu/%llu/%llu", th[a].code, (unsigned long long)th[a].calls,
                        (unsigned long long)th[a].not2, (unsigned long long)th[a].skip);
            fprintf(stderr, "\n");
        }
#undef PF
#undef US
        last = now;
    }
}
#ifdef _WIN32
static DWORD WINAPI stats_main(LPVOID arg) { (void)arg; stats_loop(); return 0; }
#else
static void* stats_main(void* arg) { (void)arg; stats_loop(); return NULL; }
#endif

void inf2_dlogic_install(void)
{
    static int done;
    if (done) return;
    done = 1;
    const int all = env_flag("PS3_D", 1);
    const int chk = env_flag("PS3_D_CHECK", 0);
    s_tick = all && env_flag("PS3_D_TICK", 1);
    s_poll = s_tick && env_flag("PS3_D_POLL", 1);
    s_poll_chk = s_poll && env_flag("PS3_D_POLL_CHECK", 0);   /* (needs the native tick: PS3_D_CHECK runs the game's) */
    s_qsort = all && env_flag("PS3_D_QSORT", 1);
    s_memcpy = all && env_flag("PS3_D_MEMCPY", 0);   /* default off: see the header */
    s_tick_chk = all && (chk || env_flag("PS3_D_TICK_CHECK", 0));
    s_qsort_chk = all && (chk || env_flag("PS3_D_QSORT_CHECK", 0));
    { const char* e = getenv("PS3_D_STATS"); if (e && *e) s_stats_s = atoi(e); }
    s_timing = env_flag("PS3_D_TIMING", 0) || s_tick_chk || s_qsort_chk;   /* (the checks time both sides anyway) */
    {
        const char* e = getenv("PS3_STATE_DIGEST");
        if (e && *e && strcmp(e, "0") != 0) {
            s_digest = fopen(strcmp(e, "1") == 0 ? "dlogic_digest.txt" : e, "w");
            if (!s_digest) fprintf(stderr, "[dlogic] PS3_STATE_DIGEST: cannot open %s\n", e);
        }
    }
    tsc_calibrate();
    /* a part that is off leaves its slot empty (the lifted function, one never-taken branch) */
    if (s_tick || s_tick_chk || s_digest) ppu_hook_set(0x0032E030u, hook_tick);
    if (s_tick_chk) ppu_hook_set(0x006AB340u, hook_copy_resize);
    if (s_qsort || s_qsort_chk) ppu_hook_set(0x00629138u, hook_qsort);
    if (s_memcpy) { ppu_hook_set(0x00626900u, hook_memcpy); ppu_hook_set(0x00630538u, hook_memset); }
    fprintf(stderr, "[dlogic] track D: TICK %s, POLL %s, HELP %s, QSORT %s, MEMCPY %s%s%s (PS3_D=0: all off)\n",
            s_tick_chk ? "check" : s_tick ? "on" : "off", s_tick && !s_tick_chk && s_poll ? (s_poll_chk ? "check" : "on") : "off",
            s_tick && !s_tick_chk && s_poll && inf2_dlogic_help ? "on (job runner)" : "off",
            s_qsort_chk ? "check" : s_qsort ? "on" : "off", s_memcpy ? "on" : "off",
            s_digest ? ", state digest on" : "", s_stats_s > 0 ? "" : ", no counters");
    if (s_stats_s > 0) {
#ifdef _WIN32
        CreateThread(NULL, 1u << 16, stats_main, NULL, 0, NULL);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, stats_main, NULL) == 0) pthread_detach(t);
#endif
    }
}
