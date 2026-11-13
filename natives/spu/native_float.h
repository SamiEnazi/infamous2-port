/* natives/spu/native_float.h -- the SPU float ops of the native rewrites, the fast way.
 *
 * The rewrites keep the game's single-precision results by doing their float arithmetic through the
 * engine's SPU helpers (spu_fm, spu_fma, ...). On x86-64 those names are the per-lane reference forms:
 * four scalar double operations and their range tests for every 4-lane op, out of line in a build
 * without a training profile. The translated SPU code does not use them: it uses the *_lean forms
 * (engine/runtime/spu/spu_helpers.h), one packed operation in the SPU threads' own float mode (round
 * toward zero, flush to zero, denormals are zero) and the exact path only for a lane that leaves the
 * ordinary range -- the same bits (engine tests/test_spu_float_rz_x86.c; the job replay compares every
 * output byte of the rewrites against the translated jobs).
 *
 * Measured 2026-11-13 on a player-style build cut down to 4 cores / 8 threads (the cathedral spire):
 * the reference forms were 19% of one SPU thread over the five workers (spu__fma_any 12.8, spu_fm 6.3).
 *
 * PS3_NATIVE_FLOAT_LEAN=0: the reference forms, as before. Include after spu_helpers.h.
 */
#ifndef INF2_NATIVE_FLOAT_H
#define INF2_NATIVE_FLOAT_H

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(SPU_SCALAR_HELPERS)
#include <stdlib.h>

#if defined(_MSC_VER)
__declspec(selectany) int g_native_float_lean = -1;
#else
__attribute__((weak)) int g_native_float_lean = -1;
#endif
static __attribute__((noinline, cold)) int native_float_lean_init(void)
{
    const char* e = getenv("PS3_NATIVE_FLOAT_LEAN");
    const int on = !(e && e[0] == '0');
    __atomic_store_n(&g_native_float_lean, on, __ATOMIC_RELAXED);
    return on;
}
static inline int native_float_lean(void)
{
    const int v = __atomic_load_n(&g_native_float_lean, __ATOMIC_RELAXED);
    return __builtin_expect(v >= 0, 1) ? v : native_float_lean_init();
}
static inline u128 nat_fa(u128 a, u128 b) { return __builtin_expect(native_float_lean(), 1) ? spu_fa_lean(a, b) : spu_fa(a, b); }
static inline u128 nat_fs(u128 a, u128 b) { return __builtin_expect(native_float_lean(), 1) ? spu_fs_lean(a, b) : spu_fs(a, b); }
static inline u128 nat_fm(u128 a, u128 b) { return __builtin_expect(native_float_lean(), 1) ? spu_fm_lean(a, b) : spu_fm(a, b); }
static inline u128 nat_fma(u128 a, u128 b, u128 c)  { return __builtin_expect(native_float_lean(), 1) ? spu_fma_lean(a, b, c)  : spu_fma(a, b, c); }
static inline u128 nat_fms(u128 a, u128 b, u128 c)  { return __builtin_expect(native_float_lean(), 1) ? spu_fms_lean(a, b, c)  : spu_fms(a, b, c); }
static inline u128 nat_fnms(u128 a, u128 b, u128 c) { return __builtin_expect(native_float_lean(), 1) ? spu_fnms_lean(a, b, c) : spu_fnms(a, b, c); }
#define spu_fa   nat_fa
#define spu_fs   nat_fs
#define spu_fm   nat_fm
#define spu_fma  nat_fma
#define spu_fms  nat_fms
#define spu_fnms nat_fnms
#endif

#endif /* INF2_NATIVE_FLOAT_H */
