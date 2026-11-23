/* ps3_dbgdelay.h -- (sz1, 2026-11-20) bench-only causal delays: wait a given time without doing work.
 *
 * ps3_dbg_wait_ns(ns, spin): spin = 0 -> MONITORX + MWAITX with a TSC timeout (Zen 2, user mode: the thread waits in
 * C0 without issuing work, so the delay draws next to no power and leaves the core to its SMT sibling; the form of
 * wws_runner.c's PS3_JOBDELAY), spin = 1 -> a PAUSE loop on the TSC (the power of a busy-waiting thread).
 * The TSC rate is measured once per translation unit, on the first delay (5 ms of CLOCK_MONOTONIC); a thread that
 * comes while another is measuring skips its delay. Header-only and static: the files that use it are also linked
 * into the job replay / test binaries. x86-64 Linux only (elsewhere a no-op). */
#ifndef PS3EMU_PS3_DBGDELAY_H
#define PS3EMU_PS3_DBGDELAY_H
#include <stdint.h>
#if defined(__x86_64__) && defined(__linux__)
#include <time.h>
#include <x86intrin.h>
static volatile double s_dbgd_tsc_ns;  /* TSC ticks per ns; 0 = not measured (an aligned double: x86-64 reads it whole) */
static __attribute__((noinline, unused)) double ps3_dbg_tsc_ns(void)
{
    double v = s_dbgd_tsc_ns;
    if (v > 0) return v;
    static int busy;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL)) return 0.0;
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); const uint64_t r0 = __rdtsc();
    do clock_gettime(CLOCK_MONOTONIC, &b); while ((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec) < 5000000ll);
    v = (double)(__rdtsc() - r0) / (double)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
    s_dbgd_tsc_ns = v;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return v;
}
static inline __attribute__((unused)) void ps3_dbg_wait_ns(uint64_t ns, int spin)
{
    const double r = s_dbgd_tsc_ns > 0 ? s_dbgd_tsc_ns : ps3_dbg_tsc_ns();
    if (r <= 0) return;
    const uint64_t t0 = __rdtsc(), len = (uint64_t)((double)ns * r);
    if (spin) { while (__rdtsc() - t0 < len) __builtin_ia32_pause(); return; }
    char line[128] __attribute__((aligned(64)));   /* a line nobody writes: MWAITX ends by its timer */
    for (;;) {
        const uint64_t el = __rdtsc() - t0;
        if (el >= len) break;
        const uint64_t left = len - el;
        __asm__ volatile(".byte 0x0f,0x01,0xfa" :: "a"(line + 64), "c"(0), "d"(0) : "memory");   /* monitorx */
        __asm__ volatile(".byte 0x0f,0x01,0xfb" :: "a"(0xF0), "b"((uint32_t)(left > 0xFFFFFFFFull ? 0xFFFFFFFFull : left)),
                         "c"(2) : "memory");                                                       /* mwaitx, timer on */
    }
}
#else
static inline __attribute__((unused)) void ps3_dbg_wait_ns(uint64_t ns, int spin) { (void)ns; (void)spin; }
#endif
#endif
