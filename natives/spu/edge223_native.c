/* inFAMOUS 2: Edge Geometry's hot SPU kernels, natively (job module 223).
 *
 * Ported (cpu agent, 2026-11-09) from ThiagoFraga761/infamous2-pc work/port/edge_kernels_native.c
 * (MIT, Copyright (c) 2026 the infamous2-recomp contributors -- notice below), written there for
 * the US disc's module jobmod_6d1c65171899_eA2A0 (image 209). Our EU 1.04 module
 * jobmod_d0d15ec83992_ea2a0 (image 223) has the same entry offset and every kernel address
 * (0xD0A8 0xA3C8 0xA798 0x4448 0x99E8/0x98B0 0xC7B8) as a lifted function. Title knowledge for this game: lives in
 * natives/spu of the port (published 2026-11-13, v1.04 EU/US code; another EBOOT stops the build), never in
 * the engine.
 *
 * The original needs AVX-512; this PC (i5-13600K) has none. The same code runs on AVX2 through
 * SIMDe 0.8.2 (simd-everywhere, MIT; the 68 headers this file needs are in natives/spu/simde-0.8.2): every _mm512_* call is the simde_mm512_*
 * of the same name, which works on two 256-bit halves; the few SIMDe lacks are the cpu_mm*
 * shims below, lane for lane what the instruction does. The single-precision arithmetic stays
 * IEEE single on the host's AVX/FMA units at the MXCSR the kernel sets (round toward zero, FTZ,
 * DAZ: CSR_FAST, the SPU threads' own mode), so the results are the original's; the FMA ops
 * must be the fused ones (SIMDE_X86_FMA_NATIVE, checked below), never a multiply and an add.
 * Other changes: our image id, the toolkit's spu_native_register, the compare / capture modes
 * and the stats line left out, _aligned_malloc for aligned_alloc.
 *
 * PS3_NATIVE_EDGE=0: all off. Per kernel: PS3_NATIVE_EDGE_SKIN, _XF, _CULL, _DEC, _ENC, _IDX = 0/1.
 * edge2 (2026-11-09): the fast paths are hand-written AVX2 now (see "AVX2 fast paths"); every kernel
 * but idx defaults on. PS3_NATIVE_EDGE_AVX2=0 = the SIMDe forms, and then only skin (and enc) default
 * on -- the behaviour before (natives.txt hooks skin, xf, cull, dec; a declined hook runs the lift).
 *
 * The original header follows.
 *
 inFAMOUS 2 (BCUS98125): Edge Geometry's hot SPU kernels, natively.
 *
 * Job module 6d1c65171899 (image 209) is Edge Geometry: per mesh segment it
 * decompresses vertices and indexes, skins, culls and compresses for the RSX.
 * This file replaces pure local-store functions of it with host code that
 * leaves the registers the SPU ABI makes observable and the whole local store
 * exactly as the lifted code would (rpi/edge-geom-kernels/RESEARCH.md):
 *
 *   0xD0A8  skinning (blend matrices per vertex, transform positions and up to
 *           three more streams, renormalise them; calls 0x6E98, normalise)
 *   0xA3C8  transform for culling, 0xA798 triangle culling
 *   0x4448  vertex decompression, the job's one special format pair (u16
 *           positions with an offset and a scale, 24-bit unit vectors)
 *   0x99E8  vertex compression for output format 1 (a chunk of 0x9780: three
 *           floats and two X11Y11Z10 words per vertex)
 *   0xC7B8  index decompression, the compressed triangle lists (flags, 2-bit
 *           triangle codes and bit-packed, delta-coded vertex numbers)
 *
 * Floats follow the SPU: truncation, denormal operands and results as zero,
 * no infinities. The fast paths run four vertices per AVX-512 register and
 * check every result; a batch that meets an operand or result the fast path
 * does not model (denormal, exponent 255, |x| >= FLT_MAX) is recomputed by the
 * exact per-vertex path, which uses runtime/spu/spu_helpers.h like the lift.
 * A call whose buffers overlap, or that would take an error path, returns 0
 * before writing anything and the lifted function runs instead.
 *
 * The lift calls spu_native_entry at each address (tools/lift_spu.py,
 * NATIVE_ENTRIES). PS3RECOMP_NATIVE_EDGE: 1 (default) on; 0 off; 2 runs the
 * native kernel on a copy of the local store, then the lifted function, and
 * compares the local store and r0, r1, r80-r127, logging any difference.
 * The decompression, compression and index kernels are newer, each with its
 * own switch of the same values: PS3RECOMP_NATIVE_EDGE_DEC,
 * PS3RECOMP_NATIVE_EDGE_ENC and PS3RECOMP_NATIVE_EDGE_IDX (off unless set
 * here; main.cpp sets the first two to 1; PS3RECOMP_NATIVE_EDGE=0 still turns
 * everything off).
 *
 * Built like the lifted SPU sources (-march=x86-64-v4): it needs AVX-512. 
 * Thiago's LICENSE (MIT): Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions: The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
 * "AS IS", WITHOUT WARRANTY OF ANY KIND. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <immintrin.h>

#include "spu_context.h"
#include "spu_helpers.h"
/* trim (2026-11-10): the toolkit's run-time SPU instruction alternatives (runtime/spu/spu_patch.h,
 * project tools/spu_patches.txt). Switch mesh_trim = 1 (PS3RECOMP_MESH_TRIM=0, the default) is
 * Yahfz's RPCS3 patch "Disable Mesh Trimming": the cull's `brnz $r4, 0xA80C` at 0xA7C0 a nop, so
 * every call takes the job's own r4 = 0 path (no culling) whatever its cull mode -- and so does
 * edge_cull_core. A toolkit without the header: the old behaviour (the switch does not exist). */
#if defined(__has_include)
#  if __has_include("spu_patch.h")
#    include "spu_patch.h"
#    define EDGE_HAVE_SPU_PATCH 1
#  endif
#endif

/* Only the SIMDe headers this file uses: the umbrella avx512.h pulls in fixupimm_round.h, which
 * clang-cl rejects ("imm8 must be constant"). */
#define SIMDE_AVX512_DIR "simde-0.8.2/simde/x86/avx512/"
#include "simde-0.8.2/simde/x86/avx512/types.h"
#include "simde-0.8.2/simde/x86/avx512/add.h"
#include "simde-0.8.2/simde/x86/avx512/and.h"
#include "simde-0.8.2/simde/x86/avx512/blend.h"
#include "simde-0.8.2/simde/x86/avx512/broadcast.h"
#include "simde-0.8.2/simde/x86/avx512/cast.h"
#include "simde-0.8.2/simde/x86/avx512/cmp.h"
#include "simde-0.8.2/simde/x86/avx512/cmpeq.h"
#include "simde-0.8.2/simde/x86/avx512/cmpge.h"
#include "simde-0.8.2/simde/x86/avx512/cmpgt.h"
#include "simde-0.8.2/simde/x86/avx512/cvt.h"
#include "simde-0.8.2/simde/x86/avx512/cvtt.h"
#include "simde-0.8.2/simde/x86/avx512/extract.h"
#include "simde-0.8.2/simde/x86/avx512/fmadd.h"
#include "simde-0.8.2/simde/x86/avx512/fmsub.h"
#include "simde-0.8.2/simde/x86/avx512/fnmadd.h"
#include "simde-0.8.2/simde/x86/avx512/insert.h"
#include "simde-0.8.2/simde/x86/avx512/loadu.h"
#include "simde-0.8.2/simde/x86/avx512/max.h"
#include "simde-0.8.2/simde/x86/avx512/mov.h"
#include "simde-0.8.2/simde/x86/avx512/mov_mask.h"
#include "simde-0.8.2/simde/x86/avx512/mul.h"
#include "simde-0.8.2/simde/x86/avx512/mullo.h"
#include "simde-0.8.2/simde/x86/avx512/or.h"
#include "simde-0.8.2/simde/x86/avx512/permutex2var.h"
#include "simde-0.8.2/simde/x86/avx512/permutexvar.h"
#include "simde-0.8.2/simde/x86/avx512/set1.h"
#include "simde-0.8.2/simde/x86/avx512/setr.h"
#include "simde-0.8.2/simde/x86/avx512/setzero.h"
#include "simde-0.8.2/simde/x86/avx512/shuffle.h"
#include "simde-0.8.2/simde/x86/avx512/slli.h"
#include "simde-0.8.2/simde/x86/avx512/sllv.h"
#include "simde-0.8.2/simde/x86/avx512/srli.h"
#include "simde-0.8.2/simde/x86/avx512/srlv.h"
#include "simde-0.8.2/simde/x86/avx512/storeu.h"
#include "simde-0.8.2/simde/x86/avx512/sub.h"
#include "simde-0.8.2/simde/x86/avx512/ternarylogic.h"
#include "simde-0.8.2/simde/x86/avx512/test.h"
#include "simde-0.8.2/simde/x86/avx512/unpackhi.h"
#include "simde-0.8.2/simde/x86/avx512/unpacklo.h"
#include "simde-0.8.2/simde/x86/avx512/xor.h"
#undef SIMDE_AVX512_DIR

#if !defined(SIMDE_X86_AVX2_NATIVE) || !defined(SIMDE_X86_FMA_NATIVE)
#error "edge223_native.c needs AVX2 and FMA (the SPU sources' /arch:AVX2): SIMDe would fall back to unfused multiply-add"
#endif

/* ---- what SIMDe 0.8.2 lacks: lane for lane the AVX-512 instruction ---- */
/* SIMDe's own two are wrong here: under clang-cl (MS ABI) the packed-struct trick of its
 * simde_mm512_loadu_si512 keeps the 64-byte alignment of the vector type, so it compiles to an
 * ALIGNED load (vmovdqa: a crash on any LS address that is not 32-aligned); and its
 * simde_mm512_mask_storeu_epi32 fallback stores all 64 bytes, zeros in the masked-off lanes,
 * where the instruction leaves those bytes alone. (cpu agent) */
static inline simde__m512i cpu_mm512_loadu_si512(const void* p)
{
    simde__m512i r; memcpy(&r, p, sizeof r); return r;
}
static inline void cpu_mm512_mask_storeu_epi32(void* p, simde__mmask16 k, simde__m512i a)
{
    union { simde__m512i z; __m256i y[2]; } x; x.z = a;
    const __m256i bits = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
    const __m256i m0 = _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(k & 0xFF), bits), bits);
    const __m256i m1 = _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(k >> 8), bits), bits);
    _mm256_maskstore_epi32((int*)p, m0, x.y[0]);
    _mm256_maskstore_epi32((int*)p + 8, m1, x.y[1]);
}
typedef union { simde__m512i z; __m256i y[2]; } cpu_zy;
static inline simde__m512i cpu_mm512_srav_epi32(simde__m512i a, simde__m512i c)
{
    cpu_zy x, n, r; x.z = a; n.z = c;
    r.y[0] = _mm256_srav_epi32(x.y[0], n.y[0]); r.y[1] = _mm256_srav_epi32(x.y[1], n.y[1]);
    return r.z;
}
static inline simde__mmask16 cpu_mm512_testn_epi32_mask(simde__m512i a, simde__m512i b)
{
    return simde_mm512_cmpeq_epi32_mask(simde_mm512_and_si512(a, b), simde_mm512_setzero_si512());
}
static inline simde__mmask16 cpu_mm512_cmplt_epu32_mask(simde__m512i a, simde__m512i b)
{
    return simde_mm512_cmp_epu32_mask(a, b, SIMDE_MM_CMPINT_LT);
}
/* unsigned 32-bit -> single in the current rounding mode, one rounding: hi*65536 and lo are
 * exact singles, so their sum is rounded once, like the instruction */
static inline simde__m512 cpu_mm512_cvtepu32_ps(simde__m512i a)
{
    cpu_zy x; x.z = a;
    union { simde__m512 z; __m256 y[2]; } r;
    for (int h = 0; h < 2; h++) {
        const __m256 hi = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_srli_epi32(x.y[h], 16)), _mm256_set1_ps(65536.0f));
        const __m256 lo = _mm256_cvtepi32_ps(_mm256_and_si256(x.y[h], _mm256_set1_epi32(0xFFFF)));
        r.y[h] = _mm256_add_ps(hi, lo);
    }
    return r.z;
}
static inline uint16_t cpu_mm256_test_epi16_mask(__m256i a, __m256i b)
{
    uint16_t t[16]; _mm256_storeu_si256((__m256i*)t, _mm256_and_si256(a, b));
    uint16_t m = 0;
    for (int i = 0; i < 16; i++) m |= (uint16_t)((t[i] != 0) << i);
    return m;
}
static inline __m256i cpu_mm256_mask_max_epu16(__m256i src, uint16_t k, __m256i a, __m256i b)
{
    const __m256i bits = _mm256_setr_epi16(1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, (short)0x8000);
    const __m256i sel = _mm256_cmpeq_epi16(_mm256_and_si256(_mm256_set1_epi16((short)k), bits), bits);
    return _mm256_blendv_epi8(src, _mm256_max_epu16(a, b), sel);
}
static inline void cpu_mm_mask_storeu_epi16(void* p, uint8_t k, __m128i a)
{
    uint16_t t[8]; _mm_storeu_si128((__m128i*)t, a);
    for (int i = 0; i < 8; i++) if (k >> i & 1) memcpy((uint8_t*)p + 2 * i, &t[i], 2);
}

#define EDGE_IMAGE 223   /* our image id of jobmod_d0d15ec83992_ea2a0 (Thiago: 209) */
#ifdef EDGE_HARNESS
int g_edge_stat[8];
#define EDGE_STAT(i) (g_edge_stat[i]++)
#else
#define EDGE_STAT(i) ((void)0)
#endif
/* groups a kernel's exact path redid, for the stats line */
static unsigned long long g_dec_exact, g_enc_exact;
#define EDGE_EXACT(c) __atomic_fetch_add(&(c), 1, __ATOMIC_RELAXED)
#define LSM 0x3FFFFu
#define LSQ 0x3FFF0u
#define F1 0x3F800000u
#define INV255 0x3B808080u
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ------------------------------------------------------------------ exact per-quad helpers */
typedef u128 Q;
static inline uint32_t be32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline Q lq(const uint8_t* ls, uint32_t a)
{
    const __m128i v = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (a & LSQ))),
                                       _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    Q r; memcpy(&r, &v, 16); return r;
}
static inline void sq(uint8_t* ls, uint32_t a, Q q)
{
    __m128i v; memcpy(&v, &q, 16);
    _mm_storeu_si128((__m128i*)(ls + (a & LSQ)), _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
static inline Q V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { Q r; r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d; return r; }
static inline Q yzx(Q v) { return V4(v._u32[1], v._u32[2], v._u32[0], v._u32[3]); }
static inline Q zxy(Q v) { return V4(v._u32[2], v._u32[0], v._u32[1], v._u32[3]); }
static inline Q spl(Q v, int i) { return V4(v._u32[i], v._u32[i], v._u32[i], v._u32[i]); }
/* rotqby by the low four bits of addr: the word at addr inside its quad */
static inline uint32_t word_at(const uint8_t* ls, uint32_t addr)
{
    const uint8_t* q = ls + (addr & LSQ); const int s = addr & 15;
    return (uint32_t)q[s] << 24 | q[(s + 1) & 15] << 16 | q[(s + 2) & 15] << 8 | q[(s + 3) & 15];
}
static inline Q quad_at(const uint8_t* ls, uint32_t addr)
{
    const __m128i rot = _mm_and_si128(_mm_add_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15), _mm_set1_epi8((char)(addr & 15))), _mm_set1_epi8(15));
    const __m128i v = _mm_shuffle_epi8(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (addr & LSQ))), rot),
                                       _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    Q r; memcpy(&r, &v, 16); return r;
}

/* ------------------------------------------------------------------ 16-lane SPU floats
 * Four quads per register, lane 4*j+i = word i of quad j.
 *
 * The SPU threads run with DAZ in MXCSR (spu_fp_enter, runtime/spu/
 * spu_context.h, SPU_FP_DAZ): the helpers of runtime/spu/spu_helpers.h then
 * read an exponent-0 operand as zero, as the SPU does. The fast paths run
 * with MXCSR = truncation, flush-to-zero and denormals-are-zero. Then an x86
 * result is the helpers' whenever no operand has exponent 255 and nothing
 * overflows -- except that a zero may carry a sign the SPU never gives it.
 * Signs of zeros never reach a nonzero value in these kernels (no division,
 * and compares treat both zeros alike), so outputs are made +0 where their
 * exponent is 0, and that is all. Exponent 255 and overflow are excluded by
 * bounding what a batch reads from the LS (RESEARCH.md section 1.3); a batch
 * outside the bound runs the exact path, at the caller's MXCSR.
 * The fast functions are noinline: the compiler does not order floating-point
 * arithmetic against MXCSR writes, but it cannot move it out of a call. */
typedef simde__m512i Z;
#define CSR_FAST 0xFFC0u                       /* RZ | FZ | DAZ, all exceptions masked */
#define B_IN 0x48800000                        /* 2^18 */
#define B_RSQ 0x71800000                       /* 2^100: a factor from fi */
#define NOINL __attribute__((noinline))
static inline simde__m512 zf(Z x) { return simde_mm512_castsi512_ps(x); }
static inline Z zi(simde__m512 x) { return simde_mm512_castps_si512(x); }
static inline Z FM(Z a, Z b) { return zi(simde_mm512_mul_ps(zf(a), zf(b))); }
static inline Z FMA(Z a, Z b, Z c) { return zi(simde_mm512_fmadd_ps(zf(a), zf(b), zf(c))); }
static inline Z FMS(Z a, Z b, Z c) { return zi(simde_mm512_fmsub_ps(zf(a), zf(b), zf(c))); }
static inline Z canon(Z r) { return simde_mm512_maskz_mov_epi32(simde_mm512_test_epi32_mask(r, simde_mm512_set1_epi32(0x7F800000)), r); }
/* a value read from the LS: its magnitude goes to mi; kb collects lanes
 * found bad along the way */
typedef struct { Z mi; simde__mmask16 kb; } Chk;
static inline void chk_init(Chk* c) { c->mi = simde_mm512_setzero_si512(); c->kb = 0; }
static inline Z chk(Z x, Chk* c)
{
    c->mi = simde_mm512_max_epu32(c->mi, simde_mm512_and_si512(x, simde_mm512_set1_epi32(0x7FFFFFFF)));
    return x;
}
static inline int chk_bad(const Chk* c)
{
    return (simde_mm512_cmpge_epu32_mask(c->mi, simde_mm512_set1_epi32(B_IN)) | c->kb) != 0;
}
static inline Z bswap16(Z v)
{
    return simde_mm512_shuffle_epi8(v, simde_mm512_broadcast_i32x4(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
/* four consecutive quads at a (lqd semantics: a & ~15; the caller keeps them inside the LS) */
static inline Z ld16(const uint8_t* ls, uint32_t a) { return bswap16(cpu_mm512_loadu_si512(ls + (a & LSQ))); }
static inline void st16(uint8_t* ls, uint32_t a, Z v) { simde_mm512_storeu_si512(ls + (a & LSQ), bswap16(v)); }
/* 48 bytes at p as quads 0..2, quad 3 zero (the span check leaves 16 readable bytes after) */
static inline Z ld12(const uint8_t* p) { return simde_mm512_maskz_mov_epi32(0x0FFF, bswap16(cpu_mm512_loadu_si512(p))); }
#define YZX(v) simde_mm512_shuffle_epi32((v), (int)0xC9)
#define ZXY(v) simde_mm512_shuffle_epi32((v), (int)0xD2)

/* fi (runtime/spu/spu_helpers.h), sixteen lanes; k_rsq_lut is frsqest's table */
static const uint32_t k_rsq_lut[64] = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA
};
static inline Z fi16(Z ya, Z est)
{
    const Z expo = simde_mm512_and_si512(simde_mm512_srli_epi32(est, 23), simde_mm512_set1_epi32(0xFF));
    const Z base = simde_mm512_and_si512(simde_mm512_srli_epi32(est, 10), simde_mm512_set1_epi32(0x1FFF));
    const Z step = simde_mm512_and_si512(est, simde_mm512_set1_epi32(0x3FF));
    const Z y = simde_mm512_and_si512(ya, simde_mm512_set1_epi32(0x7FFFF));
    const Z mh = simde_mm512_sub_epi32(simde_mm512_slli_epi32(simde_mm512_or_si512(base, simde_mm512_set1_epi32(0x2000)), 11),
                                  simde_mm512_srli_epi32(simde_mm512_add_epi32(simde_mm512_mullo_epi32(step, y), simde_mm512_set1_epi32(255)), 8));
    const simde__mmask16 below1 = simde_mm512_cmpgt_epi32_mask(simde_mm512_set1_epi32(0x1000000), mh);
    const Z frac = simde_mm512_and_si512(simde_mm512_mask_mov_epi32(simde_mm512_srli_epi32(mh, 1), below1, mh), simde_mm512_set1_epi32(0x7FFFFF));
    const Z e2 = simde_mm512_mask_sub_epi32(expo, below1, expo, simde_mm512_set1_epi32(1));
    const simde__mmask16 zero = simde_mm512_cmpeq_epi32_mask(expo, simde_mm512_setzero_si512()) | simde_mm512_cmpeq_epi32_mask(e2, simde_mm512_setzero_si512());
    return simde_mm512_maskz_mov_epi32((simde__mmask16)~zero, simde_mm512_or_si512(simde_mm512_and_si512(est, simde_mm512_set1_epi32((int)0x80000000u)),
                                                                     simde_mm512_or_si512(simde_mm512_slli_epi32(e2, 23), frac)));
}
/* fi(s, frsqest(s)) in one step. frsqest's estimate has no sign and an
 * exponent of at least 62, so fi's zero cases never arise; the estimate's
 * fractions enter through two tables derived from k_rsq_lut. */
static uint32_t k_rsq_a[64], k_rsq_s[64];
static void rsq_init(void)
{
    for (int i = 0; i < 64; i++) { k_rsq_a[i] = (((k_rsq_lut[i] >> 10) & 0x1FFF) | 0x2000) << 11; k_rsq_s[i] = k_rsq_lut[i] & 0x3FF; }
}
static inline Z lut64(const uint32_t* t, Z idx, simde__mmask16 hi)
{
    const Z lo = simde_mm512_permutex2var_epi32(cpu_mm512_loadu_si512(t), idx, cpu_mm512_loadu_si512(t + 16));
    return simde_mm512_mask_mov_epi32(lo, hi, simde_mm512_permutex2var_epi32(cpu_mm512_loadu_si512(t + 32), idx, cpu_mm512_loadu_si512(t + 48)));
}
static inline Z rsq16(Z s)
{
    const Z e = simde_mm512_and_si512(simde_mm512_srli_epi32(s, 23), simde_mm512_set1_epi32(0xFF));
    const Z idx = simde_mm512_srli_epi32(s, 18);
    const simde__mmask16 hi = simde_mm512_test_epi32_mask(idx, simde_mm512_set1_epi32(32));
    const Z A = lut64(k_rsq_a, idx, hi), St = lut64(k_rsq_s, idx, hi);
    const Z y = simde_mm512_and_si512(s, simde_mm512_set1_epi32(0x7FFFF));
    const Z mh = simde_mm512_sub_epi32(A, simde_mm512_srli_epi32(simde_mm512_add_epi32(simde_mm512_mullo_epi32(St, y), simde_mm512_set1_epi32(255)), 8));
    const simde__mmask16 below1 = simde_mm512_cmpgt_epi32_mask(simde_mm512_set1_epi32(0x1000000), mh);
    const Z frac = simde_mm512_and_si512(simde_mm512_mask_mov_epi32(simde_mm512_srli_epi32(mh, 1), below1, mh), simde_mm512_set1_epi32(0x7FFFFF));
    Z rexp = simde_mm512_sub_epi32(simde_mm512_set1_epi32(190), simde_mm512_srli_epi32(simde_mm512_add_epi32(e, simde_mm512_set1_epi32(1)), 1));
    rexp = simde_mm512_mask_mov_epi32(rexp, cpu_mm512_testn_epi32_mask(e, e), simde_mm512_set1_epi32(0xFF));
    rexp = simde_mm512_mask_sub_epi32(rexp, below1, rexp, simde_mm512_set1_epi32(1));
    return simde_mm512_or_si512(simde_mm512_slli_epi32(rexp, 23), frac);
}

/* 0x6E98's arithmetic: v * (r, r, r, 1), r = fi(s, frsqest(s)), s = z*z + (y*y + x*x).
 * sumsq16 gives s of each of four vectors in every lane of its block. */
static inline Z sumsq16(Z v)
{
    const Z x = simde_mm512_shuffle_epi32(v, (int)0x00), y = simde_mm512_shuffle_epi32(v, (int)0x55),
            z = simde_mm512_shuffle_epi32(v, (int)0xAA);
    return FMA(z, z, FMA(y, y, FM(x, x)));
}
/* v * (r, r, r, 1), r per block. s = 0 makes r an exponent-255 value, which
 * the fast path cannot multiply: where v's lane is zero the SPU gives +0 and
 * so does a zero factor; any other lane goes the exact way. Not canonical. */
static inline Z scale16(Z v, Z r, Chk* ck)
{
    const simde__mmask16 big = simde_mm512_cmpge_epu32_mask(simde_mm512_and_si512(r, simde_mm512_set1_epi32(0x7FFFFFFF)), simde_mm512_set1_epi32(B_RSQ)) & 0x7777;
    ck->kb |= big & ~cpu_mm512_testn_epi32_mask(v, simde_mm512_set1_epi32(0x7FFFFFFF));
    r = simde_mm512_mask_mov_epi32(simde_mm512_maskz_mov_epi32((simde__mmask16)~big, r), 0x8888, simde_mm512_set1_epi32(F1));
    return FM(v, r);
}
static inline Z norm16(Z v, Chk* ck)
{
    const Z s = sumsq16(v);
    return scale16(v, rsq16(s), ck);
}
/* ------------------------------------------------------------------ exact per-vector paths */
static inline Q norm_q(Q v)
{
    const Q s = spu_fma(spl(v, 2), spl(v, 2), spu_fma(spl(v, 1), spl(v, 1), spu_fm(spl(v, 0), spl(v, 0))));
    const Q r = spu_fi(s, spu_frsqest(s));
    return spu_fm(v, V4(r._u32[0], r._u32[0], r._u32[0], F1));
}
static inline Q xform_q(Q x, Q A, Q B, Q C) { return spu_fma(zxy(x), C, spu_fma(yzx(x), B, spu_fm(x, A))); }
static inline Q cofactor_q(Q x, Q A, Q B, Q C)
{
    const Q d1 = spu_fms(yzx(A), zxy(A), spu_fm(zxy(C), yzx(B)));
    const Q d2 = spu_fms(zxy(B), yzx(B), spu_fm(yzx(C), zxy(A)));
    const Q d3 = spu_fms(yzx(C), zxy(C), spu_fm(zxy(B), yzx(A)));
    return spu_fma(d3, zxy(x), spu_fma(d2, yzx(x), spu_fm(d1, x)));
}
/* 0x6E98 on vectors [from, n8) at p (it rounds the count up to eight) */
static NOINL int norm_block16(uint8_t* ls, uint32_t a)
{
    Chk ck; chk_init(&ck);
    const Z o = canon(norm16(chk(ld16(ls, a), &ck), &ck));
    if (unlikely(chk_bad(&ck))) return 0;
    st16(ls, a, o);
    return 1;
}
/* sixteen vectors at a: x, y, z of all of them in three registers, one estimate */
static NOINL int norm_group16(uint8_t* ls, uint32_t a)
{
    Chk ck; chk_init(&ck);
    Z V[4];
    for (int m = 0; m < 4; m++) V[m] = chk(ld16(ls, a + 64 * m), &ck);
    Z X[3];
    for (int c = 0; c < 3; c++) {
        const Z ix = simde_mm512_add_epi32(simde_mm512_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28, 0, 4, 8, 12, 16, 20, 24, 28), simde_mm512_set1_epi32(c));
        X[c] = simde_mm512_mask_mov_epi32(simde_mm512_permutex2var_epi32(V[0], ix, V[1]), 0xFF00, simde_mm512_permutex2var_epi32(V[2], ix, V[3]));
    }
    const Z R = rsq16(FMA(X[2], X[2], FMA(X[1], X[1], FM(X[0], X[0]))));
    Z o[4];
    for (int m = 0; m < 4; m++)
        o[m] = canon(scale16(V[m], simde_mm512_permutexvar_epi32(simde_mm512_setr_epi32(4 * m, 4 * m, 4 * m, 4 * m, 4 * m + 1, 4 * m + 1, 4 * m + 1, 4 * m + 1,
                                                                             4 * m + 2, 4 * m + 2, 4 * m + 2, 4 * m + 2, 4 * m + 3, 4 * m + 3, 4 * m + 3, 4 * m + 3), R), &ck));
    if (unlikely(chk_bad(&ck))) return 0;
    for (int m = 0; m < 4; m++) st16(ls, a + 64 * m, o[m]);
    return 1;
}
/* MXCSR is CSR_FAST here; csr is the caller's */
static void normalize_ls(uint8_t* ls, uint32_t p, uint32_t from, uint32_t n8, uint32_t csr)
{
    uint32_t k = from;
    for (; k + 16 <= n8; k += 16)
        if (unlikely(!norm_group16(ls, p + 16 * k)))
            for (uint32_t j = 0; j < 16; j += 4)
                if (!norm_block16(ls, p + 16 * (k + j))) {
                    _mm_setcsr(csr);
                    for (uint32_t t = 0; t < 4; t++) sq(ls, p + 16 * (k + j + t), norm_q(lq(ls, p + 16 * (k + j + t))));
                    _mm_setcsr(CSR_FAST);
                }
    for (; k < n8; k += 4) {
        if (likely(norm_block16(ls, p + 16 * k))) continue;
        _mm_setcsr(csr);
        for (uint32_t j = 0; j < 4; j++) sq(ls, p + 16 * (k + j), norm_q(lq(ls, p + 16 * (k + j))));
        _mm_setcsr(CSR_FAST);
    }
}

/* ------------------------------------------------------------------ AVX2 fast paths (edge2, 2026-11-09)
 * The 16-lane fast paths of this file written for AVX2 by hand: two quads per register (lane
 * 4*j+i = word i of quad j) or eight values of one component, the same IEEE single operations
 * in the same order on every lane, the same checks and fallbacks, vector masks instead of
 * mask registers and gathers for the tables. SIMDe's 512-bit emulation of masks and permutes
 * made the skinning kernel cost ~30% of the Edge job in the fight and the other kernels lose
 * to the lifted code. PS3_NATIVE_EDGE_AVX2=0: the SIMDe forms (and xf, cull and dec off unless
 * their own switch says on): the behaviour before. */
static int g_avx2 = 1;
typedef __m256i Y;
#define Y1(x) _mm256_set1_epi32((int)(x))
#define Y0 _mm256_setzero_si256()
static inline __m256 yf(Y x) { return _mm256_castsi256_ps(x); }
static inline Y yi(__m256 x) { return _mm256_castps_si256(x); }
static inline Y yFM(Y a, Y b) { return yi(_mm256_mul_ps(yf(a), yf(b))); }
static inline Y yFMA(Y a, Y b, Y c) { return yi(_mm256_fmadd_ps(yf(a), yf(b), yf(c))); }
static inline Y yFMS(Y a, Y b, Y c) { return yi(_mm256_fmsub_ps(yf(a), yf(b), yf(c))); }
static inline Y yabs(Y x) { return _mm256_and_si256(x, Y1(0x7FFFFFFF)); }
static inline Y ycanon(Y r) { return _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(r, Y1(0x7F800000)), Y0), r); }
static inline Y ybswap(Y v)
{
    return _mm256_shuffle_epi8(v, _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                                   3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
/* two consecutive quads at a & LSQ (the caller keeps them inside the LS) */
static inline Y yld(const uint8_t* ls, uint32_t a) { return ybswap(_mm256_loadu_si256((const __m256i*)(ls + (a & LSQ)))); }
static inline void yst(uint8_t* ls, uint32_t a, Y v) { _mm256_storeu_si256((__m256i*)(ls + (a & LSQ)), ybswap(v)); }
/* a >= b, unsigned, for a below 2^31 */
static inline Y yge(Y a, uint32_t b) { return _mm256_cmpgt_epi32(a, Y1(b - 1)); }
static inline int yany(Y m) { return !_mm256_testz_si256(m, m); }
/* fcgt's order (fkey16) */
static inline Y fkey8(Y x)
{
    const Y m = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(x, Y1(0x7F800000)), Y0), yabs(x));
    const Y neg = _mm256_srai_epi32(x, 31);
    return _mm256_sub_epi32(_mm256_xor_si256(m, neg), neg);
}
/* fi(s, frsqest(s)) (rsq16); the tables are indexed by bits 18..23 */
static inline Y rsq8(Y s)
{
    const Y e = _mm256_and_si256(_mm256_srli_epi32(s, 23), Y1(0xFF));
    const Y idx = _mm256_and_si256(_mm256_srli_epi32(s, 18), Y1(63));
    const Y A = _mm256_i32gather_epi32((const int*)k_rsq_a, idx, 4), St = _mm256_i32gather_epi32((const int*)k_rsq_s, idx, 4);
    const Y y = _mm256_and_si256(s, Y1(0x7FFFF));
    const Y mh = _mm256_sub_epi32(A, _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(St, y), Y1(255)), 8));
    const Y below1 = _mm256_cmpgt_epi32(Y1(0x1000000), mh);
    const Y frac = _mm256_and_si256(_mm256_blendv_epi8(_mm256_srli_epi32(mh, 1), mh, below1), Y1(0x7FFFFF));
    Y rexp = _mm256_sub_epi32(Y1(190), _mm256_srli_epi32(_mm256_add_epi32(e, Y1(1)), 1));
    rexp = _mm256_blendv_epi8(rexp, Y1(0xFF), _mm256_cmpeq_epi32(e, Y0));
    rexp = _mm256_add_epi32(rexp, below1);              /* below1 is -1 there */
    return _mm256_or_si256(_mm256_slli_epi32(rexp, 23), frac);
}
/* scale16 on two vectors: v * (r, r, r, 1), r broadcast per quad; kb collects the lanes it cannot do */
static inline Y scale8(Y v, Y r, Y* kb)
{
    const Y big = _mm256_and_si256(yge(yabs(r), B_RSQ), _mm256_setr_epi32(-1, -1, -1, 0, -1, -1, -1, 0));
    *kb = _mm256_or_si256(*kb, _mm256_andnot_si256(_mm256_cmpeq_epi32(yabs(v), Y0), big));
    return yFM(v, _mm256_blend_epi32(_mm256_andnot_si256(big, r), Y1(F1), 0x88));
}
/* x, y, z of eight quads (lanes: quads 0 2 4 6 | 1 3 5 7) */
static inline void ysoa(Y v01, Y v23, Y v45, Y v67, Y* X, Y* Yc, Y* Zc)
{
    const Y t0 = _mm256_unpacklo_epi32(v01, v23), t1 = _mm256_unpackhi_epi32(v01, v23);
    const Y t2 = _mm256_unpacklo_epi32(v45, v67), t3 = _mm256_unpackhi_epi32(v45, v67);
    *X = _mm256_unpacklo_epi64(t0, t2); *Yc = _mm256_unpackhi_epi64(t0, t2); *Zc = _mm256_unpacklo_epi64(t1, t3);
}
/* norm_group16 on eight vectors at a */
static NOINL int norm_group8(uint8_t* ls, uint32_t a)
{
    const Y v[4] = { yld(ls, a), yld(ls, a + 32), yld(ls, a + 64), yld(ls, a + 96) };
    const Y mi = _mm256_max_epu32(_mm256_max_epu32(yabs(v[0]), yabs(v[1])), _mm256_max_epu32(yabs(v[2]), yabs(v[3])));
    if (unlikely(yany(yge(mi, B_IN)))) return 0;
    Y X, Yc, Zc;
    ysoa(v[0], v[1], v[2], v[3], &X, &Yc, &Zc);
    const Y R = rsq8(yFMA(Zc, Zc, yFMA(Yc, Yc, yFM(X, X))));
    Y kb = Y0, o[4];
    o[0] = ycanon(scale8(v[0], _mm256_shuffle_epi32(R, 0x00), &kb));
    o[1] = ycanon(scale8(v[1], _mm256_shuffle_epi32(R, 0x55), &kb));
    o[2] = ycanon(scale8(v[2], _mm256_shuffle_epi32(R, 0xAA), &kb));
    o[3] = ycanon(scale8(v[3], _mm256_shuffle_epi32(R, 0xFF), &kb));
    if (unlikely(yany(kb))) return 0;
    for (int m = 0; m < 4; m++) yst(ls, a + 32 * m, o[m]);
    return 1;
}
/* normalize_ls; n8 - from is a multiple of 8 */
static void normalize_ls_y(uint8_t* ls, uint32_t p, uint32_t from, uint32_t n8, uint32_t csr)
{
    for (uint32_t k = from; k < n8; k += 8) {
        if (likely(norm_group8(ls, p + 16 * k))) continue;
        _mm_setcsr(csr);
        for (uint32_t j = 0; j < 8; j++) sq(ls, p + 16 * (k + j), norm_q(lq(ls, p + 16 * (k + j))));
        _mm_setcsr(CSR_FAST);
    }
}

/* ------------------------------------------------------------------ overlap guard */
/* w: 0 read, 1 written. A kernel whose job writes its outputs in separate
 * phases (one loop after another) may tag them with the phase, w = 2, 3, ...,
 * and write them in that order too: two written spans of different phases
 * may then overlap, because the later phase overwrites the earlier one in
 * both codes and the final bytes agree. Spans of one phase are written
 * interleaved, so they still may not overlap, nor may any written span and a
 * read one. */
typedef struct { uint32_t lo, hi; int w; } Span;
static int spans_ok(const Span* s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i].hi > SPU_LS_SIZE || s[i].lo > s[i].hi) return 0;
        for (int j = i + 1; j < n; j++)
            if ((s[i].w || s[j].w) && s[i].lo < s[j].hi && s[j].lo < s[i].hi &&
                !(s[i].w >= 2 && s[j].w >= 2 && s[i].w != s[j].w)) return 0;
    }
    return 1;
}

/* fm(cuflt(w, 155), 1/255) for every weight byte, through the helpers */
static uint32_t g_wtab[256];
static void wtab_init(void)
{
    for (int w = 0; w < 256; w += 4) {
        const Q r = spu_fm(spu_cuflt(V4(w, w + 1, w + 2, w + 3), 155), V4(INV255, INV255, INV255, INV255));
        for (int i = 0; i < 4; i++) g_wtab[w + i] = r._u32[i];
    }
}

/* ================================================================== 0xD0A8: skinning
 *
 * r3 the job context, r4 the matrix palette, r5 the number of matrices, r6
 * the per-vertex bone indexes and weights. RESEARCH.md section 2. */
typedef struct {
    uint32_t M, IW, pos, scr, s60, st[3];
    int nst, multi, r105, renorm;
    uint32_t maxi;
    const uint8_t* okm;   /* matrix m may take the fast path */
    const Z* pal;         /* matrix m in host order, quad 3 zero; 256..511 all zero */
} Skin;

/* one vertex, exactly: blend (or pick) the matrix, write it to the scratch,
 * transform the position and the streams */
static NOINL void skin_vertex_q(uint8_t* ls, const Skin* k, uint32_t v, uint32_t sv, uint32_t iw)
{
    Q A, B, C;
    if (k->multi) {
        const uint32_t a = (iw & LSQ) + 8 * v;
        const uint8_t* b = ls + a;
        Q w = spu_fm(spu_cuflt(V4(b[0], b[2], b[4], b[6]), 155), V4(INV255, INV255, INV255, INV255));
        Q q[3];
        for (int r = 0; r < 3; r++) {
            Q acc = spu_fm(spl(w, 0), lq(ls, k->M + 16 * r + 48 * b[1]));
            acc = spu_fma(spl(w, 1), lq(ls, k->M + 16 * r + 48 * b[3]), acc);
            acc = spu_fma(spl(w, 2), lq(ls, k->M + 16 * r + 48 * b[5]), acc);
            q[r] = spu_fma(spl(w, 3), lq(ls, k->M + 16 * r + 48 * b[7]), acc);
        }
        A = q[0]; B = q[1]; C = q[2];
    } else {
        const uint32_t a = iw + (v & ~1u);
        const uint32_t i = ls[(v & 1) ? ((a & ~15u) + ((a + 1) & 15)) & LSM : a & LSM];
        A = lq(ls, k->M + 48 * i); B = lq(ls, k->M + 16 + 48 * i); C = lq(ls, k->M + 32 + 48 * i);
    }
    const Q T = V4(A._u32[3], B._u32[3], C._u32[3], 0);
    A._u32[3] = F1; B._u32[3] = 0; C._u32[3] = 0;
    const Q p = lq(ls, k->pos + 16 * v);
    sq(ls, k->scr + 48 * sv, A); sq(ls, k->scr + 48 * sv + 16, B); sq(ls, k->scr + 48 * sv + 32, C);
    sq(ls, k->pos + 16 * v, spu_fma(zxy(p), C, spu_fma(yzx(p), B, spu_fma(p, A, T))));
    if (k->r105) {
        sq(ls, k->s60 + 16 * v, cofactor_q(lq(ls, k->s60 + 16 * v), A, B, C));
    }
    for (int s = 0; s < k->nst; s++) {
        sq(ls, k->st[s] + 16 * v, xform_q(lq(ls, k->st[s] + 16 * v), A, B, C));
    }
}

/* four vertices v..v+3 (the scratch slots sv..sv+3); 0 if the batch must go the exact way */
static inline __attribute__((always_inline)) int skin_batch16(uint8_t* ls, const Skin* k, uint32_t v, uint32_t sv, uint32_t iw,
                                                              const int MULTI, const int RENORM, const int R105, const int NST)
{
    Chk ck; chk_init(&ck);
    Z q[4];
    if (MULTI) {
        const uint8_t* b = ls + (iw & LSQ) + 8 * v;
        /* a bone of weight 0 takes matrix 256 + index, all zero (see the maxi scan) */
#define EIX(w, i) ((unsigned)(i) | (unsigned)((w) == 0) << 8)
        if (!(k->okm[EIX(b[0], b[1])] & k->okm[EIX(b[2], b[3])] & k->okm[EIX(b[4], b[5])] & k->okm[EIX(b[6], b[7])] &
              k->okm[EIX(b[8], b[9])] & k->okm[EIX(b[10], b[11])] & k->okm[EIX(b[12], b[13])] & k->okm[EIX(b[14], b[15])] &
              k->okm[EIX(b[16], b[17])] & k->okm[EIX(b[18], b[19])] & k->okm[EIX(b[20], b[21])] & k->okm[EIX(b[22], b[23])] &
              k->okm[EIX(b[24], b[25])] & k->okm[EIX(b[26], b[27])] & k->okm[EIX(b[28], b[29])] & k->okm[EIX(b[30], b[31])])) return 0;
#pragma GCC unroll 4
        for (int j = 0; j < 4; j++) {
            const uint8_t* bj = b + 8 * j;
            Z acc = FM(simde_mm512_set1_epi32(g_wtab[bj[0]]), k->pal[EIX(bj[0], bj[1])]);
            acc = FMA(simde_mm512_set1_epi32(g_wtab[bj[2]]), k->pal[EIX(bj[2], bj[3])], acc);
            acc = FMA(simde_mm512_set1_epi32(g_wtab[bj[4]]), k->pal[EIX(bj[4], bj[5])], acc);
            q[j] = canon(FMA(simde_mm512_set1_epi32(g_wtab[bj[6]]), k->pal[EIX(bj[6], bj[7])], acc));
        }
#undef EIX
    } else {
        /* the matrix as it is: no arithmetic, so no canonical form either */
#pragma GCC unroll 4
        for (int j = 0; j < 4; j++) {
            const uint32_t a = iw + ((v + j) & ~1u);
            const uint32_t i = ls[(j & 1) ? ((a & ~15u) + ((a + 1) & 15)) & LSM : a & LSM];
            if (!k->okm[i]) return 0;
            q[j] = k->pal[i];
        }
    }
    /* q[j] = (row0, row1, row2, 0) of vertex j. T = (row0.w, row1.w, row2.w, 0) per vertex. */
    const Z t01 = simde_mm512_permutex2var_epi32(q[0], simde_mm512_setr_epi32(3, 7, 11, 0, 19, 23, 27, 0, 0, 0, 0, 0, 0, 0, 0, 0), q[1]);
    const Z t23 = simde_mm512_permutex2var_epi32(q[2], simde_mm512_setr_epi32(0, 0, 0, 0, 0, 0, 0, 0, 3, 7, 11, 0, 19, 23, 27, 0), q[3]);
    const Z T = simde_mm512_maskz_mov_epi32(0x7777, simde_mm512_mask_mov_epi32(t01, 0xFF00, t23));
#pragma GCC unroll 4
    for (int j = 0; j < 4; j++) q[j] = simde_mm512_mask_mov_epi32(q[j], 0x0888, simde_mm512_setr_epi32(0, 0, 0, F1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    const Z a01 = simde_mm512_shuffle_i64x2(q[0], q[1], 0x44), a23 = simde_mm512_shuffle_i64x2(q[2], q[3], 0x44);
    const Z c01 = simde_mm512_shuffle_i64x2(q[0], q[1], 0xEE), c23 = simde_mm512_shuffle_i64x2(q[2], q[3], 0xEE);
    const Z A = simde_mm512_shuffle_i64x2(a01, a23, 0x88), B = simde_mm512_shuffle_i64x2(a01, a23, 0xDD), C = simde_mm512_shuffle_i64x2(c01, c23, 0x88);
    const Z p = chk(ld16(ls, k->pos + 16 * v), &ck);
    const Z po = canon(FMA(ZXY(p), C, FMA(YZX(p), B, FMA(p, A, T))));
    Z o[4];                                   /* the streams, then S60 */
#pragma GCC unroll 3
    for (int s = 0; s < NST; s++) {
        const Z x = chk(ld16(ls, k->st[s] + 16 * v), &ck);
        o[s] = FMA(ZXY(x), C, FMA(YZX(x), B, FM(x, A)));
    }
    if (R105) {
        const Z x = chk(ld16(ls, k->s60 + 16 * v), &ck);
        const Z d1 = FMS(YZX(A), ZXY(A), FM(ZXY(C), YZX(B)));
        const Z d2 = FMS(ZXY(B), YZX(B), FM(YZX(C), ZXY(A)));
        const Z d3 = FMS(YZX(C), ZXY(C), FM(ZXY(B), YZX(A)));
        o[NST] = FMA(d3, ZXY(x), FMA(d2, YZX(x), FM(d1, x)));
    }
    if (unlikely(chk_bad(&ck))) return 0;
    {   /* the scratch takes 12 consecutive quads: A0 B0 C0 A1 | B1 C1 A2 B2 | C2 A3 B3 C3 */
        uint8_t* d = ls + ((k->scr + 48 * sv) & LSQ);
        simde_mm512_storeu_si512(d,       bswap16(simde_mm512_permutex2var_epi64(q[0], simde_mm512_setr_epi64(0, 1, 2, 3, 4, 5, 8, 9), q[1])));
        simde_mm512_storeu_si512(d + 64,  bswap16(simde_mm512_permutex2var_epi64(q[1], simde_mm512_setr_epi64(2, 3, 4, 5, 8, 9, 10, 11), q[2])));
        simde_mm512_storeu_si512(d + 128, bswap16(simde_mm512_permutex2var_epi64(q[2], simde_mm512_setr_epi64(4, 5, 8, 9, 10, 11, 12, 13), q[3])));
    }
    st16(ls, k->pos + 16 * v, po);
#pragma GCC unroll 3
    for (int s = 0; s < NST; s++) st16(ls, k->st[s] + 16 * v, canon(o[s]));
    if (R105) st16(ls, k->s60 + 16 * v, canon(o[NST]));
    return 1;
}

/* the exact path for four vertices, at the caller's MXCSR */
static NOINL void skin_slow4(uint8_t* ls, const Skin* k, uint32_t v, uint32_t sv, uint32_t iw, uint32_t csr)
{
    EDGE_STAT(1);
    _mm_setcsr(csr);
    for (uint32_t t = 0; t < 4; t++) skin_vertex_q(ls, k, v + t, sv + t, iw);
    _mm_setcsr(CSR_FAST);
}
/* every chunk; MXCSR is CSR_FAST (set by the caller: nothing in here writes
 * it). One instance per kind of call, so the batch keeps its streams in registers. */
static inline __attribute__((always_inline)) void skin_run_t(uint8_t* ls, const Skin* k, uint32_t n4, uint32_t chunk, uint32_t iw, uint32_t csr,
                                                             const int MULTI, const int RENORM, const int R105, const int NST)
{
    for (uint32_t rem = n4, base = 0; rem; ) {
        const uint32_t cnt = rem > chunk ? chunk : rem;
        for (uint32_t j = 0; j < cnt; j += 4)
            if (unlikely(!skin_batch16(ls, k, base + j, j, iw, MULTI, RENORM, R105, NST))) skin_slow4(ls, k, base + j, j, iw, csr);
        base += cnt;
        rem -= cnt;
    }
}
typedef void (*SkinRun)(uint8_t*, const Skin*, uint32_t, uint32_t, uint32_t, uint32_t);
#define SKIN_RUN(m, r, x, n) \
    static NOINL void skin_run_##m##r##x##n(uint8_t* ls, const Skin* k, uint32_t n4, uint32_t c, uint32_t iw, uint32_t csr) \
    { skin_run_t(ls, k, n4, c, iw, csr, m, r, x, n); }
#define SKIN_RUN4(m, r, x) SKIN_RUN(m, r, x, 0) SKIN_RUN(m, r, x, 1) SKIN_RUN(m, r, x, 2) SKIN_RUN(m, r, x, 3)
SKIN_RUN4(1, 0, 0) SKIN_RUN4(1, 1, 0) SKIN_RUN4(1, 1, 1) SKIN_RUN4(0, 0, 0) SKIN_RUN4(0, 1, 0) SKIN_RUN4(0, 1, 1)
#define SKIN_ROW(m, r, x) { skin_run_##m##r##x##0, skin_run_##m##r##x##1, skin_run_##m##r##x##2, skin_run_##m##r##x##3 }
/* [multi][renorm][r105][nst]; r105 without renorm does not occur (types 3 and 6 renormalise) */
static const SkinRun k_skin_run[2][2][2][4] = {
    { { SKIN_ROW(0, 0, 0), SKIN_ROW(0, 0, 0) }, { SKIN_ROW(0, 1, 0), SKIN_ROW(0, 1, 1) } },
    { { SKIN_ROW(1, 0, 0), SKIN_ROW(1, 0, 0) }, { SKIN_ROW(1, 1, 0), SKIN_ROW(1, 1, 1) } },
};

/* skin_batch16 for AVX2: the four vertices as two pairs. pal[m] is read as two registers,
 * (row0 | row1) and (row2 | 0). Everything is computed before anything is stored. */
static inline __attribute__((always_inline)) int skin_batch4_y(uint8_t* ls, const Skin* k, uint32_t v, uint32_t sv, uint32_t iw,
                                                               const int MULTI, const int RENORM, const int R105, const int NST)
{
    const Y* pal = (const Y*)k->pal;
    Y lo[4], hi[4];
    if (MULTI) {
        const uint8_t* b = ls + (iw & LSQ) + 8 * v;
#define EIX(w, i) ((unsigned)(i) | (unsigned)((w) == 0) << 8)
        if (!(k->okm[EIX(b[0], b[1])] & k->okm[EIX(b[2], b[3])] & k->okm[EIX(b[4], b[5])] & k->okm[EIX(b[6], b[7])] &
              k->okm[EIX(b[8], b[9])] & k->okm[EIX(b[10], b[11])] & k->okm[EIX(b[12], b[13])] & k->okm[EIX(b[14], b[15])] &
              k->okm[EIX(b[16], b[17])] & k->okm[EIX(b[18], b[19])] & k->okm[EIX(b[20], b[21])] & k->okm[EIX(b[22], b[23])] &
              k->okm[EIX(b[24], b[25])] & k->okm[EIX(b[26], b[27])] & k->okm[EIX(b[28], b[29])] & k->okm[EIX(b[30], b[31])])) return 0;
#pragma GCC unroll 4
        for (int j = 0; j < 4; j++) {
            const uint8_t* bj = b + 8 * j;
            const unsigned e0 = 2 * EIX(bj[0], bj[1]), e1 = 2 * EIX(bj[2], bj[3]), e2 = 2 * EIX(bj[4], bj[5]), e3 = 2 * EIX(bj[6], bj[7]);
            const Y w0 = Y1(g_wtab[bj[0]]), w1 = Y1(g_wtab[bj[2]]), w2 = Y1(g_wtab[bj[4]]), w3 = Y1(g_wtab[bj[6]]);
            lo[j] = ycanon(yFMA(w3, pal[e3], yFMA(w2, pal[e2], yFMA(w1, pal[e1], yFM(w0, pal[e0])))));
            hi[j] = ycanon(yFMA(w3, pal[e3 + 1], yFMA(w2, pal[e2 + 1], yFMA(w1, pal[e1 + 1], yFM(w0, pal[e0 + 1])))));
        }
#undef EIX
    } else {
#pragma GCC unroll 4
        for (int j = 0; j < 4; j++) {
            const uint32_t a = iw + ((v + j) & ~1u);
            const uint32_t i = ls[(j & 1) ? ((a & ~15u) + ((a + 1) & 15)) & LSM : a & LSM];
            if (!k->okm[i]) return 0;
            lo[j] = pal[2 * i]; hi[j] = pal[2 * i + 1];
        }
    }
    Y A[2], B[2], C[2], T[2], po[2], o[4][2];
    Y mi = Y0;
#pragma GCC unroll 2
    for (int h = 0; h < 2; h++) {
        const Y a = _mm256_permute2x128_si256(lo[2 * h], lo[2 * h + 1], 0x20);
        const Y bq = _mm256_permute2x128_si256(lo[2 * h], lo[2 * h + 1], 0x31);
        const Y c = _mm256_permute2x128_si256(hi[2 * h], hi[2 * h + 1], 0x20);
        T[h] = _mm256_unpackhi_epi64(_mm256_unpackhi_epi32(a, bq), _mm256_unpackhi_epi32(c, Y0));
        A[h] = _mm256_blend_epi32(a, Y1(F1), 0x88); B[h] = _mm256_blend_epi32(bq, Y0, 0x88); C[h] = _mm256_blend_epi32(c, Y0, 0x88);
        const Y p = yld(ls, k->pos + 16 * v + 32 * h);
        mi = _mm256_max_epu32(mi, yabs(p));
        po[h] = ycanon(yFMA(_mm256_shuffle_epi32(p, 0xD2), C[h], yFMA(_mm256_shuffle_epi32(p, 0xC9), B[h], yFMA(p, A[h], T[h]))));
#pragma GCC unroll 3
        for (int s = 0; s < NST; s++) {
            const Y x = yld(ls, k->st[s] + 16 * v + 32 * h);
            mi = _mm256_max_epu32(mi, yabs(x));
            o[s][h] = yFMA(_mm256_shuffle_epi32(x, 0xD2), C[h], yFMA(_mm256_shuffle_epi32(x, 0xC9), B[h], yFM(x, A[h])));
        }
        if (R105) {
            const Y x = yld(ls, k->s60 + 16 * v + 32 * h);
            mi = _mm256_max_epu32(mi, yabs(x));
#define YZX8(q) _mm256_shuffle_epi32((q), 0xC9)
#define ZXY8(q) _mm256_shuffle_epi32((q), 0xD2)
            const Y d1 = yFMS(YZX8(A[h]), ZXY8(A[h]), yFM(ZXY8(C[h]), YZX8(B[h])));
            const Y d2 = yFMS(ZXY8(B[h]), YZX8(B[h]), yFM(YZX8(C[h]), ZXY8(A[h])));
            const Y d3 = yFMS(YZX8(C[h]), ZXY8(C[h]), yFM(ZXY8(B[h]), YZX8(A[h])));
            o[NST][h] = yFMA(d3, ZXY8(x), yFMA(d2, YZX8(x), yFM(d1, x)));
#undef YZX8
#undef ZXY8
        }
    }
    if (unlikely(yany(yge(mi, B_IN)))) return 0;
#pragma GCC unroll 2
    for (int h = 0; h < 2; h++) {
        /* the scratch: A0 B0 | C0 A1 | B1 C1 per pair */
        const uint32_t d = k->scr + 48 * sv + 96 * h;
        yst(ls, d, _mm256_permute2x128_si256(A[h], B[h], 0x20));
        yst(ls, d + 32, _mm256_permute2x128_si256(C[h], A[h], 0x30));
        yst(ls, d + 64, _mm256_permute2x128_si256(B[h], C[h], 0x31));
        yst(ls, k->pos + 16 * v + 32 * h, po[h]);
#pragma GCC unroll 3
        for (int s = 0; s < NST; s++) yst(ls, k->st[s] + 16 * v + 32 * h, ycanon(o[s][h]));
        if (R105) yst(ls, k->s60 + 16 * v + 32 * h, ycanon(o[NST][h]));
    }
    return 1;
}
static inline __attribute__((always_inline)) void skin_run_y_t(uint8_t* ls, const Skin* k, uint32_t n4, uint32_t chunk, uint32_t iw, uint32_t csr,
                                                               const int MULTI, const int RENORM, const int R105, const int NST)
{
    for (uint32_t rem = n4, base = 0; rem; ) {
        const uint32_t cnt = rem > chunk ? chunk : rem;
        for (uint32_t j = 0; j < cnt; j += 4)
            if (unlikely(!skin_batch4_y(ls, k, base + j, j, iw, MULTI, RENORM, R105, NST))) skin_slow4(ls, k, base + j, j, iw, csr);
        base += cnt;
        rem -= cnt;
    }
}
#define SKIN_RUN_Y(m, r, x, n) \
    static NOINL void skin_run_y_##m##r##x##n(uint8_t* ls, const Skin* k, uint32_t n4, uint32_t c, uint32_t iw, uint32_t csr) \
    { skin_run_y_t(ls, k, n4, c, iw, csr, m, r, x, n); }
#define SKIN_RUN_Y4(m, r, x) SKIN_RUN_Y(m, r, x, 0) SKIN_RUN_Y(m, r, x, 1) SKIN_RUN_Y(m, r, x, 2) SKIN_RUN_Y(m, r, x, 3)
SKIN_RUN_Y4(1, 0, 0) SKIN_RUN_Y4(1, 1, 0) SKIN_RUN_Y4(1, 1, 1) SKIN_RUN_Y4(0, 0, 0) SKIN_RUN_Y4(0, 1, 0) SKIN_RUN_Y4(0, 1, 1)
#define SKIN_ROW_Y(m, r, x) { skin_run_y_##m##r##x##0, skin_run_y_##m##r##x##1, skin_run_y_##m##r##x##2, skin_run_y_##m##r##x##3 }
static const SkinRun k_skin_run_y[2][2][2][4] = {
    { { SKIN_ROW_Y(0, 0, 0), SKIN_ROW_Y(0, 0, 0) }, { SKIN_ROW_Y(0, 1, 0), SKIN_ROW_Y(0, 1, 1) } },
    { { SKIN_ROW_Y(1, 0, 0), SKIN_ROW_Y(1, 0, 0) }, { SKIN_ROW_Y(1, 1, 0), SKIN_ROW_Y(1, 1, 1) } },
};

static int edge_skin_core(u128* gpr, uint8_t* ls)
{
    const uint32_t S = gpr[3]._u32[0], M = gpr[4]._u32[0], NM = gpr[5]._u32[0], IW = gpr[6]._u32[0];
    const uint32_t sp = gpr[1]._u32[0], sp2 = sp - 816;
#ifdef EDGE_HARNESS
    if (unlikely(!g_wtab[255])) { rsq_init(); wtab_init(); }
#endif
    if ((sp & 15) || sp < 0x400 || sp + 0x20 > SPU_LS_SIZE || (S & 15) || S + 0x90 > SPU_LS_SIZE) return 0;
    const Q s0 = lq(ls, S);
    const uint8_t b0 = s0._u32[0] >> 24, type = (s0._u32[1] >> 8) & 15, fmt = s0._u32[1] & 0xFF;
    const int body = !(b0 & 0x10) && NM && type;
    Skin k; memset(&k, 0, sizeof k);
    uint32_t n4 = 0, chunk = 4, nm_conv = 0, stride = 48;
    if (body) {
        if (!(b0 & 0x80)) return 0;                       /* the job's "no extra uniform table" error */
        const int nonuni = type == 3 || type == 6;
        const uint32_t s60 = be32(ls + S + 0x60);
        int n = s60 == 0 ? 0 : (nonuni ? 0 : 1);
        uint32_t ptr[9]; ptr[0] = s60;
        for (int i = 0; i < 4; i++) {
            const uint32_t a = word_at(ls, S + 4 * i + 100), b = word_at(ls, S + 4 * i + 116);
            ptr[n] = a; n += a != 0;
            ptr[n] = b; n += b != 0;
        }
        if (n > 3) return 0;                              /* "more than 3 uniforms" error */
        const uint32_t numv = (s0._u32[2] >> 16) & 0xFFFF;
        if (!numv) return 0;
        k.nst = n; for (int i = 0; i < n; i++) k.st[i] = ptr[i];
        k.r105 = nonuni && s60 != 0; k.s60 = s60;
        k.multi = (uint8_t)(type - 4) > 2;
        k.renorm = !(type == 4 || type == 1);
        k.M = M & LSQ; k.IW = IW; k.pos = word_at(ls, S + 0x5C);
        n4 = (numv + 3) & ~3u;
        k.scr = sp2 + 176;
        if (n4 > 11) {
            const uint32_t r39 = 4 * (b0 & 15) + S;
            k.scr = word_at(ls, r39 + 16);
            chunk = (uint32_t)(((uint64_t)n4 * 0xAAAAAAABull) >> 33) & ~3u;
        }
        if (fmt == 2) { nm_conv = NM; stride = 64; }
        else if (fmt <= 1) { nm_conv = NM; stride = fmt ? 64 : 48; }
        /* every buffer, for the overlap guard; the fast path also wants them 16-aligned */
        if ((k.pos | k.scr | k.s60 | k.st[0] | k.st[1] | k.st[2]) & 15) return 0;
        uint32_t maxi = 0;
        if (k.multi) {
            /* odd bytes of 8 per vertex, where the weight before them is not
             * zero: a zero weight makes the matrix irrelevant (fm(+0, x) is +0
             * and fma(+0, x, a) is fma(+0, 0, a) for every x, checked against
             * the helpers), and real meshes leave garbage indexes there */
            const uint8_t* p = ls + (IW & LSQ); __m256i m = _mm256_setzero_si256();
            for (uint32_t i = 0; i < 8 * n4; i += 32) {
                const __m256i h = _mm256_loadu_si256((const __m256i*)(p + i));
                m = cpu_mm256_mask_max_epu16(m, cpu_mm256_test_epi16_mask(h, _mm256_set1_epi16(0xFF)), m, _mm256_and_si256(h, _mm256_set1_epi16((short)0xFF00)));
            }
            uint16_t t[16]; _mm256_storeu_si256((__m256i*)t, m);
            for (int i = 0; i < 16; i++) maxi = t[i] > maxi ? t[i] : maxi;
            maxi >>= 8;
        } else if (!(IW & 1)) {                           /* bytes IW .. IW+n4-1 */
            for (uint32_t i = 0; i < n4; i++) { uint32_t x = ls[(IW + i) & LSM]; maxi = x > maxi ? x : maxi; }
        } else for (uint32_t i = 0; i < n4; i += 2) {   /* the pair reads wrap inside a quad */
            const uint32_t a = IW + i, x = ls[a & LSM], y = ls[((a & ~15u) + ((a + 1) & 15)) & LSM];
            maxi = x > maxi ? x : maxi; maxi = y > maxi ? y : maxi;
        }
        const uint32_t n8 = (numv + 7) & ~7u, nn = k.renorm ? n8 : n4;
        Span sp_[12]; int ns = 0;
        sp_[ns++] = (Span){ sp2, sp + 0x20, 1 };
        sp_[ns++] = (Span){ S, S + 0x90, 1 };
        /* the palette: converted in place, then read as 48-byte matrices. The
         * fast path loads 64 bytes per matrix and drops the last 16, so those
         * only have to be inside the LS (the next buffer usually starts there). */
        const uint32_t mhi = k.M + 48 * (maxi + 1), mc = k.M + stride * nm_conv;
        if (mhi + 16 > SPU_LS_SIZE) return 0;
        /* span 2: the conversion; span 3: what the blend reads, which can run
         * past the palette (padding vertexes carry garbage indexes) but is only read */
        sp_[ns++] = (Span){ k.M, mc > k.M ? mc : k.M + 1, nm_conv != 0 };
        sp_[ns++] = (Span){ k.M, mhi, 0 };
        sp_[ns++] = (Span){ IW & LSQ, k.multi ? (IW & LSQ) + 8 * n4 : IW + n4 + 2, 0 };
        sp_[ns++] = (Span){ k.pos, k.pos + 16 * n4, 1 };
        if (k.scr != sp2 + 176) sp_[ns++] = (Span){ k.scr, k.scr + 48 * chunk + 16, 1 };
        else if (k.scr + 48 * chunk > sp + 0x20) return 0;
        if (k.r105) sp_[ns++] = (Span){ k.s60, k.s60 + 16 * nn, 1 };
        for (int i = 0; i < k.nst; i++) sp_[ns++] = (Span){ k.st[i], k.st[i] + 16 * nn, 1 };
        /* every pair but the palette's two spans with each other */
        for (int i = 0; i < ns; i++)
            for (int j = i + 1; j < ns; j++) {
                if (i == 2 && j == 3) continue;
                const Span pr[2] = { sp_[i], sp_[j] };
                if (!spans_ok(pr, 2)) return 0;
            }
        k.maxi = maxi;
    }

    /* prologue: callee-saved registers, the link, the back chain */
    for (int r = 80; r <= 107; r++) sq(ls, sp - 16 * (r - 79), gpr[r]);
    sq(ls, sp + 0x10, gpr[0]);
    sq(ls, sp2, gpr[1]);
    if (body) {
        /* the palette into the job's layout, in place. Words only move, so
         * they move in LS byte order. */
        for (uint32_t m = 0; m < nm_conv; m++) {
            uint8_t* src = ls + k.M + stride * m;
            uint8_t* dst = ls + k.M + 48 * m;
            const __m128i a = _mm_loadu_si128((const __m128i*)src), b = _mm_loadu_si128((const __m128i*)(src + 16)),
                          c = _mm_loadu_si128((const __m128i*)(src + 32));
            __m128i q0, q1, q2;
            if (fmt == 2) {   /* (a0 b1 c2 d0) (b0 c1 a2 d1) (c0 a1 b2 d2) */
                const __m128i d = _mm_loadu_si128((const __m128i*)(src + 48));
                q0 = _mm_blend_epi32(_mm_blend_epi32(a, b, 2), _mm_blend_epi32(c, _mm_shuffle_epi32(d, 0x00), 8), 12);
                q1 = _mm_blend_epi32(_mm_blend_epi32(b, c, 2), _mm_blend_epi32(a, _mm_shuffle_epi32(d, 0x55), 8), 12);
                q2 = _mm_blend_epi32(_mm_blend_epi32(c, a, 2), _mm_blend_epi32(b, _mm_shuffle_epi32(d, 0xAA), 8), 12);
            } else {          /* (a0 b1 c2 a3) (a1 b2 c0 b3) (a2 b0 c1 c3) */
                q0 = _mm_blend_epi32(_mm_blend_epi32(a, b, 2), c, 4);
                q1 = _mm_blend_epi32(_mm_blend_epi32(_mm_shuffle_epi32(a, 0x01), _mm_shuffle_epi32(b, 0xE8), 10), _mm_shuffle_epi32(c, 0x00), 4);
                q2 = _mm_blend_epi32(_mm_blend_epi32(_mm_shuffle_epi32(a, 0x02), _mm_shuffle_epi32(b, 0x00), 2), _mm_shuffle_epi32(c, 0xD4), 12);
            }
            _mm_storeu_si128((__m128i*)dst, q0); _mm_storeu_si128((__m128i*)(dst + 16), q1); _mm_storeu_si128((__m128i*)(dst + 32), q2);
        }
        /* the stream table the job keeps on its stack */
        {
            int n = k.s60 == 0 ? 0 : ((type == 3 || type == 6) ? 0 : 1);
            sq(ls, sp2 + 0x20, lq(ls, S + 0x60));
            for (int i = 0; i < 4; i++) {
                const uint32_t a = S + 4 * i + 100, b = S + 4 * i + 116;
                const Q qa = quad_at(ls, a); sq(ls, sp2 + 32 + 16 * n, qa); n += qa._u32[0] != 0;
                const Q qb = quad_at(ls, b); sq(ls, sp2 + 32 + 16 * n, qb); n += qb._u32[0] != 0;
            }
        }
        /* the palette in host order for the fast path, and which matrices it may take */
        /* 256..511 stay zero. On the heap, 64-byte aligned: Windows does not promise a
         * thread-local block more than 16 (cpu agent) */
        static _Thread_local Z* pal;
        if (unlikely(!pal)) {
            if (!(pal = (Z*)_aligned_malloc(512 * sizeof(Z), 64))) abort();   /* LS already written: no decline here */
            memset(pal, 0, 512 * sizeof(Z));
        }
        uint8_t okm[512];
        memset(okm + 256, 1, 256);
        if (g_avx2) {
            /* the same 64 bytes per matrix: rows 0..2 in host order, then a zero quad */
            Y* py = (Y*)pal;
            for (uint32_t m = 0; m <= k.maxi; m++) {
                const uint8_t* src = ls + k.M + 48 * m;
                const Y lo = ybswap(_mm256_loadu_si256((const __m256i*)src));
                const Y hi = _mm256_zextsi128_si256(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(src + 32)),
                                                                     _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
                _mm256_store_si256(py + 2 * m, lo); _mm256_store_si256(py + 2 * m + 1, hi);
                okm[m] = !yany(yge(_mm256_max_epu32(yabs(lo), yabs(hi)), B_IN));
            }
        } else
        for (uint32_t m = 0; m <= k.maxi; m++) {
            Chk ck; chk_init(&ck);
            pal[m] = chk(ld12(ls + k.M + 48 * m), &ck);
            okm[m] = !chk_bad(&ck);
        }
        k.okm = okm; k.pal = pal;
        const uint32_t csr = _mm_getcsr();
        _mm_setcsr(CSR_FAST);
        (g_avx2 ? k_skin_run_y : k_skin_run)[k.multi][k.renorm][k.r105][k.nst](ls, &k, n4, chunk, IW, csr);
        if (k.renorm) {
            const uint32_t numv = (be32(ls + S + 8) >> 16) & 0xFFFF, n8 = (numv + 7) & ~7u;
            /* 0x6E98 over each stream, as the job does once all chunks are skinned */
            void (*const norm)(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t) = g_avx2 ? normalize_ls_y : normalize_ls;
            for (int s = k.nst - 1; s >= 0; s--) norm(ls, k.st[s], 0, n8, csr);
            if (k.r105) norm(ls, k.s60, 0, n8, csr);
        }
        _mm_setcsr(csr);
        {
            Q q = lq(ls, S + 0x50);
            q._u32[((sp2 + 8) >> 2) & 3] = M > IW ? IW : M;
            sq(ls, S + 0x50, q);
        }
    }
    /* epilogue */
    for (int r = 80; r <= 107; r++) gpr[r] = lq(ls, sp - 16 * (r - 79));
    gpr[0] = lq(ls, sp + 0x10);
    return 1;
}


/* ================================================================== 0xA3C8: transform for culling
 *
 * r3 the job context. Each position goes through the world matrix (S+0xB0,
 * 3x4) and the viewport info's 4x4 matrix (S+0xF0), is divided by w (frest,
 * fi, one Newton step) and mapped to the screen (S+0x130 scales, S+0x140
 * offsets). The scratch (the extra uniform table) gets (sx, sy, code, 0) per
 * vertex, code the outcode bits (RESEARCH.md section 3). */
static const uint32_t k_rcp_lut[32] = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07
};
static inline Z frest16(Z a)
{
    const Z e = simde_mm512_and_si512(simde_mm512_srli_epi32(a, 23), simde_mm512_set1_epi32(0xFF));
    const Z f = simde_mm512_permutex2var_epi32(cpu_mm512_loadu_si512(k_rcp_lut), simde_mm512_srli_epi32(a, 18), cpu_mm512_loadu_si512(k_rcp_lut + 16));
    Z rexp = simde_mm512_maskz_sub_epi32(cpu_mm512_cmplt_epu32_mask(e, simde_mm512_set1_epi32(253)), simde_mm512_set1_epi32(253), e);
    rexp = simde_mm512_mask_mov_epi32(rexp, cpu_mm512_testn_epi32_mask(e, e), simde_mm512_set1_epi32(255));
    return simde_mm512_or_si512(simde_mm512_and_si512(a, simde_mm512_set1_epi32((int)0x80000000u)),
                           simde_mm512_or_si512(simde_mm512_slli_epi32(rexp, 23), simde_mm512_and_si512(f, simde_mm512_set1_epi32(0x7FFFFF))));
}
/* fcgt's order: exponent-0 values are 0, the rest order as sign-magnitude */
static inline Z fkey16(Z x)
{
    const Z m = simde_mm512_maskz_and_epi32(simde_mm512_test_epi32_mask(x, simde_mm512_set1_epi32(0x7F800000)), x, simde_mm512_set1_epi32(0x7FFFFFFF));
    return simde_mm512_mask_sub_epi32(m, simde_mm512_movepi32_mask(x), simde_mm512_setzero_si512(), m);
}
static inline int32_t fkey(uint32_t x) { int32_t m = (x & 0x7F800000u) ? (int32_t)(x & 0x7FFFFFFFu) : 0; return (x >> 31) ? -m : m; }

typedef struct {
    uint32_t pos, scr, n4;
    Q P[4], sc, of;                 /* P = view-projection x world, exact */
    uint32_t x0, y0, x1, y1, dmin, dmax;
} Xf;

/* vertices 4g..4g+3, exactly as the job computes them (its own lane layout) */
static NOINL void xf_group_q(uint8_t* ls, const Xf* k, uint32_t g)
{
    Q v[4];
    for (int j = 0; j < 4; j++) v[j] = lq(ls, k->pos + 64 * g + 16 * j);
    const Q X = V4(v[0]._u32[0], v[1]._u32[0], v[2]._u32[0], v[3]._u32[0]);
    const Q Y = V4(v[0]._u32[1], v[1]._u32[1], v[2]._u32[1], v[3]._u32[1]);
    const Q Zc = V4(v[0]._u32[2], v[1]._u32[2], v[2]._u32[2], v[3]._u32[2]);
    Q c[4];
    for (int r = 0; r < 4; r++)
        c[r] = spu_fma(Zc, spl(k->P[r], 2), spu_fma(Y, spl(k->P[r], 1), spu_fma(X, spl(k->P[r], 0), spl(k->P[r], 3))));
    const Q W = c[3];
    const Q r10 = spu_fi(W, spu_frest(W));
    const Q iw = spu_fma(spu_fnms(r10, W, V4(F1, F1, F1, F1)), r10, r10);
    Q o[3];
    for (int i = 0; i < 3; i++) {
        Q t = spu_fm(c[i], iw);
        for (int j = 0; j < 4; j++) t._u32[j] ^= W._u32[j] & 0x80000000u;
        o[i] = spu_fma(t, spl(k->sc, i), spl(k->of, i));
    }
    for (int j = 0; j < 4; j++) {
        const int32_t sx = fkey(o[0]._u32[j]), sy = fkey(o[1]._u32[j]), sz = fkey(o[2]._u32[j]);
        const uint32_t code = (sx > fkey(k->x1)) << 7 | (fkey(k->x0) > sx) << 6 | (sy > fkey(k->y1)) << 5 | (fkey(k->y0) > sy) << 4 |
                              (sz > fkey(k->dmax)) << 3 | (fkey(k->dmin) > sz) << 2 | (fkey(W._u32[j]) > 0) << 1;
        sq(ls, k->scr + 64 * g + 16 * j, V4(o[0]._u32[j], o[1]._u32[j], code, 0));
    }
}

/* sixteen vertices from 16*b (n of them valid, a multiple of 4); 0: do them exactly */
static inline __attribute__((always_inline)) int xf_batch16(uint8_t* ls, const Xf* k, uint32_t b, uint32_t n)
{
    const simde__mmask16 lanes = (simde__mmask16)(n >= 16 ? 0xFFFF : (1u << n) - 1);
    Z V[4];
    for (int m = 0; m < 4; m++) V[m] = 4 * m < (int)n ? ld16(ls, k->pos + 256 * b + 64 * m) : simde_mm512_setzero_si512();
    Z C[3];
    for (int c = 0; c < 3; c++) {
        const Z ix = simde_mm512_add_epi32(simde_mm512_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28, 0, 4, 8, 12, 16, 20, 24, 28), simde_mm512_set1_epi32(c));
        C[c] = simde_mm512_mask_mov_epi32(simde_mm512_permutex2var_epi32(V[0], ix, V[1]), 0xFF00, simde_mm512_permutex2var_epi32(V[2], ix, V[3]));
    }
    Z mi = simde_mm512_setzero_si512(), mo = simde_mm512_setzero_si512();
    for (int c = 0; c < 3; c++) mi = simde_mm512_mask_max_epu32(mi, lanes, mi, simde_mm512_and_si512(C[c], simde_mm512_set1_epi32(0x7FFFFFFF)));
    Z c[4];
    for (int r = 0; r < 4; r++)
        c[r] = FMA(C[2], simde_mm512_set1_epi32(k->P[r]._u32[2]), FMA(C[1], simde_mm512_set1_epi32(k->P[r]._u32[1]),
                   FMA(C[0], simde_mm512_set1_epi32(k->P[r]._u32[0]), simde_mm512_set1_epi32(k->P[r]._u32[3]))));
    const Z W = canon(c[3]);
    const Z r10 = fi16(W, frest16(W));
    const Z iw = FMA(zi(simde_mm512_fnmadd_ps(zf(r10), zf(W), simde_mm512_set1_ps(1.0f))), r10, r10);
    mo = simde_mm512_mask_max_epu32(mo, lanes, mo, simde_mm512_and_si512(r10, simde_mm512_set1_epi32(0x7FFFFFFF)));
    mo = simde_mm512_mask_max_epu32(mo, lanes, mo, simde_mm512_and_si512(iw, simde_mm512_set1_epi32(0x7FFFFFFF)));
    const Z sgn = simde_mm512_and_si512(W, simde_mm512_set1_epi32((int)0x80000000u));
    Z o[3];
    for (int i = 0; i < 3; i++) {
        const Z t = simde_mm512_xor_si512(canon(FM(c[i], iw)), sgn);
        o[i] = canon(FMA(t, simde_mm512_set1_epi32(k->sc._u32[i]), simde_mm512_set1_epi32(k->of._u32[i])));
        mo = simde_mm512_mask_max_epu32(mo, lanes, mo, simde_mm512_and_si512(t, simde_mm512_set1_epi32(0x7FFFFFFF)));
        mo = simde_mm512_mask_max_epu32(mo, lanes, mo, simde_mm512_and_si512(o[i], simde_mm512_set1_epi32(0x7FFFFFFF)));
    }
    if (unlikely((simde_mm512_cmpge_epu32_mask(mi, simde_mm512_set1_epi32(0x5D800000)) | simde_mm512_cmpge_epu32_mask(mo, simde_mm512_set1_epi32(0x7F7FFFFF))) != 0)) return 0;
    const Z kx = fkey16(o[0]), ky = fkey16(o[1]), kz = fkey16(o[2]);
    Z code = simde_mm512_maskz_mov_epi32(simde_mm512_cmpgt_epi32_mask(kx, simde_mm512_set1_epi32(fkey(k->x1))), simde_mm512_set1_epi32(0x80));
    code = simde_mm512_mask_or_epi32(code, simde_mm512_cmpgt_epi32_mask(simde_mm512_set1_epi32(fkey(k->x0)), kx), code, simde_mm512_set1_epi32(0x40));
    code = simde_mm512_mask_or_epi32(code, simde_mm512_cmpgt_epi32_mask(ky, simde_mm512_set1_epi32(fkey(k->y1))), code, simde_mm512_set1_epi32(0x20));
    code = simde_mm512_mask_or_epi32(code, simde_mm512_cmpgt_epi32_mask(simde_mm512_set1_epi32(fkey(k->y0)), ky), code, simde_mm512_set1_epi32(0x10));
    code = simde_mm512_mask_or_epi32(code, simde_mm512_cmpgt_epi32_mask(kz, simde_mm512_set1_epi32(fkey(k->dmax))), code, simde_mm512_set1_epi32(0x08));
    code = simde_mm512_mask_or_epi32(code, simde_mm512_cmpgt_epi32_mask(simde_mm512_set1_epi32(fkey(k->dmin)), kz), code, simde_mm512_set1_epi32(0x04));
    code = simde_mm512_mask_or_epi32(code, simde_mm512_cmpgt_epi32_mask(fkey16(W), simde_mm512_setzero_si512()), code, simde_mm512_set1_epi32(0x02));
    /* back to one quad per vertex: q[i] block j = vertex 4j+i, then a 4x4 block transpose */
    const Z xy_lo = simde_mm512_unpacklo_epi32(o[0], o[1]), xy_hi = simde_mm512_unpackhi_epi32(o[0], o[1]);
    const Z c_lo = simde_mm512_unpacklo_epi32(code, simde_mm512_setzero_si512()), c_hi = simde_mm512_unpackhi_epi32(code, simde_mm512_setzero_si512());
    const Z q0 = simde_mm512_unpacklo_epi64(xy_lo, c_lo), q1 = simde_mm512_unpackhi_epi64(xy_lo, c_lo);
    const Z q2 = simde_mm512_unpacklo_epi64(xy_hi, c_hi), q3 = simde_mm512_unpackhi_epi64(xy_hi, c_hi);
    const Z t0 = simde_mm512_shuffle_i64x2(q0, q1, 0x44), t1 = simde_mm512_shuffle_i64x2(q2, q3, 0x44);
    const Z t2 = simde_mm512_shuffle_i64x2(q0, q1, 0xEE), t3 = simde_mm512_shuffle_i64x2(q2, q3, 0xEE);
    const Z out[4] = { simde_mm512_shuffle_i64x2(t0, t1, 0x88), simde_mm512_shuffle_i64x2(t0, t1, 0xDD),
                       simde_mm512_shuffle_i64x2(t2, t3, 0x88), simde_mm512_shuffle_i64x2(t2, t3, 0xDD) };
    for (int m = 0; 4 * m < (int)n && m < 4; m++) st16(ls, k->scr + 256 * b + 64 * m, out[m]);
    return 1;
}

/* the exact path for a batch, at the caller's MXCSR */
static NOINL void xf_slow(uint8_t* ls, const Xf* k, uint32_t b, uint32_t n, uint32_t csr)
{
    EDGE_STAT(3);
    _mm_setcsr(csr);
    for (uint32_t g = 0; g < n / 4; g++) xf_group_q(ls, k, 4 * b + g);
    _mm_setcsr(CSR_FAST);
}
/* MXCSR is CSR_FAST */
static NOINL void xf_run(uint8_t* ls, const Xf* k, uint32_t csr)
{
    for (uint32_t b = 0; 16 * b < k->n4; b++) {
        const uint32_t n = k->n4 - 16 * b > 16 ? 16 : k->n4 - 16 * b;
        if (unlikely(!xf_batch16(ls, k, b, n))) xf_slow(ls, k, b, n, csr);
    }
}

/* frest16 / fi16 for AVX2 */
static inline Y frest8(Y a)
{
    const Y e = _mm256_and_si256(_mm256_srli_epi32(a, 23), Y1(0xFF));
    const Y f = _mm256_i32gather_epi32((const int*)k_rcp_lut, _mm256_and_si256(_mm256_srli_epi32(a, 18), Y1(31)), 4);
    Y rexp = _mm256_and_si256(_mm256_cmpgt_epi32(Y1(253), e), _mm256_sub_epi32(Y1(253), e));
    rexp = _mm256_blendv_epi8(rexp, Y1(255), _mm256_cmpeq_epi32(e, Y0));
    return _mm256_or_si256(_mm256_and_si256(a, Y1(0x80000000u)),
                           _mm256_or_si256(_mm256_slli_epi32(rexp, 23), _mm256_and_si256(f, Y1(0x7FFFFF))));
}
static inline Y fi8(Y ya, Y est)
{
    const Y expo = _mm256_and_si256(_mm256_srli_epi32(est, 23), Y1(0xFF));
    const Y base = _mm256_and_si256(_mm256_srli_epi32(est, 10), Y1(0x1FFF));
    const Y step = _mm256_and_si256(est, Y1(0x3FF));
    const Y y = _mm256_and_si256(ya, Y1(0x7FFFF));
    const Y mh = _mm256_sub_epi32(_mm256_slli_epi32(_mm256_or_si256(base, Y1(0x2000)), 11),
                                  _mm256_srli_epi32(_mm256_add_epi32(_mm256_mullo_epi32(step, y), Y1(255)), 8));
    const Y below1 = _mm256_cmpgt_epi32(Y1(0x1000000), mh);
    const Y frac = _mm256_and_si256(_mm256_blendv_epi8(_mm256_srli_epi32(mh, 1), mh, below1), Y1(0x7FFFFF));
    const Y e2 = _mm256_add_epi32(expo, below1);
    const Y zero = _mm256_or_si256(_mm256_cmpeq_epi32(expo, Y0), _mm256_cmpeq_epi32(e2, Y0));
    return _mm256_andnot_si256(zero, _mm256_or_si256(_mm256_and_si256(est, Y1(0x80000000u)),
                                                     _mm256_or_si256(_mm256_slli_epi32(e2, 23), frac)));
}
typedef struct { Y P[4][4], sc[3], of[3], x0, x1, y0, y1, dmin, dmax; } XfY;
/* xf_batch16 on eight vertices from v0 (FULL: 8, else 4); 0: do them exactly */
static inline __attribute__((always_inline)) int xf_batch8(uint8_t* ls, const Xf* k, const XfY* c, uint32_t v0, const int FULL)
{
    const uint32_t a = k->pos + 16 * v0;
    const Y v01 = yld(ls, a), v23 = yld(ls, a + 32);
    const Y v45 = FULL ? yld(ls, a + 64) : Y0, v67 = FULL ? yld(ls, a + 96) : Y0;
    /* lanes: vertices 0 2 4 6 | 1 3 5 7 */
    const Y valid = FULL ? Y1(-1) : _mm256_setr_epi32(-1, -1, 0, 0, -1, -1, 0, 0);
    Y X, Yc, Zc;
    ysoa(v01, v23, v45, v67, &X, &Yc, &Zc);
    const Y mi = _mm256_and_si256(_mm256_max_epu32(_mm256_max_epu32(yabs(X), yabs(Yc)), yabs(Zc)), valid);
    Y cr[4];
    for (int r = 0; r < 4; r++) cr[r] = yFMA(Zc, c->P[r][2], yFMA(Yc, c->P[r][1], yFMA(X, c->P[r][0], c->P[r][3])));
    const Y W = ycanon(cr[3]);
    const Y r10 = fi8(W, frest8(W));
    const Y iw = yFMA(yi(_mm256_fnmadd_ps(yf(r10), yf(W), _mm256_set1_ps(1.0f))), r10, r10);
    Y mo = _mm256_max_epu32(yabs(r10), yabs(iw));
    const Y sgn = _mm256_and_si256(W, Y1(0x80000000u));
    Y o[3];
    for (int i = 0; i < 3; i++) {
        const Y t = _mm256_xor_si256(ycanon(yFM(cr[i], iw)), sgn);
        o[i] = ycanon(yFMA(t, c->sc[i], c->of[i]));
        mo = _mm256_max_epu32(mo, _mm256_max_epu32(yabs(t), yabs(o[i])));
    }
    mo = _mm256_and_si256(mo, valid);
    if (unlikely(yany(_mm256_or_si256(yge(mi, 0x5D800000), yge(mo, 0x7F7FFFFF))))) return 0;
    const Y kx = fkey8(o[0]), ky = fkey8(o[1]), kz = fkey8(o[2]);
    Y code = _mm256_and_si256(_mm256_cmpgt_epi32(kx, c->x1), Y1(0x80));
    code = _mm256_or_si256(code, _mm256_and_si256(_mm256_cmpgt_epi32(c->x0, kx), Y1(0x40)));
    code = _mm256_or_si256(code, _mm256_and_si256(_mm256_cmpgt_epi32(ky, c->y1), Y1(0x20)));
    code = _mm256_or_si256(code, _mm256_and_si256(_mm256_cmpgt_epi32(c->y0, ky), Y1(0x10)));
    code = _mm256_or_si256(code, _mm256_and_si256(_mm256_cmpgt_epi32(kz, c->dmax), Y1(0x08)));
    code = _mm256_or_si256(code, _mm256_and_si256(_mm256_cmpgt_epi32(c->dmin, kz), Y1(0x04)));
    code = _mm256_or_si256(code, _mm256_and_si256(_mm256_cmpgt_epi32(fkey8(W), Y0), Y1(0x02)));
    /* back to (sx, sy, code, 0) per vertex, in order */
    const Y lo = _mm256_unpacklo_epi32(o[0], o[1]), hi = _mm256_unpackhi_epi32(o[0], o[1]);
    const Y cl = _mm256_unpacklo_epi32(code, Y0), ch = _mm256_unpackhi_epi32(code, Y0);
    const uint32_t d = k->scr + 16 * v0;
    yst(ls, d, _mm256_unpacklo_epi64(lo, cl));
    yst(ls, d + 32, _mm256_unpackhi_epi64(lo, cl));
    if (FULL) { yst(ls, d + 64, _mm256_unpacklo_epi64(hi, ch)); yst(ls, d + 96, _mm256_unpackhi_epi64(hi, ch)); }
    return 1;
}
static NOINL void xf_slow8(uint8_t* ls, const Xf* k, uint32_t v, uint32_t n, uint32_t csr)
{
    _mm_setcsr(csr);
    for (uint32_t g = v / 4; g < (v + n) / 4; g++) xf_group_q(ls, k, g);
    _mm_setcsr(CSR_FAST);
}
/* MXCSR is CSR_FAST */
static NOINL void xf_run_y(uint8_t* ls, const Xf* k, uint32_t csr)
{
    XfY c;
    for (int r = 0; r < 4; r++) for (int i = 0; i < 4; i++) c.P[r][i] = Y1(k->P[r]._u32[i]);
    for (int i = 0; i < 3; i++) { c.sc[i] = Y1(k->sc._u32[i]); c.of[i] = Y1(k->of._u32[i]); }
    c.x0 = Y1(fkey(k->x0)); c.x1 = Y1(fkey(k->x1)); c.y0 = Y1(fkey(k->y0)); c.y1 = Y1(fkey(k->y1));
    c.dmin = Y1(fkey(k->dmin)); c.dmax = Y1(fkey(k->dmax));
    uint32_t v = 0;
    for (; v + 8 <= k->n4; v += 8)
        if (unlikely(!xf_batch8(ls, k, &c, v, 1))) xf_slow8(ls, k, v, 8, csr);
    if (v < k->n4 && unlikely(!xf_batch8(ls, k, &c, v, 0))) xf_slow8(ls, k, v, 4, csr);   /* n4 is a multiple of 4 */
}

/* 0 if the job's code has to run; nothing written */
static int xf_prepare(const u128* gpr, const uint8_t* ls, Xf* k)
{
    const uint32_t S = gpr[3]._u32[0], sp = gpr[1]._u32[0];
    if ((sp & 15) || sp < 0x100 || sp > SPU_LS_SIZE || (S & 15) || S + 0x150 > SPU_LS_SIZE) return 0;
    const Q s0 = lq(ls, S);
    const uint32_t numv = (s0._u32[2] >> 16) & 0xFFFF;
    if (!numv) return 0;                                  /* the pipelined loop would store its fill-in */
    k->n4 = ((numv + 3) >> 2) << 2;
    k->pos = lq(ls, S + 0x50)._u32[3];
    k->scr = word_at(ls, S + 4 * (s0._u32[0] >> 24 & 15) + 16);
    if ((k->pos & 15) || (k->scr & 15)) return 0;
    const uint32_t nr = (k->n4 + 15) & ~15u;              /* the fast path reads whole batches */
    Span sp_[4];
    sp_[0] = (Span){ sp - 0x30, sp, 1 };
    sp_[1] = (Span){ S, S + 0x150, 0 };
    sp_[2] = (Span){ k->pos, k->pos + 16 * nr, 0 };
    sp_[3] = (Span){ k->scr, k->scr + 16 * k->n4, 1 };
    return spans_ok(sp_, 4);
}
/* the matrices and bounds as the job's prologue makes them, the frame, every vertex */
static void xf_exec(u128* gpr, uint8_t* ls, Xf* k)
{
    const uint32_t S = gpr[3]._u32[0], sp = gpr[1]._u32[0];
    const Q n0 = lq(ls, S + 0xB0), n1 = lq(ls, S + 0xC0), n2 = lq(ls, S + 0xD0), n3 = V4(0, 0, 0, F1);
    for (int r = 0; r < 4; r++) {
        const Q m = lq(ls, S + 0xF0 + 16 * r);
        k->P[r] = spu_fma(spl(m, 3), n3, spu_fma(spl(m, 2), n2, spu_fma(spl(m, 1), n1, spu_fm(spl(m, 0), n0))));
    }
    k->sc = lq(ls, S + 0x130); k->of = lq(ls, S + 0x140);
    const Q e = lq(ls, S + 0xE0);
    const uint32_t h0 = e._u32[0] >> 16, h1 = e._u32[0] & 0xFFFF, h2 = e._u32[1] >> 16, h3 = e._u32[1] & 0xFFFF;
    const Q bf = spu_cuflt(V4(h0, h1, (h0 + h2) & 0xFFFF, (h1 + h3) & 0xFFFF), 155);
    k->x0 = bf._u32[0]; k->y0 = bf._u32[1]; k->x1 = bf._u32[2]; k->y1 = bf._u32[3];
    const int gt = fkey(e._u32[2]) > fkey(e._u32[3]);
    k->dmin = gt ? e._u32[3] : e._u32[2]; k->dmax = gt ? e._u32[2] : e._u32[3];
    sq(ls, sp - 0x10, gpr[80]);
    sq(ls, sp - 0x30, gpr[1]);
    /* the fast path takes these as operands unchecked */
    int ok = 1;
    for (int r = 0; r < 4 && ok; r++) for (int i = 0; i < 4; i++) if ((k->P[r]._u32[i] & 0x7FFFFFFFu) >= 0x5D800000u) ok = 0;
    for (int i = 0; i < 3 && ok; i++)
        if ((k->sc._u32[i] & 0x7FFFFFFFu) >= 0x5D800000u || (k->of._u32[i] & 0x7FFFFFFFu) >= 0x5D800000u) ok = 0;
    if (ok) {
        const uint32_t csr = _mm_getcsr();
        _mm_setcsr(CSR_FAST);
        (g_avx2 ? xf_run_y : xf_run)(ls, k, csr);
        _mm_setcsr(csr);
    } else
        for (uint32_t g = 0; g < k->n4 / 4; g++) xf_group_q(ls, k, g);
    gpr[80] = lq(ls, sp - 0x10);
}
static int edge_xf_core(u128* gpr, uint8_t* ls)
{
    Xf k;
    if (!xf_prepare(gpr, ls, &k)) return 0;
    xf_exec(gpr, ls, &k);
    return 1;
}

/* ================================================================== 0xA798: triangle culling
 *
 * r3 the job context, r4 the cull mode, r5 the first vertex of the records.
 * Transforms the vertices (0xA3C8, natively), then tests each triangle of the
 * index buffer (S+0x50, u16 x3 each) against the records and compacts the
 * survivors in place, four triangles per step (RESEARCH.md section 4). */
static void cull_tables_init(void);
static uint8_t g_fnms_lsb[8];               /* LSB of fnms(a, b, c) on masks: [a<<2 | b<<1 | c] */
static int g_fast_cull;                     /* the table is c | (a & b), as cull_batch16 assumes */
static void fnms_init(void)
{
    for (int i = 0; i < 8; i++) {
        const uint32_t a = (i & 4) ? ~0u : 0, b = (i & 2) ? ~0u : 0, c = (i & 1) ? ~0u : 0;
        g_fnms_lsb[i] = spu_fnms(V4(a, a, a, a), V4(b, b, b, b), V4(c, c, c, c))._u32[0] & 1;
    }
    cull_tables_init();
    g_fast_cull = 1;
    for (int i = 0; i < 8; i++) if (g_fnms_lsb[i] != ((i & 1) | ((i >> 2) & (i >> 1) & 1))) g_fast_cull = 0;
}
typedef struct {
    uint32_t B, nt, ng, end, rec, sp2, snap, wind, m65, kept;
    uint8_t* idx;                            /* the index buffer as it was */
} Cull;
/* PS3RECOMP_EDGE_GPU (edgegpu, 2026-11-11): the GPU's clipper and face culling do the job's triangle culling.
 * 0 = the job culls (default). 1 = no cull stage at all: no transform, no per-triangle test, every triangle of
 * the segment is kept (the index list stays as the segment has it). 2 = the transform runs and a segment whose
 * every vertex is outside one and the same frustum plane is dropped whole (what the job does with it too),
 * the per-triangle tests (outside, back face, zero area) and the compaction are skipped.
 * PS3RECOMP_EDGE_TSC=1: cycles per kernel (and the cull's transform / triangle loop apart) in the exit report. */
static int g_gpu, g_tsc;
static unsigned long long g_cs_calls, g_cs_nt, g_cs_kept, g_cs_zero, g_cs_xf_cyc, g_cs_run_cyc, g_cs_gpu_cull;
#define CSADD(v, n) __atomic_fetch_add(&(v), (unsigned long long)(n), __ATOMIC_RELAXED)
static inline uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }

/* the largest of n big-endian u16 at ls + (B & LSM) (the wrap of the LS address kept for the odd case), AVX2:
 * the cull's scan of its index buffer, which was a scalar loop over 3 * nt halfwords (edgegpu, 2026-11-11) */
static inline uint32_t max_be16(const uint8_t* ls, uint32_t B, uint32_t n)
{
    uint32_t i = 0, m = 0;
    if ((B & LSM) + 2u * n <= SPU_LS_SIZE) {
        const uint8_t* p = ls + (B & LSM);
        const __m256i sw = _mm256_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        __m256i acc = _mm256_setzero_si256();
        for (; i + 16 <= n; i += 16) acc = _mm256_max_epu16(acc, _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(p + 2 * i)), sw));
        __m128i a = _mm_max_epu16(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
        a = _mm_minpos_epu16(_mm_xor_si128(a, _mm_set1_epi32(-1)));                 /* min of ~v = ~max */
        m = 0xFFFFu - (uint32_t)(_mm_cvtsi128_si32(a) & 0xFFFF);
        for (; i < n; i++) { const uint32_t v = be16(p + 2 * i); m = v > m ? v : m; }
        return m;
    }
    for (; i < n; i++) { const uint32_t v = be16(ls + ((B + 2 * i) & LSM)); m = v > m ? v : m; }
    return m;
}
/* the cull bits of group g (bit 3 = its first triangle), exactly: the job's lanes are its four triangles */
static NOINL unsigned cull_group_q(const uint8_t* ls, const Cull* k, uint32_t g)
{
    Q r[3][4];                               /* [vertex of the triangle][triangle] */
    for (int t = 0; t < 4; t++)
        for (int v = 0; v < 3; v++) r[v][t] = lq(ls, k->rec + 16 * be16(k->idx + 24 * g + 6 * t + 2 * v));
    Q sx[3], sy[3];
    uint32_t orc[4], andc[4];
    for (int v = 0; v < 3; v++) {
        sx[v] = V4(r[v][0]._u32[0], r[v][1]._u32[0], r[v][2]._u32[0], r[v][3]._u32[0]);
        sy[v] = V4(r[v][0]._u32[1], r[v][1]._u32[1], r[v][2]._u32[1], r[v][3]._u32[1]);
    }
    for (int t = 0; t < 4; t++) { orc[t] = r[0][t]._u32[2] | r[1][t]._u32[2] | r[2][t]._u32[2]; andc[t] = r[0][t]._u32[2] & r[1][t]._u32[2] & r[2][t]._u32[2]; }
    const Q area = spu_fms(spu_fs(sx[1], sx[0]), spu_fs(sy[2], sy[0]), spu_fm(spu_fs(sy[1], sy[0]), spu_fs(sx[2], sx[0])));
    const Q S4 = V4(k->snap, k->snap, k->snap, k->snap), H = V4(0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u);
    Q X[3], Y[3];
    for (int v = 0; v < 3; v++) { X[v] = spu_cflts(spu_fma(sx[v], S4, H), 173); Y[v] = spu_cflts(spu_fma(sy[v], S4, H), 173); }
    unsigned m = 0;
    for (int t = 0; t < 4; t++) {
        const int valid = !(orc[t] & 4) && (int32_t)(andc[t] & 2) > 0;
        const int outside = (andc[t] & 252) != 0;
        const int front = fkey(area._u32[t] ^ k->wind) > 0;
        const int nocover = (X[0]._u32[t] == X[1]._u32[t] && X[1]._u32[t] == X[2]._u32[t]) ||
                            (Y[0]._u32[t] == Y[1]._u32[t] && Y[1]._u32[t] == Y[2]._u32[t]);
        const int cull = (valid && nocover) | g_fnms_lsb[(valid && front) << 2 | k->m65 << 1 | outside];
        m |= (unsigned)cull << (3 - t);
    }
    return m;
}

/* a nibble's bits reversed (bit 3 = the group's first triangle), by table (linux2, 2026-11-12): clang 22.1.8's SLP
 * vectorizer, targeting x86_64-linux-gnu, turned the shift-and-mask form of this in cull_batch8 into one shuffle that
 * reversed all eight lanes, swapping the two groups' masks (job replay ck-rec3 3/15 Edge jobs; the Windows target
 * compiled it right). The table is the same function with nothing for the vectorizer to fold. */
static const uint8_t k_rev4[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
/* sixteen triangles (groups 4b..4b+3) at once; MXCSR is CSR_FAST. 0: the batch needs the exact way.
 * A triangle whose projection is not valid is culled or kept by its outcodes
 * alone, so its area and snapped corners are not checked (nor used). */
static inline __attribute__((always_inline)) int cull_batch16(const uint8_t* ls, const Cull* k, uint32_t b, uint16_t* masks)
{
    const uint8_t* ip = k->idx + 96 * b;
    const Z w0 = cpu_mm512_loadu_si512(ip), w1 = cpu_mm512_loadu_si512(ip + 64);
    Z sx[3], sy[3], cd[3];
    for (int v = 0; v < 3; v++) {
        const Z wi = simde_mm512_add_epi16(simde_mm512_setr_epi32(0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45), simde_mm512_set1_epi32(v));
        Z ix = simde_mm512_and_si512(simde_mm512_permutex2var_epi16(w0, wi, w1), simde_mm512_set1_epi32(0xFFFF));
        ix = simde_mm512_or_si512(simde_mm512_srli_epi32(ix, 8), simde_mm512_and_si512(simde_mm512_slli_epi32(ix, 8), simde_mm512_set1_epi32(0xFF00)));
        uint32_t a[16];
        simde_mm512_storeu_si512(a, simde_mm512_and_si512(simde_mm512_add_epi32(simde_mm512_set1_epi32(k->rec), simde_mm512_slli_epi32(ix, 4)), simde_mm512_set1_epi32(LSQ)));
        /* the records, four per register: R[m] block j = record of triangle 4m+j */
        Z R[4];
        for (int m = 0; m < 4; m++) {
            Z r = simde_mm512_castsi128_si512(_mm_loadu_si128((const __m128i*)(ls + a[4 * m])));
            r = simde_mm512_inserti32x4(r, _mm_loadu_si128((const __m128i*)(ls + a[4 * m + 1])), 1);
            r = simde_mm512_inserti32x4(r, _mm_loadu_si128((const __m128i*)(ls + a[4 * m + 2])), 2);
            R[m] = bswap16(simde_mm512_inserti32x4(r, _mm_loadu_si128((const __m128i*)(ls + a[4 * m + 3])), 3));
        }
        const Z ix0 = simde_mm512_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28, 0, 4, 8, 12, 16, 20, 24, 28);
        sx[v] = simde_mm512_mask_mov_epi32(simde_mm512_permutex2var_epi32(R[0], ix0, R[1]), 0xFF00, simde_mm512_permutex2var_epi32(R[2], ix0, R[3]));
        const Z ix1 = simde_mm512_add_epi32(ix0, simde_mm512_set1_epi32(1)), ix2 = simde_mm512_add_epi32(ix0, simde_mm512_set1_epi32(2));
        sy[v] = simde_mm512_mask_mov_epi32(simde_mm512_permutex2var_epi32(R[0], ix1, R[1]), 0xFF00, simde_mm512_permutex2var_epi32(R[2], ix1, R[3]));
        cd[v] = simde_mm512_mask_mov_epi32(simde_mm512_permutex2var_epi32(R[0], ix2, R[1]), 0xFF00, simde_mm512_permutex2var_epi32(R[2], ix2, R[3]));
    }
    const Z orc = simde_mm512_or_si512(simde_mm512_or_si512(cd[0], cd[1]), cd[2]), andc = simde_mm512_and_si512(simde_mm512_and_si512(cd[0], cd[1]), cd[2]);
    const simde__mmask16 valid = cpu_mm512_testn_epi32_mask(orc, simde_mm512_set1_epi32(4)) & simde_mm512_cmpgt_epi32_mask(simde_mm512_and_si512(andc, simde_mm512_set1_epi32(2)), simde_mm512_setzero_si512());
    const simde__mmask16 outside = simde_mm512_test_epi32_mask(andc, simde_mm512_set1_epi32(252));
    Z mx = simde_mm512_setzero_si512();
    for (int v = 0; v < 3; v++) {
        mx = simde_mm512_mask_max_epu32(mx, valid, mx, simde_mm512_and_si512(sx[v], simde_mm512_set1_epi32(0x7FFFFFFF)));
        mx = simde_mm512_mask_max_epu32(mx, valid, mx, simde_mm512_and_si512(sy[v], simde_mm512_set1_epi32(0x7FFFFFFF)));
    }
    if (unlikely(simde_mm512_cmpge_epu32_mask(mx, simde_mm512_set1_epi32(0x4E000000)))) return 0;   /* 2^29: the snap stays an int32 */
    const Z area = FMS(zi(simde_mm512_sub_ps(zf(sx[1]), zf(sx[0]))), zi(simde_mm512_sub_ps(zf(sy[2]), zf(sy[0]))),
                       FM(zi(simde_mm512_sub_ps(zf(sy[1]), zf(sy[0]))), zi(simde_mm512_sub_ps(zf(sx[2]), zf(sx[0])))));
    const simde__mmask16 front = simde_mm512_cmpgt_epi32_mask(fkey16(simde_mm512_xor_si512(area, simde_mm512_set1_epi32(k->wind))), simde_mm512_setzero_si512());
    Z X[3], Y[3];
    for (int v = 0; v < 3; v++) {
        X[v] = simde_mm512_cvttps_epi32(simde_mm512_fmadd_ps(zf(sx[v]), simde_mm512_set1_ps(k->snap == F1 ? 1.0f : 2.0f), simde_mm512_set1_ps(0.5f)));
        Y[v] = simde_mm512_cvttps_epi32(simde_mm512_fmadd_ps(zf(sy[v]), simde_mm512_set1_ps(k->snap == F1 ? 1.0f : 2.0f), simde_mm512_set1_ps(0.5f)));
    }
    const simde__mmask16 nocover = (simde_mm512_cmpeq_epi32_mask(X[0], X[1]) & simde_mm512_cmpeq_epi32_mask(X[1], X[2])) |
                              (simde_mm512_cmpeq_epi32_mask(Y[0], Y[1]) & simde_mm512_cmpeq_epi32_mask(Y[1], Y[2]));
    const simde__mmask16 cull = (valid & nocover) | outside | (k->m65 ? (valid & front) : 0);
    /* lane 4g+t is triangle t of group g; the job's bits run the other way */
    for (int g = 0; g < 4; g++) masks[g] = k_rev4[(cull >> (4 * g)) & 15];
    return 1;
}

/* the job's stores for one step: m the cull bits of the group (15: nothing kept) */
/* pshufb controls for the stores: the kept entries of a group (from its two
 * quads), and the views of them shifted by the output's offset in its quad */
static uint8_t g_cmp[16][4][16];             /* [mask][comp1 from G0, comp1 from G1, comp2 from G0, comp2 from G1] */
static uint8_t g_shl[16][16], g_hi[16][16], g_rot[16][16];
static void cull_tables_init(void)
{
    for (int m = 0; m < 16; m++) {
        int src[32], n = 0;
        for (int t = 0; t < 4; t++) if (!(m >> (3 - t) & 1)) for (int j = 0; j < 6; j++) src[n++] = 6 * t + j;
        for (; n < 32; n++) src[n] = -1;
        for (int j = 0; j < 16; j++) {
            g_cmp[m][0][j] = src[j] >= 0 && src[j] < 16 ? src[j] : 0x80;
            g_cmp[m][1][j] = src[j] >= 16 ? src[j] - 16 : 0x80;
            g_cmp[m][2][j] = src[16 + j] >= 0 && src[16 + j] < 16 ? src[16 + j] : 0x80;
            g_cmp[m][3][j] = src[16 + j] >= 16 ? src[16 + j] - 16 : 0x80;
        }
    }
    for (int off = 0; off < 16; off++)
        for (int i = 0; i < 16; i++) {
            g_shl[off][i] = i >= off ? i - off : 0x80;
            g_hi[off][i] = i < off ? 16 - off + i : 0x80;
            g_rot[off][i] = (i - off) & 15;
        }
}
static inline __m128i ld128(const uint8_t* p) { return _mm_loadu_si128((const __m128i*)p); }
static inline void cull_store(uint8_t* ls, Cull* k, uint32_t g, unsigned m)
{
    const __m128i G0 = ld128(k->idx + 24 * g), G1 = ld128(k->idx + 24 * g + 16);
    const __m128i c1 = _mm_or_si128(_mm_shuffle_epi8(G0, ld128(g_cmp[m][0])), _mm_shuffle_epi8(G1, ld128(g_cmp[m][1])));
    const __m128i c2 = _mm_or_si128(_mm_shuffle_epi8(G0, ld128(g_cmp[m][2])), _mm_shuffle_epi8(G1, ld128(g_cmp[m][3])));
    const uint32_t ptr = k->B + 6 * k->kept, off = ptr & 15;
    const __m128i lt = _mm_cmpgt_epi8(_mm_set1_epi8((char)off), _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
    const __m128i shl = ld128(g_shl[off]);
    uint8_t* q0 = ls + (ptr & LSQ);
    _mm_storeu_si128((__m128i*)q0, _mm_blendv_epi8(_mm_shuffle_epi8(c1, shl), ld128(q0), lt));
    _mm_storeu_si128((__m128i*)(ls + ((((int32_t)ptr > (int32_t)(k->end - 17) ? k->sp2 + 32 : ptr) + 16) & LSQ)),
                     _mm_or_si128(_mm_shuffle_epi8(c1, ld128(g_hi[off])), _mm_shuffle_epi8(c2, shl)));
    _mm_storeu_si128((__m128i*)(ls + ((((int32_t)ptr > (int32_t)(k->end - 33) ? k->sp2 + 32 : ptr) + 32) & LSQ)),
                     _mm_shuffle_epi8(c2, ld128(g_rot[off])));
    k->kept += 4 - __builtin_popcount(m);
}

/* a group the exact way, at the caller's MXCSR */
static NOINL unsigned cull_slow(const uint8_t* ls, const Cull* k, uint32_t g, uint32_t csr)
{
    _mm_setcsr(csr);
    const unsigned m = cull_group_q(ls, k, g);
    _mm_setcsr(CSR_FAST);
    return m;
}
/* every group; MXCSR is CSR_FAST */
static NOINL void cull_run(uint8_t* ls, Cull* k, uint32_t csr)
{
    const unsigned last = (k->nt & 3) ? 15u >> (k->nt & 3) : 0;
    for (uint32_t g = 0; g < k->ng; ) {
        uint16_t m4[4];
        if (g + 4 <= k->ng && g_fast_cull && likely(cull_batch16(ls, k, g / 4, m4))) {
            for (int j = 0; j < 4; j++, g++) cull_store(ls, k, g, g + 1 == k->ng ? (m4[j] | last) : m4[j]);
            continue;
        }
        if (g + 4 <= k->ng && g_fast_cull) EDGE_STAT(4);
        unsigned m = cull_slow(ls, k, g, csr);
        if (g + 1 == k->ng) m |= last;
        cull_store(ls, k, g, m);
        g++;
    }
}

/* cull_batch16 for AVX2: eight triangles (groups g0, g0 + 1), lane t = triangle t, the records
 * gathered word by word. MXCSR is CSR_FAST. 0: the groups need the exact way. */
static inline __attribute__((always_inline)) int cull_batch8(const uint8_t* ls, const Cull* k, uint32_t g0, uint16_t* masks)
{
    const uint8_t* ip = k->idx + 24 * g0;
    const Y t6 = _mm256_setr_epi32(0, 6, 12, 18, 24, 30, 36, 42);
    Y sx[3], sy[3], cd[3];
    for (int v = 0; v < 3; v++) {
        const Y w = _mm256_i32gather_epi32((const int*)(ip + 2 * v), t6, 1);   /* the index, LS byte order, in the low half */
        const Y ix = _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(w, Y1(0xFF)), 8), _mm256_and_si256(_mm256_srli_epi32(w, 8), Y1(0xFF)));
        const Y a = _mm256_and_si256(_mm256_add_epi32(Y1(k->rec), _mm256_slli_epi32(ix, 4)), Y1(LSQ));
        sx[v] = ybswap(_mm256_i32gather_epi32((const int*)ls, a, 1));
        sy[v] = ybswap(_mm256_i32gather_epi32((const int*)(ls + 4), a, 1));
        cd[v] = ybswap(_mm256_i32gather_epi32((const int*)(ls + 8), a, 1));
    }
    const Y orc = _mm256_or_si256(_mm256_or_si256(cd[0], cd[1]), cd[2]), andc = _mm256_and_si256(_mm256_and_si256(cd[0], cd[1]), cd[2]);
    const Y valid = _mm256_and_si256(_mm256_cmpeq_epi32(_mm256_and_si256(orc, Y1(4)), Y0), _mm256_cmpgt_epi32(_mm256_and_si256(andc, Y1(2)), Y0));
    const Y inside = _mm256_cmpeq_epi32(_mm256_and_si256(andc, Y1(252)), Y0);
    Y mx = Y0;
    for (int v = 0; v < 3; v++) mx = _mm256_max_epu32(mx, _mm256_max_epu32(yabs(sx[v]), yabs(sy[v])));
    if (unlikely(yany(yge(_mm256_and_si256(mx, valid), 0x4E000000)))) return 0;   /* 2^29: the snap stays an int32 */
    const Y area = yFMS(yi(_mm256_sub_ps(yf(sx[1]), yf(sx[0]))), yi(_mm256_sub_ps(yf(sy[2]), yf(sy[0]))),
                        yFM(yi(_mm256_sub_ps(yf(sy[1]), yf(sy[0]))), yi(_mm256_sub_ps(yf(sx[2]), yf(sx[0])))));
    const Y front = _mm256_cmpgt_epi32(fkey8(_mm256_xor_si256(area, Y1(k->wind))), Y0);
    const __m256 sn = _mm256_set1_ps(k->snap == F1 ? 1.0f : 2.0f), half = _mm256_set1_ps(0.5f);
    Y X[3], Yv[3];
    for (int v = 0; v < 3; v++) {
        X[v] = _mm256_cvttps_epi32(_mm256_fmadd_ps(yf(sx[v]), sn, half));
        Yv[v] = _mm256_cvttps_epi32(_mm256_fmadd_ps(yf(sy[v]), sn, half));
    }
    const Y nocover = _mm256_or_si256(_mm256_and_si256(_mm256_cmpeq_epi32(X[0], X[1]), _mm256_cmpeq_epi32(X[1], X[2])),
                                      _mm256_and_si256(_mm256_cmpeq_epi32(Yv[0], Yv[1]), _mm256_cmpeq_epi32(Yv[1], Yv[2])));
    Y cull = _mm256_or_si256(_mm256_and_si256(valid, nocover), _mm256_xor_si256(inside, Y1(-1)));
    if (k->m65) cull = _mm256_or_si256(cull, _mm256_and_si256(valid, front));
    const unsigned cm = (unsigned)_mm256_movemask_ps(yf(cull));
    for (int g = 0; g < 2; g++) masks[g] = k_rev4[(cm >> (4 * g)) & 15];   /* the job's bits run the other way: bit 3 = the group's first triangle */
    return 1;
}
/* cull_run with cull_batch8; MXCSR is CSR_FAST */
static NOINL void cull_run_y(uint8_t* ls, Cull* k, uint32_t csr)
{
    const unsigned last = (k->nt & 3) ? 15u >> (k->nt & 3) : 0;
    for (uint32_t g = 0; g < k->ng; ) {
        uint16_t m2[2];
        if (g + 2 <= k->ng && g_fast_cull && likely(cull_batch8(ls, k, g, m2))) {
            for (int j = 0; j < 2; j++, g++) cull_store(ls, k, g, g + 1 == k->ng ? (m2[j] | last) : m2[j]);
            continue;
        }
        unsigned m = cull_slow(ls, k, g, csr);
        if (g + 1 == k->ng) m |= last;
        cull_store(ls, k, g, m);
        g++;
    }
}

/* trim: 1 = the lifted 0xA7C0 runs its alternative 1 (a nop for the brnz): the r4 = 0 path */
static int edge_cull_trim_off(void)
{
#ifdef EDGE_HAVE_SPU_PATCH
    static volatile int slot = -2;   /* found at the first call, after every image registered */
    int s = slot;
    if (s == -2) slot = s = spu_patch_find("mesh_trim");
    return s >= 0 && __atomic_load_n(&g_spu_patch[s], __ATOMIC_ACQUIRE) == 1;
#else
    return 0;
#endif
}
static int edge_cull_core(u128* gpr, uint8_t* ls)
{
    const uint32_t S = gpr[3]._u32[0], sp = gpr[1]._u32[0], sp2 = sp - 128;
    /* the lifted instruction tests the switch once per call, at 0xA7C0; this is the same test */
    const uint32_t R4 = edge_cull_trim_off() ? 0u : gpr[4]._u32[0], R5 = gpr[5]._u32[0];
    if ((sp & 15) || sp < 0x200 || sp + 0x20 > SPU_LS_SIZE || (S & 15) || S + 0x1B0 > SPU_LS_SIZE) return 0;
    const Q s0 = lq(ls, S);
    Cull k; memset(&k, 0, sizeof k);
    Xf x;
    u128 g2[128];
    uint32_t r10 = 0;
    if (R4) {
        if (!(s0._u32[0] >> 31)) return 0;                     /* the job's "culling without ..." error */
        if (word_at(ls, S + 0x194)) return 0;                  /* a custom transform: SPU code of the game's */
        memcpy(g2, gpr, sizeof g2);
        for (int i = 0; i < 4; i++) g2[1]._u32[i] = gpr[1]._u32[i] - 128;
        g2[80] = gpr[4];
        if (!xf_prepare(g2, ls, &x)) return 0;
        const uint32_t mode = lq(ls, S + 0x150)._u32[0] >> 24;
        r10 = R4 | (mode == 5 ? 0x80000000u : 0);
        if (r10 >= 1 && r10 <= 3) {
            k.B = word_at(ls, S + 0x50);
            if (k.B & 15) return 0;
            const uint32_t nidx = lq(ls, S + 0x1A0)._u32[0] >> 16;   /* the halfword the count goes back to */
            k.nt = (uint32_t)(((uint64_t)nidx * 0xAAAAAAABull) >> 33);
            k.ng = (k.nt + 3) >> 2;
            k.end = k.B + ((6 * k.nt + 15) & ~15u);
            k.rec = x.scr + 16 * R5;
            k.sp2 = sp2;
            k.snap = (mode == 3 || mode == 4) ? 0x40000000u : F1;
            const uint32_t flavor = (s0._u32[1] >> 12) & 15;
            k.wind = (r10 == 2 ? 0 : 0x80000000u) ^ ((flavor == 0 || flavor == 2) ? 0x80000000u : 0);
            k.m65 = r10 >= 2;
            /* the records the triangles name */
            const uint32_t maxi = max_be16(ls, k.B, 3 * k.nt);
            if (k.B + 24 * k.ng + 64 > SPU_LS_SIZE || k.rec + 16 * (maxi + 1) > SPU_LS_SIZE) return 0;
            /* the records are the transform's output: they may overlap it, nothing else */
            const Span w1[5] = { { sp2 - 0x30, sp + 0x20, 1 }, { S, S + 0x1B0, 1 }, { k.B, k.B + 24 * k.ng + 48, 1 },
                                 { x.pos, x.pos + 16 * ((x.n4 + 15) & ~15u), 0 }, { x.scr, x.scr + 16 * x.n4, 1 } };
            const Span w2[4] = { { sp2 - 0x30, sp + 0x20, 1 }, { S, S + 0x1B0, 1 }, { k.B, k.B + 24 * k.ng + 48, 1 },
                                 { k.rec, k.rec + 16 * (maxi + 1), 0 } };
            if (!spans_ok(w1, 5) || !spans_ok(w2, 4)) return 0;
        }
    }
    /* prologue */
    sq(ls, sp - 0x10, gpr[80]); sq(ls, sp - 0x20, gpr[81]); sq(ls, sp - 0x30, gpr[82]);
    sq(ls, sp + 0x10, gpr[0]); sq(ls, sp - 0x80, gpr[1]);
    Q r3;
    if (!R4) {
        if (word_at(ls, S + 0xC) != 0xFFFFFFFFu) {
            const Q v = lq(ls, S + 0x50);
            Q q = lq(ls, S + 0x50); q._u32[((sp2 + 8) >> 2) & 3] = v._u32[0]; sq(ls, S + 0x50, q);
        }
        const Q h = quad_at(ls, S + 0x1A0 + 14);
        r3 = V4(h._u32[0] & 0xFFFF, h._u32[1] & 0xFFFF, h._u32[2] & 0xFFFF, h._u32[3] & 0xFFFF);
    } else {
        const unsigned long long c0 = g_tsc ? __rdtsc() : 0;
        if (!(g_gpu == 1 && r10 >= 1 && r10 <= 3)) xf_exec(g2, ls, &x);
        const unsigned long long c1 = g_tsc ? __rdtsc() : 0;
        if (r10 >= 1 && r10 <= 3) {
            if (g_gpu == 1) {
                k.kept = k.nt;                                  /* the GPU culls: all of them */
                CSADD(g_cs_gpu_cull, 1);
            } else if (g_gpu == 2) {
                /* the records the triangles name: (sx, sy, outcode, 0) at k.rec; a segment whose vertices
                 * are all outside the same plane has every triangle culled by the job too */
                uint32_t andc = 0xFFFFFFFFu;
                const uint32_t maxi = max_be16(ls, k.B, 3 * k.nt);
                for (uint32_t i = 0; i <= maxi; i++) andc &= lq(ls, k.rec + 16 * i)._u32[2];
                k.kept = (k.nt && (andc & 252)) ? 0 : k.nt;
                CSADD(g_cs_gpu_cull, 1);
            } else {
                static _Thread_local uint8_t idx[24 * 5462 + 128];   /* 65535 indexes at most */
                memcpy(idx, ls + k.B, 24 * k.ng + 64);
                k.idx = idx;
                cull_store(ls, &k, 0, 15);                          /* the pipeline's two empty steps */
                cull_store(ls, &k, 0, 15);
                const uint32_t csr = _mm_getcsr();
                _mm_setcsr(CSR_FAST);
                (g_avx2 ? cull_run_y : cull_run)(ls, &k, csr);
                _mm_setcsr(csr);
            }
            CSADD(g_cs_calls, 1); CSADD(g_cs_nt, k.nt); CSADD(g_cs_kept, k.kept);
            if (k.nt && !k.kept) CSADD(g_cs_zero, 1);
            if (g_tsc) { const unsigned long long c2 = __rdtsc(); CSADD(g_cs_xf_cyc, c1 - c0); CSADD(g_cs_run_cyc, c2 - c1); }
        }
        const uint32_t n = 3 * k.kept;
        Q h = lq(ls, S + 0x1A0);
        {
            uint8_t b[16]; for (int i = 0; i < 4; i++) { b[4 * i] = h._u32[i] >> 24; b[4 * i + 1] = h._u32[i] >> 16; b[4 * i + 2] = h._u32[i] >> 8; b[4 * i + 3] = h._u32[i]; }
            const int slot = (sp2 & 0xE);
            b[slot] = n >> 8; b[slot + 1] = n;
            sq(ls, S + 0x1A0, V4(be32(b), be32(b + 4), be32(b + 8), be32(b + 12)));
        }
        const uint32_t B = word_at(ls, S + 0x50);
        Q q = lq(ls, S + 0x50); q._u32[((sp2 + 8) >> 2) & 3] = (B + 6 * k.kept + 15) & ~15u; sq(ls, S + 0x50, q);
        r3 = V4(n, n, n, n);
    }
    gpr[3] = r3;
    gpr[80] = lq(ls, sp - 0x10); gpr[81] = lq(ls, sp - 0x20); gpr[82] = lq(ls, sp - 0x30);
    gpr[0] = lq(ls, sp + 0x10);
    return 1;
}

/* ================================================================== 0x4448: vertex decompression
 *
 * r3 the job context S, r4 the segment's format info T, r5 the stream
 * descriptor D. Only the special format pair is native (RESEARCH.md section
 * 5): primary format with attributes 1 and 2, nine bytes per vertex at
 * D+20 (three u16 position components, then a 24-bit unit vector), and an
 * optional secondary format (D+0x30) with attribute 3, three bytes per vertex
 * at D+24 (another unit vector). Every other format goes through the job's
 * generic attribute loop, which stays lifted.
 *
 * The job takes three free uniform tables (slot bytes at S+0x170 become 1, 2,
 * 3), points S+0x5C, S+0x60 and S+0x64 at them, and writes per vertex:
 *   position c = (u16_c + X[c]) * 2^(b-16), as csflt(v >> (16-b)) +
 *     cuflt(v << (16+b)) * 2^-32, lane 3 = 1.0 (X: four int32 at D+40, b: the
 *     byte at T+0x1B);
 *   unit vector: a and b the top two 10-bit fields of the 24 bits, each
 *     dequantised through the mantissa of 1.0 and fma(m, K1, K2) to about
 *     [-0.707, 0.707]; the third component sqrt(1 - a^2 - b^2) by frsqest/fi;
 *     the low nibble indexes a shuffle table at 0xF380 that places the three
 *     components, gives the rebuilt one its sign and lane 3 its +-1.0.
 * Vertices go in pairs (the count rounded up to two, so a padding vertex is
 * decoded from whatever follows the stream); the unit-vector loops are
 * pipelined one pair deep and their first iteration stores two zero quads
 * just past the end of the output. */
#define DEC_TAB 0xF380u
#define DEC_K1 0x46352E7Cu
#define DEC_K2 0xC6353150u
static inline Q splat4(uint32_t x) { return V4(x, x, x, x); }
static inline uint32_t ls_be16(const uint8_t* ls, uint32_t a) { return (uint32_t)ls[a & LSM] << 8 | ls[(a + 1) & LSM]; }

/* one position, through the helpers in the job's lane layout */
static inline Q dec_pos_q(const uint8_t* ls, uint32_t rec, Q X, Q shr, Q shl)
{
    const Q v = spu_a(V4(ls_be16(ls, rec), ls_be16(ls, rec + 2), ls_be16(ls, rec + 4), 0), X);
    Q r = spu_fa(spu_csflt(spu_rotma(v, shr), 155), spu_cuflt(spu_shl(v, shl), 123));
    r._u32[3] = F1;
    return r;
}
/* two unit vectors (records at ra and rb), through the helpers */
static inline void dec_uv_pair_q(const uint8_t* ls, uint32_t ra, uint32_t rb, Q* na, Q* nb)
{
    const uint32_t a0 = ls[ra & LSM], a1 = ls[(ra + 1) & LSM], a2 = ls[(ra + 2) & LSM];
    const uint32_t b0 = ls[rb & LSM], b1 = ls[(rb + 1) & LSM], b2 = ls[(rb + 2) & LSM];
    const Q q = V4(F1 | ((a0 << 8 | a1) >> 6), F1 | (((a1 << 8 | a2) >> 4) & 0x3FF),
                   F1 | ((b0 << 8 | b1) >> 6), F1 | (((b1 << 8 | b2) >> 4) & 0x3FF));
    const Q m = spu_fma(splat4(DEC_K1), q, splat4(DEC_K2));
    const Q sq = spu_fm(m, m);
    const Q s = spu_fs(splat4(F1), spu_fa(sq, spu_shlqbyi(sq, 4)));
    const Q h = spu_fm(s, spu_fi(s, spu_frsqest(s)));
    const Q msk_sign = V4(0x80000000u, 0x80000000u, 0x80000000u, 0xFFFFFFFFu);
    const Q msk_sel = V4(0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0);
    const Q base = V4(0, 0, 0, 0x0C0D0E0Fu);
    const Q ta = lq(ls, DEC_TAB + 16 * (a2 & 15)), tb = lq(ls, DEC_TAB + 16 * (b2 & 15));
    *na = spu_shufb(spu_selb(h, ta, msk_sign), m, spu_selb(base, ta, msk_sel));
    *nb = spu_shufb(spu_selb(h, tb, msk_sign), m, spu_a(spu_selb(base, tb, msk_sel), V4(0x08080808u, 0x08080808u, 0x08080808u, 0)));
}
typedef struct {
    uint32_t S, P, P2, U1, U2, U3, n2, tan;
    uint32_t slot[3];
    Q X, shr, shl;
} Dec;

/* the first free slot byte of q (0 = free), or -1 */
static inline int dec_free_slot(Q q)
{
    for (int i = 0; i < 16; i++) if (!((q._u32[i >> 2] >> (24 - 8 * (i & 3))) & 0xFF)) return i;
    return -1;
}
static inline void dec_set_slot(Q* q, int i, uint32_t v)
{
    const int sh = 24 - 8 * (i & 3);
    q->_u32[i >> 2] = (q->_u32[i >> 2] & ~(0xFFu << sh)) | v << sh;
}
static NOINL void dec_exact_pos(uint8_t* ls, const Dec* k)
{
    for (uint32_t i = 0; i < k->n2; i++) sq(ls, k->U1 + 16 * i, dec_pos_q(ls, k->P + 9 * i, k->X, k->shr, k->shl));
}
static NOINL void dec_exact_uv(uint8_t* ls, uint32_t rec, uint32_t stride, uint32_t dst, uint32_t n2)
{
    for (uint32_t i = 0; i < n2; i += 2) {
        Q a, b;
        dec_uv_pair_q(ls, rec + stride * i, rec + stride * (i + 1), &a, &b);
        sq(ls, dst + 16 * i, a); sq(ls, dst + 16 * i + 16, b);
    }
}

/* The fast path, four vertices per register (one quad each, the output's
 * layout). The positions need no check: integers in, csflt + cuflt * 2^-32
 * out, so every operand and result is a normal single or +0 and x86 under
 * CSR_FAST rounds them like the helpers. The unit vectors depend on 20 input
 * bits only, and the harness checks the fast decode against the exact one on
 * all of them (t_uv, RESEARCH.md 7.1): m stays within +-0.7072 and s = 1 - t in
 * (0, 1], so every value is a normal single and h is positive. s = +0 would
 * make frsqest's estimate an exponent-255 value, whose product with +0 is +0
 * on the SPU and a NaN on x86; it never occurs on this grid, but h is forced
 * to +0 there all the same. Selector nibbles 12-15 index past the shuffle
 * table into the job's error strings (meshes use 0-11: three axes, two signs,
 * two handednesses); a group that meets one runs its pairs the exact way. */
static const uint32_t k_dec_tab[64] = {
    0x80010203, 0x10111213, 0x94151617, 0xBF800000, 0x00010203, 0x10111213, 0x14151617, 0xBF800000,
    0x80010203, 0x10111213, 0x94151617, 0x3F800000, 0x00010203, 0x10111213, 0x14151617, 0x3F800000,
    0x90111213, 0x00010203, 0x94151617, 0xBF800000, 0x10111213, 0x00010203, 0x14151617, 0xBF800000,
    0x90111213, 0x00010203, 0x94151617, 0x3F800000, 0x10111213, 0x00010203, 0x14151617, 0x3F800000,
    0x90111213, 0x14151617, 0x80010203, 0xBF800000, 0x10111213, 0x14151617, 0x00010203, 0xBF800000,
    0x90111213, 0x14151617, 0x80010203, 0x3F800000, 0x10111213, 0x14151617, 0x00010203, 0x3F800000,
    0x4552524F, 0x523A2061, 0x7474656D, 0x70742074, 0x6F207573, 0x6520736B, 0x696E6E69, 0x6E672077,
    0x6974686F, 0x75742061, 0x6C6C6F63, 0x6174696E, 0x6720616E, 0x20657874, 0x72612075, 0x6E69666F
};
/* per selector and lane: where the lane comes from in (h, ma, mb, w) -- 0..3 --
 * and the sign the rebuilt component takes; built from k_dec_tab */
static uint32_t k_dec_src[64], k_dec_sgn[16];
static uint8_t k_dec_tab_be[256];          /* k_dec_tab as the LS holds it */
static int g_dec_ok;
static void dec_tables_init(void)
{
    int ok = 1;
    for (int s = 0; s < 12; s++) {
        const uint32_t* t = k_dec_tab + 4 * s;
        for (int c = 0; c < 3; c++) {
            const uint32_t w = t[c] & 0x7FFFFFFFu;
            k_dec_src[4 * s + c] = w == 0x00010203u ? 0 : w == 0x10111213u ? 1 : w == 0x14151617u ? 2 : (ok = 0, 0);
        }
        k_dec_src[4 * s + 3] = 3;
        k_dec_sgn[s] = t[0] & 0x80000000u;
        if ((t[0] ^ t[2]) & 0x80000000u) ok = 0;          /* A's sign is word 0's, B's word 2's */
    }
    for (int i = 0; i < 64; i++) { const uint32_t w = __builtin_bswap32(k_dec_tab[i]); memcpy(k_dec_tab_be + 4 * i, &w, 4); }
    g_dec_ok = ok;
}
static inline Z bcast4(Q q) { return simde_mm512_broadcast_i32x4(_mm_setr_epi32((int)q._u32[0], (int)q._u32[1], (int)q._u32[2], (int)q._u32[3])); }
/* sixteen bytes at each of four records (LS byte order; the caller keeps them inside the LS) */
static inline Z ld4rec(const uint8_t* ls, uint32_t a, uint32_t stride)
{
    Z v = simde_mm512_castsi128_si512(_mm_loadu_si128((const __m128i*)(ls + a)));
    v = simde_mm512_inserti32x4(v, _mm_loadu_si128((const __m128i*)(ls + a + stride)), 1);
    v = simde_mm512_inserti32x4(v, _mm_loadu_si128((const __m128i*)(ls + a + 2 * stride)), 2);
    return simde_mm512_inserti32x4(v, _mm_loadu_si128((const __m128i*)(ls + a + 3 * stride)), 3);
}
/* host-order words from record bytes: (b0 << 8 | b1), (b2 << 8 | b3), (b4 << 8 | b5) */
#define DEC_PSHUF(o) simde_mm512_broadcast_i32x4(_mm_setr_epi8((o) + 1, (o), -1, -1, (o) + 3, (o) + 2, -1, -1, (o) + 5, (o) + 4, -1, -1, -1, -1, -1, -1))
/* a unit vector at byte o: (b0 << 8 | b1, b1 << 8 | b2, b2, 0) */
#define DEC_USHUF(o) simde_mm512_broadcast_i32x4(_mm_setr_epi8((o) + 1, (o), -1, -1, (o) + 2, (o) + 1, -1, -1, (o) + 2, -1, -1, -1, -1, -1, -1, -1))
static inline Z dec_pos4(Z raw, Z X, Z shr, Z shl)
{
    const Z v = simde_mm512_add_epi32(raw, X);
    const simde__m512 hi = simde_mm512_cvtepi32_ps(cpu_mm512_srav_epi32(v, shr));
    const simde__m512 lo = simde_mm512_mul_ps(cpu_mm512_cvtepu32_ps(simde_mm512_sllv_epi32(v, shl)), simde_mm512_set1_ps(0x1p-32f));
    return simde_mm512_mask_mov_epi32(zi(simde_mm512_add_ps(hi, lo)), 0x8888, simde_mm512_set1_epi32(F1));
}
/* four unit vectors from u = DEC_USHUF words; returns the vertices whose
 * selector is past the table (their lanes are not the job's) */
static inline simde__mmask16 dec_uv4(Z u, Z* out)
{
    const Z sel = simde_mm512_and_si512(simde_mm512_shuffle_epi32(u, (int)0xAA), simde_mm512_set1_epi32(15));
    const simde__mmask16 bad = simde_mm512_cmpge_epu32_mask(sel, simde_mm512_set1_epi32(12));
    const Z seli = simde_mm512_maskz_mov_epi32((simde__mmask16)~bad, sel);
    const Z q = simde_mm512_or_si512(simde_mm512_set1_epi32(F1),
                                simde_mm512_and_si512(simde_mm512_srlv_epi32(u, simde_mm512_broadcast_i32x4(_mm_setr_epi32(6, 4, 0, 0))), simde_mm512_set1_epi32(0x3FF)));
    const Z m = FMA(simde_mm512_set1_epi32(DEC_K1), q, simde_mm512_set1_epi32(DEC_K2));
    const Z sq = FM(m, m);
    const Z s = zi(simde_mm512_sub_ps(simde_mm512_set1_ps(1.0f), simde_mm512_add_ps(zf(sq), zf(simde_mm512_shuffle_epi32(sq, (int)0xF9)))));
    const Z h = zi(simde_mm512_maskz_mul_ps(simde_mm512_test_epi32_mask(s, simde_mm512_set1_epi32(0x7F800000)), zf(s), zf(rsq16(s))));
    const Z hs = simde_mm512_or_si512(simde_mm512_and_si512(h, simde_mm512_set1_epi32(0x7FFFFFFF)), simde_mm512_permutexvar_epi32(seli, cpu_mm512_loadu_si512(k_dec_sgn)));
    const Z lane = simde_mm512_broadcast_i32x4(_mm_setr_epi32(0, 1, 2, 3));
    const Z ti = simde_mm512_add_epi32(simde_mm512_slli_epi32(seli, 2), lane);
    const simde__mmask16 thi = simde_mm512_test_epi32_mask(ti, simde_mm512_set1_epi32(32));
    /* V = (h with the table's sign, ma, mb, the table's lane 3) per vertex */
    Z V = simde_mm512_mask_mov_epi32(simde_mm512_shuffle_epi32(m, (int)0x90), 0x1111, hs);
    V = simde_mm512_mask_mov_epi32(V, 0x8888, lut64(k_dec_tab, ti, thi));
    const Z src = lut64(k_dec_src, ti, thi);
    *out = simde_mm512_permutexvar_epi32(simde_mm512_add_epi32(simde_mm512_setr_epi32(0, 0, 0, 0, 4, 4, 4, 4, 8, 8, 8, 8, 12, 12, 12, 12), src), V);
    return bad;
}
static inline void st16n(uint8_t* ls, uint32_t a, Z v, uint32_t nq)
{
    if (likely(nq >= 4)) st16(ls, a, v);
    else cpu_mm512_mask_storeu_epi32(ls + (a & LSQ), (simde__mmask16)((1u << (4 * nq)) - 1), bswap16(v));
}
/* positions of all n2 vertices; b <= 16 */
static NOINL void dec_fast_pos(uint8_t* ls, const Dec* k, uint32_t b)
{
    const Z X = bcast4(k->X), shr = simde_mm512_set1_epi32((int)(16 - b)), shl = simde_mm512_set1_epi32((int)(16 + b));
    const Z ctl = DEC_PSHUF(0);
    for (uint32_t i = 0; i < k->n2; i += 4)
        st16n(ls, (k->U1 & LSQ) + 16 * i, dec_pos4(simde_mm512_shuffle_epi8(ld4rec(ls, k->P + 9 * i, 9), ctl), X, shr, shl), k->n2 - i);
}
/* the unit vectors of n2 vertices from records at rec (stride bytes apart),
 * the vector at byte o of each: the fast pass over all of them, then the
 * pairs it could not do, exactly. bad[g] collects group g's vertices that
 * need the exact path. */
static NOINL uint32_t dec_fast_uv_pass(uint8_t* ls, uint32_t rec, uint32_t stride, Z ctl, uint32_t dst, uint32_t n2, uint16_t* bad)
{
    uint32_t any = 0;
    for (uint32_t i = 0; i < n2; i += 4) {
        Z o;
        const uint32_t nq = n2 - i;
        simde__mmask16 m = dec_uv4(simde_mm512_shuffle_epi8(ld4rec(ls, rec + stride * i, stride), ctl), &o);
        st16n(ls, dst + 16 * i, o, nq);
        if (nq < 4) m &= 0x00FF;
        bad[i / 4] = m; any |= m;
    }
    return any;
}
static NOINL void dec_exact_uv_pair(uint8_t* ls, uint32_t ra, uint32_t rb, uint32_t dst)
{
    Q a, b;
    dec_uv_pair_q(ls, ra, rb, &a, &b);
    sq(ls, dst, a); sq(ls, dst + 16, b);
}
/* MXCSR is CSR_FAST here; csr is the caller's */
static void dec_fast_uv(uint8_t* ls, uint32_t rec, uint32_t stride, uint32_t o, uint32_t dst, uint32_t n2, uint32_t csr)
{
    static _Thread_local uint16_t bad[SPU_LS_SIZE / 64];
    dst &= LSQ;
    if (likely(!dec_fast_uv_pass(ls, rec, stride, o ? DEC_USHUF(6) : DEC_USHUF(0), dst, n2, bad))) return;
    _mm_setcsr(csr);
    for (uint32_t i = 0; i < n2; i += 4) {
        if (bad[i / 4]) EDGE_EXACT(g_dec_exact);
        for (uint32_t p = 0; p < 4; p += 2)
            if (bad[i / 4] & (0xFFu << (4 * p)))
                dec_exact_uv_pair(ls, rec + stride * (i + p) + o, rec + stride * (i + p + 1) + o, dst + 16 * (i + p));
    }
    _mm_setcsr(CSR_FAST);
}
/* dec_fast_pos for AVX2: two vertices per register; n2 is even */
static NOINL void dec_fast_pos_y(uint8_t* ls, const Dec* k, uint32_t b)
{
    const Y X = _mm256_setr_epi32((int)k->X._u32[0], (int)k->X._u32[1], (int)k->X._u32[2], (int)k->X._u32[3],
                                  (int)k->X._u32[0], (int)k->X._u32[1], (int)k->X._u32[2], (int)k->X._u32[3]);
    const Y shr = Y1(16 - b), shl = Y1(16 + b);
    const Y ctl = _mm256_setr_epi8(1, 0, -1, -1, 3, 2, -1, -1, 5, 4, -1, -1, -1, -1, -1, -1,
                                   1, 0, -1, -1, 3, 2, -1, -1, 5, 4, -1, -1, -1, -1, -1, -1);
    const uint32_t dst = k->U1 & LSQ;
    for (uint32_t i = 0; i < k->n2; i += 2) {
        const uint8_t* r = ls + k->P + 9 * i;
        const Y raw = _mm256_shuffle_epi8(_mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((const __m128i*)r)),
                                                                  _mm_loadu_si128((const __m128i*)(r + 9)), 1), ctl);
        const Y v = _mm256_add_epi32(raw, X);
        const __m256 hi = _mm256_cvtepi32_ps(_mm256_srav_epi32(v, shr));
        /* cuflt: hi16 * 65536 and lo16 are exact singles, their sum is rounded once */
        const Y u = _mm256_sllv_epi32(v, shl);
        const __m256 uf = _mm256_add_ps(_mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_srli_epi32(u, 16)), _mm256_set1_ps(65536.0f)),
                                        _mm256_cvtepi32_ps(_mm256_and_si256(u, Y1(0xFFFF))));
        const Y o = yi(_mm256_add_ps(hi, _mm256_mul_ps(uf, _mm256_set1_ps(0x1p-32f))));
        yst(ls, dst + 16 * i, _mm256_blend_epi32(o, Y1(F1), 0x88));
    }
}
/* per selector (0..11): the sources of lanes 0..2 in (h, ma, mb) at bits 2c, the sign of h at
 * bit 8, a negative lane 3 (-1.0) at bit 9; from k_dec_src / k_dec_sgn / k_dec_tab */
static _Alignas(32) uint32_t k_dec_desc[16];
static int g_dec_y_ok = 1;
static void dec_desc_init(void)
{
    for (int s = 0; s < 12; s++)
        k_dec_desc[s] = k_dec_src[4 * s] | k_dec_src[4 * s + 1] << 2 | k_dec_src[4 * s + 2] << 4 |
                        (k_dec_sgn[s] ? 1u << 8 : 0) | ((k_dec_tab[4 * s + 3] >> 31) << 9);
    for (int s = 12; s < 16; s++) k_dec_desc[s] = k_dec_desc[0];
    for (int s = 0; s < 12; s++) if ((k_dec_tab[4 * s + 3] & 0x7FFFFFFFu) != F1) g_dec_y_ok = 0;
}
/* dec_uv4 for eight vertices, one component per register (lane = vertex): the three record
 * bytes gathered as one word; writes nv (<= 8, even) quads at dst and returns the vertices whose
 * selector is past the table (bit i = vertex i; their quads are not the job's) */
static inline unsigned dec_uv8(uint8_t* ls, uint32_t rec, uint32_t stride, uint32_t dst, uint32_t nv)
{
    const Y lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const Y on = _mm256_cmpgt_epi32(Y1(nv), lanes);
    const Y w = _mm256_mask_i32gather_epi32(Y0, (const int*)(ls + rec), _mm256_mullo_epi32(lanes, Y1(stride)), on, 1);
    const Y b0 = _mm256_and_si256(w, Y1(0xFF)), b1 = _mm256_and_si256(_mm256_srli_epi32(w, 8), Y1(0xFF)),
            b2 = _mm256_and_si256(_mm256_srli_epi32(w, 16), Y1(0xFF));
    const Y sel = _mm256_and_si256(b2, Y1(15));
    const Y bad = _mm256_cmpgt_epi32(sel, Y1(11));
    const Y qa = _mm256_or_si256(Y1(F1), _mm256_srli_epi32(_mm256_or_si256(_mm256_slli_epi32(b0, 8), b1), 6));
    const Y qb = _mm256_or_si256(Y1(F1), _mm256_and_si256(_mm256_srli_epi32(_mm256_or_si256(_mm256_slli_epi32(b1, 8), b2), 4), Y1(0x3FF)));
    const Y ma = yFMA(Y1(DEC_K1), qa, Y1(DEC_K2)), mb = yFMA(Y1(DEC_K1), qb, Y1(DEC_K2));
    const Y s = yi(_mm256_sub_ps(_mm256_set1_ps(1.0f), _mm256_add_ps(yf(yFM(ma, ma)), yf(yFM(mb, mb)))));
    const Y h = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(s, Y1(0x7F800000)), Y0), yFM(s, rsq8(s)));
    /* the selector's descriptor (a bad selector takes selector 0's, as dec_uv4 does) */
    const Y seli = _mm256_andnot_si256(bad, sel);
    const Y d = _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(_mm256_load_si256((const __m256i*)k_dec_desc), seli),
                                   _mm256_permutevar8x32_epi32(_mm256_load_si256((const __m256i*)(k_dec_desc + 8)), seli),
                                   _mm256_cmpgt_epi32(seli, Y1(7)));
    const Y hs = _mm256_or_si256(yabs(h), _mm256_slli_epi32(_mm256_and_si256(d, Y1(1u << 8)), 23));
    Y c[4];
    for (int i = 0; i < 3; i++) {
        const Y src = _mm256_and_si256(_mm256_srli_epi32(d, 2 * i), Y1(3));
        c[i] = _mm256_blendv_epi8(_mm256_blendv_epi8(hs, ma, _mm256_cmpeq_epi32(src, Y1(1))), mb, _mm256_cmpeq_epi32(src, Y1(2)));
    }
    c[3] = _mm256_or_si256(Y1(F1), _mm256_slli_epi32(_mm256_and_si256(d, Y1(1u << 9)), 22));
    /* to one quad per vertex */
    const Y t0 = _mm256_unpacklo_epi32(c[0], c[1]), t1 = _mm256_unpackhi_epi32(c[0], c[1]);
    const Y t2 = _mm256_unpacklo_epi32(c[2], c[3]), t3 = _mm256_unpackhi_epi32(c[2], c[3]);
    const Y u0 = _mm256_unpacklo_epi64(t0, t2), u1 = _mm256_unpackhi_epi64(t0, t2);   /* vertices 0 | 4, 1 | 5 */
    const Y u2 = _mm256_unpacklo_epi64(t1, t3), u3 = _mm256_unpackhi_epi64(t1, t3);   /* 2 | 6, 3 | 7 */
    yst(ls, dst, _mm256_permute2x128_si256(u0, u1, 0x20));
    if (nv > 2) yst(ls, dst + 32, _mm256_permute2x128_si256(u2, u3, 0x20));
    if (nv > 4) yst(ls, dst + 64, _mm256_permute2x128_si256(u0, u1, 0x31));
    if (nv > 6) yst(ls, dst + 96, _mm256_permute2x128_si256(u2, u3, 0x31));
    return (unsigned)_mm256_movemask_ps(yf(_mm256_and_si256(bad, on)));
}
/* the fast pass over all n2 vertices (noinline: the compiler does not order arithmetic against MXCSR writes) */
static NOINL unsigned dec_fast_uv_pass_y(uint8_t* ls, uint32_t rec, uint32_t stride, uint32_t o, uint32_t dst, uint32_t n2, uint8_t* bad)
{
    unsigned any = 0;
    for (uint32_t i = 0; i < n2; i += 8) {
        const unsigned m = dec_uv8(ls, rec + stride * i + o, stride, dst + 16 * i, n2 - i < 8 ? n2 - i : 8);
        bad[i / 8] = (uint8_t)m; any |= m;
    }
    return any;
}
/* dec_fast_uv for AVX2: the unit vector at byte o of each record; MXCSR is CSR_FAST, csr the caller's */
static void dec_fast_uv_y(uint8_t* ls, uint32_t rec, uint32_t stride, uint32_t o, uint32_t dst, uint32_t n2, uint32_t csr)
{
    static _Thread_local uint8_t bad[SPU_LS_SIZE / 16];
    dst &= LSQ;
    if (likely(!dec_fast_uv_pass_y(ls, rec, stride, o, dst, n2, bad))) return;
    _mm_setcsr(csr);
    for (uint32_t i = 0; i < n2; i += 8) {
        if (!bad[i / 8]) continue;
        EDGE_EXACT(g_dec_exact);
        for (uint32_t p = 0; p < 8 && i + p < n2; p += 2)
            if (bad[i / 8] & (3u << p))
                dec_exact_uv_pair(ls, rec + stride * (i + p) + o, rec + stride * (i + p + 1) + o, dst + 16 * (i + p));
    }
    _mm_setcsr(CSR_FAST);
}
#ifdef EDGE_HARNESS
static NOINL void dec_selftest_fast(uint8_t* ls, uint32_t rec, uint32_t n, uint8_t* out, uint16_t* bad)
{
    const Z ctl = DEC_USHUF(0);
    for (uint32_t i = 0; i < n; i += 4) {
        Z o;
        bad[i / 4] = dec_uv4(simde_mm512_shuffle_epi8(ld4rec(ls, rec + 3 * i, 3), ctl), &o);
        simde_mm512_storeu_si512(out + 16 * i, bswap16(o));
    }
}
/* Every 24-bit unit vector through the fast decode and the exact one, as
 * vertex A of a pair and (with a scrambled partner) as vertex B. ls holds the
 * module image; 0x20000.. is scratch. Returns the number of differences. */
long edge_dec_uv_selftest(uint8_t* ls)
{
    rsq_init(); dec_tables_init();
    enum { N = 4096 };
    static uint8_t out[16 * N]; static uint16_t bad[N / 4];
    const uint32_t rec = 0x20000, csr = _mm_getcsr();
    long diff = 0, nbad = 0;
    for (uint32_t v0 = 0; v0 < (1u << 24); v0 += N / 2) {
        for (uint32_t j = 0; j < N; j++) {
            const uint32_t v = v0 + j / 2, x = (j & 1) ? ((v * 0x9E3779B1u) >> 8) & 0xFFFFFF : v;
            ls[rec + 3 * j] = x >> 16; ls[rec + 3 * j + 1] = x >> 8; ls[rec + 3 * j + 2] = x;
        }
        memset(ls + rec + 3 * N, 0, 64);
        _mm_setcsr(CSR_FAST);
        dec_selftest_fast(ls, rec, N, out, bad);
        _mm_setcsr(csr);
        for (uint32_t j = 0; j < N; j += 2) {
            Q a, b;
            dec_uv_pair_q(ls, rec + 3 * j, rec + 3 * j + 3, &a, &b);
            const uint16_t bm = bad[j / 4] >> (4 * (j & 3));
            Q fa = lq(out, 16 * j), fb = lq(out, 16 * j + 16);
            if (!(bm & 0x0F) && memcmp(&a, &fa, 16)) { if (diff++ < 8) fprintf(stderr, "uv A %06X: %08X %08X %08X %08X vs %08X %08X %08X %08X\n", (ls[rec + 3 * j] << 16) | (ls[rec + 3 * j + 1] << 8) | ls[rec + 3 * j + 2], a._u32[0], a._u32[1], a._u32[2], a._u32[3], fa._u32[0], fa._u32[1], fa._u32[2], fa._u32[3]); }
            if (!(bm & 0xF0) && memcmp(&b, &fb, 16)) { if (diff++ < 8) fprintf(stderr, "uv B\n"); }
            nbad += ((bm & 0x0F) != 0) + ((bm & 0xF0) != 0);
            const uint32_t sa = ls[rec + 3 * j + 2] & 15, sb = ls[rec + 3 * j + 5] & 15;
            if (((bm & 0x0F) != 0) != (sa >= 12) || ((bm & 0xF0) != 0) != (sb >= 12)) diff++;
        }
    }
    fprintf(stderr, "uv selftest: %ld differences, %ld vertices past the table\n", diff, nbad);
    return diff;
}
#endif
/* Why the decompression kernel left a call to the lifted code, for the stats
 * line: the in-game share of fallbacks is the cost still left in the lift, and
 * each reason has a different fix. */
enum { DEC_FB_GENERIC, DEC_FB_COUNT, DEC_FB_SLOTS, DEC_FB_OVERLAP, DEC_FB_OTHER, DEC_FB_N };
static const char* const k_dec_fb_name[DEC_FB_N] = { "generic formats", "zero count", "no free slot", "overlap", "other" };
static unsigned long long g_dec_fb[DEC_FB_N];
/* PS3RECOMP_EDGE_DEC_LAYOUT=0 restores the strict overlap guard (no spans of
 * different loops may overlap), the kill switch for the layout change. */
static int g_dec_layout = 1;
#define DEC_FALLBACK(r) do { __atomic_fetch_add(&g_dec_fb[r], 1, __ATOMIC_RELAXED); return 0; } while (0)
static int edge_dec_core(u128* gpr, uint8_t* ls)
{
    const uint32_t S = gpr[3]._u32[0], T = gpr[4]._u32[0], D = gpr[5]._u32[0], sp = gpr[1]._u32[0];
    if ((sp & 15) || sp < 0x400 || sp + 0x20 > SPU_LS_SIZE || (S & 15) || S + 0x180 > SPU_LS_SIZE) DEC_FALLBACK(DEC_FB_OTHER);
    /* the special format pair, tested on the aligned quads as the job does */
    const uint32_t F = word_at(ls, D + 44), F2 = word_at(ls, D + 0x30);
    const Q f0 = lq(ls, F), f1 = lq(ls, F + 16);
    if ((f0._u32[0] >> 24) != 2 || (f0._u32[2] & 0xFF) != 1 || (f1._u32[0] & 0xFF) != 2) DEC_FALLBACK(DEC_FB_GENERIC);
    if (F2) {
        const Q g = lq(ls, F2);
        if ((g._u32[0] >> 24) != 1 || (g._u32[2] & 0xFF) != 3) DEC_FALLBACK(DEC_FB_GENERIC);
    }
    Dec k; memset(&k, 0, sizeof k);
    k.S = S; k.tan = F2 != 0;
    k.P = word_at(ls, D + 20); k.P2 = word_at(ls, D + 24);
    const uint32_t numv = lq(ls, S)._u32[2] >> 16;
    if (!numv) DEC_FALLBACK(DEC_FB_COUNT);                /* the job's loop would run 2^27 times */
    k.n2 = (numv + 1) & ~1u;
    Q slots = lq(ls, S + 0x170);
    for (uint32_t j = 0; j < 2 + k.tan; j++) {
        const int i = dec_free_slot(slots);
        if (i < 0) DEC_FALLBACK(DEC_FB_SLOTS);
        dec_set_slot(&slots, i, j + 1);
        k.slot[j] = i;
    }
    k.U1 = word_at(ls, S + 16 + 4 * k.slot[0]); k.U2 = word_at(ls, S + 16 + 4 * k.slot[1]);
    k.U3 = k.tan ? word_at(ls, S + 16 + 4 * k.slot[2]) : 0;
    const uint32_t X = word_at(ls, D + 40);
    k.X = V4(word_at(ls, X), word_at(ls, X + 4), word_at(ls, X + 8), word_at(ls, X + 12));
    const uint32_t b = lq(ls, T + 0x10)._u32[2] & 0xFF;
    k.shr = V4(b - 16, b - 16, b - 16, 0); k.shl = V4(16 + b, 16 + b, 16 + b, 32);
    /* The overlap guard: outputs against each other and against everything
     * read. The job lays the uniform tables out back to back, 16 bytes times
     * the count rounded up to 8 apart (0x7198, loop 0x7330), so with a count
     * of 0 or 7 mod 8 the two zero quads past the normals are the first two
     * tangents. That was the in-game fallback: a quarter of the segments
     * with tangents. The two loops run one after the other: 0x4A30 writes
     * the positions, the normals and the normals' zero quads, interleaved
     * (its first iteration stores a pair of positions and then the zero
     * quads), and only then does 0x4D18 write the tangents and their zero
     * quads. So a span of the second loop may overlap one of the first: the
     * tangents overwrite it in the job and in the kernel, which keeps that
     * order. Spans of one loop still may not overlap (phases 2 and 3). */
    Span s[14]; int ns = 0;
    const int ph2 = g_dec_layout ? 2 : 1, ph3 = g_dec_layout ? 3 : 1;   /* 1: the strict guard of before */
    s[ns++] = (Span){ sp - 0x50, sp + 0x20, 1 };
    s[ns++] = (Span){ S + 0x50, S + 0x70, 1 };
    s[ns++] = (Span){ S + 0x170, S + 0x180, 1 };
    s[ns++] = (Span){ S, S + 0x50, 0 };
    s[ns++] = (Span){ (k.U2 & LSQ) + 16 * k.n2, (k.U2 & LSQ) + 16 * (k.n2 + 2), ph2 };
    s[ns++] = (Span){ k.U1 & LSQ, (k.U1 & LSQ) + 16 * k.n2, ph2 };
    s[ns++] = (Span){ k.U2 & LSQ, (k.U2 & LSQ) + 16 * k.n2, ph2 };
    if (k.tan) {
        s[ns++] = (Span){ (k.U3 & LSQ) + 16 * k.n2, (k.U3 & LSQ) + 16 * (k.n2 + 2), ph3 };
        s[ns++] = (Span){ k.U3 & LSQ, (k.U3 & LSQ) + 16 * k.n2, ph3 };
    }
    s[ns++] = (Span){ k.P, k.P + 9 * k.n2, 0 };
    if (k.tan) s[ns++] = (Span){ k.P2, k.P2 + 3 * k.n2, 0 };
    s[ns++] = (Span){ X & LSQ, (X & LSQ) + 32, 0 };
    s[ns++] = (Span){ 0x4000, 0xF6B0, 0 };              /* the module: tables, constants, code */
    s[ns++] = (Span){ D & LSQ, (D & LSQ) + 64, 0 };
    if (!spans_ok(s, ns)) DEC_FALLBACK(DEC_FB_OVERLAP);
    if (((T + 0x10) & LSQ) + 16 > SPU_LS_SIZE || (F & LSQ) + 32 > SPU_LS_SIZE) DEC_FALLBACK(DEC_FB_OTHER);
    /* the outputs must not reach what the job reads after writing them: T, F, F2 */
    {
        const uint32_t rd[3] = { (T + 0x10) & LSQ, F & LSQ, F2 & LSQ };
        for (int r = 0; r < 3; r++)
            for (int i = 0; i < ns; i++)
                if (s[i].w && rd[r] < s[i].hi && s[i].lo < rd[r] + 32) DEC_FALLBACK(DEC_FB_OVERLAP);
    }

    /* prologue */
    sq(ls, sp - 0x10, gpr[80]); sq(ls, sp - 0x20, gpr[81]); sq(ls, sp - 0x30, gpr[82]);
    sq(ls, sp + 0x10, gpr[0]); sq(ls, sp - 0x50, gpr[1]);
    {
        Q q = lq(ls, S + 0x50); q._u32[1] = k.P; q._u32[2] = k.P; q._u32[3] = k.U1; sq(ls, S + 0x50, q);
        q = lq(ls, S + 0x60); q._u32[0] = k.U2; if (k.tan) q._u32[1] = k.U3; sq(ls, S + 0x60, q);
        sq(ls, S + 0x170, slots);
    }
    const int fast = g_dec_ok && (!g_avx2 || g_dec_y_ok) && k.P + 9 * (k.n2 + 2) + 16 <= SPU_LS_SIZE && (!k.tan || k.P2 + 3 * (k.n2 + 2) + 16 <= SPU_LS_SIZE) &&
                     !memcmp(ls + DEC_TAB, k_dec_tab_be, sizeof k_dec_tab_be);
    /* Everything of the first loop before anything of the second (see the
     * guard above); within a loop the spans are disjoint, so any order. */
    sq(ls, k.U2 + 16 * k.n2, splat4(0)); sq(ls, k.U2 + 16 * k.n2 + 16, splat4(0));
    if (!fast || b > 16) dec_exact_pos(ls, &k);
    if (!fast) {
        dec_exact_uv(ls, k.P + 6, 9, k.U2, k.n2);
        if (k.tan) {
            sq(ls, k.U3 + 16 * k.n2, splat4(0)); sq(ls, k.U3 + 16 * k.n2 + 16, splat4(0));
            dec_exact_uv(ls, k.P2, 3, k.U3, k.n2);
        }
    } else {
        const uint32_t csr = _mm_getcsr();
        _mm_setcsr(CSR_FAST);
        void (*const uv)(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = g_avx2 ? dec_fast_uv_y : dec_fast_uv;
        if (b <= 16) (g_avx2 ? dec_fast_pos_y : dec_fast_pos)(ls, &k, b);
        uv(ls, k.P, 9, 6, k.U2, k.n2, csr);
        if (k.tan) {
            sq(ls, k.U3 + 16 * k.n2, splat4(0)); sq(ls, k.U3 + 16 * k.n2 + 16, splat4(0));
            uv(ls, k.P2, 3, 0, k.U3, k.n2, csr);
        }
        _mm_setcsr(csr);
    }
    gpr[80] = lq(ls, sp - 0x10); gpr[81] = lq(ls, sp - 0x20); gpr[82] = lq(ls, sp - 0x30);
    gpr[0] = lq(ls, sp + 0x10);
    return 1;
}

/* ================================================================== 0x99E8: vertex compression, output format 1
 *
 * 0x9780 compresses the segment's vertices for the RSX through a jump table on
 * the output format (S+4); 0x99E8 is the case for format 1, 20 bytes per
 * vertex: the position's three floats as they are, then the two unit-vector
 * streams packed as X11Y11Z10. The three streams are the uniform tables whose
 * slot bytes at S+0x170 hold 1 (positions), 2 and 3; per component
 * i = cflts(fma(scale, v, bias), 142) (scale and bias from the job's constants
 * at 0xE920/0xE930, about 0.9995 and -2^-11), and the word is
 * (z & 0xFFC00000) | (y >> 21) << 11 | x >> 21.
 *
 * The case is a chunk of 0x9780, entered from its jump table with the
 * registers 0x9780 set up (S in r81, the output pointer in r82, the count
 * rounded up to two in r33, masks and constants in r26..r44), and it goes on
 * at 0x98B0, which updates the output pointer and returns. So the hook
 * transfers there (tools/lift_spu.py, an entry with a continuation), and
 * these registers are its inputs: the kernel checks they hold what 0x9780
 * puts in them and falls back otherwise. Nothing the chunk writes to a
 * register is read at 0x98B0 or later: r80 is overwritten there before any
 * read, and r2..r79 are dead.
 *
 * The output loop writes two vertices (40 bytes) per step at any alignment:
 * four quads, the first merged with the bytes already in the LS before the
 * output pointer; past its last vertex, the last step leaves the pattern
 * (n, t, 0, 0) of the last vertex's packed words, repeated from 32 bytes
 * after the last step's start, up to the end of its fourth quad. */
#define ENC_OUT_QUADS(o, n2) ((((o) + 20 * (n2) + 8) & ~15u) + 16)   /* end of the last step's fourth quad */
static const uint32_t k_enc_scale[4] = { 0x3F7FE000, 0x3F7FE000, 0x3F7FC000, 0 };
static const uint32_t k_enc_bias[4] = { 0xBA000000, 0xBA000000, 0xBA800000, 0 };
static inline int q_eq(Q a, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    return a._u32[0] == w0 && a._u32[1] == w1 && a._u32[2] == w2 && a._u32[3] == w3;
}
static inline int q_same(Q a, Q b) { return !memcmp(&a, &b, sizeof a); }
static inline uint32_t enc_pack_q(Q v, Q scale, Q bias)
{
    const Q c = spu_cflts(spu_fma(scale, v, bias), 142);
    return (c._u32[2] & 0xFFC00000u) | (c._u32[1] >> 21) << 11 | c._u32[0] >> 21;
}
static inline void ls_w32(uint8_t* ls, uint32_t a, uint32_t v)
{
    ls[a & LSM] = v >> 24; ls[(a + 1) & LSM] = v >> 16; ls[(a + 2) & LSM] = v >> 8; ls[(a + 3) & LSM] = v;
}
typedef struct { uint32_t O, n2, U1, U2, U3; Q scale, bias; } Enc;
/* the trailing pattern of the last step (the LS bytes past the last vertex),
 * at the caller's MXCSR */
static NOINL void enc_tail(uint8_t* ls, const Enc* k)
{
    const uint32_t last = k->n2 - 1;
    const uint32_t n = enc_pack_q(lq(ls, k->U2 + 16 * last), k->scale, k->bias), t = enc_pack_q(lq(ls, k->U3 + 16 * last), k->scale, k->bias);
    const uint32_t g = k->O + 20 * k->n2 - 8, end = ENC_OUT_QUADS(k->O, k->n2);
    const uint8_t pat[16] = { n >> 24, n >> 16, n >> 8, n, t >> 24, t >> 16, t >> 8, t };
    for (uint32_t a = k->O + 20 * k->n2; a < end; a++) ls[a & LSM] = pat[(a - g) & 15];
}
static NOINL void enc_exact(uint8_t* ls, const Enc* k, uint32_t from, uint32_t to)
{
    for (uint32_t v = from; v < to; v++) {
        const Q p = lq(ls, k->U1 + 16 * v);
        const uint32_t n = enc_pack_q(lq(ls, k->U2 + 16 * v), k->scale, k->bias), t = enc_pack_q(lq(ls, k->U3 + 16 * v), k->scale, k->bias);
        const uint32_t o = k->O + 20 * v;
        ls_w32(ls, o, p._u32[0]); ls_w32(ls, o + 4, p._u32[1]); ls_w32(ls, o + 8, p._u32[2]);
        ls_w32(ls, o + 12, n); ls_w32(ls, o + 16, t);
    }
}
/* The fast path, four vertices per register. With the job's scale and bias
 * (|scale| < 1, |bias| tiny) a component without exponent 255 gives an fma
 * that cannot overflow, and x86 under CSR_FAST rounds it like the helpers; a
 * denormal or tiny result converts to 0 on both sides. cflts saturates where
 * cvtt gives 0x80000000: only values >= 1.0 need fixing. A group with an
 * exponent-255 component is left to the exact path (returns 0, writes nothing). */
static inline Z enc_pack4(Z v, Z scale, Z bias)
{
    const simde__m512 f = simde_mm512_fmadd_ps(zf(scale), zf(v), zf(bias));
    Z c = simde_mm512_cvttps_epi32(simde_mm512_mul_ps(f, simde_mm512_set1_ps(0x1p31f)));
    c = simde_mm512_mask_mov_epi32(c, simde_mm512_cmp_ps_mask(f, simde_mm512_set1_ps(1.0f), SIMDE_CMP_GE_OQ), simde_mm512_set1_epi32(0x7FFFFFFF));
    const Z y = simde_mm512_shuffle_epi32(c, (int)0x55), z = simde_mm512_shuffle_epi32(c, (int)0xAA);
    return simde_mm512_ternarylogic_epi32(simde_mm512_and_si512(z, simde_mm512_set1_epi32((int)0xFFC00000u)), simde_mm512_slli_epi32(simde_mm512_srli_epi32(y, 21), 11),
                                     simde_mm512_srli_epi32(c, 21), 0xFE);   /* lane 0 of each quad */
}
static NOINL int enc_fast4(uint8_t* ls, const Enc* k, uint32_t v, uint32_t nv)
{
    const Z N = ld16(ls, k->U2 + 16 * v), T = ld16(ls, k->U3 + 16 * v);
    const Z e = simde_mm512_set1_epi32(0x7F800000);
    if (unlikely((simde_mm512_cmpeq_epi32_mask(simde_mm512_and_si512(N, e), e) | simde_mm512_cmpeq_epi32_mask(simde_mm512_and_si512(T, e), e)) & 0x7777)) return 0;
    const Z scale = bcast4(k->scale), bias = bcast4(k->bias);
    const Z n = bswap16(enc_pack4(N, scale, bias)), t = bswap16(enc_pack4(T, scale, bias));
    /* (x, y, z, n) per vertex in LS byte order, the position's bytes as they are */
    const Z r = simde_mm512_mask_mov_epi32(cpu_mm512_loadu_si512(ls + k->U1 + 16 * v), 0x8888, simde_mm512_shuffle_epi32(n, (int)0x00));
    uint8_t* o = ls + k->O + 20 * v;
    _mm_storeu_si128((__m128i*)o, simde_mm512_castsi512_si128(r));
    _mm_storeu_si32(o + 16, simde_mm512_castsi512_si128(t));
    _mm_storeu_si128((__m128i*)(o + 20), simde_mm512_extracti32x4_epi32(r, 1));
    _mm_storeu_si32(o + 36, simde_mm512_extracti32x4_epi32(t, 1));
    if (nv > 2) {
        _mm_storeu_si128((__m128i*)(o + 40), simde_mm512_extracti32x4_epi32(r, 2));
        _mm_storeu_si32(o + 56, simde_mm512_extracti32x4_epi32(t, 2));
        _mm_storeu_si128((__m128i*)(o + 60), simde_mm512_extracti32x4_epi32(r, 3));
        _mm_storeu_si32(o + 76, simde_mm512_extracti32x4_epi32(t, 3));
    }
    return 1;
}
/* the slot byte holding id, or -1 */
static inline int enc_slot(Q q, uint32_t id)
{
    for (int i = 0; i < 16; i++) if (((q._u32[i >> 2] >> (24 - 8 * (i & 3))) & 0xFF) == id) return i;
    return -1;
}
static int edge_enc_core(u128* gpr, uint8_t* ls)
{
    const uint32_t S = gpr[81]._u32[0], O = gpr[82]._u32[0], n2 = gpr[33]._u32[0];
    /* what 0x9780 leaves in the chunk's input registers */
    if (gpr[27]._u32[0] != S + 16 || !q_eq(gpr[37], 0x003FF800u, 0x003FF800u, 0x003FF800u, 0x003FF800u) ||
        !q_eq(gpr[44], 0x03030303u, 0x03030303u, 0x03030303u, 0x03030303u) ||
        !q_eq(gpr[26], 0x20212223u, 0x24252627u, 0x28292A2Bu, 0x2C2D2E2Fu) ||
        !q_eq(gpr[30], 0x10111213u, 0x14151617u, 0x18191A1Bu, 0x1C1D1E1Fu) ||
        !q_eq(gpr[41], 0x08090001u, 0x18191011u, 0x08090001u, 0x18191011u) ||
        !q_eq(gpr[31], 0x0D04051Cu, 0x1D14150Cu, 0x0D04051Cu, 0x1D14150Cu) ||
        !q_eq(lq(ls, 0xE910), 0xFFFFFFFBu, 0xFFFFFFFEu, 0, 0) ||
        !q_same(gpr[29], lq(ls, 0xE920)) || !q_same(gpr[28], lq(ls, 0xE930)))
        return 0;
    if (!n2 || (n2 & 1) || n2 > 0x2000 || (S & 15) || S + 0x180 > SPU_LS_SIZE) return 0;
    const Q slots = lq(ls, S + 0x170);
    const int i1 = enc_slot(slots, 1), i2 = enc_slot(slots, 2), i3 = enc_slot(slots, 3);
    if (i1 < 0 || i2 < 0 || i3 < 0) return 0;
    Enc k;
    k.O = O; k.n2 = n2; k.scale = gpr[29]; k.bias = gpr[28];
    k.U1 = word_at(ls, S + 16 + 4 * i1) & LSQ; k.U2 = word_at(ls, S + 16 + 4 * i2) & LSQ; k.U3 = word_at(ls, S + 16 + 4 * i3) & LSQ;
    /* the overlap guard: the output against everything read (the streams with the job's read-ahead) */
    Span s[6]; int ns = 0;
    s[ns++] = (Span){ O & LSQ, ENC_OUT_QUADS(O, n2), 1 };
    s[ns++] = (Span){ S + 0x10, S + 0x50, 0 };
    s[ns++] = (Span){ S + 0x170, S + 0x180, 0 };
    s[ns++] = (Span){ k.U1, k.U1 + 16 * (n2 + 4), 0 };
    s[ns++] = (Span){ k.U2, k.U2 + 16 * (n2 + 4), 0 };
    s[ns++] = (Span){ k.U3, k.U3 + 16 * (n2 + 4), 0 };
    if (!spans_ok(s, ns)) return 0;
    {
        const Span m[2] = { s[0], { 0x4000, 0xF6B0, 0 } };   /* the module: its constants and code */
        if (!spans_ok(m, 2)) return 0;
    }
    if (q_eq(k.scale, k_enc_scale[0], k_enc_scale[1], k_enc_scale[2], k_enc_scale[3]) &&
        q_eq(k.bias, k_enc_bias[0], k_enc_bias[1], k_enc_bias[2], k_enc_bias[3])) {
        const uint32_t csr = _mm_getcsr();
        _mm_setcsr(CSR_FAST);
        for (uint32_t v = 0; v < n2; v += 4)
            if (unlikely(!enc_fast4(ls, &k, v, n2 - v))) {
                EDGE_EXACT(g_enc_exact);
                _mm_setcsr(csr);
                enc_exact(ls, &k, v, v + 4 < n2 ? v + 4 : n2);
                _mm_setcsr(CSR_FAST);
            }
        _mm_setcsr(csr);
    } else
        enc_exact(ls, &k, 0, n2);
    enc_tail(ls, &k);
    return 1;
}

/* ================================================================== 0xC7B8: index decompression
 *
 * 0xD030 calls it for the compressed index formats (S byte 3, high nibble 2
 * or 3) with r3 = S and r4 = B, the segment's index data, and it rewrites B in
 * place as the triangle list of u16 indexes the cull reads. T, the triangle
 * count, is the u16 at S+0x1A0 times 0xAAAB (as mpy does it: signed 16-bit
 * operands, the low 16 bits kept), which is the count / 3. B holds:
 *
 *   +0 N0 (u16), +2 a bias (u16), +4 L (u16), +6 c (byte): a header;
 *   +8 L bytes of flags, one bit per vertex reference, most significant first;
 *   then CB = (2T + 7) / 8 bytes of 2-bit triangle codes;
 *   then N0 values of c bits each, packed most significant first.
 *
 * The job decodes it in four loops, each over the result of the one before:
 *
 *   1. The values, unpacked in place to u16 at OUT = B + 16-aligned(8 + L +
 *      CB), 32 at a time (so 32 * ceil(N0 / 32), the last ones from whatever
 *      bits follow), then delta-decoded per quad: v[q] = raw[q] - bias +
 *      v[q - 1], eight halfword lanes, mod 2^16. c = 0 stores ceil(N0 / 8)
 *      zero quads instead of unpacking.
 *   2. The vertex list at LIST = B + 16-aligned(6T), 16 * ceil(L / 2) u16
 *      entries: a set flag takes the next value, a clear one the next number
 *      of a counter from 0.
 *   3. The codes copied, whole quads, to CODES = the end of the list.
 *   4. The triangles, eight per group of 16 code bits, 3 quads per group at
 *      B + 48 g for every group (so up to seven past T, from the bits and
 *      entries that follow). Code 3 takes three new entries; codes 0, 1 and 2
 *      take one, n, with an edge of the previous triangle p (zeros at the
 *      start): (p0, p2, n), (p2, p1, n), (p1, p0, n).
 *
 * Then 0x64A8 stores the end, B + 48 * groups, at S+0x58.
 *
 * The loops are software pipelined, and the in-place writes run close behind
 * the reads: the unpack goes backwards from the end, the triangles are stored
 * after the next group is loaded. The kernel does the same work in the same
 * order of loops, reading what each loop would read, and falls back where a
 * loop could read bytes it has itself overwritten (the overlap guards below),
 * on a B or S off a quad boundary, c above 15, an empty flag stream or no
 * triangle, all of which the job's loops treat in ways not modelled here.
 * The lift reads its shuffle masks and constants from the module's data at
 * 0xEB70..0xF000; the kernel does not use them, but checks they hold the
 * module's bytes (a hash) so that a different module never runs this. */
#define IDX_TAB_LO 0xEB70u
#define IDX_TAB_HI 0xF000u
#define IDX_TAB_HASH 0x6007FBFCAD636EBCull
static int g_idx_tab_ok;                   /* the hash matched once: from then on a cheaper compare */
static uint8_t g_idx_tab[IDX_TAB_HI - IDX_TAB_LO];
static uint64_t idx_tab_hash(const uint8_t* p)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < IDX_TAB_HI - IDX_TAB_LO; i += 8) {
        uint64_t w; memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull; h ^= h >> 29;
    }
    return h;
}
static int idx_tables_ok(const uint8_t* ls)
{
    const uint8_t* p = ls + IDX_TAB_LO;
    if (likely(__atomic_load_n(&g_idx_tab_ok, __ATOMIC_ACQUIRE))) return !memcmp(p, g_idx_tab, sizeof g_idx_tab);
    if (idx_tab_hash(p) != IDX_TAB_HASH) return 0;
    /* Racing threads copy the same bytes; the flag is published after. */
    memcpy(g_idx_tab, p, sizeof g_idx_tab);
    __atomic_store_n(&g_idx_tab_ok, 1, __ATOMIC_RELEASE);
    return 1;
}
#ifdef EDGE_HARNESS
unsigned long long edge_idx_tab_hash(const uint8_t* ls) { return idx_tab_hash(ls + IDX_TAB_LO); }
#endif
/* Limits of the host buffers (real segments are far below them). */
#define IDX_MAXL 4096u                       /* flag bytes: up to 16 * 2048 list entries */
#define IDX_MAXT 8192u                       /* triangles: 1024 groups */
#define IDX_MAXV (8192u + 16 * IDX_MAXL + 64)   /* values: 32 * 256 unpacked, or as many as the flags */
#define IDX_MAXE (8 * IDX_MAXL + 24 * (IDX_MAXT / 8) + 64)
#define IDX_BUF_BYTES (2 * (IDX_MAXV + IDX_MAXE + IDX_MAXT / 8 + 64))
/* Loop 2 per flag byte m: the pshufb that takes the values of the set flags,
 * in order, from the next eight (host-order u16), the counter's offset for
 * the clear ones, and which lanes are set (lane j is bit 7 - j). */
static _Alignas(16) uint8_t k_idx_shuf[256][16];
static _Alignas(16) uint16_t k_idx_cofs[256][8];
static simde__mmask8 k_idx_sel[256];
static _Alignas(16) uint8_t k_idx_selv[256][16];   /* edge2: k_idx_sel as a byte mask (pblendvb) */
/* Loop 4 per pair of triangles (four code bits, the first triangle's in the
 * high two): the permute indexes into (previous pair, next entries) for the
 * pair's six indexes, and the entries the pair takes. */
static _Alignas(16) uint16_t k_idx_pair[16][8];
static uint8_t k_idx_take[16];
/* edge2: k_idx_pair as two pshufb controls, the lanes from the previous pair and from the entries */
static _Alignas(16) uint8_t k_idx_cp[16][16], k_idx_ce[16][16];
static void idx_pair_tri(int code, const uint16_t* p, uint16_t next, uint16_t* t, int* used)
{
    /* p: the previous triangle's three sources; next: the first unused entry's */
    if (code == 3) { t[0] = next; t[1] = next + 1; t[2] = next + 2; *used = 3; return; }
    static const int ia[3] = { 0, 2, 1 }, ib[3] = { 2, 1, 0 };
    t[0] = p[ia[code]]; t[1] = p[ib[code]]; t[2] = next; *used = 1;
}
static void idx_tables_init(void)
{
    for (int nib = 0; nib < 16; nib++) {
        /* sources: 3..5 the previous pair's second triangle, 8.. the entries */
        const uint16_t p[3] = { 3, 4, 5 };
        uint16_t t1[3], t2[3]; int u1, u2;
        idx_pair_tri(nib >> 2, p, 8, t1, &u1);
        idx_pair_tri(nib & 3, t1, (uint16_t)(8 + u1), t2, &u2);
        for (int j = 0; j < 3; j++) { k_idx_pair[nib][j] = t1[j]; k_idx_pair[nib][3 + j] = t2[j]; }
        k_idx_pair[nib][6] = k_idx_pair[nib][7] = 0;
        k_idx_take[nib] = (uint8_t)(u1 + u2);
        for (int j = 0; j < 8; j++) {
            const int src = k_idx_pair[nib][j] & 15, e = src >= 8, l = src & 7;
            k_idx_cp[nib][2 * j] = e ? 0x80 : (uint8_t)(2 * l); k_idx_cp[nib][2 * j + 1] = e ? 0x80 : (uint8_t)(2 * l + 1);
            k_idx_ce[nib][2 * j] = e ? (uint8_t)(2 * l) : 0x80; k_idx_ce[nib][2 * j + 1] = e ? (uint8_t)(2 * l + 1) : 0x80;
        }
    }
    for (int m = 0; m < 256; m++) {
        int ones = 0;
        k_idx_sel[m] = 0;
        for (int j = 0; j < 8; j++) {
            const int f = (m >> (7 - j)) & 1;
            k_idx_shuf[m][2 * j] = f ? (uint8_t)(2 * ones) : 0x80;
            k_idx_shuf[m][2 * j + 1] = f ? (uint8_t)(2 * ones + 1) : 0x80;
            k_idx_cofs[m][j] = (uint16_t)(j - ones);
            k_idx_selv[m][2 * j] = k_idx_selv[m][2 * j + 1] = f ? 0xFF : 0;
            if (f) { k_idx_sel[m] |= (simde__mmask8)(1u << j); ones++; }
        }
    }
}
static inline uint32_t ls_rd16(const uint8_t* ls, uint32_t a) { return (uint32_t)ls[a] << 8 | ls[a + 1]; }
static inline void ls_wr16(uint8_t* ls, uint32_t a, uint32_t v) { ls[a] = (uint8_t)(v >> 8); ls[a + 1] = (uint8_t)v; }
static inline int idx_disjoint(uint32_t alo, uint32_t ahi, uint32_t blo, uint32_t bhi) { return ahi <= blo || bhi <= alo; }
/* Why the index kernel left a call to the lifted code (the stats line). */
enum { IDX_FB_SHAPE, IDX_FB_OVERLAP, IDX_FB_TABLES, IDX_FB_N };
static const char* const k_idx_fb_name[IDX_FB_N] = { "unmodelled shape", "overlap", "module data" };
static unsigned long long g_idx_fb[IDX_FB_N];
#ifdef EDGE_HARNESS
const unsigned long long* edge_idx_fallbacks(void) { return g_idx_fb; }
#endif
#define IDX_FALLBACK(r) do { __atomic_fetch_add(&g_idx_fb[r], 1, __ATOMIC_RELAXED); return 0; } while (0)
static int edge_idx_core(u128* gpr, uint8_t* ls)
{
    const uint32_t S = gpr[3]._u32[0], B = gpr[4]._u32[0], sp = gpr[1]._u32[0];
    if ((S & 15) || (B & 15) || (sp & 15) || S + 0x1B0 > SPU_LS_SIZE || B + 16 > SPU_LS_SIZE) IDX_FALLBACK(IDX_FB_SHAPE);
    const uint32_t T = (uint32_t)((int32_t)(int16_t)ls_rd16(ls, S + 0x1A0) * (int32_t)(int16_t)0xAAAB) & 0xFFFF;
    const uint32_t N0 = ls_rd16(ls, B), bias = ls_rd16(ls, B + 2), L = ls_rd16(ls, B + 4), c = ls[B + 6];
    /* The unpack loop keeps its byte offset in 16-bit products (mpy), which
     * wrap past N0 = 32 * 32768 / (4c); real segments have a few hundred. */
    if (!T || !L || c > 15 || (c && !N0) || N0 > 8192 || L > IDX_MAXL || T > IDX_MAXT) IDX_FALLBACK(IDX_FB_SHAPE);
    const uint32_t CB = (2 * T + 7) >> 3;
    const uint32_t BS = B + 8 + L + CB, OUT = B + ((8 + L + CB + 15) & ~15u);
    const uint32_t nb = (N0 + 31) >> 5;
    const uint32_t LIST = B + ((6 * T + 15) & ~15u), it = (8 * L + 15) >> 4, nl = 16 * it;
    const uint32_t CODES = LIST + 32 * it, q3 = (CB + 15) >> 4, CSRC = B + 8 + L;
    const uint32_t G = (T + 7) >> 3, END = B + 48 * G;
    /* Every span inside the LS (the lift would wrap; such a call is not a real one). */
    const uint32_t v_end = OUT + 64 * nb;     /* c = 0 zeroes ceil(N0 / 8) <= 4 nb quads of it */
    if (v_end + 64 > SPU_LS_SIZE || BS + 4 * c * nb + 32 > SPU_LS_SIZE || CODES + 16 * q3 + 64 > SPU_LS_SIZE || END > SPU_LS_SIZE)
        IDX_FALLBACK(IDX_FB_SHAPE);
    /* Nothing the job writes may reach S's quad at 0x50, the stack, the module. */
    {
        const uint32_t whi = v_end > CODES + 16 * q3 ? v_end : CODES + 16 * q3;
        const uint32_t wend = whi > END ? whi : END;
        if (!idx_disjoint(B, wend, S + 0x50, S + 0x60) || !idx_disjoint(B, wend, S + 0x1A0, S + 0x1B0) ||
            !idx_disjoint(B, wend, 0x4000, 0xF6B0) || !idx_disjoint(B, wend, sp - 0x400, sp + 0x40))
            IDX_FALLBACK(IDX_FB_OVERLAP);
    }
    if (!idx_tables_ok(ls)) IDX_FALLBACK(IDX_FB_TABLES);
    /* The guards of loops 2-4 need the flag count and the list positions;
     * both come from bytes no earlier loop writes (the flags and codes lie
     * before OUT), so they are taken first and the call can still fall back
     * having written nothing. */
    uint32_t nflag = 0;
    for (uint32_t i = 0; i < 2 * it; i++) nflag += (uint32_t)__builtin_popcount(ls[B + 8 + i]);
    /* loop 2 reads the values (two quads past where it is, ahead) and the
     * flags while it stores the list */
    if (!idx_disjoint(OUT, OUT + 2 * nflag + 48, LIST, LIST + 2 * nl) || !idx_disjoint(B, B + 8 + 2 * it + 16, LIST, LIST + 2 * nl))
        IDX_FALLBACK(IDX_FB_OVERLAP);
    /* loop 3 reads a quad ahead of the one it stores */
    if (!idx_disjoint(CSRC & ~15u, ((CSRC + 16 * q3) & ~15u) + 32, CODES, CODES + 16 * q3)) IDX_FALLBACK(IDX_FB_OVERLAP);
    /* Loop 4 loads group g's codes and entries after storing the groups up
     * to g - 2 and the pipeline's first, garbage, group at B. Each triangle
     * takes at least one entry, so group g reads from LIST + 16g or later,
     * and the codes lie past the list. That is past B + 48 * max(1, g - 1)
     * for every 1 <= g < G: G >= 2 means T >= 9, so LIST - B >= 6T >= 54,
     * and 32(G - 1) - 48 < 4T. So no call needs a check here. The entries
     * read stay inside the LS (at most 24 per group), and so do the values
     * loop 2 reads. */
    if (LIST + 48 * G + 64 > SPU_LS_SIZE || OUT + 2 * nflag + 64 > SPU_LS_SIZE) IDX_FALLBACK(IDX_FB_SHAPE);

    /* The loops run on host-order copies (values, list entries), and each
     * stores its buffer to the LS as the job leaves it. */
    static _Thread_local uint16_t* buf;
    if (unlikely(!buf) && !(buf = (uint16_t*)_aligned_malloc(IDX_BUF_BYTES, 64))) IDX_FALLBACK(IDX_FB_SHAPE);
    uint16_t* const V = buf;
    uint16_t* const E = buf + IDX_MAXV;
    const __m128i bsw = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);

    /* loop 1: the values (from the bits as they are before the job's
     * backwards, in-place unpack reaches them: it never reads a byte it has
     * written, RESEARCH.md 6.1), then the delta per quad lane */
    const uint32_t nv = 32 * nb;
    if (c) {
        const uint32_t sh = 64 - c;
        for (uint32_t i = 0; i < nv; i++) {
            const uint32_t bit = i * c;
            uint64_t w; memcpy(&w, ls + BS + (bit >> 3), 8);
            V[i] = (uint16_t)((__builtin_bswap64(w) << (bit & 7)) >> sh);
        }
    } else {
        const uint32_t nz = 8 * ((N0 + 7) >> 3);   /* zeroed; the rest of the block keeps the LS */
        memset(V, 0, 2 * nz);
        for (uint32_t i = nz; i < nv; i += 8)
            _mm_storeu_si128((__m128i*)(V + i), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + OUT + 2 * i)), bsw));
    }
    {
        const __m128i bv = _mm_set1_epi16((short)bias);
        __m128i prev = _mm_setzero_si128();
        for (uint32_t q = 0; q < 4 * nb; q++) {
            prev = _mm_add_epi16(_mm_sub_epi16(_mm_loadu_si128((const __m128i*)(V + 8 * q)), bv), prev);
            _mm_storeu_si128((__m128i*)(V + 8 * q), prev);
            _mm_storeu_si128((__m128i*)(ls + OUT + 16 * q), _mm_shuffle_epi8(prev, bsw));
        }
    }
    /* values past the unpacked blocks are whatever the LS holds there (only
     * garbage flags reach them) */
    for (uint32_t i = nv; i < nflag + 8; i++) V[i] = (uint16_t)ls_rd16(ls, OUT + 2 * i);
    /* loop 2: eight entries per flag byte, the set ones from the values, the
     * others from the counter */
    {
        uint32_t k = 0, ctr = 0;
        for (uint32_t i = 0; i < 2 * it; i++) {
            const uint32_t m = ls[B + 8 + i];
            const __m128i vv = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(V + k)), _mm_load_si128((const __m128i*)k_idx_shuf[m]));
            const __m128i cc = _mm_add_epi16(_mm_set1_epi16((short)ctr), _mm_load_si128((const __m128i*)k_idx_cofs[m]));
            const __m128i o = g_avx2 ? _mm_blendv_epi8(cc, vv, _mm_load_si128((const __m128i*)k_idx_selv[m]))
                                     : simde_mm_mask_blend_epi16(k_idx_sel[m], cc, vv);
            _mm_storeu_si128((__m128i*)(E + 8 * i), o);
            _mm_store_si128((__m128i*)(ls + LIST + 16 * i), _mm_shuffle_epi8(o, bsw));
            const uint32_t p = (uint32_t)__builtin_popcount(m);
            k += p; ctr += 8 - p;
        }
    }
    /* loop 3 */
    memcpy(ls + CODES, ls + CSRC, 16 * q3);
    /* loop 4: entries past the list are what the LS holds after loop 3 (the
     * copied codes and on); every group is built from that state before any
     * is stored (see the guard above), and the codes are copied first */
    {
        uint16_t* const C = E + IDX_MAXE;
        /* the entries the groups take (the 8-lane loads read past them, but
         * the permutes use only those) */
        uint32_t need = 8 * G;
        for (uint32_t g = 0; g < G; g++) {
            const uint32_t cw = ls_rd16(ls, CODES + 2 * g);
            C[g] = (uint16_t)cw;
            need += 2 * (uint32_t)__builtin_popcount(cw & (cw >> 1) & 0x5555);
        }
        for (uint32_t e = nl; e < need; e++) E[e] = (uint16_t)ls_rd16(ls, LIST + 2 * e);
        /* Two triangles per step: one two-source permute of the previous
         * pair (its second triangle in lanes 3..5) and the next entries, by
         * the pair's four code bits (k_idx_pair); the result is the next
         * step's previous pair. */
        __m128i prev = _mm_setzero_si128();
        uint32_t pos = 0;
        uint8_t* o = ls + B;
        for (uint32_t g = 0; g < G; g++) {
            const uint32_t cw = C[g];
            for (int h = 12; h >= 0; h -= 4, o += 12) {
                const uint32_t nib = (cw >> h) & 15;
                const __m128i en = _mm_loadu_si128((const __m128i*)(E + pos));
                if (g_avx2) {
                    prev = _mm_or_si128(_mm_shuffle_epi8(prev, _mm_load_si128((const __m128i*)k_idx_cp[nib])),
                                        _mm_shuffle_epi8(en, _mm_load_si128((const __m128i*)k_idx_ce[nib])));
                    const __m128i w = _mm_shuffle_epi8(prev, bsw);
                    _mm_storel_epi64((__m128i*)o, w);
                    const uint32_t hi = (uint32_t)_mm_extract_epi32(w, 2); memcpy(o + 8, &hi, 4);
                } else {
                    prev = simde_mm_permutex2var_epi16(prev, _mm_load_si128((const __m128i*)k_idx_pair[nib]), en);
                    cpu_mm_mask_storeu_epi16(o, 0x3F, _mm_shuffle_epi8(prev, bsw));
                }
                pos += k_idx_take[nib];
            }
        }
    }
    /* 0x64A8: S+0x58 = the end */
    ls_w32(ls, S + 0x58, END);
    return 1;
}

/* ================================================================== the hooks
 *
 * A kernel is int core(u128* gpr, uint8_t* ls): it reads and writes only the
 * registers and the local store it is given, and returns 0 (having written
 * nothing) where the lifted function has to run. */
typedef int (*EdgeCore)(u128* gpr, uint8_t* ls);
typedef struct {
    uint32_t addr; const char* name; EdgeCore core;
    /* a chunk inside a guest function goes on at cont when done (0: a
     * function, which returns through r0); r80 is then dead, not compared */
    uint32_t cont;
    /* its own switch (0: PS3RECOMP_NATIVE_EDGE alone); kernels with one are
     * off unless it is set, and PS3RECOMP_NATIVE_EDGE=0 turns everything off */
    const char* env;
    const unsigned long long* exact;   /* groups the kernel's exact path redid, or 0 */
    int mode;
    unsigned long long calls, native, fallback, cmp, bad, ns_nat, ns_spu, captured;
} EdgeKernel;
static EdgeKernel g_kern[] = {
    { .addr = 0xD0A8, .name = "skin", .core = edge_skin_core },
    { .addr = 0xA3C8, .name = "cull transform", .core = edge_xf_core },
    { .addr = 0xA798, .name = "cull", .core = edge_cull_core },
    { .addr = 0x4448, .name = "vertex decompression", .core = edge_dec_core, .env = "PS3RECOMP_NATIVE_EDGE_DEC", .exact = &g_dec_exact },
    { .addr = 0x99E8, .name = "vertex compression", .core = edge_enc_core, .cont = 0x98B0, .env = "PS3RECOMP_NATIVE_EDGE_ENC", .exact = &g_enc_exact },
    { .addr = 0xC7B8, .name = "index decompression", .core = edge_idx_core, .env = "PS3RECOMP_NATIVE_EDGE_IDX" },
};
#define NKERN ((int)(sizeof g_kern / sizeof g_kern[0]))

#ifdef EDGE_HARNESS
int edge_skin_native(spu_context* c) { return edge_skin_core(c->gpr, c->ls); }
EdgeCore edge_core_for(uint32_t addr)
{
    if (!g_wtab[255]) { rsq_init(); wtab_init(); fnms_init(); dec_tables_init(); idx_tables_init(); }
    if (getenv("PS3RECOMP_EDGE_DEC_LAYOUT")) g_dec_layout = atoi(getenv("PS3RECOMP_EDGE_DEC_LAYOUT")) != 0;
    for (int i = 0; i < NKERN; i++) if (g_kern[i].addr == addr) return g_kern[i].core;
    return 0;
}
#endif

#ifndef EDGE_HARNESS
static int s_init = -1;
static const char* const k_env[] = { "PS3_NATIVE_EDGE_SKIN", "PS3_NATIVE_EDGE_XF", "PS3_NATIVE_EDGE_CULL",
                                     "PS3_NATIVE_EDGE_DEC", "PS3_NATIVE_EDGE_ENC", "PS3_NATIVE_EDGE_IDX" };
static void edge_init(void)
{
    static int busy;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL)) {      /* another SPU thread is initialising */
        while (__atomic_load_n(&s_init, __ATOMIC_ACQUIRE) < 0) _mm_pause();
        return;
    }
    const char* e = getenv("PS3_NATIVE_EDGE");
    const int m = !(e && e[0] == '0');
    rsq_init(); wtab_init(); fnms_init(); dec_tables_init(); dec_desc_init(); idx_tables_init();
    { const char* ea = getenv("PS3_NATIVE_EDGE_AVX2"); g_avx2 = !(ea && ea[0] == '0'); }
    { const char* eg = getenv("PS3RECOMP_EDGE_GPU"); g_gpu = eg ? atoi(eg) : 0; if (g_gpu < 0 || g_gpu > 2) g_gpu = 0; }
    { const char* et = getenv("PS3RECOMP_EDGE_TSC"); g_tsc = et && et[0] == '1'; }
    { const char* dl = getenv("PS3RECOMP_EDGE_DEC_LAYOUT"); g_dec_layout = dl ? atoi(dl) != 0 : 1; }
    _Static_assert(sizeof k_env / sizeof k_env[0] == NKERN, "one switch per kernel");
    for (int i = 0; i < NKERN; i++) {
        EdgeKernel* k = &g_kern[i];
        const char* ke = getenv(k_env[i]);
        /* with the AVX2 forms every kernel but idx defaults on; without them only the skinning pays
         * (cpu agent's measurement), so xf, cull and dec then default to the lifted code. The index
         * kernel stays opt-in (PS3_NATIVE_EDGE_IDX=1 and 0xC7B8 in natives.txt): edge2 hooked it,
         * the replay passed (ck-rec2/ck-rec3/spu3-p2, Edge 248 -> 235 us), but of its two fight runs
         * one stalled before the title and one walked a different path (scene intact) */
        const int dflt = k->core == edge_idx_core ? 0 : g_avx2 ? 1 : (k->core == edge_skin_core || k->core == edge_enc_core);
        k->mode = m && (ke ? ke[0] != '0' : dflt);
    }
    fprintf(stderr, "[edge] native Edge Geometry kernels (image %d, %s, gpu-cull %d):", EDGE_IMAGE, g_avx2 ? "AVX2" : "SIMDe on AVX2", g_gpu);
    for (int i = 0; i < NKERN; i++) fprintf(stderr, " %s=%s", g_kern[i].name, g_kern[i].mode ? "on" : "off");
    fprintf(stderr, "\n");
    __atomic_store_n(&s_init, 1, __ATOMIC_RELEASE);
}
static void edge_report_now(void);
static int edge_entry(spu_context* ctx, EdgeKernel* k)
{
    if (unlikely(__atomic_load_n(&s_init, __ATOMIC_ACQUIRE) < 0)) edge_init();
    if (!k->mode) return 0;
    const unsigned long long t0 = g_tsc ? __rdtsc() : 0;
    const int ok = k->core(ctx->gpr, ctx->ls);
    if (g_tsc) __atomic_fetch_add(&k->ns_nat, __rdtsc() - t0, __ATOMIC_RELAXED);   /* cycles, in the field's slot */
    if (ok) {
        const unsigned long long nn = __atomic_fetch_add(&k->native, 1, __ATOMIC_RELAXED) + 1;
        if (g_tsc && k->core == edge_cull_core && (nn & 0x3F) == 0) edge_report_now();   /* a replay never runs the destructor */
        return 1;
    }
    __atomic_fetch_add(&k->fallback, 1, __ATOMIC_RELAXED);
    return 0;
}
#define EDGE_HOOK(i) static int edge_hook_##i(spu_context* ctx) { return edge_entry(ctx, &g_kern[i]); }
EDGE_HOOK(0)
EDGE_HOOK(1)
EDGE_HOOK(2)
EDGE_HOOK(3)
EDGE_HOOK(4)
EDGE_HOOK(5)
/* at exit (a job replay, a game run): how many calls each kernel took and declined */
static void edge_report_now(void)
{
    if (s_init < 0) return;
    fprintf(stderr, "[edge] native kernels:");
    for (int i = 0; i < NKERN; i++)
        if (g_kern[i].native || g_kern[i].fallback)
            fprintf(stderr, " %s %llu native / %llu lifted;", g_kern[i].name, g_kern[i].native, g_kern[i].fallback);
    fprintf(stderr, " exact groups dec %llu enc %llu\n", g_dec_exact, g_enc_exact);
    fprintf(stderr, "[edge] cull stage: %llu calls, %llu triangles in, %llu kept (%.1f%%), %llu segments dropped whole, %llu by the GPU switch\n",
            g_cs_calls, g_cs_nt, g_cs_kept, g_cs_nt ? 100.0 * g_cs_kept / g_cs_nt : 0.0, g_cs_zero, g_cs_gpu_cull);
    if (g_tsc) {
        fprintf(stderr, "[edge-tsc] cycles per kernel:");
        for (int i = 0; i < NKERN; i++) fprintf(stderr, " %s=%llu", g_kern[i].name, g_kern[i].ns_nat);
        fprintf(stderr, " cull.xf=%llu cull.run=%llu\n", g_cs_xf_cyc, g_cs_run_cyc);
    }
}
__attribute__((destructor)) static void edge_report(void) { edge_report_now(); }
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
__attribute__((constructor)) static void edge_register(void)
{
    static int (*const hooks[])(spu_context*) = { edge_hook_0, edge_hook_1, edge_hook_2, edge_hook_3, edge_hook_4, edge_hook_5 };
    _Static_assert(sizeof hooks / sizeof hooks[0] == NKERN, "one hook per kernel");
    for (int i = 0; i < NKERN; i++) spu_native_register(EDGE_IMAGE, g_kern[i].addr, hooks[i]);
}
#endif
#ifdef EDGE_HARNESS
/* micro-benchmark hook: time cull_batch16 alone on the state of a cull call */
double edge_bench_cull_batch(u128* gpr, uint8_t* ls, int reps)
{
    const uint32_t S = gpr[3]._u32[0];
    Cull k; memset(&k, 0, sizeof k);
    Xf x; u128 g2[128]; memcpy(g2, gpr, sizeof g2);
    for (int i = 0; i < 4; i++) g2[1]._u32[i] -= 128;
    if (!xf_prepare(g2, ls, &x)) return -1;
    xf_exec(g2, ls, &x);
    k.B = word_at(ls, S + 0x50);
    const uint32_t nidx = lq(ls, S + 0x1A0)._u32[0] >> 16;
    k.nt = (uint32_t)(((uint64_t)nidx * 0xAAAAAAABull) >> 33); k.ng = (k.nt + 3) >> 2;
    k.rec = x.scr + 16 * gpr[5]._u32[0]; k.snap = F1; k.m65 = 1;
    static uint8_t idx[24 * 5462 + 128];
    memcpy(idx, ls + k.B, 24 * k.ng + 64); k.idx = idx;
    struct timespec t0, t1; uint16_t m4[4]; unsigned acc = 0;
    const uint32_t nb = k.ng / 4;
    if (!nb) return -1;
    const uint32_t csr = _mm_getcsr(); _mm_setcsr(CSR_FAST);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < reps; r++) for (uint32_t b = 0; b < nb; b++) { cull_batch16(ls, &k, b, m4); acc += m4[0] + m4[1] + m4[2] + m4[3]; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    _mm_setcsr(csr);
    static volatile unsigned sink; sink = acc;
    return ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / ((double)reps * nb);
}
#endif
