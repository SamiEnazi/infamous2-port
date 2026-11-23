/* From the Nintendo Switch fork github.com/4evry1/infamous2-port-NX (branch switch-nx @ 49fc23333713,
 * switch/natives/job212_native.c, MIT licence of the project), unchanged except this note and a portable path
 * for the NEON fill in j212_core (PC build): there it builds for
 * the Switch only; here it is part of the PC build (natives/spu). The SPU modules and image ids are the same. */
/* job212_native.c -- inFAMOUS 2 (BCES01143): the start-up clear of the data area of SPU job 212
 * (jobmod_87c8e5f63136_e2f28, LS 0x701C) and of job 210 (jobmod_867e5d2348b3_e2898, LS 0x698C), natively. Switch
 * build only (switch/natives/: the PC build compiles every C file of natives/spu); written for this port, not
 * derived from another project.
 *
 * What it does. Every job module starts with the same loop (its crt0): it clears the module's uninitialised data,
 *     do { store quadword r5 (zero here) at LS (r3 + r126); r3 += 16; r6 = (r4 > r3) } while (r6 word 0);
 * with r3 the first address, r4 the end (both link-time addresses), r126 the load offset (zero for these jobs),
 * all four words of r3 / r4 / r6 taking part; the first pass always runs. Then the job goes on with its static
 * constructors (212: 0x6F74, 210: 0x68E4). Job 212 clears 0x8FD0..0x10890, 30 KB in 1900 passes of about eight
 * host instructions, on every run of the job: 0.1% of all busy samples on the console (hz-0.52, gameplay idle at a
 * dock), job 210's 0.075%. The other 25 modules have the same function (the same text but for the continuation);
 * they are not in the hot list and are not hooked.
 *
 * The model does what the loop does, with one fill: it works out the number of passes from word 0 of r3 / r4
 * (b > a: ceil((b - a) / 16), else 1), stores that many quadwords of r5 (a zero fill is a memset), and leaves r3
 * (every word + 16 * passes) and r6 (per word, r4 > r3 unsigned: word 0 is zero, the loop's exit) as the loop
 * leaves them. No other register or state changes. The lifted function then continues at the address of
 * natives-switch.txt (the continuation) as it does after its own loop.
 *
 * What it declines (before it writes anything; the lifted loop then runs): a loop whose r3 word 0 would pass
 * 2^32 (it would go on from low addresses), and one whose stores would pass the end of the 256 KB local store
 * (they would wrap round).
 *
 * PS3_NATIVE_J212: 1 (default) on, 0 off (the lifted loop), 2 a check run: every call the model takes runs on a
 * copy of the local store and of all 128 registers, then the loop runs as the lifted function has it (the
 * statements below, on the real context) and the two stores and all registers are compared; the loop's result is
 * what stays. (The lifted function cannot be run itself in a check run: it goes on to the rest of the job.)
 * PS3_SPU_NATIVE=0 turns every native rewrite off. The stats line (every 4096 calls) counts, per job, the calls
 * the model took and declined and, in the check run, the calls compared and the ones that differ. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"
#include "spu_helpers.h"

int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));

enum { ROW_212 = 0, ROW_210, ROW_N };
static const struct { int image; uint32_t addr; } k_row[ROW_N] = { { 212, 0x701Cu }, { 210, 0x698Cu } };

/* ---------------------------------------------------------------------------------- the model */
/* the loop on (gpr, ls): 1 = done, 0 = declined and nothing written */
static int j212_core(u128* gpr, uint8_t* ls)
{
    const uint32_t a = gpr[3]._u32[0], b = gpr[4]._u32[0], d = gpr[126]._u32[0];
    const uint64_t passes = b > a ? ((uint64_t)(b - a) + 15u) >> 4 : 1u;
    const uint64_t span = passes << 4;
    const uint32_t lo = (a + d) & (SPU_LS_MASK & ~0xFu);        /* the first store's address */
    if ((uint64_t)a + span > 0xFFFFFFFFull || (uint64_t)lo + span > SPU_LS_SIZE) return 0;
    uint8_t* p = ls + lo;
    if (!(gpr[5]._u64[0] | gpr[5]._u64[1])) memset(p, 0, (size_t)span);
    else {
#if defined(__aarch64__)
        const uint8x16_t v = vrev32q_u8(vld1q_u8(gpr[5]._u8));  /* what spu_ls_write128 stores */
        uint8_t* const e = p + span;
        for (; p + 64 <= e; p += 64) { vst1q_u8(p, v); vst1q_u8(p + 16, v); vst1q_u8(p + 32, v); vst1q_u8(p + 48, v); }
        for (; p < e; p += 16) vst1q_u8(p, v);
#else   /* (PC build) the same pattern, bytes reversed in each word as spu_ls_write128 stores them */
        uint8_t v[16];
        for (int i = 0; i < 16; i++) v[i] = gpr[5]._u8[(i & ~3) | (3 - (i & 3))];
        uint8_t* const e = p + span;
        for (; p < e; p += 16) memcpy(p, v, 16);
#endif
    }
#if defined(__aarch64__)
    const uint32x4_t r3 = vaddq_u32(vld1q_u32(gpr[3]._u32), vdupq_n_u32((uint32_t)span));
    vst1q_u32(gpr[3]._u32, r3);
    vst1q_u32(gpr[6]._u32, vcgtq_u32(vld1q_u32(gpr[4]._u32), r3));
#else
    for (int i = 0; i < 4; i++) {
        const uint32_t r3 = gpr[3]._u32[i] + (uint32_t)span;
        gpr[3]._u32[i] = r3;
        gpr[6]._u32[i] = gpr[4]._u32[i] > r3 ? 0xFFFFFFFFu : 0u;
    }
#endif
    return 1;
}

/* the lifted function's loop, statement by statement (the check run's reference) */
static void j212_loop(spu_context* ctx)
{
    do {
        spu_ls_write128(ctx, ctx->gpr[3]._u32[0] + ctx->gpr[126]._u32[0], ctx->gpr[5]);
        ctx->gpr[3] = spu_ai(ctx->gpr[3], 16);
        ctx->gpr[6] = spu_clgt(ctx->gpr[4], ctx->gpr[3]);
    } while (ctx->gpr[6]._u32[0] != 0);
}

/* ---------------------------------------------------------------------------------- entry, check run, stats */
typedef struct { unsigned long long calls, native, declined, cmp, bad; } Stat;
static Stat s_st[ROW_N];
static int s_mode = -1;                                    /* PS3_NATIVE_J212 */
static unsigned long long s_calls;
static _Thread_local uint8_t* t_ls;                        /* the check run's copy of the store */

#define ADD(f, n) __atomic_fetch_add(&(f), (n), __ATOMIC_RELAXED)

static void stats(unsigned long long n)
{
    fprintf(stderr, "[j212] %llu calls", n);
    for (int r = 0; r < ROW_N; r++) {
        const Stat* t = &s_st[r];
        fprintf(stderr, "; job %d: native %llu, declined %llu", k_row[r].image, t->native, t->declined);
        if (s_mode == 2) fprintf(stderr, ", compared %llu, differ %llu", t->cmp, t->bad);
    }
    fputc('\n', stderr);
}

/* mode 2: the model on a copy, the loop on the real context, then the comparison */
static __attribute__((noinline)) int j212_check(spu_context* ctx, int row)
{
    Stat* st = &s_st[row];
    if (!t_ls && !(t_ls = (uint8_t*)malloc(SPU_LS_SIZE))) return 0;
    u128 g[128];
    memcpy(g, ctx->gpr, sizeof g);
    memcpy(t_ls, SPU_LS_BASE(ctx), SPU_LS_SIZE);
    if (!j212_core(g, t_ls)) { ADD(st->declined, 1); return 0; }
    const uint32_t a = ctx->gpr[3]._u32[0], b = ctx->gpr[4]._u32[0], d = ctx->gpr[126]._u32[0];
    j212_loop(ctx);
    const int ls_bad = memcmp(SPU_LS_BASE(ctx), t_ls, SPU_LS_SIZE) != 0;
    int reg = -1;
    for (int r = 0; r < 128 && reg < 0; r++)
        if (memcmp(&ctx->gpr[r], &g[r], 16)) reg = r;
    ADD(st->cmp, 1);
    if (ls_bad || reg >= 0) {
        const unsigned long long nb = __atomic_add_fetch(&st->bad, 1, __ATOMIC_RELAXED);
        if (nb <= 12)
            fprintf(stderr, "[j212] check DIFFER (job %d) #%llu: r3=%08X r4=%08X r126=%08X: local store %s, register r%d\n",
                    k_row[row].image, nb, a, b, d, ls_bad ? "differs" : "same", reg);
    }
    return 1;
}

static int j212_entry(spu_context* ctx, int row)
{
    if (__builtin_expect(__atomic_load_n(&s_mode, __ATOMIC_ACQUIRE) < 0, 0)) {
        const char* e = getenv("PS3_NATIVE_J212");
        int m = e && e[0] ? (e[0] == '2' ? 2 : e[0] != '0') : 1, unset = -1;
        if (__atomic_compare_exchange_n(&s_mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))   /* the first thread says so */
            fprintf(stderr, "[j212] native start-up data clear (jobs %d at 0x%X and %d at 0x%X): %s\n", k_row[ROW_212].image,
                    k_row[ROW_212].addr, k_row[ROW_210].image, k_row[ROW_210].addr,
                    m == 2 ? "check run against the lifted loop" : m ? "on" : "off");
    }
    if (!s_mode) return 0;
    Stat* st = &s_st[row];
    const unsigned long long n = __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);
    if (!(n & 0xFFF)) stats(n);
    ADD(st->calls, 1);
    int r;
    if (s_mode == 2) r = j212_check(ctx, row);
    else if (!(r = j212_core(ctx->gpr, SPU_LS_BASE(ctx)))) ADD(st->declined, 1);
    if (r) ADD(st->native, 1);
    return r;
}
static int j212_entry_212(spu_context* ctx) { return j212_entry(ctx, ROW_212); }
static int j212_entry_210(spu_context* ctx) { return j212_entry(ctx, ROW_210); }

__attribute__((constructor)) static void j212_register(void)
{
    spu_native_register(k_row[ROW_212].image, k_row[ROW_212].addr, j212_entry_212);
    spu_native_register(k_row[ROW_210].image, k_row[ROW_210].addr, j212_entry_210);
}
