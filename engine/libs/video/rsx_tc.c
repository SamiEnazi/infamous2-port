/*
 * ps3recomp - Track C (native/gpu): switches, startup line, counters and the
 * draw digest / check. See rsx_tc.h.
 */
#include "rsx_tc.h"
#include "rsx_dispatch.h"
#include "../../runtime/platform/host_place.h"   /* ps3_host_core_scarce: the split's default */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static unsigned long long tc_ms(void) { return GetTickCount64(); }
#else
#include <time.h>
static unsigned long long tc_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
#endif

/* ---- switches ------------------------------------------------------------- */
static const struct { const char* env; int def; const char* what; } s_tc_tab[TC_N] = {
    [TC_LBLWAIT] = { "PS3_RSX_LBLWAIT", 1, "split acquires wait only for a queued write of their label" },
    [TC_WALKON]  = { "PS3_RSX_WALKON",  1, "split walker keeps walking while the engine thread works" },
    [TC_SPLIT]   = { "PS3_RSX_SPLIT",  -1, "FIFO walker + dispatch / draw engine on two threads (default: on unless cores are scarce)" },
    [TC_BT]      = { "PS3_RSX_BT",      0, "backend draw work on the recording worker" },
    [TC_SPWAKE]  = { "PS3_RSX_SPWAKE",  1, "split exec thread wakes the walker only when its wait is over" },
    [TC_BARBATCH]= { "PS3_RSX_BARBATCH",-1, "Vulkan barriers the GPU does not need left out (default: on, off on AMD)" },
    [TC_FBF1X]   = { "PS3_RSX_FBF1X",   -1, "framebuffer fetch at render scale 1.0 too (default: on, off on AMD)" },
    [TC_GROWBUF] = { "PS3_RSX_GROWBUF", 1, "program memo buffers grow-only (no realloc per miss)" },
    [TC_FLIPFIRST]={ "PS3_RSX_FLIPFIRST",1, "split walker never walks on past a flip request" },
};
static int s_tc_val[TC_N];
static int s_tc_init;

static void tc_init(void)
{
    for (int i = 0; i < TC_N; i++) {
        const char* e = getenv(s_tc_tab[i].env);
        s_tc_val[i] = (e && *e) ? (*e != '0') : s_tc_tab[i].def;
        /* -1: on with cores to spare (>= 8 physical: PS3_CORE_SCARCE_BELOW), off on a small CPU,
         * where a second busy RSX thread and its walk-on polling would take a core the main and
         * job threads need. Measured (2026-11-16, 13600K, runner on): city +3..+14%, fight +11/-1%. */
        if (s_tc_val[i] < 0 && i == TC_SPLIT) s_tc_val[i] = !ps3_host_core_scarce();
    }
    __atomic_store_n(&s_tc_init, 1, __ATOMIC_RELEASE);
}
int rsx_tc(int id)
{
    if (__builtin_expect(!__atomic_load_n(&s_tc_init, __ATOMIC_ACQUIRE), 0)) tc_init();
    return (id >= 0 && id < TC_N) ? s_tc_val[id] : 0;
}

/* ---- digest --------------------------------------------------------------- */
static int s_dg_mode = -1;     /* 0 off, 1 log, 2 check */
static FILE* s_dg_file;
int rsx_tc_digest_mode(void)
{
    if (__builtin_expect(s_dg_mode < 0, 0)) {
        const char* e = getenv("PS3_RSX_DIGEST");
        int m = 0;
        if (e && *e && *e != '0') {
            if (!strcmp(e, "check")) m = 2;
            else {
                m = 1;
                if (strcmp(e, "1") != 0) {
                    s_dg_file = fopen(e, "w");
                    if (!s_dg_file) fprintf(stderr, "[track-c] PS3_RSX_DIGEST: cannot write %s (stderr instead)\n", e);
                }
            }
        }
        s_dg_mode = m;
    }
    return s_dg_mode;
}

void rsx_tc_banner(void)
{
    static int done;
    if (done) return;
    done = 1;
    (void)rsx_tc(0);
    char line[1024];
    int k = snprintf(line, sizeof line, "[track-c] RSX thread switches:");
    for (int i = 0; i < TC_N && k > 0 && k < (int)sizeof line - 80; i++)
        k += snprintf(line + k, sizeof line - (size_t)k, " %s=%d", s_tc_tab[i].env + 8, s_tc_val[i]);
    const int dm = rsx_tc_digest_mode();
    if (k > 0 && k < (int)sizeof line - 80)
        k += snprintf(line + k, sizeof line - (size_t)k, "; digest %s (PS3_RSX_<NAME>=0|1, PS3_RSX_DIGEST=1|<file>|check)",
                      dm == 2 ? "CHECK" : dm == 1 ? "log" : "off");
    fprintf(stderr, "%s\n", line);
}

/* A fast 64-bit hash over u64 words (xor-multiply-fold, four lanes). Not
 * cryptographic; equal inputs give equal hashes, which is all a check needs. */
static inline u64 tc_mix(u64 h) { h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33; return h; }
static u64 tc_hash_words(const void* p, size_t bytes, u64 seed)
{
    const u8* b = (const u8*)p;
    u64 a0 = seed ^ 0x9E3779B97F4A7C15ull, a1 = seed + 0x632BE59BD9B4E019ull, a2 = ~seed, a3 = seed * 3u + 1u;
    size_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        u64 w0, w1, w2, w3;
        memcpy(&w0, b + i, 8); memcpy(&w1, b + i + 8, 8); memcpy(&w2, b + i + 16, 8); memcpy(&w3, b + i + 24, 8);
        a0 = (a0 ^ w0) * 0x100000001B3ull; a1 = (a1 ^ w1) * 0x100000001B3ull;
        a2 = (a2 ^ w2) * 0x100000001B3ull; a3 = (a3 ^ w3) * 0x100000001B3ull;
        a0 = (a0 << 31) | (a0 >> 33); a1 = (a1 << 29) | (a1 >> 35);
        a2 = (a2 << 27) | (a2 >> 37); a3 = (a3 << 25) | (a3 >> 39);
    }
    for (; i < bytes; i++) a0 = (a0 ^ b[i]) * 0x100000001B3ull;
    return tc_mix(a0 ^ tc_mix(a1) ^ (tc_mix(a2) << 1) ^ (tc_mix(a3) >> 1) ^ (u64)bytes);
}

/* The 3D register file but the transform-program and transform-constant
 * upload windows (0x0B80..0x0BFF, 0x1F00..0x1F7F): their own registers are
 * read by the dispatcher alone and the split does not carry them. */
u64 rsx_tc_state_hash(const rsx_dispatch* r)
{
    u64 h = tc_hash_words(r->regs, 0x0B80u, 1);
    h = tc_hash_words((const u8*)r->regs + 0x0C00u, 0x1F00u - 0x0C00u, h);
    h = tc_hash_words((const u8*)r->regs + 0x1F80u, 0x2000u - 0x1F80u, h);
    h = tc_hash_words(r->vp, sizeof r->vp, h);
    h = tc_hash_words(r->constants, sizeof r->constants, h);
    const u64 sc[2] = { ((u64)r->current_primitive << 32) | (u32)r->in_begin_end, (u64)r->pair_packets };
    return tc_hash_words(sc, sizeof sc, h);
}

u64 g_rsx_tc_ref;
u32 g_rsx_tc_src;
u32 g_rsx_tc_src_exec = ~0u;
static volatile u64 s_src_n[512];
void rsx_tc_src_draw(u32 src) { __atomic_add_fetch(&s_src_n[src & 511u], 1, __ATOMIC_RELAXED); }
static u64 s_pair[2];
void rsx_tc_pair_reset(int side) { s_pair[side & 1] = 0x243F6A8885A308D3ull; }
void rsx_tc_pair_note(int side, u32 kind, u32 a, u32 b)
{
    u64* p = &s_pair[side & 1];
    *p = tc_mix(*p ^ (((u64)kind << 56) ^ ((u64)a << 24) ^ (u64)b ^ 0x13198A2E03707344ull));
}
u64 rsx_tc_pair_hash(int side) { return s_pair[side & 1]; }

/* ---- counters --------------------------------------------------------------- */
static volatile u64 s_cnt[TCC_N];
void rsx_tc_count(int c, u64 n) { if (c >= 0 && c < TCC_N) __atomic_add_fetch(&s_cnt[c], n, __ATOMIC_RELAXED); }

static struct { u32 draws, frame; u64 h; } s_fr;
static u64 s_mm_first;            /* mismatches printed */
void rsx_tc_draw(u64 h, int ref_valid, u64 ref)
{
    s_fr.draws++;
    s_fr.h = tc_mix(s_fr.h ^ h) + 0x9E3779B97F4A7C15ull;
    rsx_tc_count(TCC_DRAWS, 1);
    if (ref_valid) {
        rsx_tc_count(TCC_CHECKED, 1);
        if (ref != h) {
            rsx_tc_count(TCC_MISMATCH, 1);
            if (s_mm_first++ < 24)
                fprintf(stderr, "[track-c] MISMATCH frame %u draw %u: engine state %016llX, the other side %016llX"
                                " (the split walker's instance at the draw's END)\n",
                        s_fr.frame, s_fr.draws, (unsigned long long)h, (unsigned long long)ref);
        }
    }
}
void rsx_tc_frame(void)
{
    const int dm = rsx_tc_digest_mode();
    if (dm) {
        if (s_dg_file) {
            fprintf(s_dg_file, "frame %u draws %u digest %016llX\n", s_fr.frame, s_fr.draws, (unsigned long long)s_fr.h);
            if ((s_fr.frame & 63u) == 0) fflush(s_dg_file);
        } else if ((s_fr.frame % 60u) == 0)
            fprintf(stderr, "[track-c] digest frame %u draws %u digest %016llX\n", s_fr.frame, s_fr.draws, (unsigned long long)s_fr.h);
    }
    s_fr.frame++; s_fr.draws = 0; s_fr.h = 0;
    rsx_tc_tick();
}

void rsx_tc_tick(void)
{
    static int period = -1;
    static unsigned long long t0;
    static volatile long busy;
    if (period < 0) {
        const char* e = getenv("PS3_RSX_STATS");
        period = e && *e ? atoi(e) : 0;
        if (period <= 0 && rsx_tc_digest_mode() == 2) period = 10;
        if (period < 0) period = 0;
    }
    if (!period) return;
    const unsigned long long now = tc_ms();
    if (!t0) { t0 = now; return; }
    if (now - t0 < (unsigned long long)period * 1000ull) return;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return;
    const double sec = (double)(now - t0) / 1000.0;
    u64 c[TCC_N];
    for (int i = 0; i < TCC_N; i++) c[i] = __atomic_exchange_n(&s_cnt[i], 0ull, __ATOMIC_RELAXED);
    fprintf(stderr, "[track-c] %.1f s: split acquires %.0f/s (waited %.0f, skipped %.0f), walker waits %.2f ms/s, syncs %.0f/s,"
                    " walk-on %.0f/s (%.2f ms/s), exec wakes %.0f/s; draws %.0f/s, checked %llu, MISMATCHES %llu\n",
            sec, c[TCC_ACQ] / sec, c[TCC_ACQ_WAIT] / sec, c[TCC_ACQ_SKIP] / sec, c[TCC_WAIT_US] / 1000.0 / sec,
            c[TCC_SYNC] / sec, c[TCC_WALKON] / sec, c[TCC_WALKON_US] / 1000.0 / sec, c[TCC_WAKES] / sec, c[TCC_DRAWS] / sec,
            (unsigned long long)c[TCC_CHECKED], (unsigned long long)c[TCC_MISMATCH]);
    {   /* draws by source: the main ring, and CALLed lists by the EA's top byte */
        u64 n[512], tot = 0;
        for (int i = 0; i < 512; i++) { n[i] = __atomic_exchange_n(&s_src_n[i], 0ull, __ATOMIC_RELAXED); tot += n[i]; }
        if (tot) {
            char line[512];
            int k = snprintf(line, sizeof line, "[track-c] draws by source: main ring %.1f%%", 100.0 * (double)n[0] / (double)tot);
            for (int r = 0; r < 6 && k > 0 && k < (int)sizeof line - 48; r++) {
                int best = -1;
                for (int i = 256; i < 512; i++) if (n[i] && (best < 0 || n[i] > n[best])) best = i;
                if (best < 0) break;
                k += snprintf(line + k, sizeof line - (size_t)k, ", CALL to 0x%02Xxxxxxx %.1f%%", best & 255,
                              100.0 * (double)n[best] / (double)tot);
                n[best] = 0;
            }
            fprintf(stderr, "%s\n", line);
        }
    }
    if (c[TCC_BAR_FULL] + c[TCC_BAR_FULL_SKIP] + c[TCC_BAR_IMG] + c[TCC_BAR_IMG_SKIP])
        fprintf(stderr, "[track-c] barriers per s: full %.0f (left out %.0f), image %.0f (left out %.0f), copies/clears sharing one %.0f\n",
                c[TCC_BAR_FULL] / sec, c[TCC_BAR_FULL_SKIP] / sec, c[TCC_BAR_IMG] / sec, c[TCC_BAR_IMG_SKIP] / sec,
                c[TCC_BAR_CHAIN] / sec);
    t0 = now;
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}
