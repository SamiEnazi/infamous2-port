/* From the Nintendo Switch fork github.com/4evry1/infamous2-port-NX (branch switch-nx @ 49fc23333713,
 * switch/natives/job211_native.c, MIT licence of the project), unchanged except this note: there it builds for
 * the Switch only; here it is part of the PC build (natives/spu). The SPU modules and image ids are the same. */
/* job211_native.c -- inFAMOUS 2 (BCES01143): the normal-map stage of SPU job 211 (jobmod_87926738de5c_e4640, LS
 * 0x45F8) natively, and the spectrum and transform passes it calls (0x40E8, 0x54E8, 0x5118, 0x6EB8, 0x7860, 0x67A8).
 * Switch build only (switch/natives/: the PC build compiles every C file of natives/spu); written for
 * this port, not derived from another project.
 *
 * What the job is. A 64 x 64 grid simulation, once a frame (water: "writes 16 KB of floats and 21.8 KB of 16-bit pairs
 * centred on 0x8000"). Its worker calls 0x45F8 with
 *     r3 = A  a 16 KB buffer of the spectrum, then at A + 0x4000 the word EA of where the 16-bit result goes
 *     r4 = F  the work buffer: two 16 KB arrays of floats (F + 0 and F + 0x4000, 64 rows of 64 floats), a table of 64 row
 *             pointers for each at F + 0x8000 and F + 0x8100, and from F + 0x82F0 the output
 *     r5 = the DMA tag
 * What 0x45F8 does, in order:
 *   1. fills the two pointer tables (F + 256 * i and F + 0x4000 + 256 * i) and clears a word at F + 0x8200;
 *   2. calls 0x40E8 (the spectrum: the first pass over A and F) and 0x80B8 twice (the 2-D transform of each array, over
 *      each table); the native calls them as the lifted function does, and the passes they run are hooked (below);
 *   3. makes a normal map from the two arrays: for every texel i of 4096, in the order of a Z curve (x from the even
 *      bits of i, y from the odd bits), h1 and h2 are the floats at pointer1[x] + 4 y and pointer2[x] + 4 y, the normal
 *      is n = (-h1, 1, -h2) / |n| (|n| from a reciprocal square root estimate with one Newton step, the reciprocal of
 *      that from a reciprocal estimate with one step), and the 16-bit pair (n.-h2 * 32767 + 32768, n.-h1 * 32767 +
 *      32768, each truncated) goes to F + 0x82F0 + 4 i;
 *   4. PUTs the 16 KB (LS F + 0x82F0 to the EA at A + 0x4000), builds the mip chain after it in the local store
 *      (32 x 32, 16 x 16 ... 2 x 2 texels, each the average of four consecutive texels of the level above, per 16-bit
 *      half (a + b + c + d + 2) >> 2, 5456 bytes), PUTs the chain (at EA + 16384), makes the 1 x 1 texel the same way and
 *      PUTs its 4 bytes (at EA + 16384 + 5456).
 * The normal-map loop was 0.46% of all busy samples lifted (hz-0.52, gameplay idle at a dock): 4096 texels, each a
 * bit-by-bit decode of the Z curve and about 40 float operations through the register file in memory.
 *
 * The passes. 0x80B8 (it stays the lifted function: its own loops are 0.01% of the busy samples) is the real 2-D transform
 * of a 64 x 64 array of floats in place, a split-radix transform of the kind found in FFT libraries, on the path of its
 * argument r5 = -1: with the tables a call builds the first time (the twiddle factors at F + 0x8228 and those of the real
 * step at F + 0x82A8, 0x7D48 and 0x7A48, which stay lifted: not a sample of them in the profile) it makes the pairs of
 * rows, calls 0x54E8, 0x6EB8, the 0.5 loop, 0x7860, 0x5118 and ends in 0x67A8 (a tail call). Together with the
 * spectrum they were 1.0% of the busy samples lifted (hz-0.52): 0x6EB8 0.30%, 0x67A8 and its blocks 0.33%, 0x40E8 and
 * 0x4030 0.17%, 0x7860 0.11%, 0x5118 0.10%, 0x54E8 0.04%. Each is hooked at its own entry (natives-switch.txt), so the lifted
 * 0x80B8 calls it as it calls a lifted function, and each takes a call whose arguments it models and declines before it
 * writes anything otherwise:
 *   0x40E8 (r3 = A, r4 = F)   for every row y and four columns at a time: the wave vector and its length from the constant
 *       at LS 0x8970 (set the first time, as the lifted code does), a phase from A, the time terms at A + 0x4004 and
 *       A + 0x4008 and the per-lane weights in the module, wrapped by 0x4030 and written back to A, then the sine and cosine
 *       of it (the job's octant polynomials) times the second array of A, and the interleaved complex values written to
 *       the two work arrays through the two pointer tables. It runs the lifted code's own operations on the same four
 *       lanes (the helpers' speculative forms, the lean ones for a block they refuse), in its order.
 *   0x6EB8 (r3 = n1 rows, r4 = n2 floats a row, r5 = the row table, r6 = w)   the complex transform along the columns:
 *       radix-4 stages l = 1, 4 ... while 2l < n1 (a block without twiddles, a block of 45 degrees, blocks kk = 2, 3 ...
 *       with the factors of w), then a radix-2 stage; four columns at a time on a lane a column.
 *   0x67A8 (the same arguments)   the same transform along each row (l counted in floats: 2, 8, 32 ...): the first stage
 *       (runs of one complex number) four rows at a time on a lane a row, the others four numbers at a time.
 *   0x7860 (r3 = n1, r4 = n2, r5 = the row table, r6 = nc, r7 = c)   the real step of each row: the pairs a[m-2-2t],
 *       a[m-1-2t] and a[m+2+2t], a[m+3+2t] (m = n2 / 2) with the factors c[kk], c[nc-kk], kk = (4nc / n2)(t + 1).
 *   0x54E8, 0x5118 (r3 = n1, r4 = n2, r5 = ip, r6 = the row table)   the bit reversal of the rows and inside each row: the
 *       index table ip[0..m-1] they build in the work buffer (the lifted code writes it too) and the swaps.
 * A pass takes a call whose row table is one run of equal strides (rows word aligned, inside the store, not overlapping
 * each other, the table, w / c / ip or the module or the stack: the lifted code reads the pointers where it needs them),
 * n1 and n2 powers of two within 4..512 (0x5118 and 0x7860: n2 >= 8), and index tables that stay inside the rows; the data
 * of 0x40E8 may be anything. A pass leaves r0, r1 and r80.. alone and writes the rows and nothing else but the index
 * table (0x54E8, 0x5118) or, for 0x40E8, the first half of A and the constant at 0x8970 with its flag (the lifted
 * functions' own spills below the stack pointer are dead data).
 *
 * What the native does. It does 1. and 2. as the lifted function does (the same registers at each call, the same stack
 * pointer), then 3. and 4. on the local store: the LS words it reads are read where the lifted loop reads them (through
 * the pointer tables, which the second pass may have changed), in the same order as it writes the output, so a table
 * that pointed into the output would give the lifted result as well. The three PUTs are the lifted code's channel
 * writes (the MFC does them). The local store ends as the lifted function leaves it but for its stack frames below the
 * stack pointer (spills of r80..r84, r0, r1, and the callees' own), and the registers a caller can see (r0, r1, r80..r127)
 * are what it found. It declines, before it does anything, an argument that is not quadword aligned or whose buffers
 * leave the local store or overlap each other, the module (LS 0x4000..0x89FF) or the stack, and the job recorder being on.
 *
 * Floats. The arithmetic is the SPU's single precision. For ordinary heights (|h| < 2^24: n is the one thing here that
 * could overflow, and it cannot) the texel runs on plain floats under FPCR round-toward-zero + flush-to-zero, the
 * mode of the SPU threads, with the helpers' own estimate tables and steps; any other height, or any other mode, takes
 * the engine's float helpers (spu_fm_lean ...) with the lifted code's operands, exact for every input. The transform
 * passes have two forms: raw (plain scalar or four-lane floats under FPCR round-toward-zero + flush-to-zero, one
 * operation per instruction: they are the SPU's for every value below FLT_MAX in magnitude but for the sign of a
 * zero, which nothing reads and every stored result loses) when the data of the call is below 2^50 and its twiddle
 * factors below 4 (a pass multiplies a magnitude by less than 2^9 a stage and has at most five stages: nothing reaches
 * FLT_MAX or exponent 255), and exact (the engine's lean helpers on one lane, the lifted code's operations in its
 * order) for anything else; 0x40E8 is on the helpers' speculative form with the lean one as its fallback.
 *
 * PS3_NATIVE_J211: 1 (default) on, 0 off (the lifted function), 2 a check run: the native runs, its local store and
 * registers are kept, the state before it is put back, the lifted function runs, and the two are compared (the local
 * store outside the stack, r0, r1, r80..r127); the lifted result is what stays. In a check run the native's three PUTs
 * land in guest memory first and the lifted function's then overwrite them, and the PUTs' addresses and sizes are not
 * compared: a diagnostic mode, not one to play with. The hooked passes are checked the same way, each on its own call
 * (the lifted function runs without any hook), and a declined call must have changed nothing. PS3_NATIVE_J211_SLOW=1
 * uses the helpers for every texel and the exact form for every pass. PS3_NATIVE_J211_PASSES=<hex mask> chooses the
 * hooks (default 3F; 0 = none): bit 0 0x40E8, 1 0x54E8, 2 0x5118, 3 0x6EB8, 4 0x7860, 5 0x67A8.
 * PS3_SPU_NATIVE=0 turns every native rewrite off. The stats lines (every 10 s) count the calls the native took and
 * declined, those of each pass, the raw ones among them, and in the check run the calls compared and the ones that
 * differ. */
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

extern volatile int g_spu_jobrec_mode;
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
extern void jobmod_87926738de5c_e4640_spu_func_000040E8(spu_context* ctx);
extern void jobmod_87926738de5c_e4640_spu_func_000080B8(spu_context* ctx);

#ifndef J211_COV
#define J211_COV(n) ((void)0)
#endif

#define J211_IMAGE 211
#define J211_FUNC  0x45F8u

#define OUT0   0x82F0u              /* the normal map in the work buffer */
#define MIP0   (OUT0 + 0x4000u)     /* the mip chain after it */
#define MIPLEN 5456u
#define END    (MIP0 + MIPLEN + 16u)

static inline uint32_t R32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t R16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline void W32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void W16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 SPLAT(uint32_t w) { return V4(w, w, w, w); }
static inline int ovl(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1 && a0 < a1 && b0 < b1; }
/* lqd + rotqby: the word at byte `a` of the local store as the job's scalar load reads it (the quadword of a, rotated by a) */
static inline uint32_t LSW(const uint8_t* ls, uint32_t a)
{
    const uint8_t* q = ls + (a & (SPU_LS_MASK & ~0xFu));
    const uint32_t r = a & 15;
    return (uint32_t)q[r] << 24 | (uint32_t)q[(r + 1) & 15] << 16 | (uint32_t)q[(r + 2) & 15] << 8 | q[(r + 3) & 15];
}

/* ---------------------------------------------------------------------------------- the scalar float form */
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
static inline float FF(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static inline uint32_t FU(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
static inline int small_w(uint32_t w) { return ((w >> 23) & 0xFFu) < 0x97u; }      /* |x| < 2^24 (or a zero / denormal) */
static inline int fp_rzfz(void)
{
#if defined(__aarch64__)
    uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return (v & 0x01C00000u) == 0x01C00000u;                /* RMode = round toward zero, FZ */
#elif defined(__x86_64__) || defined(__i386__)
    return (_mm_getcsr() & 0xE040u) == 0xE040u;             /* RC = toward zero, FTZ, DAZ */
#else
    return 0;
#endif
}
/* spu_frsqest, spu_frest and spu_fi on one lane: the helpers' own tables and steps */
static inline uint32_t rsqrt_est(uint32_t bits)
{
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t rexp = exp == 0u ? 0xFFu : 190u - (exp + 1u) / 2u;
    return (rexp << 23) | (spu_frsqest_fraction_lut[(bits >> 18) & 0x3Fu] & 0x7FFFFFu);
}
static inline uint32_t recip_est(uint32_t bits)
{
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t rexp = exp == 0u ? 255u : exp >= 253u ? 0u : 253u - exp;
    return (bits & 0x80000000u) | (rexp << 23) | (spu_frest_fraction_lut[(bits >> 18) & 0x1Fu] & 0x7FFFFFu);
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

/* One texel of the normal map from the two heights (float words): the two 16-bit halves, first the one from -h2.
 * Plain floats for ordinary heights, the helpers for the rest. */
static inline uint32_t texel_fast(uint32_t h1b, uint32_t h2b)
{
    const float h1 = FF(h1b), h2 = FF(h2b);
    const float s = __builtin_fmaf(h2, h2, __builtin_fmaf(h1, h1, 1.0f));      /* >= 1: the minimum test of the job is always true */
    const float r = FF(interp(FU(s), rsqrt_est(FU(s))));
    const float r2 = r * r, rh = r * 0.5f;
    const float e = __builtin_fmaf(-s, r2, 1.0f);
    const float y = __builtin_fmaf(e, rh, r);
    const float sq = y * s;
    const uint32_t sb = FU(sq);
    const float rr = FF(interp(sb, recip_est(sb)));
    const float e2 = __builtin_fmaf(-rr, sq, 1.0f);
    const float inv = __builtin_fmaf(e2, rr, rr);
    const float nx = inv * FF(h2b ^ 0x80000000u), nz = inv * FF(h1b ^ 0x80000000u);
    const float ox = __builtin_fmaf(nx, 32767.0f, 32768.0f), oz = __builtin_fmaf(nz, 32767.0f, 32768.0f);
    const uint32_t u1 = ox > 0.0f ? (uint32_t)ox : 0u, u2 = oz > 0.0f ? (uint32_t)oz : 0u;
    return (u1 & 0xFFFFu) << 16 | (u2 & 0xFFFFu);
}
static uint32_t texel_slow(uint32_t h1b, uint32_t h2b)
{
    const u128 one = SPLAT(0x3F800000u), half = SPLAT(0x3F000000u), sg = SPLAT(0x80000000u);
    const u128 H1 = SPLAT(h1b), H2 = SPLAT(h2b);
    const u128 s1 = spu_fma_lean(H1, H1, one);
    const u128 n2 = spu_xor(H2, sg), n1 = spu_xor(H1, sg);
    const u128 s = spu_fma_lean(H2, H2, s1);
    const u128 big = spu_fcgt_lean(s, SPLAT(0x00800000u));
    const u128 est = spu_frsqest(s);
    const u128 r = spu_fi(s, est);
    const u128 r2 = spu_fm_lean(r, r), rh = spu_fm_lean(r, half);
    const u128 e = spu_fnms_lean(s, r2, one);
    const u128 y = spu_fma_lean(e, rh, r);
    const u128 sq = spu_fm_lean(y, s);
    const u128 z = spu_selb(V4(0, 0, 0, 0), sq, big);
    const u128 rc = spu_frest(z);
    const u128 rr = spu_fi(z, rc);
    const u128 e2 = spu_fnms_lean(rr, z, one);
    const u128 inv = spu_fma_lean(e2, rr, rr);
    const u128 nx = spu_fm_lean(inv, n2), nz = spu_fm_lean(inv, n1);
    const u128 c1 = SPLAT(0x46FFFE00u), c2 = SPLAT(0x47000000u);
    const u128 ox = spu_fma_lean(nx, c1, c2), oz = spu_fma_lean(nz, c1, c2);
    const u128 u1 = spu_cfltu(ox, 173), u2 = spu_cfltu(oz, 173);
    return (u1._u32[0] & 0xFFFFu) << 16 | (u2._u32[0] & 0xFFFFu);
}

/* the x and y of a texel on the job's Z curve: the even and the odd bits of i */
static inline uint32_t compact1(uint32_t v)
{
    v &= 0x55555555u; v = (v | (v >> 1)) & 0x33333333u; v = (v | (v >> 2)) & 0x0F0F0F0Fu;
    v = (v | (v >> 4)) & 0x00FF00FFu; return (v | (v >> 8)) & 0x0000FFFFu;
}

/* ---------------------------------------------------------------------------------- the function */
static int s_fast = 1;                                       /* PS3_NATIVE_J211_SLOW=1: always the helpers */

/* a PUT as the lifted code issues it: the five channel writes between a di and an ei */
static void put_cmd(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag_word)
{
    ctx->int_enable = 0;
    spu_wrch(ctx, MFC_LSA, SPLAT(lsa));
    spu_wrch(ctx, MFC_EAL, SPLAT(ea));
    spu_wrch(ctx, MFC_Size, SPLAT(size));
    spu_wrch(ctx, MFC_TagID, SPLAT(tag_word));
    spu_wrch(ctx, MFC_Cmd, SPLAT(32));
    ctx->int_enable = 1;
}

static void call_lifted(spu_context* ctx, void (*fn)(spu_context*), uint32_t link)
{
    ctx->gpr[0] = spu_link(link);
    { int32_t _si = (int32_t)ctx->image_id; ctx->host_depth++; fn(ctx); spu_drain_call(ctx, link); ctx->host_depth--; spu_img_restore(ctx, _si); }
}

/* the whole function on the real context; 0 = declined, nothing done */
static int j211_run(spu_context* ctx, int fast_ok)
{
    uint8_t* const ls = SPU_LS_BASE(ctx);
    const uint32_t A = ctx->gpr[3]._u32[0], F = ctx->gpr[4]._u32[0], sp = ctx->gpr[1]._u32[0];
    if (g_spu_jobrec_mode || ((A | F | sp) & 15u) || (uint64_t)A + 0x4010u > SPU_LS_SIZE || (uint64_t)F + END > SPU_LS_SIZE || sp < 0x1000u ||
        (uint64_t)sp + 0x40u > SPU_LS_SIZE || ovl(F, F + END, A, A + 0x4010u) || ovl(F, F + END, sp - 0x400u, sp + 0x20u) ||
        ovl(A, A + 0x4010u, sp - 0x400u, sp + 0x20u) || ovl(F, F + END, 0x4000u, 0x8A00u) || ovl(A, A + 0x4010u, 0x4000u, 0x8A00u))
        return 0;
    const u128 r0 = ctx->gpr[0], r1 = ctx->gpr[1], r4 = ctx->gpr[4];
    const u128 tagq = ctx->gpr[5];
    const int fast = fast_ok && fp_rzfz();
    /* 1. the pointer tables, the cleared word */
    for (uint32_t i = 0; i < 64; i++) { W32(ls + F + 0x8000u + 4 * i, F + 256u * i); W32(ls + F + 0x8100u + 4 * i, F + 0x4000u + 256u * i); }
    ctx->gpr[1] = spu_ai(ctx->gpr[1], -112);
    { const uint32_t wa = F + 0x8200u + (ctx->gpr[1]._u32[0] & 0xCu);       /* cwx(r1, 0x8200): the word of the quadword r1 selects */
      W32(ls + wa, 0); }
    /* 2. the transform passes, lifted */
    call_lifted(ctx, jobmod_87926738de5c_e4640_spu_func_000040E8, 0x46B0u);
    {
        const u128 a8 = spu_a(r4, spu_ila(0x8228)), a2 = spu_a(r4, spu_ila(0x8200));
        ctx->gpr[5] = spu_fsmbi(0xFFFF); ctx->gpr[6] = spu_a(r4, spu_ila(0x8000)); ctx->gpr[7] = spu_shlqbyi(a2, 0); ctx->gpr[8] = spu_ori(a8, 0);
        ctx->gpr[3] = spu_il(64); ctx->gpr[4] = spu_il(64);
        call_lifted(ctx, jobmod_87926738de5c_e4640_spu_func_000080B8, 0x46E0u);
        ctx->gpr[7] = spu_shlqbyi(a2, 0); ctx->gpr[5] = spu_fsmbi(0xFFFF); ctx->gpr[6] = spu_a(r4, spu_ila(0x8100)); ctx->gpr[8] = spu_shlqbyi(a8, 0);
        ctx->gpr[3] = spu_il(64); ctx->gpr[4] = spu_il(64);
        call_lifted(ctx, jobmod_87926738de5c_e4640_spu_func_000080B8, 0x4710u);
    }
    /* 3. the normal map */
    uint8_t* out = ls + F + OUT0;
    for (uint32_t i = 0; i < 4096; i++) {
        const uint32_t x = compact1(i), y = compact1(i >> 1);
        const uint32_t p1 = LSW(ls, F + 0x8000u + 4 * x), p2 = LSW(ls, F + 0x8100u + 4 * x);
        const uint32_t h1 = LSW(ls, p1 + 4 * y), h2 = LSW(ls, p2 + 4 * y);
        const uint32_t v = fast && small_w(h1) && small_w(h2) ? texel_fast(h1, h2) : texel_slow(h1, h2);
        W32(out + 4 * i, v);
        J211_COV(fast && small_w(h1) && small_w(h2) ? 0 : 1);
    }
    /* 4. the PUT of the map, the mip chain, its PUT, the 1 x 1 texel, its PUT */
    const uint32_t ea0 = R32(ls + A + 0x4000u), tagw = tagq._u32[0];
    put_cmd(ctx, F + OUT0, ea0, 16384, tagw);
    uint32_t prev = F + OUT0, cur = F + MIP0, size = 32, end;
    for (;;) {
        const uint32_t n = size * size;
        for (uint32_t j = 0; j < n; j++) {
            const uint8_t* s = ls + prev + 16 * j;
            W16(ls + cur + 4 * j, (R16(s) + R16(s + 4) + R16(s + 8) + R16(s + 12) + 2u) >> 2);
            W16(ls + cur + 4 * j + 2, (R16(s + 2) + R16(s + 6) + R16(s + 10) + R16(s + 14) + 2u) >> 2);
        }
        end = cur + 4 * n;
        size >>= 1; prev = cur;
        if (!(size > 1)) break;
        cur = end;
    }
    put_cmd(ctx, F + MIP0, ea0 + 16384u, end - (F + MIP0), tagw);
    {
        const uint8_t* s = ls + prev;
        W16(ls + end, (R16(s) + R16(s + 4) + R16(s + 8) + R16(s + 12) + 2u) >> 2);
        W16(ls + end + 2, (R16(s + 2) + R16(s + 6) + R16(s + 10) + R16(s + 14) + 2u) >> 2);
    }
    put_cmd(ctx, end, ea0 + 16384u + (end - (F + MIP0)), 4, tagw);
    ctx->gpr[0] = r0; ctx->gpr[1] = r1;
    return 1;
}

/* ---------------------------------------------------------------------------------- the passes: shared parts */
/* The functions below are the transform passes the job calls from 0x45F8 (through 0x80B8, which stays the lifted
 * function): each is hooked at its own entry (natives-switch.txt) and takes a call whose arguments it models, or
 * declines before it writes anything. Their data is rows of floats: an array of row pointers (the table) points at
 * rows of the work buffer. The model takes a table whose rows are one run of equal strides (what the work buffer
 * is), each row word aligned and inside the store, the rows not overlapping each other or anything else the pass
 * touches; the lifted code reads the pointers where it needs them and would do something else for any other table. */
#define ALW static inline __attribute__((always_inline))

static inline int pw2(uint32_t n, uint32_t lo, uint32_t hi) { return n >= lo && n <= hi && !(n & (n - 1)); }
static inline int ovl64(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 < b1 && b0 < a1; }
static inline int sp_ok(uint32_t sp) { return sp >= 0x1000u && !(sp & 15u) && (uint64_t)sp + 0x40u <= SPU_LS_SIZE; }
/* a range the pass writes: clear of the module (LS 0x4000..0x89FF) and of the stack the callers keep */
static inline int clear_of(uint64_t lo, uint64_t hi, uint32_t sp)
{
    return !ovl64(lo, hi, 0x4000u, 0x8A00u) && !ovl64(lo, hi, (uint64_t)sp - 0x400u, (uint64_t)sp + 0x20u);
}

typedef struct { uint32_t base, stride; } Rows;
/* The table at tab (n words): row i at base + i * stride, a row of len bytes, all inside the store. */
static int rows_geom(const uint8_t* ls, uint32_t tab, uint32_t n, uint32_t len, Rows* r)
{
    if ((tab & 3u) || !n || (uint64_t)tab + 4ull * n > SPU_LS_SIZE) return 0;
    const uint32_t b = R32(ls + tab);
    uint32_t s = len;
    if (n > 1) { const uint32_t b1 = R32(ls + tab + 4); if (b1 <= b) return 0; s = b1 - b; }
    if (((b | s) & 3u) || s < len || (uint64_t)b + (uint64_t)(n - 1) * s + len > SPU_LS_SIZE) return 0;
    for (uint32_t i = 2; i < n; i++) if (R32(ls + tab + 4 * i) != b + i * s) return 0;      /* b + i * s: below 2^18 */
    r->base = b; r->stride = s;
    return 1;
}
static inline uint64_t rows_end(const Rows* r, uint32_t n, uint32_t len) { return (uint64_t)r->base + (uint64_t)(n - 1) * r->stride + len; }

/* The largest biased exponent of n words (big-endian floats). */
static uint32_t words_exp(const uint8_t* p, uint32_t n)
{
    uint32_t mx = 0;
    for (uint32_t i = 0; i < n; i++) { const uint32_t e = R32(p + 4 * i) & 0x7F800000u; mx = e > mx ? e : mx; }
    return mx >> 23;
}
static uint32_t rows_exp(const uint8_t* ls, const Rows* r, uint32_t n, uint32_t words)
{
    uint32_t mx = 0;
    for (uint32_t i = 0; i < n; i++) { const uint32_t e = words_exp(ls + r->base + (uint64_t)i * r->stride, words); mx = e > mx ? e : mx; }
    return mx;
}
/* The raw form (plain floats; see the floats paragraph at the top) is taken when every float it reads is below
 * 2^50 in magnitude (biased exponent <= 0xB0) and every twiddle factor below 4 (<= 0x80). A pass has at most
 * four radix-4 stages and one radix-2 (rows of at most 512 floats), and one stage multiplies a magnitude by less
 * than 2^9 (a sum of four inputs, times a twiddle factor: below 4 for the first two powers, and the third power's
 * below 4 + 2 * 4 * 4 = 36, two such products added: 4 * 36 * 2 = 288 < 2^9): the largest value is below
 * 2^50 * 2^36 * 2 = 2^87, far from FLT_MAX, and no operand has exponent 255. */
#define E_DATA 0xB0u
#define E_TW   0x80u

enum { M_RAW = 0, M_EXACT = 1, M_SPEC = 2 };
/* The operations of a pass, by form (m is a constant where these are inlined). raw: the host's operations under
 * FPCR round-toward-zero + flush-to-zero are the SPU's for every value below FLT_MAX in magnitude (a product
 * truncated to the single result, an add of the exact sum truncated, a fused multiply-add one truncation), but
 * for the sign of a zero: nothing reads that sign (a zero is only an operand of sums and products, which give
 * the same nonzero values either way), and every result stored is flushed (STF). exact: the engine's helpers on
 * one lane, right for every input. */
ALW float fa_(const int m, float a, float b) { return m == M_RAW ? a + b : FF(spu_fa_lean(SPLAT(FU(a)), SPLAT(FU(b)))._u32[0]); }
ALW float fs_(const int m, float a, float b) { return m == M_RAW ? a - b : FF(spu_fs_lean(SPLAT(FU(a)), SPLAT(FU(b)))._u32[0]); }
ALW float fm_(const int m, float a, float b) { return m == M_RAW ? a * b : FF(spu_fm_lean(SPLAT(FU(a)), SPLAT(FU(b)))._u32[0]); }
ALW float fma_(const int m, float a, float b, float c)         /* a * b + c */
{
    return m == M_RAW ? __builtin_fmaf(a, b, c) : FF(spu_fma_lean(SPLAT(FU(a)), SPLAT(FU(b)), SPLAT(FU(c)))._u32[0]);
}
ALW float fms_(const int m, float a, float b, float c)         /* a * b - c */
{
    return m == M_RAW ? __builtin_fmaf(a, b, -c) : FF(spu_fms_lean(SPLAT(FU(a)), SPLAT(FU(b)), SPLAT(FU(c)))._u32[0]);
}
ALW float LDF(const uint8_t* p) { return FF(R32(p)); }
/* a result as the SPU leaves it: a zero is +0 */
ALW void STF(uint8_t* p, float v) { const uint32_t b = FU(v); W32(p, (b & 0x7F800000u) ? b : 0u); }

/* ---------------------------------------------------------------------------------- the radix-4 / radix-2 stages */
/* One butterfly group: four runs of cnt complex numbers (re, im) each, p0..p3 the start of each run. These are the
 * loop bodies of 0x6EB8 (column transform: the runs are four rows) and 0x67A8 (row transform: the runs are four
 * quarters of a row) -- the lifted code's operations, in its order, one complex number at a time. */
ALW void bf0(const int m, uint8_t* p0, uint8_t* p1, uint8_t* p2, uint8_t* p3, uint32_t cnt)          /* no twiddle */
{
    for (uint32_t c = 0; c < cnt; c++) {
        const uint32_t o = 8 * c;
        const float a0r = LDF(p0 + o), a0i = LDF(p0 + o + 4), a1r = LDF(p1 + o), a1i = LDF(p1 + o + 4);
        const float a2r = LDF(p2 + o), a2i = LDF(p2 + o + 4), a3r = LDF(p3 + o), a3i = LDF(p3 + o + 4);
        const float x0r = fa_(m, a0r, a1r), x1r = fs_(m, a0r, a1r), x2r = fa_(m, a2r, a3r), x3r = fs_(m, a2r, a3r);
        const float x0i = fa_(m, a0i, a1i), x1i = fs_(m, a0i, a1i), x2i = fa_(m, a2i, a3i), x3i = fs_(m, a2i, a3i);
        STF(p0 + o, fa_(m, x0r, x2r)); STF(p0 + o + 4, fa_(m, x0i, x2i));
        STF(p2 + o, fs_(m, x0r, x2r)); STF(p2 + o + 4, fs_(m, x0i, x2i));
        STF(p1 + o, fa_(m, x1r, x3i)); STF(p1 + o + 4, fs_(m, x1i, x3r));
        STF(p3 + o, fs_(m, x1r, x3i)); STF(p3 + o + 4, fa_(m, x1i, x3r));
    }
}
ALW void bfw(const int m, float wn4r, uint8_t* p0, uint8_t* p1, uint8_t* p2, uint8_t* p3, uint32_t cnt)  /* the 45 degree block */
{
    for (uint32_t c = 0; c < cnt; c++) {
        const uint32_t o = 8 * c;
        const float a0r = LDF(p0 + o), a0i = LDF(p0 + o + 4), a1r = LDF(p1 + o), a1i = LDF(p1 + o + 4);
        const float a2r = LDF(p2 + o), a2i = LDF(p2 + o + 4), a3r = LDF(p3 + o), a3i = LDF(p3 + o + 4);
        const float x0r = fa_(m, a0r, a1r), x1r = fs_(m, a0r, a1r), x2r = fa_(m, a2r, a3r), x3r = fs_(m, a2r, a3r);
        const float x0i = fa_(m, a0i, a1i), x1i = fs_(m, a0i, a1i), x2i = fa_(m, a2i, a3i), x3i = fs_(m, a2i, a3i);
        STF(p0 + o, fa_(m, x0r, x2r)); STF(p0 + o + 4, fa_(m, x0i, x2i));
        STF(p2 + o, fs_(m, x0i, x2i)); STF(p2 + o + 4, fs_(m, x2r, x0r));
        const float pp = fa_(m, x1r, x3i), qq = fs_(m, x3i, x1r), rr = fs_(m, x1i, x3r), ss = fa_(m, x1i, x3r);
        const float u0 = fa_(m, pp, rr), u1 = fs_(m, rr, pp), u2 = fa_(m, qq, ss), u3 = fs_(m, qq, ss);
        STF(p1 + o, fm_(m, wn4r, u0)); STF(p1 + o + 4, fm_(m, wn4r, u1));
        STF(p3 + o, fm_(m, wn4r, u2)); STF(p3 + o + 4, fm_(m, wn4r, u3));
    }
}
typedef struct { float k1r, k1i, k2r, k2i, k3r, k3i; } Tw;
/* the twiddle factors of block kk (>= 2) from the table w: w[2kk], w[2kk+1], w[kk] (negated for an odd kk),
 * w[kk +- 1] (+1 for an even kk), and the third power's from the first two */
ALW Tw twid(const int m, const uint8_t* w, uint32_t kk)
{
    const int s = (kk & 1u) ? -1 : 1;
    Tw t;
    t.k1r = LDF(w + 8 * kk); t.k1i = LDF(w + 8 * kk + 4);
    t.k2r = fm_(m, (float)s, LDF(w + 4 * kk));
    t.k2i = LDF(w + 4 * (kk + s));
    const float d = fa_(m, t.k1r, t.k1r), e = fm_(m, t.k2i, -2.0f);
    t.k3i = fms_(m, t.k2i, d, t.k1i);
    t.k3r = fma_(m, t.k1i, e, t.k1r);
    return t;
}
ALW void bft(const int m, const Tw* t, uint8_t* p0, uint8_t* p1, uint8_t* p2, uint8_t* p3, uint32_t cnt)    /* with twiddles */
{
    for (uint32_t c = 0; c < cnt; c++) {
        const uint32_t o = 8 * c;
        const float a0r = LDF(p0 + o), a0i = LDF(p0 + o + 4), a1r = LDF(p1 + o), a1i = LDF(p1 + o + 4);
        const float a2r = LDF(p2 + o), a2i = LDF(p2 + o + 4), a3r = LDF(p3 + o), a3i = LDF(p3 + o + 4);
        const float x0r = fa_(m, a0r, a1r), x1r = fs_(m, a0r, a1r), x2r = fa_(m, a2r, a3r), x3r = fs_(m, a2r, a3r);
        const float x0i = fa_(m, a0i, a1i), x1i = fs_(m, a0i, a1i), x2i = fa_(m, a2i, a3i), x3i = fs_(m, a2i, a3i);
        STF(p0 + o, fa_(m, x0r, x2r));
        const float d02r = fs_(m, x0r, x2r), s13i = fs_(m, x1i, x3r), a13i = fa_(m, x1i, x3r);
        const float d02i = fs_(m, x0i, x2i);
        STF(p0 + o + 4, fa_(m, x0i, x2i));
        const float s13r = fa_(m, x1r, x3i), d13r = fs_(m, x1r, x3i);
        const float v1 = fm_(m, t->k2i, d02r), v2 = fm_(m, t->k1i, s13i), v3 = fm_(m, t->k3i, a13i), v4 = fm_(m, t->k2i, d02i);
        const float u1 = fm_(m, t->k1i, s13r), u2 = fm_(m, t->k3i, d13r);
        STF(p2 + o + 4, fms_(m, t->k2r, d02i, v1));
        STF(p1 + o, fma_(m, t->k1r, s13r, v2));
        STF(p3 + o, fma_(m, t->k3r, d13r, v3));
        STF(p2 + o, fma_(m, t->k2r, d02r, v4));
        STF(p1 + o + 4, fms_(m, t->k1r, s13i, u1));
        STF(p3 + o + 4, fms_(m, t->k3r, a13i, u2));
    }
}
ALW void bf2(const int m, uint8_t* p0, uint8_t* p1, uint32_t cnt)                                  /* the last radix-2 stage */
{
    for (uint32_t c = 0; c < cnt; c++) {
        const uint32_t o = 8 * c;
        const float a0r = LDF(p0 + o), a0i = LDF(p0 + o + 4), a1r = LDF(p1 + o), a1i = LDF(p1 + o + 4);
        STF(p0 + o, fa_(m, a0r, a1r)); STF(p0 + o + 4, fa_(m, a0i, a1i));
        STF(p1 + o, fs_(m, a0r, a1r)); STF(p1 + o + 4, fs_(m, a0i, a1i));
    }
}

/* ---- the same butterflies on four complex numbers at a time (the raw form only: the operations are the scalar
 * ones above, lane by lane; a lane is one complex number of a run, or, for the first stage of a row transform
 * whose runs are one complex number long, the same number of four rows) */
typedef float FV __attribute__((vector_size(16)));
typedef uint8_t B16 __attribute__((vector_size(16)));
typedef uint32_t U4 __attribute__((vector_size(16)));
typedef uint64_t U2 __attribute__((vector_size(16)));
typedef struct { FV r, i; } C4;
typedef struct { FV k1r, k1i, k2r, k2i, k3r, k3i; } TwV;
ALW FV vdup(float x) { return (FV){ x, x, x, x }; }
ALW FV vfl(FV x) { const U4 u = (U4)x; return (FV)(u & (U4)((u & 0x7F800000u) != 0)); }     /* a zero is +0 */
ALW U4 bs4(U4 u)                                              /* a byte swap in each word */
{
    const B16 b = (B16)u;
    return (U4)__builtin_shufflevector(b, b, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
}
ALW C4 c_split(U4 a, U4 b)       /* two quads of (re, im, re, im) as stored (big-endian) -> re and im lanes */
{
    const FV x = (FV)bs4(a), y = (FV)bs4(b);
    return (C4){ __builtin_shufflevector(x, y, 0, 2, 4, 6), __builtin_shufflevector(x, y, 1, 3, 5, 7) };
}
ALW C4 c_ld(const uint8_t* p)                                  /* four consecutive complex numbers */
{
    U4 a, b; __builtin_memcpy(&a, p, 16); __builtin_memcpy(&b, p + 16, 16);
    return c_split(a, b);
}
ALW void c_st(uint8_t* p, C4 c)
{
    const FV r = vfl(c.r), i = vfl(c.i);
    const U4 a = bs4((U4)__builtin_shufflevector(r, i, 0, 4, 1, 5)), b = bs4((U4)__builtin_shufflevector(r, i, 2, 6, 3, 7));
    __builtin_memcpy(p, &a, 16); __builtin_memcpy(p + 16, &b, 16);
}
ALW C4 c_ld4(const uint8_t* p0, const uint8_t* p1, const uint8_t* p2, const uint8_t* p3)   /* one complex number from each of four places */
{
    uint64_t t0, t1, t2, t3;
    __builtin_memcpy(&t0, p0, 8); __builtin_memcpy(&t1, p1, 8); __builtin_memcpy(&t2, p2, 8); __builtin_memcpy(&t3, p3, 8);
    return c_split((U4)(U2){ t0, t1 }, (U4)(U2){ t2, t3 });
}
ALW void c_st4(uint8_t* p0, uint8_t* p1, uint8_t* p2, uint8_t* p3, C4 c)
{
    const FV r = vfl(c.r), i = vfl(c.i);
    const U2 a = (U2)bs4((U4)__builtin_shufflevector(r, i, 0, 4, 1, 5)), b = (U2)bs4((U4)__builtin_shufflevector(r, i, 2, 6, 3, 7));
    const uint64_t t0 = a[0], t1 = a[1], t2 = b[0], t3 = b[1];
    __builtin_memcpy(p0, &t0, 8); __builtin_memcpy(p1, &t1, 8); __builtin_memcpy(p2, &t2, 8); __builtin_memcpy(p3, &t3, 8);
}
ALW TwV twv(const Tw* t) { return (TwV){ vdup(t->k1r), vdup(t->k1i), vdup(t->k2r), vdup(t->k2i), vdup(t->k3r), vdup(t->k3i) }; }
ALW FV vfma(FV a, FV b, FV c) { return __builtin_elementwise_fma(a, b, c); }            /* a * b + c */
ALW void v0(C4 a0, C4 a1, C4 a2, C4 a3, C4* o0, C4* o1, C4* o2, C4* o3)
{
    const FV x0r = a0.r + a1.r, x1r = a0.r - a1.r, x2r = a2.r + a3.r, x3r = a2.r - a3.r;
    const FV x0i = a0.i + a1.i, x1i = a0.i - a1.i, x2i = a2.i + a3.i, x3i = a2.i - a3.i;
    *o0 = (C4){ x0r + x2r, x0i + x2i }; *o2 = (C4){ x0r - x2r, x0i - x2i };
    *o1 = (C4){ x1r + x3i, x1i - x3r }; *o3 = (C4){ x1r - x3i, x1i + x3r };
}
ALW void vw(FV wn4r, C4 a0, C4 a1, C4 a2, C4 a3, C4* o0, C4* o1, C4* o2, C4* o3)
{
    const FV x0r = a0.r + a1.r, x1r = a0.r - a1.r, x2r = a2.r + a3.r, x3r = a2.r - a3.r;
    const FV x0i = a0.i + a1.i, x1i = a0.i - a1.i, x2i = a2.i + a3.i, x3i = a2.i - a3.i;
    *o0 = (C4){ x0r + x2r, x0i + x2i }; *o2 = (C4){ x0i - x2i, x2r - x0r };
    const FV pp = x1r + x3i, qq = x3i - x1r, rr = x1i - x3r, ss = x1i + x3r;
    const FV u0 = pp + rr, u1 = rr - pp, u2 = qq + ss, u3 = qq - ss;
    *o1 = (C4){ wn4r * u0, wn4r * u1 }; *o3 = (C4){ wn4r * u2, wn4r * u3 };
}
ALW void vt(const TwV* t, C4 a0, C4 a1, C4 a2, C4 a3, C4* o0, C4* o1, C4* o2, C4* o3)
{
    const FV x0r = a0.r + a1.r, x1r = a0.r - a1.r, x2r = a2.r + a3.r, x3r = a2.r - a3.r;
    const FV x0i = a0.i + a1.i, x1i = a0.i - a1.i, x2i = a2.i + a3.i, x3i = a2.i - a3.i;
    const FV d02r = x0r - x2r, s13i = x1i - x3r, a13i = x1i + x3r, d02i = x0i - x2i, s13r = x1r + x3i, d13r = x1r - x3i;
    const FV v1 = t->k2i * d02r, v2 = t->k1i * s13i, v3 = t->k3i * a13i, v4 = t->k2i * d02i, u1 = t->k1i * s13r, u2 = t->k3i * d13r;
    o0->r = x0r + x2r; o0->i = x0i + x2i;
    o2->i = vfma(t->k2r, d02i, -v1); o1->r = vfma(t->k1r, s13r, v2); o3->r = vfma(t->k3r, d13r, v3);
    o2->r = vfma(t->k2r, d02r, v4); o1->i = vfma(t->k1r, s13i, -u1); o3->i = vfma(t->k3r, a13i, -u2);
}
ALW void bf0v(uint8_t* p0, uint8_t* p1, uint8_t* p2, uint8_t* p3, uint32_t cnt)
{
    for (uint32_t c = 0; c < cnt; c += 4) {
        const uint32_t o = 8 * c;
        C4 o0, o1, o2, o3;
        v0(c_ld(p0 + o), c_ld(p1 + o), c_ld(p2 + o), c_ld(p3 + o), &o0, &o1, &o2, &o3);
        c_st(p0 + o, o0); c_st(p1 + o, o1); c_st(p2 + o, o2); c_st(p3 + o, o3);
    }
}
ALW void bfwv(float wn4r, uint8_t* p0, uint8_t* p1, uint8_t* p2, uint8_t* p3, uint32_t cnt)
{
    const FV w = vdup(wn4r);
    for (uint32_t c = 0; c < cnt; c += 4) {
        const uint32_t o = 8 * c;
        C4 o0, o1, o2, o3;
        vw(w, c_ld(p0 + o), c_ld(p1 + o), c_ld(p2 + o), c_ld(p3 + o), &o0, &o1, &o2, &o3);
        c_st(p0 + o, o0); c_st(p1 + o, o1); c_st(p2 + o, o2); c_st(p3 + o, o3);
    }
}
ALW void bftv(const Tw* tw, uint8_t* p0, uint8_t* p1, uint8_t* p2, uint8_t* p3, uint32_t cnt)
{
    const TwV t = twv(tw);
    for (uint32_t c = 0; c < cnt; c += 4) {
        const uint32_t o = 8 * c;
        C4 o0, o1, o2, o3;
        vt(&t, c_ld(p0 + o), c_ld(p1 + o), c_ld(p2 + o), c_ld(p3 + o), &o0, &o1, &o2, &o3);
        c_st(p0 + o, o0); c_st(p1 + o, o1); c_st(p2 + o, o2); c_st(p3 + o, o3);
    }
}
ALW void bf2v(uint8_t* p0, uint8_t* p1, uint32_t cnt)
{
    for (uint32_t c = 0; c < cnt; c += 4) {
        const uint32_t o = 8 * c;
        const C4 a0 = c_ld(p0 + o), a1 = c_ld(p1 + o);
        c_st(p0 + o, (C4){ a0.r + a1.r, a0.i + a1.i }); c_st(p1 + o, (C4){ a0.r - a1.r, a0.i - a1.i });
    }
}

/* ---------------------------------------------------------------------------------- 0x6EB8, 0x67A8 */
/* 0x6EB8 (r3 = rows n1, r4 = floats a row n2, r5 = the row table, r6 = w): the complex transform of the columns
 * (n2 / 2 columns of n1 complex numbers, a row holds (re, im) pairs), in place: radix-4 stages l = 1, 4, 16 ...
 * while 2l < n1, each a first block without twiddles, a block of 45 degrees and blocks kk = 2, 3 ... with the
 * twiddle factors of w, then the radix-2 stage when n1 is not a power of 4. vec: four columns at a time (n2 >= 8). */
ALW void cftbcol_run(const int m, const int vec, uint8_t* ls, const Rows* R, uint32_t n1, uint32_t n2, const uint8_t* w)
{
    const uint32_t cnt = n2 >> 1;
#define RW(i) (ls + R->base + (uint64_t)(i) * R->stride)
    uint32_t l = 1;
    while ((l << 1) < n1) {
        const uint32_t mm = l << 2;
        for (uint32_t j = 0; j < l; j++) {
            if (vec) bf0v(RW(j), RW(j + l), RW(j + 2 * l), RW(j + 3 * l), cnt); else bf0(m, RW(j), RW(j + l), RW(j + 2 * l), RW(j + 3 * l), cnt);
        }
        if (mm < n1) {
            const float wn4r = LDF(w + 8);
            for (uint32_t j = mm; j < mm + l; j++) {
                if (vec) bfwv(wn4r, RW(j), RW(j + l), RW(j + 2 * l), RW(j + 3 * l), cnt); else bfw(m, wn4r, RW(j), RW(j + l), RW(j + 2 * l), RW(j + 3 * l), cnt);
            }
            for (uint32_t kk = 2; (kk + 1) * mm <= n1; kk++) {
                const Tw t = twid(m, w, kk);
                for (uint32_t o = 0; o < l; o++) {
                    const uint32_t j = kk * mm + o;
                    if (vec) bftv(&t, RW(j), RW(j + l), RW(j + 2 * l), RW(j + 3 * l), cnt); else bft(m, &t, RW(j), RW(j + l), RW(j + 2 * l), RW(j + 3 * l), cnt);
                }
            }
        }
        l = mm;
    }
    if (l < n1) for (uint32_t j = 0; j < l; j++) { if (vec) bf2v(RW(j), RW(j + l), cnt); else bf2(m, RW(j), RW(j + l), cnt); }
#undef RW
}
/* One radix-4 stage of a row (P the row, l counted in floats, the runs l / 2 complex numbers): the first block, the
 * 45 degree block and the twiddle blocks kk = 2 .. while (kk + 1) * 4l <= n2; returns the next l. vec: four
 * complex numbers at a time (l >= 8). */
ALW uint32_t row_stage(const int m, const int vec, uint8_t* P, uint32_t n2, const uint8_t* w, uint32_t l)
{
    const uint32_t mm = 4 * l, cnt = l >> 1;
#define AT(f) (P + 4 * (f))
    if (vec) bf0v(AT(0), AT(l), AT(2 * l), AT(3 * l), cnt); else bf0(m, AT(0), AT(l), AT(2 * l), AT(3 * l), cnt);
    if (n2 > mm) {
        const float wn4r = LDF(w + 8);
        if (vec) bfwv(wn4r, AT(mm), AT(mm + l), AT(mm + 2 * l), AT(mm + 3 * l), cnt); else bfw(m, wn4r, AT(mm), AT(mm + l), AT(mm + 2 * l), AT(mm + 3 * l), cnt);
        for (uint32_t kk = 2; (kk + 1) * mm <= n2; kk++) {
            const Tw t = twid(m, w, kk);
            if (vec) bftv(&t, AT(kk * mm), AT(kk * mm + l), AT(kk * mm + 2 * l), AT(kk * mm + 3 * l), cnt);
            else bft(m, &t, AT(kk * mm), AT(kk * mm + l), AT(kk * mm + 2 * l), AT(kk * mm + 3 * l), cnt);
        }
    }
#undef AT
    return mm;
}
/* The first stage (l = 2, runs of one complex number: group kk is the four numbers at floats 8kk + 0, 2, 4, 6) for
 * four rows at once, a lane a row. */
ALW void row_stage1_x4(uint8_t* P0, uint8_t* P1, uint8_t* P2, uint8_t* P3, uint32_t n2, const uint8_t* w)
{
    const FV wn4r = vdup(n2 > 8 ? LDF(w + 8) : 0.0f);
    for (uint32_t kk = 0; kk < (n2 >> 3); kk++) {
        const uint32_t o = 32 * kk;
#define LD4(f) c_ld4(P0 + o + 8 * (f), P1 + o + 8 * (f), P2 + o + 8 * (f), P3 + o + 8 * (f))
#define ST4(f, v) c_st4(P0 + o + 8 * (f), P1 + o + 8 * (f), P2 + o + 8 * (f), P3 + o + 8 * (f), (v))
        C4 o0, o1, o2, o3;
        const C4 a0 = LD4(0), a1 = LD4(1), a2 = LD4(2), a3 = LD4(3);
        if (kk == 0) v0(a0, a1, a2, a3, &o0, &o1, &o2, &o3);
        else if (kk == 1) vw(wn4r, a0, a1, a2, a3, &o0, &o1, &o2, &o3);
        else { const Tw t = twid(M_RAW, w, kk); const TwV tv = twv(&t); vt(&tv, a0, a1, a2, a3, &o0, &o1, &o2, &o3); }
        ST4(0, o0); ST4(1, o1); ST4(2, o2); ST4(3, o3);
#undef LD4
#undef ST4
    }
}
/* 0x67A8 (r3 = rows n1, r4 = floats a row n2, r5 = the row table, r6 = w): the same transform along each row
 * (n2 / 2 complex numbers, l counted in floats: l = 2, 8, 32 ...). */
ALW void cftbrow_run(const int m, const int vec, uint8_t* ls, const Rows* R, uint32_t n1, uint32_t n2, const uint8_t* w)
{
#define ROW(i) (ls + R->base + (uint64_t)(i) * R->stride)
    uint32_t i = 0;
    if (vec) {                                       /* the first stage: four rows at a time, then the others */
        for (; i + 4 <= n1; i += 4) row_stage1_x4(ROW(i), ROW(i + 1), ROW(i + 2), ROW(i + 3), n2, w);
        for (; i < n1; i++) (void)row_stage(m, 0, ROW(i), n2, w, 2);
    }
    for (i = 0; i < n1; i++) {
        uint8_t* const P = ROW(i);
        uint32_t l = vec ? 8 : 2;
        while (n2 > 2 * l) l = row_stage(m, vec, P, n2, w, l);
        if (n2 > l) { if (vec) bf2v(P, P + 4 * l, l >> 1); else bf2(m, P, P + 4 * l, l >> 1); }
    }
#undef ROW
}

/* The hooks: each runs the pass on the real context and returns 0 (declined, nothing done), 1 (raw form) or 2
 * (exact form). The float registers and the stack are not touched: the lifted passes use r2..r79 only, and a
 * caller reads none of them after a call. */
static int hk_cftbcol(spu_context* ctx, int fast)
{
    uint8_t* const ls = SPU_LS_BASE(ctx);
    const uint32_t n1 = ctx->gpr[3]._u32[0], n2 = ctx->gpr[4]._u32[0], tab = ctx->gpr[5]._u32[0], w = ctx->gpr[6]._u32[0], sp = ctx->gpr[1]._u32[0];
    Rows R;
    if (g_spu_jobrec_mode || !pw2(n1, 4, 512) || !pw2(n2, 4, 512) || !sp_ok(sp) || !rows_geom(ls, tab, n1, 4 * n2, &R)) return 0;
    const uint64_t re = rows_end(&R, n1, 4 * n2), wl = 4ull * (n1 / 2 + 8);
    if ((w & 3u) || (uint64_t)w + wl > SPU_LS_SIZE || !clear_of(R.base, re, sp) || ovl64(R.base, re, w, w + wl) ||
        ovl64(R.base, re, tab, (uint64_t)tab + 4ull * n1)) return 0;
    const int raw = fast && fp_rzfz() && rows_exp(ls, &R, n1, n2) <= E_DATA && words_exp(ls + w, n1 / 2 + 8) <= E_TW;
    if (raw) { if (n2 >= 8) cftbcol_run(M_RAW, 1, ls, &R, n1, n2, ls + w); else cftbcol_run(M_RAW, 0, ls, &R, n1, n2, ls + w); }
    else cftbcol_run(M_EXACT, 0, ls, &R, n1, n2, ls + w);
    J211_COV(2 + !raw);
    return 1 + !raw;
}
static int hk_cftbrow(spu_context* ctx, int fast)
{
    uint8_t* const ls = SPU_LS_BASE(ctx);
    const uint32_t n1 = ctx->gpr[3]._u32[0], n2 = ctx->gpr[4]._u32[0], tab = ctx->gpr[5]._u32[0], w = ctx->gpr[6]._u32[0], sp = ctx->gpr[1]._u32[0];
    Rows R;
    if (g_spu_jobrec_mode || n1 < 1 || n1 > 1024 || !pw2(n2, 4, 512) || !sp_ok(sp) || !rows_geom(ls, tab, n1, 4 * n2, &R)) return 0;
    const uint64_t re = rows_end(&R, n1, 4 * n2), wl = 4ull * (n2 / 4 + 8);
    if ((w & 3u) || (uint64_t)w + wl > SPU_LS_SIZE || !clear_of(R.base, re, sp) || ovl64(R.base, re, w, w + wl) ||
        ovl64(R.base, re, tab, (uint64_t)tab + 4ull * n1)) return 0;
    const int raw = fast && fp_rzfz() && rows_exp(ls, &R, n1, n2) <= E_DATA && words_exp(ls + w, n2 / 4 + 8) <= E_TW;
    if (raw) { if (n2 >= 8) cftbrow_run(M_RAW, 1, ls, &R, n1, n2, ls + w); else cftbrow_run(M_RAW, 0, ls, &R, n1, n2, ls + w); }
    else cftbrow_run(M_EXACT, 0, ls, &R, n1, n2, ls + w);
    J211_COV(4 + !raw);
    return 1 + !raw;
}

/* ---------------------------------------------------------------------------------- 0x7860 */
/* 0x7860 (r3 = rows n1, r4 = floats a row n2, r5 = the row table, r6 = nc, r7 = c): the real-transform step of
 * each row, pairs a[m-2-2t], a[m-1-2t] and a[m+2+2t], a[m+3+2t] (m = n2 / 2) with the factors of the table c
 * (c[kk], c[nc-kk], kk = ks * (t + 1), ks = 4 * nc / n2). */
ALW void rftb_row(const int m, uint8_t* P, uint32_t n2, uint32_t nc, uint32_t ks, const uint8_t* c)
{
    const uint32_t mh = n2 >> 1, iters = (mh - 2) >> 1;
    for (uint32_t t = 0; t < iters; t++) {
        const uint32_t kk = ks * (t + 1), pj = mh - 2 - 2 * t, pk = (n2 - mh) + 2 + 2 * t;
        uint8_t *const aj = P + 4 * pj, *const ak = P + 4 * pk;
        const float xj = LDF(aj), xj1 = LDF(aj + 4), yk = LDF(ak), yk1 = LDF(ak + 4);
        const float wkr = fs_(m, 0.5f, LDF(c + 4 * kk)), wki = LDF(c + 4 * (nc - kk));
        const float xi = fa_(m, xj1, yk1), xr = fs_(m, xj, yk);
        const float p1 = fm_(m, wki, xi), p2 = fm_(m, wki, xr);
        const float yr = fma_(m, wkr, xr, p1), yi = fms_(m, wkr, xi, p2);
        STF(aj, fs_(m, xj, yr)); STF(aj + 4, fs_(m, xj1, yi));
        STF(ak, fa_(m, yk, yr)); STF(ak + 4, fs_(m, yk1, yi));
    }
}
/* the same four rows at a time, a lane a row (the raw form) */
ALW void rftb_x4(uint8_t* P0, uint8_t* P1, uint8_t* P2, uint8_t* P3, uint32_t n2, uint32_t nc, uint32_t ks, const uint8_t* c)
{
    const uint32_t mh = n2 >> 1, iters = (mh - 2) >> 1;
    for (uint32_t t = 0; t < iters; t++) {
        const uint32_t kk = ks * (t + 1), pj = 4 * (mh - 2 - 2 * t), pk = 4 * ((n2 - mh) + 2 + 2 * t);
        const C4 xj = c_ld4(P0 + pj, P1 + pj, P2 + pj, P3 + pj), yk = c_ld4(P0 + pk, P1 + pk, P2 + pk, P3 + pk);     /* (re, im) = two floats */
        const FV wkr = vdup(0.5f - LDF(c + 4 * kk)), wki = vdup(LDF(c + 4 * (nc - kk)));
        const FV xi = xj.i + yk.i, xr = xj.r - yk.r, p1 = wki * xi, p2 = wki * xr;
        const FV yr = vfma(wkr, xr, p1), yi = vfma(wkr, xi, -p2);
        c_st4(P0 + pj, P1 + pj, P2 + pj, P3 + pj, (C4){ xj.r - yr, xj.i - yi });
        c_st4(P0 + pk, P1 + pk, P2 + pk, P3 + pk, (C4){ yk.r + yr, yk.i - yi });
    }
}
static int hk_rftb(spu_context* ctx, int fast)
{
    uint8_t* const ls = SPU_LS_BASE(ctx);
    const uint32_t n1 = ctx->gpr[3]._u32[0], n2 = ctx->gpr[4]._u32[0], tab = ctx->gpr[5]._u32[0], nc = ctx->gpr[6]._u32[0], c = ctx->gpr[7]._u32[0],
                   sp = ctx->gpr[1]._u32[0];
    Rows R;
    if (g_spu_jobrec_mode || n1 < 1 || n1 > 1024 || !pw2(n2, 8, 512) || nc > 4096u || !sp_ok(sp) || !rows_geom(ls, tab, n1, 4 * n2, &R)) return 0;
    const uint64_t re = rows_end(&R, n1, 4 * n2), cl = 4ull * (nc + 1);
    if ((c & 3u) || (uint64_t)c + cl > SPU_LS_SIZE || !clear_of(R.base, re, sp) || ovl64(R.base, re, c, c + cl) ||
        ovl64(R.base, re, tab, (uint64_t)tab + 4ull * n1)) return 0;
    const uint32_t ks = (4 * nc) / n2;
    const int raw = fast && fp_rzfz() && rows_exp(ls, &R, n1, n2) <= E_DATA && words_exp(ls + c, nc + 1) <= E_TW;
#define ROW(i) (ls + R.base + (uint64_t)(i) * R.stride)
    uint32_t i = 0;
    if (raw) for (; i + 4 <= n1; i += 4) rftb_x4(ROW(i), ROW(i + 1), ROW(i + 2), ROW(i + 3), n2, nc, ks, ls + c);
    for (; i < n1; i++) { if (raw) rftb_row(M_RAW, ROW(i), n2, nc, ks, ls + c); else rftb_row(M_EXACT, ROW(i), n2, nc, ks, ls + c); }
#undef ROW
    J211_COV(6 + !raw);
    return 1 + !raw;
}

/* ---------------------------------------------------------------------------------- 0x54E8, 0x5118 */
static void memswap(uint8_t* a, uint8_t* b, uint32_t n)        /* n a multiple of 8 */
{
    for (uint32_t i = 0; i < n; i += 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8); memcpy(&y, b + i, 8); memcpy(a + i, &y, 8); memcpy(b + i, &x, 8);
    }
}
/* 0x54E8 (r3 = rows n1, r4 = floats a row n2, r5 = ip, r6 = the row table): the bit reversal of the rows: it
 * builds the index table ip[0..m-1] (ip[0] = 0; while l > 2m: l >>= 1, ip[m+j] = ip[j] + l, m <<= 1) and swaps the
 * rows a[ip[i]+j] and a[i+ip[j]] (j < i < m), and, when l == 2m, also a[ip[i]+j+m] and a[m+i+ip[j]]. */
static int hk_bitrv_col(spu_context* ctx, int fast)
{
    (void)fast;
    uint8_t* const ls = SPU_LS_BASE(ctx);
    const uint32_t n1 = ctx->gpr[3]._u32[0], n2 = ctx->gpr[4]._u32[0], ip = ctx->gpr[5]._u32[0], tab = ctx->gpr[6]._u32[0], sp = ctx->gpr[1]._u32[0];
    Rows R;
    if (g_spu_jobrec_mode || !pw2(n1, 4, 512) || !pw2(n2, 4, 512) || (ip & 3u) || !sp_ok(sp) || !rows_geom(ls, tab, n1, 4 * n2, &R)) return 0;
    uint32_t ipv[64], m = 1, l = n1;
    ipv[0] = 0;
    while (l > 2 * m) { l >>= 1; for (uint32_t j = 0; j < m; j++) ipv[m + j] = ipv[j] + l; m <<= 1; }
    const int two = !(2 * m > l);                                    /* 2m == l: the second swap too */
    for (uint32_t i = 1; i < m; i++)
        for (uint32_t j = 0; j < i; j++) {
            if (ipv[i] + j + (two ? m : 0) >= n1 || i + ipv[j] + (two ? m : 0) >= n1) return 0;
        }
    const uint64_t re = rows_end(&R, n1, 4 * n2), ie = (uint64_t)ip + 4ull * m;
    if (ie > SPU_LS_SIZE || !clear_of(R.base, re, sp) || !clear_of(ip, ie, sp) || ovl64(R.base, re, ip, ie) ||
        ovl64(R.base, re, tab, (uint64_t)tab + 4ull * n1) || ovl64(ip, ie, tab, (uint64_t)tab + 4ull * n1)) return 0;
    for (uint32_t t = 0; t < m; t++) W32(ls + ip + 4 * t, ipv[t]);
#define RW(i) (ls + R.base + (uint64_t)(i) * R.stride)
    for (uint32_t i = 1; i < m; i++)
        for (uint32_t j = 0; j < i; j++) {
            if (ipv[i] + j != i + ipv[j]) memswap(RW(ipv[i] + j), RW(i + ipv[j]), 4 * n2);
            if (two && ipv[i] + j + m != m + i + ipv[j]) memswap(RW(ipv[i] + j + m), RW(m + i + ipv[j]), 4 * n2);
        }
#undef RW
    return 1;
}
/* 0x5118 (r3 = rows n1, r4 = floats a row n2, r5 = ip, r6 = the row table): the bit reversal inside each row, of
 * the pairs (re, im): the table ip[0..m-1] (ip[0] = 0; l = n2 / 2; { ip[m+j] = ip[j] + l; m <<= 1 } while l > 4m
 * (l >>= 1)), and the pairs j1 = ip[k] + 2j and k1 = 2k + ip[j] swapped (j < k < m), and, when l == 4m, the pairs
 * j1 + 2m and k1 + 2m too. */
static int hk_bitrv_row(spu_context* ctx, int fast)
{
    (void)fast;
    uint8_t* const ls = SPU_LS_BASE(ctx);
    const uint32_t n1 = ctx->gpr[3]._u32[0], n2 = ctx->gpr[4]._u32[0], ip = ctx->gpr[5]._u32[0], tab = ctx->gpr[6]._u32[0], sp = ctx->gpr[1]._u32[0];
    Rows R;
    if (g_spu_jobrec_mode || n1 < 1 || n1 > 1024 || !pw2(n2, 8, 512) || (ip & 3u) || !sp_ok(sp) || !rows_geom(ls, tab, n1, 4 * n2, &R)) return 0;
    uint32_t ipv[64], m = 1, l = n2 >> 1;
    ipv[0] = 0;
    for (;;) {
        for (uint32_t j = 0; j < m; j++) ipv[m + j] = ipv[j] + l;
        m <<= 1;
        if (!(l > 4 * m)) break;
        l >>= 1;
    }
    const int two = !(4 * m > l);                                    /* 4m == l: the second pair of swaps too */
    for (uint32_t k = 1; k < m; k++)
        for (uint32_t j = 0; j < k; j++) {
            const uint32_t j1 = ipv[k] + 2 * j, k1 = 2 * k + ipv[j], x = two ? 2 * m : 0;
            if (((j1 | k1) & 1u) || j1 + x + 1 >= n2 || k1 + x + 1 >= n2) return 0;
        }
    const uint64_t re = rows_end(&R, n1, 4 * n2), ie = (uint64_t)ip + 4ull * m;
    if (ie > SPU_LS_SIZE || !clear_of(R.base, re, sp) || !clear_of(ip, ie, sp) || ovl64(R.base, re, ip, ie) ||
        ovl64(R.base, re, tab, (uint64_t)tab + 4ull * n1) || ovl64(ip, ie, tab, (uint64_t)tab + 4ull * n1)) return 0;
    for (uint32_t t = 0; t < m; t++) W32(ls + ip + 4 * t, ipv[t]);
    for (uint32_t i = 0; i < n1; i++) {
        uint8_t* const P = ls + R.base + (uint64_t)i * R.stride;
        for (uint32_t k = 1; k < m; k++)
            for (uint32_t j = 0; j < k; j++) {
                const uint32_t j1 = ipv[k] + 2 * j, k1 = 2 * k + ipv[j];
                if (j1 != k1) memswap(P + 4 * j1, P + 4 * k1, 8);
                if (two && j1 + 2 * m != k1 + 2 * m) memswap(P + 4 * (j1 + 2 * m), P + 4 * (k1 + 2 * m), 8);
            }
    }
    return 1;
}

/* ---------------------------------------------------------------------------------- 0x40E8 */
static inline u128 LQ16(const uint8_t* ls, uint32_t a)
{
    const uint8_t* q = ls + (a & (SPU_LS_MASK & ~0xFu));
    return V4(R32(q), R32(q + 4), R32(q + 8), R32(q + 12));
}
static inline void SQ16(uint8_t* ls, uint32_t a, u128 v)
{
    uint8_t* q = ls + (a & (SPU_LS_MASK & ~0xFu));
    W32(q, v._u32[0]); W32(q + 4, v._u32[1]); W32(q + 8, v._u32[2]); W32(q + 12, v._u32[3]);
}
/* The block of 0x40E8 runs on the lifted code's own forms: M_SPEC the speculative operations (the fast form of each
 * operation and one verdict a block, right when no operation reached FLT_MAX or took an exponent-255 operand),
 * M_EXACT the lean ones, redone when the verdict is no. */
#define FA_(x, y)      (m == M_SPEC ? spu_fa_spec((x), (y), c) : spu_fa_lean((x), (y)))
#define FS_(x, y)      (m == M_SPEC ? spu_fs_spec((x), (y), c) : spu_fs_lean((x), (y)))
#define FM_(x, y)      (m == M_SPEC ? spu_fm_spec((x), (y), c) : spu_fm_lean((x), (y)))
#define FMA_(x, y, z)  (m == M_SPEC ? spu_fma_spec((x), (y), (z), c) : spu_fma_lean((x), (y), (z)))
#define FNMS_(x, y, z) (m == M_SPEC ? spu_fnms_spec((x), (y), (z), c) : spu_fnms_lean((x), (y), (z)))
#define CFLTS_(x, i)   (m == M_SPEC ? spu_cflts_spec((x), (i), c) : spu_cflts((x), (i)))
typedef struct {
    u128 r88, r89, r90, r91, r92, r93, r99, r101, r103, r104, r109, kq, k12, k13, k14, k15, k16, k21, k26, c15, c17;   /* the job's constants */
    u128 w25, w36;                                                                                                     /* |2 pi| and 1 / (2 pi) from the estimate */
    u128 r86, r96, r97, r102;                                                                                          /* the row's and the call's */
} K40;
/* 0x4030: an angle wrapped into [-pi, pi) (r3 in, r3 out; c15 = pi, c17 = 2 pi as the module holds them) */
ALW u128 wrap4030(const int m, spu_spec_acc* c, const K40* k, u128 r3)
{
    const u128 r15 = k->c15, r17 = k->c17;
    const u128 r40 = spu_ilhu(0x3F80), r13 = k->r88, r37 = spu_ori(r40, 1), r38 = spu_ilhu(0x7F80), r16 = spu_fsmbi(0x0), r33 = spu_ilhu(0x4980);
    const u128 r25 = k->w25, r11 = FA_(r15, r3);
    const u128 r36 = k->w36, r19 = spu_andc(r11, r13), r31 = spu_fcmgt_lean(r38, r11), r26 = FA_(r19, r19);
    const u128 r34 = FNMS_(r17, r36, r37), r35 = FM_(r11, r36), r7 = FMA_(r34, r35, r35);
    const u128 r32 = spu_andc(r7, r13), r30 = spu_fcmgt_lean(r33, r7), r29 = CFLTS_(r32, 173), r8 = spu_and(r30, r31);
    const u128 r21 = spu_csflt(r29, 155), r28 = spu_ai(r29, 1), r27 = spu_ai(r29, -1), r18 = spu_csflt(r28, 155), r23 = spu_csflt(r27, 155);
    const u128 r24 = FNMS_(r21, r25, r26), r20 = FNMS_(r18, r25, r26);
    const u128 r22 = spu_fcgt_lean(r19, r24), r6 = spu_selb(r21, r23, r22), r14 = spu_fcgt_lean(r19, r20), r10 = spu_selb(r18, r6, r14);
    const u128 r5 = spu_selb(r10, r7, r13), r4 = FNMS_(r5, r17, r11), r2 = spu_selb(r16, r4, r8);
    const u128 r9 = FA_(r17, r2), r12 = spu_fcgt_lean(r16, r2), t2 = spu_selb(r2, r9, r12);
    return FS_(t2, r15);
}
typedef struct { u128 th, q1a, q1b, q2a, q2b; } B40;
/* four columns: x0f the first column as a float in every lane, aq / bq the four floats of A and of B */
ALW B40 blk40(const int m, spu_spec_acc* c, const K40* k, u128 x0f, u128 aq, u128 bq)
{
    const u128 r81 = FM_(FA_(x0f, k->r109), k->kq);
    const u128 r42 = FMA_(r81, r81, k->r96);
    const u128 r40m = spu_fcgt_lean(r42, k->r101);
    const u128 r44 = spu_fi(r42, spu_frsqest(r42)), r46 = FM_(r44, r44), r45m = FM_(r44, k->r92);
    const u128 r41 = FMA_(FNMS_(r42, r46, k->r93), r45m, r44), r39 = FM_(r41, r42);
    const u128 r33 = spu_selb(k->r89, r39, r40m);
    const u128 r31m = spu_fcgt_lean(r33, k->r101);
    const u128 r35 = spu_fi(r33, spu_frsqest(r33)), r37m = FM_(r35, r35), r36m = FM_(r35, k->r92);
    const u128 r32 = FMA_(FNMS_(r33, r37m, k->r93), r36m, r35), r30m = FM_(r32, r33);
    const u128 r29m = spu_selb(k->r89, r30m, r31m);
    u128 th = FMA_(FM_(r29m, k->r97), k->r102, aq);
    th = wrap4030(m, c, k, th);
    const u128 r24 = FM_(th, k->k26), r25 = spu_selb(k->r92, r24, k->r88), r23 = FA_(r24, r25);
    const u128 r22 = CFLTS_(r23, 173), r17 = spu_csflt(r22, 155), r19 = spu_andi(r22, 3), r20 = spu_ai(r19, 1);
    const u128 r76 = spu_ceqi(spu_andi(r19, 1), 0), r78 = spu_ceqi(spu_andi(r20, 1), 0), r73 = spu_ceqi(spu_andi(r20, 2), 0), r70 = spu_ceqi(spu_andi(r19, 2), 0);
    const u128 r18 = FNMS_(r17, k->k21, th), r5 = FNMS_(r17, k->k14, r18);
    const u128 r2 = FM_(r5, r5);
    const u128 r11 = FMA_(r2, k->r103, k->k16), r7m = FMA_(r2, k->r104, k->k15), r3m = FM_(r2, r5);
    const u128 r6 = FMA_(r11, r2, k->k12), r79 = FMA_(r7m, r2, k->k13);
    const u128 r75 = FMA_(r6, r2, k->r93), r77 = FMA_(r79, r3m, r5);
    const u128 r74 = spu_selb(r75, r77, r78), r71 = spu_selb(r75, r77, r76);
    const u128 r72 = spu_xor(k->r88, r74), r69 = spu_xor(k->r88, r71);
    const u128 r68 = spu_selb(r72, r74, r73), r67 = spu_selb(r69, r71, r70);
    const u128 r66 = FM_(bq, r68), r64 = FM_(bq, r67), r65 = spu_xor(k->r88, r66);
    const u128 r62 = FM_(r64, k->r86), r63 = FM_(k->r86, r65), r61 = FM_(r81, r65), r60 = FM_(r64, r81);
    return (B40){ th, spu_shufb(r62, r63, k->r90), spu_shufb(r62, r63, k->r91), spu_shufb(r60, r61, k->r90), spu_shufb(r60, r61, k->r91) };
}
/* The raw form of the block: plain four-lane floats under FPCR round-toward-zero + flush-to-zero for the float
 * operations, the helpers for everything else (the estimates, the conversions, the compares, the shuffles), and a
 * zero made +0 where a sign of zero could be read: before an estimate, before the copysign of the half in the
 * octant step, and in the results stored. Taken when the job's constants are the ones it ships with and the
 * data and the time terms are small enough that no operation reaches FLT_MAX (hk_spectrum). */
ALW FV tf(u128 x) { FV v; __builtin_memcpy(&v, x._u32, 16); return v; }
ALW u128 tu(FV v) { u128 r; __builtin_memcpy(r._u32, &v, 16); return r; }
ALW FV vsel(FV a, FV b, FV mask) { return (FV)(((U4)a & ~(U4)mask) | ((U4)b & (U4)mask)); }      /* selb: mask ? b : a */
ALW B40 blk40r(const K40* k, FV x0f, FV aq, FV bq)
{
    const FV one = tf(k->r93), half = tf(k->r92), c15 = tf(k->c15), c17 = tf(k->c17), sgn = tf(k->r88), zero = tf(k->r89);
    const FV r81 = (x0f + tf(k->r109)) * tf(k->kq);
    const FV r42 = vfl(vfma(r81, r81, tf(k->r96)));
    const u128 r42u = tu(r42), r40m = spu_fcgt_lean(r42u, k->r101);
    const FV r44 = tf(spu_fi(r42u, spu_frsqest(r42u))), r46 = r44 * r44, r45 = r44 * half;
    const FV r41 = vfma(vfma(-r42, r46, one), r45, r44), r39 = r41 * r42;
    const FV r33 = vsel(zero, r39, tf(r40m));
    const u128 r33u = tu(r33), r31m = spu_fcgt_lean(r33u, k->r101);
    const FV r35 = tf(spu_fi(r33u, spu_frsqest(r33u))), r37 = r35 * r35, r36 = r35 * half;
    const FV r32 = vfma(vfma(-r33, r37, one), r36, r35), r30 = r32 * r33;
    const FV r29 = vsel(zero, r30, tf(r31m));
    const FV th0 = vfma(r29 * tf(k->r97), tf(k->r102), aq);
    /* 0x4030 */
    const FV w25 = tf(k->w25), w36 = tf(k->w36), t37 = (FV)((U4)one | 1u), big = tf(spu_ilhu(0x7F80)), lim = tf(spu_ilhu(0x4980));
    const FV q11 = c15 + th0, q19 = (FV)((U4)q11 & 0x7FFFFFFFu);
    const u128 q31 = spu_fcmgt_lean(tu(big), tu(q11));
    const FV q26 = q19 + q19;
    const FV q34 = vfma(-c17, w36, t37), q35 = q11 * w36, q7 = vfma(q34, q35, q35);
    const FV q32 = (FV)((U4)q7 & 0x7FFFFFFFu);
    const u128 q30 = spu_fcmgt_lean(tu(lim), tu(q7)), q29 = spu_cflts(tu(q32), 173), q8 = spu_and(q30, q31);
    const u128 q21 = spu_csflt(q29, 155), q28 = spu_ai(q29, 1), q27 = spu_ai(q29, -1), q18 = spu_csflt(q28, 155), q23 = spu_csflt(q27, 155);
    const FV q24 = vfma(-tf(q21), w25, q26), q20 = vfma(-tf(q18), w25, q26);
    const u128 q22 = spu_fcgt_lean(tu(q19), tu(q24)), q6 = spu_selb(q21, q23, q22), q14 = spu_fcgt_lean(tu(q19), tu(q20)), q10 = spu_selb(q18, q6, q14);
    const FV q5 = tf(spu_selb(q10, tu(q7), k->r88)), q4 = vfma(-q5, c17, q11), q2 = vsel(zero, q4, tf(q8)), q9 = c17 + q2;
    const u128 q12 = spu_fcgt_lean(k->r89, tu(q2));
    const FV th = vfl(vsel(q2, q9, tf(q12)) - c15);
    /* the sine and cosine */
    const FV r24 = vfl(th * tf(k->k26)), r25 = vsel(half, r24, sgn), r23 = r24 + r25;
    const u128 r22 = spu_cflts(tu(r23), 173), r17 = spu_csflt(r22, 155), r19 = spu_andi(r22, 3), r20 = spu_ai(r19, 1);
    const FV m76 = tf(spu_ceqi(spu_andi(r19, 1), 0)), m78 = tf(spu_ceqi(spu_andi(r20, 1), 0)), m73 = tf(spu_ceqi(spu_andi(r20, 2), 0)), m70 = tf(spu_ceqi(spu_andi(r19, 2), 0));
    const FV r18 = vfma(-tf(r17), tf(k->k21), th), r5 = vfma(-tf(r17), tf(k->k14), r18);
    const FV r2 = r5 * r5;
    const FV r11 = vfma(r2, tf(k->r103), tf(k->k16)), r7 = vfma(r2, tf(k->r104), tf(k->k15)), r3 = r2 * r5;
    const FV r6 = vfma(r11, r2, tf(k->k12)), r79 = vfma(r7, r2, tf(k->k13));
    const FV r75 = vfma(r6, r2, one), r77 = vfma(r79, r3, r5);
    const FV r74 = vsel(r75, r77, m78), r71 = vsel(r75, r77, m76);
    const FV r72 = (FV)((U4)r74 ^ (U4)sgn), r69 = (FV)((U4)r71 ^ (U4)sgn);
    const FV r68 = vsel(r72, r74, m73), r67 = vsel(r69, r71, m70);
    const FV r66 = bq * r68, r64 = bq * r67, r65 = (FV)((U4)r66 ^ (U4)sgn);
    const FV r62 = vfl(r64 * tf(k->r86)), r63 = vfl(tf(k->r86) * r65), r61 = vfl(r81 * r65), r60 = vfl(r64 * r81);
    return (B40){ tu(th), spu_shufb(tu(r62), tu(r63), k->r90), spu_shufb(tu(r62), tu(r63), k->r91), spu_shufb(tu(r60), tu(r61), k->r90), spu_shufb(tu(r60), tu(r61), k->r91) };
}
/* 0x40E8 (r3 = A, r4 = F): the spectrum. For each row y of 64 and four columns x0..x0+3 at a time (x0 = 0, 4 .. 28):
 * the wave vector k = (x * c, ky * c) with c = the constant at LS 0x8970 (2 pi / 1024) and ky the signed row
 * index, |k| and its square root (a reciprocal square root estimate with one Newton step each), the phase
 * A[y][x] + sqrt|k| * (the module's per-lane weights) * (50 / t) * (-1)^y * t', t and t' being the two floats at A + 0x4004
 * and A + 0x4008, wrapped by 0x4030 and written back to A; then its sine and cosine (the job's octant polynomials)
 * times the second array of A (B), and the four complex values of each of the two outputs (rows of the
 * work buffer's two arrays, through the two row tables). All in the lifted code's operations on the same four
 * lanes, in its order. */
static int hk_spectrum(spu_context* ctx, int fast)
{
    uint8_t* const ls = SPU_LS_BASE(ctx);
    const uint32_t A = ctx->gpr[3]._u32[0], F = ctx->gpr[4]._u32[0], sp = ctx->gpr[1]._u32[0];
    Rows R1, R2;
    if (g_spu_jobrec_mode || ((A | F) & 15u) || !sp_ok(sp) || (uint64_t)A + 0x4010u > SPU_LS_SIZE || (uint64_t)F + 0x8200u > SPU_LS_SIZE ||
        !rows_geom(ls, F + 0x8000u, 64, 256, &R1) || !rows_geom(ls, F + 0x8100u, 64, 256, &R2) || ((R1.base | R1.stride | R2.base | R2.stride) & 15u)) return 0;
    const uint64_t e1 = rows_end(&R1, 64, 256), e2 = rows_end(&R2, 64, 256), wa1 = (uint64_t)A + 0x2000u, ar1 = (uint64_t)A + 0x4010u;
    const uint64_t t0 = (uint64_t)F + 0x8000u, t1 = (uint64_t)F + 0x8200u;
    /* written: the first half of A and the two sets of rows; read: the rest of A, the tables and the module's constants,
     * which the writes (clear of the module) do not reach; the first rows' stores and the loads of the later ones
     * would meet if any of these overlapped, and the tables are read once here, where the lifted code reads one
     * word a row */
    if (!clear_of(A, wa1, sp) || !clear_of(R1.base, e1, sp) || !clear_of(R2.base, e2, sp) ||
        ovl64(A, wa1, R1.base, e1) || ovl64(A, wa1, R2.base, e2) || ovl64(R1.base, e1, R2.base, e2) ||
        ovl64(wa1, ar1, R1.base, e1) || ovl64(wa1, ar1, R2.base, e2) || ovl64(t0, t1, A, wa1) ||
        ovl64(t0, t1, R1.base, e1) || ovl64(t0, t1, R2.base, e2) || ovl64(t0, t1, 0x8790u, 0x8980u)) return 0;
    K40 k;
    k.r88 = spu_ilhu(0x8000); k.r89 = spu_il(0); k.r92 = spu_ilhu(0x3F00); k.r93 = spu_ilhu(0x3F80); k.r101 = spu_ilhu(0x80);
    k.r99 = spu_ila(0x10203);
    k.r90 = spu_andbi(LQ16(ls, 0x8790), 31); k.r91 = spu_andbi(LQ16(ls, 0x87A0), 31);
    k.r109 = LQ16(ls, 0x87B0);
    const u128 r113 = LQ16(ls, 0x87C0);
    k.c15 = LQ16(ls, 0x8900); k.c17 = LQ16(ls, 0x8910);
    k.w25 = spu_andc(k.c17, k.r88); k.w36 = spu_fi(k.c17, spu_frest(k.c17));
    k.r103 = spu_iohl(spu_ilhu(0xBAB2), 0x4992); k.r104 = spu_iohl(spu_ilhu(0xB94C), 0x8C6D);
    k.k26 = spu_iohl(spu_ilhu(0x3F22), 0xF983); k.k21 = spu_iohl(spu_ilhu(0x3FC9), 0xFD9); k.k14 = spu_iohl(spu_ilhu(0x33A2), 0x2168);
    k.k16 = spu_iohl(spu_ilhu(0x3D2A), 0xA035); k.k15 = spu_iohl(spu_ilhu(0x3C08), 0x8342); k.k12 = spu_iohl(spu_ilhu(0xBEFF), 0xFFDF);
    k.k13 = spu_iohl(spu_ilhu(0xBE2A), 0xAAA1);
    /* the time terms: 50 / t (a reciprocal estimate, one Newton step) and t' as lanes */
    const u128 qt = LQ16(ls, A + 16388u), qt2 = LQ16(ls, A + 16392u);
    const u128 w13 = spu_rotqbyi(qt, 4), w6 = spu_rotqbyi(qt2, 8);
    const u128 w14 = spu_frest(w13), w11 = spu_fi(w13, w14), w12 = spu_ilhu(0x4248);
    const u128 w8 = spu_fm_lean(w12, w11), w10 = spu_fnms_lean(w8, w13, w12), w3 = spu_fma_lean(w10, w11, w8);
    k.r102 = spu_shufb(w6, w6, k.r99);
    const u128 r112 = spu_shufb(w3, w3, k.r99);
    /* the constant at 0x8970 is set the first time (the lifted code tests it at every row: byte 14 of the quad at 0x8960
     * stays 0 until something else sets it, and the set below does not touch it) */
    {
        const u128 q = spu_ceqbi(spu_rotqbyi(LQ16(ls, 0x8960), 13), 0), h = spu_xsbh(q);
        if (h._u16[0] != 0) {
            const u128 r26 = spu_cbd(SPLAT(sp - 576u), 0);
            SQ16(ls, 0x8970, spu_iohl(spu_ilhu(0x3BCD), 0xE32C));
            SQ16(ls, 0x8960, spu_shufb(spu_il(1), LQ16(ls, 0x8960), r26));
        }
    }
    k.kq = LQ16(ls, 0x8970);
    /* the raw form: the job's constants as it ships them (every lane), the time terms and the data small enough that no
     * operation reaches FLT_MAX: A below 2^34, B below 2^98, 50 / t below 2^33 and t' below 2^17, so a phase is below
     * 2^50 and every product after the wrap is of values below 2^98 and 4 */
    int raw = fast && fp_rzfz();
    if (raw) {
        static const uint32_t e109[4] = { 0x00000000u, 0x3F800000u, 0x40000000u, 0x40400000u }, e113[4] = { 0x3F000000u, 0xBF000000u, 0x3F000000u, 0xBF000000u };
        for (int i = 0; i < 4; i++)
            raw &= k.r109._u32[i] == e109[i] && r113._u32[i] == e113[i] && k.c15._u32[i] == 0x40490FDAu && k.c17._u32[i] == 0x40C90FDAu && k.kq._u32[i] == 0x3BCDE32Cu;
    }
    if (raw) raw = ((r112._u32[0] >> 23) & 0xFFu) <= 0x9Fu && ((w6._u32[0] >> 23) & 0xFFu) <= 0x8Fu && words_exp(ls + A, 2048) <= 0xA0u && words_exp(ls + A + 8192u, 2048) <= 0xE0u;
    int fails = 0;
    u128 x0fs[8];                                                    /* the first column of each block as a float in every lane */
    for (uint32_t b = 0; b < 8; b++) { const u128 f = spu_csflt(spu_il((int32_t)(4 * b)), 155); x0fs[b] = spu_shufb(f, f, k.r99); }
    for (uint32_t y = 0; y < 64; y++) {
        const u128 r100 = spu_il((int32_t)y);
        const u128 r45 = spu_fsm(spu_ceqhi(spu_xsbh(spu_ceqbi(spu_andi(r100, 1), 0)), 0));           /* y odd */
        const u128 r29 = spu_fsm(spu_ceqi(spu_cgti(r100, 31), 0));                                    /* y < 32 */
        const u128 r31 = spu_fm_lean(spu_shufb(spu_csflt(spu_ai(r100, -64), 155), spu_csflt(spu_ai(r100, -64), 155), k.r99), k.kq);
        const u128 r30 = spu_fm_lean(spu_shufb(spu_csflt(r100, 155), spu_csflt(r100, 155), k.r99), k.kq);
        const u128 r40 = spu_selb(spu_ilhu(0x3F80), spu_ilhu(0xBF80), r45);
        const u128 r37 = spu_shufb(r40, r40, k.r99);
        const u128 r28 = spu_fm_lean(r112, r37);
        k.r86 = spu_selb(r31, r30, r29);
        k.r97 = spu_fm_lean(r113, r28); k.r96 = spu_fm_lean(k.r86, k.r86);
        const uint32_t a1 = A + 128u * y, a2 = A + 8192u + 128u * y, p1 = R1.base + y * R1.stride, p2 = R2.base + y * R2.stride;
        for (uint32_t b = 0; b < 8; b++) {
            const u128 x0f = x0fs[b];
            const u128 aq = LQ16(ls, a1 + 16 * b), bq = LQ16(ls, a2 + 16 * b);
            B40 o;
            int done = 0;
            if (raw) { o = blk40r(&k, tf(x0f), tf(aq), tf(bq)); done = 1; }
            else if (fails < 8) {
                spu_spec_acc sa = SPU_SPEC_INIT();
                o = blk40(M_SPEC, &sa, &k, x0f, aq, bq);
                done = SPU_SPEC_OK(sa);
                if (!done) fails++;
            }
            if (!done) o = blk40(M_EXACT, NULL, &k, x0f, aq, bq);
            SQ16(ls, a1 + 16 * b, o.th);
            SQ16(ls, p1 + 32 * b + 0x10, o.q1a); SQ16(ls, p1 + 32 * b, o.q1b);
            SQ16(ls, p2 + 32 * b + 0x10, o.q2a); SQ16(ls, p2 + 32 * b, o.q2b);
        }
    }
    J211_COV(8 + !raw);
    return 1 + !raw;
}

/* ---------------------------------------------------------------------------------- entry, check run, stats */
typedef struct { unsigned long long calls, native, raw, declined, cmp, bad; } Stat;
static Stat s_st;
static int s_mode = -1;                                      /* PS3_NATIVE_J211 */
static unsigned s_pass = 0x3Fu;                                /* PS3_NATIVE_J211_PASSES: the hooks on, a bit each (0 = none) */
static _Thread_local uint8_t* t_lsb[2];                      /* the check run's copies of the store: one pair a level (0x45F8, a pass in it) */
static _Thread_local uint8_t* t_lsn[2];
static _Thread_local int t_depth;
static _Thread_local int t_bypass;                           /* the lifted reference runs without any hook */
enum { H_SPECTRUM, H_BITRV_COL, H_BITRV_ROW, H_CFTBCOL, H_RFTB, H_CFTBROW, H_N };
typedef struct { uint32_t addr; int (*run)(spu_context*, int); Stat st; } Hook;
static Hook s_hk[H_N] = {
    { 0x40E8u, hk_spectrum, {0} }, { 0x54E8u, hk_bitrv_col, {0} }, { 0x5118u, hk_bitrv_row, {0} },
    { 0x6EB8u, hk_cftbcol, {0} }, { 0x7860u, hk_rftb, {0} }, { 0x67A8u, hk_cftbrow, {0} },
};
static unsigned long long s_diffs;                           /* the check run's reports so far (all hooks) */

#define ADD(f, n) __atomic_fetch_add(&(f), (n), __ATOMIC_RELAXED)

static void stats(void)
{
    char line[300]; int l = snprintf(line, sizeof line, "[j211] %llu calls: native %llu, SPU %llu", s_st.calls, s_st.native, s_st.declined);
    if (s_mode == 2 && l > 0 && l < (int)sizeof line) snprintf(line + l, sizeof line - l, "; compared %llu, differ %llu", s_st.cmp, s_st.bad);
    fprintf(stderr, "%s\n", line);       /* one write: other threads print too */
    if (!s_pass) return;
    char ln[900]; int n = snprintf(ln, sizeof ln, "[j211] passes:");
    for (int i = 0; i < H_N && n > 0 && n < (int)sizeof ln; i++) {
        const Stat* s = &s_hk[i].st;
        if (!((s_pass >> i) & 1u)) continue;
        n += snprintf(ln + n, sizeof ln - n, " 0x%X %llu native (%llu raw) %llu SPU", s_hk[i].addr, s->native, s->raw, s->declined);
        if (s_mode == 2 && n > 0 && n < (int)sizeof ln) n += snprintf(ln + n, sizeof ln - n, " cmp %llu differ %llu;", s->cmp, s->bad);
        else if (n > 0 && n < (int)sizeof ln) n += snprintf(ln + n, sizeof ln - n, ";");
    }
    fprintf(stderr, "%s\n", ln);
}
static void stats_tick(void)
{
    static uint64_t next;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    const uint64_t now = (uint64_t)t.tv_sec;
    uint64_t w = __atomic_load_n(&next, __ATOMIC_RELAXED);
    if (now < w || !__atomic_compare_exchange_n(&next, &w, now + 10, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    if (w) stats();
}
static int j211_mode(void)
{
    if (__builtin_expect(__atomic_load_n(&s_mode, __ATOMIC_ACQUIRE) < 0, 0)) {
        const char* e = getenv("PS3_NATIVE_J211");
        { const char* sl = getenv("PS3_NATIVE_J211_SLOW"); if (sl && sl[0] == '1') s_fast = 0; }
        { const char* pa = getenv("PS3_NATIVE_J211_PASSES"); if (pa && pa[0]) s_pass = (unsigned)strtoul(pa, NULL, 16) & 0x3Fu; }
        int m = e && e[0] ? (e[0] == '2' ? 2 : e[0] != '0') : 1, unset = -1;
        if (__atomic_compare_exchange_n(&s_mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            fprintf(stderr, "[j211] native normal map (image %d, 0x%X): %s%s; passes (mask %02X) %s\n", J211_IMAGE, J211_FUNC,
                    m == 2 ? "check run against the lifted function" : m ? "on" : "off", s_fast ? "" : ", float helpers only",
                    s_pass, m && s_pass ? "on" : "off");
    }
    return s_mode;
}

/* the check run's compare: the registers a caller keeps and the store but for the stack; 0 = equal */
static int state_differs(const spu_context* ctx, const u128* gn, const uint8_t* lsn, uint32_t sp, uint32_t* at)
{
    for (int r = 0; r < 128; r++)
        if ((r == 0 || r == 1 || r >= 80) && memcmp(&ctx->gpr[r], &gn[r], 16)) { *at = (uint32_t)r; return 1; }
    const uint32_t lo = sp - 0x400u, hi = sp + 0x20u;
    const uint8_t* const ls = SPU_LS_BASE(ctx);
    for (uint32_t a = 0; a < SPU_LS_SIZE; a += 16) {
        if (a + 16 > lo && a < hi) continue;
        if (memcmp(ls + a, lsn + a, 16)) { *at = a; return 2; }
    }
    return 0;
}
/* A check run of one function: the model on the real context; its store and registers are kept, the state before
 * it put back, the lifted function run (no hooks), and the two compared; the lifted result is what stays. A
 * declined call must have changed nothing. 0 = the call was not taken (the lifted function runs). */
static int run_checked(spu_context* ctx, uint32_t addr, int (*run)(spu_context*, int), Stat* st)
{
    const int d = t_depth;
    if (d > 1) return 0;                                      /* a hook inside a hook: not a case */
    if (!t_lsb[d]) t_lsb[d] = (uint8_t*)malloc(SPU_LS_SIZE);
    if (!t_lsn[d]) t_lsn[d] = (uint8_t*)malloc(SPU_LS_SIZE);
    uint8_t* const lsb = t_lsb[d];
    uint8_t* const lsn = t_lsn[d];
    void (*fn)(spu_context*) = spu_lookup(addr, ctx->resident_ovl);
    if (!lsb || !lsn || !fn) return 0;
    uint8_t* const ls = SPU_LS_BASE(ctx);
    memcpy(lsb, ls, SPU_LS_SIZE);
    u128 g0[128]; memcpy(g0, ctx->gpr, sizeof g0);
    const uint32_t sp = ctx->gpr[1]._u32[0], r0 = ctx->gpr[0]._u32[0];
    t_depth = d + 1;
    const int took = run(ctx, s_fast);
    t_depth = d;
    uint32_t at = 0;
    int bad = 0;
    if (!took) {
        ADD(st->declined, 1);
        if (memcmp(ls, lsb, SPU_LS_SIZE) || memcmp(ctx->gpr, g0, sizeof g0)) {
            ADD(st->cmp, 1);
            const unsigned long long nb = __atomic_add_fetch(&s_diffs, 1, __ATOMIC_RELAXED);
            ADD(st->bad, 1);
            if (nb <= 12) fprintf(stderr, "[j211] check DIFFER #%llu (0x%X): a declined call changed the state: r3=%08X r4=%08X r5=%08X r6=%08X sp=%05X\n",
                                  nb, addr, g0[3]._u32[0], g0[4]._u32[0], g0[5]._u32[0], g0[6]._u32[0], sp);
            memcpy(ls, lsb, SPU_LS_SIZE); memcpy(ctx->gpr, g0, sizeof g0);
        }
        return 0;
    }
    ADD(st->native, 1);
    if (took == 1) ADD(st->raw, 1);
    u128 gn[128]; memcpy(gn, ctx->gpr, sizeof gn);
    memcpy(lsn, ls, SPU_LS_SIZE);
    memcpy(ls, lsb, SPU_LS_SIZE); memcpy(ctx->gpr, g0, sizeof g0);
    const int32_t si = (int32_t)ctx->image_id;
    t_bypass = 1;
    ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_bypass = 0;
    bad = state_differs(ctx, gn, lsn, sp, &at);
    ADD(st->cmp, 1);
    if (bad) {
        ADD(st->bad, 1);
        const unsigned long long nb = __atomic_add_fetch(&s_diffs, 1, __ATOMIC_RELAXED);
        if (nb <= 12) fprintf(stderr, "[j211] check DIFFER #%llu (0x%X, %s %x): r3=%08X r4=%08X r5=%08X r6=%08X sp=%05X\n", nb, addr,
                              bad == 1 ? "register" : "local store", at, g0[3]._u32[0], g0[4]._u32[0], g0[5]._u32[0], g0[6]._u32[0], sp);
    }
    return 1;
}

static int hk_enter(spu_context* ctx, int ix)
{
    if (t_bypass) return 0;
    const int md = j211_mode();
    if (!md || !((s_pass >> ix) & 1u)) return 0;
    Hook* h = &s_hk[ix];
    ADD(h->st.calls, 1);
    stats_tick();
    if (md == 2) return run_checked(ctx, h->addr, h->run, &h->st);
    const int r = h->run(ctx, s_fast);
    if (!r) { ADD(h->st.declined, 1); return 0; }
    ADD(h->st.native, 1);
    if (r == 1) ADD(h->st.raw, 1);
    return 1;
}
#define HK_ENTRY(i) static int hk_entry_##i(spu_context* ctx) { return hk_enter(ctx, i); }
HK_ENTRY(H_SPECTRUM) HK_ENTRY(H_BITRV_COL) HK_ENTRY(H_BITRV_ROW) HK_ENTRY(H_CFTBCOL) HK_ENTRY(H_RFTB) HK_ENTRY(H_CFTBROW)

static int j211_entry(spu_context* ctx)
{
    if (t_bypass) return 0;
    const int md = j211_mode();
    if (!md) return 0;
    ADD(s_st.calls, 1);
    stats_tick();
    if (md != 2) {
        if (!j211_run(ctx, s_fast)) { ADD(s_st.declined, 1); return 0; }
        ADD(s_st.native, 1);
        return 1;
    }
    const int r = run_checked(ctx, J211_FUNC, j211_run, &s_st);
    return r;
}

__attribute__((constructor)) static void j211_register(void)
{
    spu_native_register(J211_IMAGE, J211_FUNC, j211_entry);
    spu_native_register(J211_IMAGE, s_hk[H_SPECTRUM].addr, hk_entry_H_SPECTRUM);
    spu_native_register(J211_IMAGE, s_hk[H_BITRV_COL].addr, hk_entry_H_BITRV_COL);
    spu_native_register(J211_IMAGE, s_hk[H_BITRV_ROW].addr, hk_entry_H_BITRV_ROW);
    spu_native_register(J211_IMAGE, s_hk[H_CFTBCOL].addr, hk_entry_H_CFTBCOL);
    spu_native_register(J211_IMAGE, s_hk[H_RFTB].addr, hk_entry_H_RFTB);
    spu_native_register(J211_IMAGE, s_hk[H_CFTBROW].addr, hk_entry_H_CFTBROW);
}
