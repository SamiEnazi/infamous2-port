/* wws_runner.c -- inFAMOUS 2's native job runner (Track B1): the WWS job managers' jobs on a host thread
 * pool, completed through the same words the game's waits read. Title side of wws_native.c.
 *
 * The game's job manager (PPU, 384-byte objects "mgr"; work/native/B1_CONTRACT.md):
 *   kick 0x38B644 appends `(len<<16|type) : desc` to the current array of the manager (two arrays,
 *   headers at mgr+164+28*s: base +0, cap +4, widx +8, seq byte +20); the flush 0x38AC78 tells SPURS
 *   (cellSpursReadyCountStore). Waits read, per array s, the u16 words of the quad at mgr+0x40+16*s:
 *   token t of array s is done when min(+2, +4..+0xE) >= t (0x54C730); the array is drained when that
 *   minimum reaches its zero-terminated end (0x54CE30, which for the active array u32[mgr+8] also wants
 *   the workload's readyCount and contention at 0).
 *
 * The runner hooks the flush. A manager is OWNED from its first flush on (PS3_NATIVE_JOBS: "1" = every
 * manager, or a list of job modules, e.g. "220,213": a manager whose first job is one of them); for an
 * owned manager the flush never reaches SPURS, so its readyCount stays 0 and no policy module ever runs
 * it. A flush (and a finished job, and a barrier cell reaching 0) claims the new entries in the policy
 * module's order:
 *   type 1 a job: queued for the pool;
 *   type 2 a barrier on the u16 cell at desc: passed when the cell is 0, else nothing after it is claimed;
 *   type 4 a marker (array switch): claiming it sets u32[mgr+8] = its array and u32[mgr+0xC] -= 1, and
 *          the claim continues in that array;
 *   type 3: claimed, nothing else.
 * Jobs of one manager run concurrently and finish in any order; the runner publishes, per array, the
 * length k of the finished prefix: +2 = k (and +0 = max(+0, k)) in one lock-line update after the jobs'
 * outputs are written. The six per-SPU words stay 0xFFFF (the PPU's reset), so min(...) = k. After a job
 * the cells its ReqDepDec commands name are decremented (the policy module's retire); a cell that
 * reaches 0 opens the barriers waiting on it.
 *
 * Waits (PS3_NATIVE_JOBS_WAIT, default on): the token wait 0x38B43C, the array wait 0x38AA14 (and so the
 * swap 0x38AE0C and the reset 0x38BA78, which call it) and the arena wait 0x38AAB8 of an owned manager
 * do not poll with the guest's 1 ms yields: the waiting thread runs that manager's queued jobs itself and
 * spins briefly between checks. The words stay exact, so every reader that is not hooked still works.
 *
 * Switches (environment / settings.ini):
 *   PS3_NATIVE_JOBS=0|1|<modules>   0 = off: every job goes to SPURS as before; unset = 1 (the default since the
 *                                   combined native/integ build, 2026-11-16; the native jobs run only with it on)
 *   PS3_NATIVE_JOBS_THREADS=<n>     pool threads (default: physical cores - 2, 2..8)
 *   PS3_NATIVE_JOBS_WAIT=0|1        the wait hooks (default 1)
 *   PS3_NATIVE_JOBS_STATS=1|<s>     a line per 10 s (or every <s> s, 2..60): jobs, per module, pool busy time,
 *                                   waits, claim->start latency, and a health line (queues, stalls, warnings)
 *   PS3_NATIVE_JOBS_HELP=0|1|2      waiters run queued jobs: never / only the ones they wait for (default) / any
 *   PS3_NATIVE_JOBS_STICKY=0|1      a pool thread takes the next job of the manager it just ran first (default 1)
 *   PS3_NATIVE_JOBS_WAITSTAT=1      a line per 10 s: time in the four job waits (runner on or off)
 *   PS3_NATIVE_JOBS_LOG=1           managers as they are first seen, ownership, dependency cells
 * Always on (cheap): a loud first-occurrence warning ([jobs] WARN, the first 8 of each kind, then every
 * 1000th) for anything the runner does not expect: an unknown entry type, a wait or a barrier stalled for
 * more than 2 s (with the manager's state), a cell of no known manager, an array reset with work left.
 */
#include "wws_native.h"
#include "wws_njob.h"
#include "../../runtime/spu/spu_context.h"
#include "../../runtime/ppu/ppu_context.h"
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/platform/host_place.h"
#include "../../include/ps3emu/ppu_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#else
#define CPU_PAUSE() ((void)0)
#endif

extern uint8_t* vm_base;
extern int spu_overlay_match_sig(const uint8_t hdr[16]);
void wws_native_verify_stats(uint64_t* verified, uint64_t* bad);   /* wws_native.c */
extern void spurs_kernel_notify(uint32_t ea);

/* ---- guest memory, big-endian -------------------------------------------------------------- */
static inline uint32_t G32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint32_t G16(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t G8(uint32_t ea) { return vm_base[ea]; }

/* the game's manager layout (v1.04) */
enum {
    MGR_CTRL_SLOT = 8, MGR_CTRL_VER = 0xC, MGR_QUAD = 0x40,
    MGR_SPURS = 132, MGR_WID = 136, MGR_ARR0 = 164, MGR_ARR_STRIDE = 28, MGR_CUR = 256, MGR_SEQ = 260,
    ARR_BASE = 0, ARR_CAP = 4, ARR_WIDX = 8, ARR_SEQ = 20,
    MGR_CELLS = 280,
};
#define HOOK_FLUSH      0x0038AC78u
#define HOOK_TOKEN_WAIT 0x0038B43Cu
#define HOOK_ARRAY_WAIT 0x0038AA14u
#define HOOK_ARENA_WAIT 0x0038AAB8u
#define HOOK_TOKEN_TEST 0x0038ABF8u
#define HOOK_DRAIN      0x0029A8ECu

/* ---- configuration ------------------------------------------------------------------------- */
static int s_on, s_all, s_threads, s_wait, s_stats, s_log, s_waitstat, s_help, s_sticky, s_stats_sec = 10;
static unsigned char s_mod_on[256];

static int env_int(const char* n, int dflt) { const char* e = getenv(n); return e && *e ? atoi(e) : dflt; }

static void cfg_init(void)
{
    const char* e = getenv("PS3_NATIVE_JOBS");
    if (!e || !*e) e = "1";            /* the default: every manager to the runner (PS3_NATIVE_JOBS=0 = SPURS) */
    if (strcmp(e, "0") != 0) {
        s_on = 1;
        if (!strcmp(e, "1") || !strcmp(e, "all")) s_all = 1;
        else for (const char* p = e; *p; ) {
            char* q; const long m = strtol(p, &q, 10);
            if (q == p) { p++; continue; }
            if (m >= 200 && m < 256) s_mod_on[m] = 1;
            p = q;
        }
    }
    s_threads = env_int("PS3_NATIVE_JOBS_THREADS", 0);
    /* physical cores - 2 (main, RSX), at most 8: on the i5-13600K (14 cores, 6 of them P-cores) 12 threads
     * burned 7.2 cores for the frame rate 8 threads gave with 5.7 and 6 with 4.5 (more E-core and SMT
     * sharing per job); 8 was the fastest of 6/8/12 in the city */
    if (s_threads <= 0) { s_threads = ps3_host_cores() - 2; if (s_threads > 8) s_threads = 8; }
    if (s_threads < 2) s_threads = 2;
    if (s_threads > 32) s_threads = 32;
    s_wait = env_int("PS3_NATIVE_JOBS_WAIT", 1);
    s_stats = env_int("PS3_NATIVE_JOBS_STATS", 0);
    s_stats_sec = s_stats > 1 ? (s_stats > 60 ? 60 : s_stats) : 10;
    s_help = env_int("PS3_NATIVE_JOBS_HELP", 1);
    s_sticky = env_int("PS3_NATIVE_JOBS_STICKY", 1);   /* 0 = waiters never run jobs, 1 = only jobs they wait for, 2 = any */
    s_log = env_int("PS3_NATIVE_JOBS_LOG", 0);
    s_waitstat = env_int("PS3_NATIVE_JOBS_WAITSTAT", 0);
}

/* ---- managers ------------------------------------------------------------------------------- */
#define MAX_MGRS 256
#define SLOT_MAX 2048                  /* entries per array (the game's arrays hold a few hundred) */
#define MQ_CAP   2048                  /* queued jobs per manager */

typedef struct qjob { uint32_t slot, index, hi, lo; int module; uint64_t t_push; } qjob;

typedef struct slot_state {
    uint32_t seq;                      /* the array's sequence byte this state belongs to (0x100 = none) */
    uint32_t claimed;                  /* entries [0, claimed) claimed */
    uint32_t k;                        /* entries [0, k) finished and published */
    uint8_t  done[SLOT_MAX];
} slot_state;

typedef struct mgr_state {
    uint32_t ea;
    int      owned;
    int      module;                   /* of its first job */
    uint32_t wid, spurs;
    SRWLOCK  lock;                     /* claims, completion, the queue */
    uint32_t active;                   /* the array claims come from (mirrors u32[mgr+8]) */
    uint32_t base[2];                  /* the arrays' bases as last seen: a change = re-initialized in place */
    uint32_t blocked_cell;             /* the barrier cell the claim waits on (0 = none) */
    uint64_t blocked_since;            /* GetTickCount64 when the claim first stopped at it */
    slot_state s[2];
    qjob     q[MQ_CAP];
    uint32_t qh, qt;
    volatile long running;             /* jobs queued or running */
} mgr_state;

static mgr_state* s_mgr[MAX_MGRS];
static volatile long s_nmgr;
static SRWLOCK s_mgr_lock = SRWLOCK_INIT;

static mgr_state* volatile s_mgr_hash[1024];  /* by (ea >> 7): the hot hooks find their manager in one load */
static mgr_state* mgr_find(uint32_t ea)
{
    mgr_state* h = s_mgr_hash[(ea >> 7) & 1023];
    if (h && h->ea == ea) return h;
    const long n = __atomic_load_n(&s_nmgr, __ATOMIC_ACQUIRE);
    for (long i = 0; i < n; i++) if (s_mgr[i]->ea == ea) return s_mgr[i];
    return NULL;
}

/* the module of a job: its descriptor's code buffer (UseBuffer of set 0 with load), by its signature */
static uint32_t s_img_ea[64]; static int s_img_mod[64]; static volatile long s_nimg;
static SRWLOCK s_img_lock = SRWLOCK_INIT;
static int module_of_desc(uint32_t desc, uint32_t len)
{
    for (uint32_t o = 0; o + 8 <= len; o += 8) {
        const uint32_t F = G32(desc + o), D = G32(desc + o + 4);
        const uint32_t cmd = (F >> 9) & 7;
        if (cmd == 5) break;
        if (cmd != 2 || ((F >> 5) & 15) != 0 || !(F & 0x8000u)) continue;
        const uint32_t ea = D & ~15u;
        const long n = __atomic_load_n(&s_nimg, __ATOMIC_ACQUIRE);
        for (long i = 0; i < n; i++) if (s_img_ea[i] == ea) return s_img_mod[i];
        const int m = spu_overlay_match_sig(vm_base + ea);
        AcquireSRWLockExclusive(&s_img_lock);      /* not s_mgr_lock: mgr_get holds that one when it asks */
        if (s_nimg < 64) { s_img_ea[s_nimg] = ea; s_img_mod[s_nimg] = m; __atomic_store_n(&s_nimg, s_nimg + 1, __ATOMIC_RELEASE); }
        ReleaseSRWLockExclusive(&s_img_lock);
        return m;
    }
    return 0;
}

static uint32_t arr_hdr(uint32_t mgr, uint32_t s) { return mgr + MGR_ARR0 + MGR_ARR_STRIDE * s; }

static mgr_state* mgr_get(uint32_t ea)
{
    mgr_state* m = mgr_find(ea);
    if (m) return m;
    AcquireSRWLockExclusive(&s_mgr_lock);
    m = mgr_find(ea);
    if (!m && s_nmgr < MAX_MGRS) {
        m = (mgr_state*)calloc(1, sizeof *m);
        m->ea = ea;
        m->wid = G32(ea + MGR_WID);
        m->spurs = G32(ea + MGR_SPURS);
        InitializeSRWLock(&m->lock);
        m->active = G32(ea + MGR_CTRL_SLOT) & 1;
        m->s[0].seq = m->s[1].seq = 0x100;
        m->base[0] = G32(arr_hdr(ea, 0) + ARR_BASE);
        m->base[1] = G32(arr_hdr(ea, 1) + ARR_BASE);
        /* ownership: decided once, here, at the manager's first flush */
        const uint32_t h = arr_hdr(ea, m->active);
        const uint32_t base = G32(h + ARR_BASE), widx = G32(h + ARR_WIDX);
        for (uint32_t i = 0; i < widx && i < 64 && !m->module; i++) {
            const uint32_t hi = G32(base + 8 * i), lo = G32(base + 8 * i + 4);
            if ((hi & 0xFFFF) == 1) m->module = module_of_desc(lo, (hi >> 16) & 511);
        }
        m->owned = s_all || (m->module >= 200 && m->module < 256 && s_mod_on[m->module]);
        s_mgr[s_nmgr] = m;
        __atomic_store_n(&s_nmgr, s_nmgr + 1, __ATOMIC_RELEASE);
        if (!s_mgr_hash[(ea >> 7) & 1023]) __atomic_store_n(&s_mgr_hash[(ea >> 7) & 1023], m, __ATOMIC_RELEASE);
        if (s_log)
            fprintf(stderr, "[jobs] manager %08X (wid %u, first job module %d): %s\n", ea, m->wid, m->module,
                    m->owned ? "native runner" : "SPURS");
    }
    ReleaseSRWLockExclusive(&s_mgr_lock);
    return m;
}

/* ---- the ready ring: one ticket (the manager) per queued job ---------------------------------------
 * A claim adds its tickets under one lock and wakes ONE sleeping thread, and only when no thread is
 * spinning (a spinner takes the first ticket itself); a thread that takes a ticket and sees more wakes the
 * next sleeper. The producer (usually the main thread, at a flush) so pays at most one wake per claim: one
 * wake per job cost it ~8 alert syscalls per flush, about a millisecond a frame in the city. A ticket whose
 * job a waiter or a sticky thread already ran is stale: the thread that pops it finds the queue empty. */
#define RING_CAP (1u << 16)
static mgr_state* s_ready[RING_CAP];
static volatile uint32_t s_rh, s_rt;
static SRWLOCK s_rlock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_rcv = CONDITION_VARIABLE_INIT;
static volatile long s_rsleepers, s_rspinners;

static void ready_add_n(mgr_state* m, uint32_t n)   /* caller holds m->lock: n tickets for jobs just queued */
{
    if (!n) return;
    AcquireSRWLockExclusive(&s_rlock);
    if (s_rt - s_rh + n > RING_CAP) { fprintf(stderr, "[jobs] FATAL: ready ring full\n"); fflush(stderr); abort(); }
    for (uint32_t i = 0; i < n; i++) s_ready[(s_rt + i) % RING_CAP] = m;
    __atomic_store_n(&s_rt, s_rt + n, __ATOMIC_RELEASE);
    const long sl = s_rsleepers;
    ReleaseSRWLockExclusive(&s_rlock);
    if (sl && !__atomic_load_n(&s_rspinners, __ATOMIC_ACQUIRE)) WakeConditionVariable(&s_rcv);
}
static mgr_state* ready_pop(void)
{
    __atomic_add_fetch(&s_rspinners, 1, __ATOMIC_ACQ_REL);
    for (int spin = 0; spin < 3000 && __atomic_load_n(&s_rh, __ATOMIC_ACQUIRE) == __atomic_load_n(&s_rt, __ATOMIC_ACQUIRE); spin++)
        CPU_PAUSE();                       /* a burst's next job usually comes within microseconds */
    AcquireSRWLockExclusive(&s_rlock);
    __atomic_sub_fetch(&s_rspinners, 1, __ATOMIC_ACQ_REL);
    while (s_rh == s_rt) {
        s_rsleepers++;
        SleepConditionVariableSRW(&s_rcv, &s_rlock, INFINITE, 0);
        s_rsleepers--;
    }
    mgr_state* m = s_ready[s_rh % RING_CAP];
    __atomic_store_n(&s_rh, s_rh + 1, __ATOMIC_RELEASE);
    const int more = s_rh != s_rt && s_rsleepers;
    ReleaseSRWLockExclusive(&s_rlock);
    if (more) WakeConditionVariable(&s_rcv);   /* the chain: the next sleeper for the next ticket */
    return m;
}

static void mq_push_locked(mgr_state* m, const qjob* j)   /* the claim adds the tickets (ready_add_n) */
{
    if (m->qt - m->qh >= MQ_CAP) { fprintf(stderr, "[jobs] FATAL: queue of %08X full\n", m->ea); fflush(stderr); abort(); }
    m->q[m->qt++ % MQ_CAP] = *j;
}
/* take a job of m (caller holds m->lock); when `relist`, m goes back to the ready ring if it has more */
static int mq_pop_locked(mgr_state* m, qjob* out)
{
    if (m->qh == m->qt) return 0;
    *out = m->q[m->qh++ % MQ_CAP];
    return 1;
}

/* the SPU numbers a job reads at API+0 (only Edge 223 does): 0..5, one per running Edge job */
static volatile long s_spu_tokens = 0x3F;
static uint32_t token_take(void)
{
    for (;;) {
        long t = __atomic_load_n(&s_spu_tokens, __ATOMIC_ACQUIRE);
        if (t) {
            const uint32_t b = (uint32_t)__builtin_ctz((unsigned)t);
            if (__atomic_compare_exchange_n(&s_spu_tokens, &t, t & ~(1L << b), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return b;
        } else SwitchToThread();
    }
}
static void token_give(uint32_t b) { __atomic_fetch_or(&s_spu_tokens, 1L << b, __ATOMIC_ACQ_REL); }

static volatile uint64_t s_mod_jobs[256], s_mod_ns[256], s_busy_ns, s_jobs_total, s_help_jobs, s_help_ns, s_wait_ns, s_waits, s_test_help;
static volatile uint64_t s_lat_ns, s_lat_n, s_lat_b[4];
enum { W_TYPE, W_WAIT, W_BARRIER, W_CELL, W_RESET, W_REINIT, W_N };
static const char* const s_warn_name[W_N] = { "unknown entry type", "stalled wait", "stalled barrier",
                                              "cell of no known manager", "array reset with work left",
                                              "manager re-initialized" };
static volatile long s_warn_n[W_N];
/* 1 = print this one: the first 8 of a kind, then every 1000th */
static int warn_take(int kind)
{
    const long n = __atomic_add_fetch(&s_warn_n[kind], 1, __ATOMIC_RELAXED);
    return n <= 8 || n % 1000 == 0;
}   /* claim -> start: sum, count, <20 us / <100 us / <1 ms / more */

static uint64_t now_ns(void)
{
    static double s_scale;
    LARGE_INTEGER c;
    if (!s_scale) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_scale = 1e9 / (double)f.QuadPart; }
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * s_scale);
}

/* ---- publishing --------------------------------------------------------------------------------- */
typedef struct pub_arg { uint32_t quad_off, k; int marker; uint32_t marker_slot; } pub_arg;
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void pub_fn(uint8_t* line, void* a)
{
    const pub_arg* p = (const pub_arg*)a;
    if (p->marker) {                   /* the claim of a marker: active array, table version */
        put32(line + MGR_CTRL_SLOT, p->marker_slot);
        put32(line + MGR_CTRL_VER, get32(line + MGR_CTRL_VER) - 1);
        return;
    }
    uint8_t* q = line + p->quad_off;
    const uint32_t h0 = ((uint32_t)q[0] << 8) | q[1];
    if (p->k > h0) { q[0] = (uint8_t)(p->k >> 8); q[1] = (uint8_t)p->k; }
    q[2] = (uint8_t)(p->k >> 8); q[3] = (uint8_t)p->k;
}
static void publish_k(mgr_state* m, uint32_t s, uint32_t k)
{
    pub_arg a = { MGR_QUAD + 16 * s, k, 0, 0 };
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* the jobs' outputs before the word */
    wws_line_update(m->ea, pub_fn, &a);
}
static void publish_marker(mgr_state* m, uint32_t slot)
{
    pub_arg a = { 0, 0, 1, slot };
    wws_line_update(m->ea, pub_fn, &a);
}

/* entries [0, k) done -> publish (caller holds m->lock) */
static void advance_locked(mgr_state* m, uint32_t s)
{
    slot_state* ss = &m->s[s];
    uint32_t k = ss->k;
    while (k < ss->claimed && ss->done[k]) k++;
    if (k != ss->k) { ss->k = k; publish_k(m, s, k); }
}

/* a slot whose sequence byte changed was reset by the producer (0x38AE0C) */
static void slot_check_locked(mgr_state* m, uint32_t s)
{
    const uint32_t seq = G8(arr_hdr(m->ea, s) + ARR_SEQ);
    slot_state* ss = &m->s[s];
    if (ss->seq == seq && G32(arr_hdr(m->ea, s) + ARR_WIDX) >= ss->claimed) return;
    if (ss->seq != 0x100 && ss->k != ss->claimed && warn_take(W_RESET))
        fprintf(stderr, "[jobs] WARN #%ld %s: manager %08X array %u reset (seq %02X -> %02X) with %u of %u claimed "
                "entries unfinished\n", s_warn_n[W_RESET], s_warn_name[W_RESET], m->ea, s, ss->seq & 0xFF, seq,
                ss->claimed - ss->k, ss->claimed);
    ss->seq = seq;
    ss->claimed = 0;
    ss->k = 0;
    memset(ss->done, 0, sizeof ss->done);
}

/* A manager re-initialized in place (0x38B10C again at the same address: a checkpoint / game-state change
 * re-creates 0xB01E80's manager with a new SPURS workload and fresh arrays, whose sequence bytes may equal the
 * old ones). The b1-roam hang: the old claim count stayed above the new write index, nothing was claimed and
 * the next wait never returned. Seen by the workload id or an array base changing; everything per instance
 * starts over (the game waited for the old instance's jobs before it destroyed it). */
static void reinit_check_locked(mgr_state* m)
{
    const uint32_t wid = G32(m->ea + MGR_WID);
    const uint32_t b0 = G32(arr_hdr(m->ea, 0) + ARR_BASE), b1 = G32(arr_hdr(m->ea, 1) + ARR_BASE);
    if (wid == m->wid && b0 == m->base[0] && b1 == m->base[1]) return;
    if (warn_take(W_REINIT))
        fprintf(stderr, "[jobs] WARN #%ld %s: manager %08X wid %u -> %u, arrays %08X/%08X -> %08X/%08X; old instance: "
                "queued %u, running %ld, claimed %u/%u, prefix %u/%u\n", s_warn_n[W_REINIT], s_warn_name[W_REINIT],
                m->ea, m->wid, wid, m->base[0], m->base[1], b0, b1, m->qt - m->qh, m->running,
                m->s[0].claimed, m->s[1].claimed, m->s[0].k, m->s[1].k);
    m->wid = wid;
    m->spurs = G32(m->ea + MGR_SPURS);
    m->base[0] = b0; m->base[1] = b1;
    m->active = G32(m->ea + MGR_CTRL_SLOT) & 1;
    m->blocked_cell = 0; m->blocked_since = 0;
    for (int s = 0; s < 2; s++) {
        m->s[s].seq = 0x100; m->s[s].claimed = 0; m->s[s].k = 0;
        memset(m->s[s].done, 0, sizeof m->s[s].done);
    }
}

/* claim what can be claimed (caller holds m->lock) */
static void claim_locked(mgr_state* m)
{
    reinit_check_locked(m);
    uint32_t pushed = 0;
    for (int guard = 0; guard < 4; guard++) {
        const uint32_t s = m->active;
        slot_check_locked(m, s);
        slot_state* ss = &m->s[s];
        const uint32_t h = arr_hdr(m->ea, s);
        const uint32_t base = G32(h + ARR_BASE);
        const uint32_t widx = G32(h + ARR_WIDX);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        int switched = 0;
        while (ss->claimed < widx) {
            const uint32_t i = ss->claimed;
            if (i >= SLOT_MAX) { fprintf(stderr, "[jobs] FATAL: array of %08X longer than %u\n", m->ea, SLOT_MAX); fflush(stderr); abort(); }
            const uint32_t hi = G32(base + 8 * i), lo = G32(base + 8 * i + 4);
            const uint32_t type = hi & 0xFFFF;
            if (type == 0) break;                      /* not visible yet */
            if (type == 2) {                           /* barrier */
                if (G16(lo) != 0) {
                    if (m->blocked_cell != lo) m->blocked_since = GetTickCount64();
                    m->blocked_cell = lo;
                    break;
                }
                m->blocked_cell = 0; m->blocked_since = 0;
                ss->done[i] = 1; ss->claimed++;
                continue;
            }
            if (type == 4) {                           /* marker: the claim moves to array `lo` */
                ss->done[i] = 1; ss->claimed++;
                publish_marker(m, lo & 1);
                m->active = lo & 1;
                switched = 1;
                break;
            }
            if (type > 4 && warn_take(W_TYPE))
                fprintf(stderr, "[jobs] WARN #%ld %s: manager %08X array %u entry %u = %08X:%08X (claimed as type 3)\n",
                        s_warn_n[W_TYPE], s_warn_name[W_TYPE], m->ea, s, i, hi, lo);
            if (type == 1) {
                qjob j = { s, i, hi, lo, module_of_desc(lo, (hi >> 16) & 511), s_stats ? now_ns() : 0 };
                __atomic_add_fetch(&m->running, 1, __ATOMIC_ACQ_REL);
                mq_push_locked(m, &j);
                pushed++;
            } else {
                ss->done[i] = 1;                       /* type 3: claimed, nothing else */
            }
            ss->claimed++;
        }
        advance_locked(m, s);
        if (!switched) break;
    }
    ready_add_n(m, pushed);
}
static void claim(mgr_state* m)
{
    AcquireSRWLockExclusive(&m->lock);
    claim_locked(m);
    ReleaseSRWLockExclusive(&m->lock);
}

/* a dependency cell reached 0: barriers waiting on it may pass */
static void cell_opened(uint32_t cell, mgr_state* from)
{
    int owner_owned = 0, any = 0;
    const long n = __atomic_load_n(&s_nmgr, __ATOMIC_ACQUIRE);
    for (long i = 0; i < n; i++) {
        mgr_state* m = s_mgr[i];
        if (cell >= m->ea + MGR_CELLS && cell < m->ea + MGR_CELLS + 16) { any = 1; owner_owned = m->owned; }
        if (m->owned && m->blocked_cell == cell) claim(m);
    }
    if (!owner_owned) {
        /* the cell belongs to a manager SPURS runs: wake its workload as the policy module did */
        const uint32_t wid = G8(cell + 2), count = G8(cell + 3);
        if (count && wid < 16 && from && from->spurs) {
            __atomic_store_n(vm_base + from->spurs + wid, (uint8_t)count, __ATOMIC_SEQ_CST);
            spurs_kernel_notify(from->spurs);
        }
        if (!any && warn_take(W_CELL))
            fprintf(stderr, "[jobs] WARN #%ld %s: cell %08X reached 0 (wid byte %02X, wake %02X); from manager %08X\n",
                    s_warn_n[W_CELL], s_warn_name[W_CELL], cell, G8(cell + 2), G8(cell + 3), from ? from->ea : 0);
    }
}

/* ---- running a job ----------------------------------------------------------------------------- */
static void run_one(wws_worker* w, uint32_t own_spu, mgr_state* m, const qjob* q)
{
    const uint32_t base = G32(arr_hdr(m->ea, q->slot) + ARR_BASE);
    const uint32_t pair = base + 8 * (q->index ^ 1);
    wws_job j = { m->ea, q->slot, q->index, q->hi, q->lo, G32(pair), G32(pair + 4), m->wid, m->spurs };
    uint32_t tok = 0xFF;
    if (q->module == 223) { tok = token_take(); wws_worker_set_spu_num(w, tok); }
    else wws_worker_set_spu_num(w, own_spu);
    const uint64_t t0 = s_stats ? now_ns() : 0;
    if (s_stats && q->t_push) {
        const uint64_t l = t0 - q->t_push;
        __atomic_add_fetch(&s_lat_ns, l, __ATOMIC_RELAXED); __atomic_add_fetch(&s_lat_n, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_lat_b[l < 20000 ? 0 : l < 100000 ? 1 : l < 1000000 ? 2 : 3], 1, __ATOMIC_RELAXED);
    }
    wws_result r;
    /* Track B2: a native job module (wws_njob.c, PS3_NATIVE_CULL) instead of the lifted job; 0 = declined */
    const int nat = wws_njob_try(w, &j, q->module, &r);
    if (!nat) wws_native_run(w, &j, &r);
    if (tok != 0xFF) token_give(tok);
    if (s_stats) {
        const uint64_t d = now_ns() - t0;
        __atomic_add_fetch(&s_busy_ns, d, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_mod_ns[r.module & 255], d, __ATOMIC_RELAXED);
    }
    if (!r.ok) {
        fprintf(stderr, "[jobs] FATAL: job %08X:%u #%u (desc %08X, module %d) not run: %s\n",
                m->ea, q->slot, q->index, q->lo, r.module, r.why ? r.why : "?");
        fflush(stderr);
        abort();
    }
    __atomic_add_fetch(&s_mod_jobs[r.module & 255], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_jobs_total, 1, __ATOMIC_RELAXED);
    /* completion: the prefix, then the dependency cells (the policy module's order), then the next
     * claim: a policy module that finishes a job claims the next entry, flushed or not (221's kick has
     * no flush of its own) */
    AcquireSRWLockExclusive(&m->lock);
    m->s[q->slot].done[q->index] = 1;
    advance_locked(m, q->slot);
    ReleaseSRWLockExclusive(&m->lock);
    __atomic_sub_fetch(&m->running, 1, __ATOMIC_ACQ_REL);
    for (uint32_t d = 0; d < r.n_deps; d++)
        if (wws_cell_dec(r.dep_ea[d]) == 1) cell_opened(r.dep_ea[d], m);
    claim(m);
}

static DWORD WINAPI pool_main(LPVOID arg)
{
    const uint32_t idx = (uint32_t)(uintptr_t)arg;
    ps3_host_place_self(PS3_PLACE_SPU);
    wws_worker* w = wws_worker_create(idx % 6);
    mgr_state* last = NULL;
    for (;;) {
        /* PS3_NATIVE_JOBS_STICKY (default on): the next job of the manager this thread just ran first --
         * a manager is one module, so its code stays in this worker's local store (the module's shareable
         * reuse); its ticket goes stale and the thread that pops it moves on. Fan-out is the tickets'. */
        mgr_state* m = NULL;
        qjob q;
        int got = 0;
        if (last && s_sticky) {
            AcquireSRWLockExclusive(&last->lock);
            got = mq_pop_locked(last, &q);
            ReleaseSRWLockExclusive(&last->lock);
            if (got) m = last;
        }
        if (!got) {
            m = ready_pop();
            AcquireSRWLockExclusive(&m->lock);
            got = mq_pop_locked(m, &q);
            ReleaseSRWLockExclusive(&m->lock);
        }
        if (got) { run_one(w, idx % 6, m, &q); last = m; }
    }
    return 0;
}

/* ---- helping: a thread that waits for a manager runs its queued jobs ------------------------------- */
static SPU_THREAD_LOCAL wws_worker* t_helper;
/* The waiter runs the manager's oldest queued job only if it needs it: a job of the array it waits on,
 * below the index it waits for (PS3_NATIVE_JOBS_HELP=1). Any other job would only delay it: a pool thread
 * runs it. The queue is in claim order, so its head is the lowest such index. */
static int help_one(mgr_state* m, uint32_t slot, uint32_t below)
{
    if (!s_help) return 0;
    qjob q;
    int got = 0;
    AcquireSRWLockExclusive(&m->lock);
    if (m->qh != m->qt) {
        const qjob* h = &m->q[m->qh % MQ_CAP];
        if (s_help >= 2 || (h->slot == slot && h->index < below)) got = mq_pop_locked(m, &q);
    }
    ReleaseSRWLockExclusive(&m->lock);
    if (!got) return 0;
    if (!t_helper) t_helper = wws_worker_create(5);
    const uint64_t t0 = s_stats ? now_ns() : 0;
    run_one(t_helper, 5, m, &q);
    if (s_stats) __atomic_add_fetch(&s_help_ns, now_ns() - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_help_jobs, 1, __ATOMIC_RELAXED);
    return 1;
}

/* the words, as 0x54C730 reads them */
static uint32_t words_min(uint32_t mgr, uint32_t s)
{
    const uint32_t q = mgr + MGR_QUAD + 16 * s;
    uint32_t v = G16(q + 2);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (uint32_t o = 4; o <= 0xE; o += 2) { const uint32_t x = G16(q + o); if (x < v) v = x; }
    return v;
}
/* array s drained (0x54CE30 with readyCount = contention = 0): the entry at the words' minimum is the end */
static int array_drained(uint32_t mgr, uint32_t s)
{
    const uint32_t base = G32(arr_hdr(mgr, s) + ARR_BASE);
    const uint32_t k = words_min(mgr, s);
    return (G32(base + 8 * k) & 0xFFFF) == 0 && G32(base + 8 * k + 4) == 0;
}

typedef int (*wait_pred)(mgr_state* m, uint32_t a, uint32_t b);
static int pred_token(mgr_state* m, uint32_t s, uint32_t t) { return words_min(m->ea, s) >= t; }
static int pred_drained(mgr_state* m, uint32_t s, uint32_t unused) { (void)unused; return array_drained(m->ea, s); }

static void mgr_dump(const char* why, mgr_state* m, uint32_t a, uint32_t b, uint64_t ms)
{
    const uint32_t s = a & 1, h = arr_hdr(m->ea, s);
    const slot_state* ss = &m->s[s];
    fprintf(stderr, "[jobs] WARN %s: manager %08X (module %d) array %u for %llu ms: wants %s%u, words min %u, claimed %u,"
            " prefix %u, widx %u, seq %02X/%02X, active %u (u32[+8] %u), queued %u, running %ld, barrier cell %08X = %u\n",
            why, m->ea, m->module, s, (unsigned long long)ms, b == 0xFFFFFFFFu ? "drained " : "index ",
            b == 0xFFFFFFFFu ? 0u : b, words_min(m->ea, s), ss->claimed, ss->k, G32(h + ARR_WIDX), ss->seq & 0xFF,
            G8(h + ARR_SEQ), m->active, G32(m->ea + 8), m->qt - m->qh, m->running, m->blocked_cell,
            m->blocked_cell ? G16(m->blocked_cell) : 0);
}
static void wait_until(mgr_state* m, wait_pred p, uint32_t a, uint32_t b)
{
    if (p(m, a, b)) return;
    const uint64_t t0 = s_stats ? now_ns() : 0;
    const uint64_t w0 = GetTickCount64();
    uint64_t next_warn = 2000;
    claim(m);                                  /* entries kicked but not flushed yet */
    for (unsigned spin = 0; !p(m, a, b); spin++) {
        if (help_one(m, a, p == pred_token ? b : 0xFFFFFFFFu)) { spin = 0; continue; }
        if (spin < 4000) CPU_PAUSE();
        else if (spin < 4200) SwitchToThread();
        else {
            Sleep(0);
            if ((spin & 255) == 0) {
                claim(m);
                const uint64_t waited = GetTickCount64() - w0;
                if (waited >= next_warn) {      /* a stall: say what the manager looks like, once per doubling */
                    next_warn *= 2;
                    if (warn_take(W_WAIT)) mgr_dump("stalled wait", m, a, p == pred_token ? b : 0xFFFFFFFFu, waited);
                }
            }
        }
    }
    if (s_stats) { __atomic_add_fetch(&s_wait_ns, now_ns() - t0, __ATOMIC_RELAXED); __atomic_add_fetch(&s_waits, 1, __ATOMIC_RELAXED); }
}

/* the token's array: by its sequence byte; -1 = an older generation (done) */
static int token_slot(uint32_t mgr, uint32_t tok)
{
    const uint32_t seq = G8(tok + 1);
    if (G8(mgr + 184) == seq) return 0;
    if (G8(mgr + 212) == seq) return 1;
    return -1;
}

/* ---- the hooks ---------------------------------------------------------------------------------- */
static volatile long s_started;
static DWORD WINAPI stats_main(LPVOID arg);
static void pool_start(void)
{
    if (__atomic_exchange_n(&s_started, 1, __ATOMIC_ACQ_REL)) return;
    for (int i = 0; i < s_threads; i++) CreateThread(NULL, 1u << 20, pool_main, (LPVOID)(uintptr_t)i, 0, NULL);
    if (s_stats) CreateThread(NULL, 1u << 16, stats_main, NULL, 0, NULL);
    fprintf(stderr, "[jobs] native job runner: %d pool threads, managers: %s, waits %s\n", s_threads,
            s_all ? "all" : getenv("PS3_NATIVE_JOBS"), s_wait ? "native" : "the game's");
}

static mgr_state* owned(uint32_t ea)
{
    mgr_state* m = mgr_find(ea);
    return m && m->owned ? m : NULL;
}

/* 0x38AC78 ring_buffer_flush(r3 mgr) = cellSpursReadyCountStore(spurs, wid, count) */
static void hook_flush(ppu_context* ctx, ppu_hook_body body)
{
    mgr_state* m = mgr_get((uint32_t)ctx->gpr[3]);
    if (!m || !m->owned) { ppu_hook_call(ctx, body); return; }
    pool_start();
    claim(m);
    ctx->gpr[3] = 0;                   /* CELL_OK */
}

/* 0x38B43C token wait(r3 mgr, r4 -> token): until done, then token byte 0 = 0xFF */
static void hook_token_wait(ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t ea = (uint32_t)ctx->gpr[3], tok = (uint32_t)ctx->gpr[4];
    mgr_state* m = owned(ea);
    if (!m) { ppu_hook_call(ctx, body); return; }
    const int s = token_slot(ea, tok);
    if (s >= 0) { wait_until(m, pred_token, (uint32_t)s, G16(tok + 2)); ctx->gpr[3] = 1; }
    vm_base[tok] = 0xFF;
}

/* 0x38AA14 array wait(r3 mgr, r4 array) */
static void hook_array_wait(ppu_context* ctx, ppu_hook_body body)
{
    mgr_state* m = owned((uint32_t)ctx->gpr[3]);
    if (!m) { ppu_hook_call(ctx, body); return; }
    wait_until(m, pred_drained, (uint32_t)ctx->gpr[4] & 1, 0);
    ctx->gpr[3] = 1;
}

/* 0x38AAB8 arena wait(r3 mgr, r4 -> token): the token done, and if it is of the current generation the
 * other array drained too; then token byte 0 = 0xFF */
static void hook_arena_wait(ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t ea = (uint32_t)ctx->gpr[3], tok = (uint32_t)ctx->gpr[4];
    mgr_state* m = owned(ea);
    if (!m) { ppu_hook_call(ctx, body); return; }
    const int s = token_slot(ea, tok);
    if (s >= 0) {
        wait_until(m, pred_token, (uint32_t)s, G16(tok + 2));
        if (G8(tok + 1) == G8(ea + MGR_SEQ)) wait_until(m, pred_drained, (uint32_t)(s ^ 1), 0);
    }
    vm_base[tok] = 0xFF;
    ctx->gpr[3] = 1;
}

/* 0x38ABF8 token test(r3 mgr, r4 -> token): 1 = done or older; never writes the token. Its callers poll it
 * (the completion drain 0x29A8EC, the anim wait 0x32F24, the Edge kicker 0x3522E8 every 500 us). A test that
 * says "not yet" first claims: 0xF4568 kicks 221 WITHOUT a flush and the kicker then only tests the token (on
 * the PS3 a policy module still polling the workload picks such an entry up; with publish-at-flush alone the
 * job waited for the next flush, about a frame, and the main thread waited for the kicker at 0x350CE0).
 * Then it runs the asked-for job on the calling thread if it is queued. */
static void hook_token_test(ppu_context* ctx, ppu_hook_body body)
{
    const uint32_t ea = (uint32_t)ctx->gpr[3], tok = (uint32_t)ctx->gpr[4];
    mgr_state* m = owned(ea);
    if (!m) { ppu_hook_call(ctx, body); return; }
    const int s = token_slot(ea, tok);
    if (s < 0) { ctx->gpr[3] = 1; return; }
    const uint32_t t = G16(tok + 2);
    if (words_min(ea, (uint32_t)s) >= t) { ctx->gpr[3] = 1; return; }
    claim(m);
    if (help_one(m, (uint32_t)s, t)) __atomic_add_fetch(&s_test_help, 1, __ATOMIC_RELAXED);
    else for (int i = 0; i < 64; i++) CPU_PAUSE();
    ctx->gpr[3] = words_min(ea, (uint32_t)s) >= t ? 1 : 0;
}

/* Track D seam (libs/game/inf2_dlogic.c, PS3_D_HELP=1 default): the native world tick's poll of an entity whose anim
 * job is not done, after the token test above found nothing of that array to run: run this manager's oldest
 * queued job (either array) on the main thread instead of spinning on. Counters: inf2_dlogic_help_*. */
extern void (*volatile inf2_dlogic_help)(uint32_t mgr, uint32_t token);
extern volatile uint64_t inf2_dlogic_help_jobs, inf2_dlogic_help_ns, inf2_dlogic_help_left;
static void dlogic_help(uint32_t ea, uint32_t tok)
{
    mgr_state* m = owned(ea);
    if (!m) { inf2_dlogic_help_left++; return; }
    const int s = token_slot(ea, tok);
    if (s < 0 || words_min(ea, (uint32_t)s) >= G16(tok + 2)) return;
    qjob q;
    AcquireSRWLockExclusive(&m->lock);
    const int got = mq_pop_locked(m, &q);
    ReleaseSRWLockExclusive(&m->lock);
    if (!got) { inf2_dlogic_help_left++; return; }
    if (!t_helper) t_helper = wws_worker_create(5);
    const uint64_t t0 = now_ns();
    run_one(t_helper, 5, m, &q);
    inf2_dlogic_help_ns += now_ns() - t0;
    inf2_dlogic_help_jobs++;
    __atomic_add_fetch(&s_help_jobs, 1, __ATOMIC_RELAXED);
}

static DWORD WINAPI stats_main(LPVOID arg)
{
    (void)arg;
    uint64_t last = 0, lastbusy = 0, lasthelp = 0, lastwait = 0, lastwn = 0, lasthns = 0, lastth = 0;
    const int P = s_stats_sec;                  /* seconds per line */
    for (;;) {
        Sleep((DWORD)P * 1000u);
        const uint64_t n = s_jobs_total, b = s_busy_ns, h = s_help_jobs, wt = s_wait_ns, wn = s_waits;
        const uint64_t hns = s_help_ns, th = s_test_help;
        uint64_t reuse, loads; wws_native_stats(NULL, &reuse, &loads);
        fprintf(stderr, "[jobs] %llu jobs/s (%llu by waiters, %.2f ms/s, %llu/s from token tests), pool busy %.0f%% of %d threads,"
                " waits %llu/s %.2f ms/s, code reuse %llu / loads %llu, managers %ld;",
                (unsigned long long)((n - last) / P), (unsigned long long)((h - lasthelp) / P),
                (double)(hns - lasthns) / 1e6 / P, (unsigned long long)((th - lastth) / P),
                (double)(b - lastbusy) / 1e9 / P / s_threads * 100.0, s_threads,
                (unsigned long long)((wn - lastwn) / P), (double)(wt - lastwait) / 1e6 / P,
                (unsigned long long)reuse, (unsigned long long)loads, s_nmgr);
        {   /* per module over the last period: jobs/s and the average run time */
            static uint64_t lj[256], lns[256];
            for (int i = 200; i < 256; i++) {
                const uint64_t jn = s_mod_jobs[i], jt = s_mod_ns[i];
                if (jn != lj[i]) fprintf(stderr, " %d:%llu/s,%.0fus", i, (unsigned long long)((jn - lj[i]) / P),
                                         (double)(jt - lns[i]) / (double)(jn - lj[i]) / 1e3);
                lj[i] = jn; lns[i] = jt;
            }
        }
        fprintf(stderr, "\n");
        {   /* claim -> start latency over the last 10 s */
            static uint64_t ll = 0, ln = 0, lb[4];
            const uint64_t a = s_lat_ns, n2 = s_lat_n, b0 = s_lat_b[0], b1 = s_lat_b[1], b2 = s_lat_b[2], b3 = s_lat_b[3];
            const double dn = (double)(n2 - ln);
            if (dn > 0)
                fprintf(stderr, "[jobs] claim->start: avg %.1f us; <20 us %.1f%%, <100 us %.1f%%, <1 ms %.1f%%, >=1 ms %.2f%%\n",
                        (double)(a - ll) / dn / 1e3, (b0 - lb[0]) * 100.0 / dn, (b1 - lb[1]) * 100.0 / dn,
                        (b2 - lb[2]) * 100.0 / dn, (b3 - lb[3]) * 100.0 / dn);
            ll = a; ln = n2; lb[0] = b0; lb[1] = b1; lb[2] = b2; lb[3] = b3;
        }
        {   /* health: deepest queue, managers stopped at a barrier for more than 2 s, warnings so far */
            uint32_t qmax = 0, blocked = 0; const uint64_t now = GetTickCount64();
            const long nm = __atomic_load_n(&s_nmgr, __ATOMIC_ACQUIRE);
            for (long i = 0; i < nm; i++) {
                mgr_state* m = s_mgr[i];
                if (!m->owned) continue;
                const uint32_t q = m->qt - m->qh; if (q > qmax) qmax = q;
                const uint64_t since = m->blocked_since;
                if (m->blocked_cell && since && now - since > 2000) {
                    blocked++;
                    if (warn_take(W_BARRIER)) mgr_dump("stalled barrier", m, m->active, 0xFFFFFFFFu, now - since);
                }
            }
            fprintf(stderr, "[jobs] health: deepest queue %u, managers at a barrier > 2 s %u, warnings", qmax, blocked);
            for (int k = 0; k < W_N; k++) fprintf(stderr, " %s %ld%s", s_warn_name[k], s_warn_n[k], k + 1 < W_N ? "," : "\n");
        }
        {   /* PS3_NATIVE_JOBS_VERIFY: the lifted policy module staged the same jobs on a copy */
            uint64_t v, bad; wws_native_verify_stats(&v, &bad);
            if (v) fprintf(stderr, "[jobs] verify: %llu jobs staged by both, %llu differ\n",
                           (unsigned long long)v, (unsigned long long)bad);
        }
        last = n; lastbusy = b; lasthelp = h; lastwait = wt; lastwn = wn; lasthns = hns; lastth = th;
    }
    return 0;
}

/* ---- PS3_NATIVE_JOBS_WAITSTAT=1: time spent in the job waits, runner on or off ------------------- */
enum { WS_TOKEN, WS_ARRAY, WS_ARENA, WS_DRAIN, WS_N };
static const char* const s_ws_name[WS_N] = { "token wait 38B43C", "array wait 38AA14", "arena wait 38AAB8", "drain 29A8EC" };
static volatile uint64_t s_ws_ns[WS_N], s_ws_n[WS_N];
static ppu_hook_fn s_ws_inner[WS_N];
static void ws_pass(ppu_context* c, ppu_hook_body b) { ppu_hook_call(c, b); }
#define WS_WRAP(kind) \
    static void ws_wrap_##kind(ppu_context* c, ppu_hook_body b) { \
        const uint64_t t0 = now_ns(); s_ws_inner[kind](c, b); \
        __atomic_add_fetch(&s_ws_ns[kind], now_ns() - t0, __ATOMIC_RELAXED); \
        __atomic_add_fetch(&s_ws_n[kind], 1, __ATOMIC_RELAXED); }
WS_WRAP(WS_TOKEN) WS_WRAP(WS_ARRAY) WS_WRAP(WS_ARENA) WS_WRAP(WS_DRAIN)
static DWORD WINAPI ws_main(LPVOID arg)
{
    (void)arg;
    uint64_t last[WS_N] = { 0 }, lastn[WS_N] = { 0 };
    for (;;) {
        Sleep(10000);
        fprintf(stderr, "[jobs] waits per s:");
        for (int k = 0; k < WS_N; k++) {
            const uint64_t ns = s_ws_ns[k], n = s_ws_n[k];
            fprintf(stderr, " %s %.2f ms (%llu)", s_ws_name[k], (double)(ns - last[k]) / 1e7, (unsigned long long)((n - lastn[k]) / 10));
            last[k] = ns; lastn[k] = n;
        }
        fprintf(stderr, "\n");
    }
    return 0;
}
static void waitstat_install(void)
{
    s_ws_inner[WS_TOKEN] = (s_on && s_wait) ? hook_token_wait : ws_pass;
    s_ws_inner[WS_ARRAY] = (s_on && s_wait) ? hook_array_wait : ws_pass;
    s_ws_inner[WS_ARENA] = (s_on && s_wait) ? hook_arena_wait : ws_pass;
    s_ws_inner[WS_DRAIN] = ws_pass;
    ppu_hook_set(HOOK_TOKEN_WAIT, ws_wrap_WS_TOKEN);
    ppu_hook_set(HOOK_ARRAY_WAIT, ws_wrap_WS_ARRAY);
    ppu_hook_set(HOOK_ARENA_WAIT, ws_wrap_WS_ARENA);
    ppu_hook_set(HOOK_DRAIN, ws_wrap_WS_DRAIN);
    CreateThread(NULL, 1u << 16, ws_main, NULL, 0, NULL);
}

/* The engine is a static library: an object nothing references is not linked, and its constructor
 * never runs. spu_jobrec.c references wws_native.c, which references this. */
void wws_runner_link(void) {}

__attribute__((constructor)) static void wws_runner_ctor(void)
{
    cfg_init();
    if (!s_on) { if (s_waitstat) waitstat_install(); return; }
    ppu_hook_set(HOOK_FLUSH, hook_flush);
    if (s_wait) {
        ppu_hook_set(HOOK_TOKEN_WAIT, hook_token_wait);
        ppu_hook_set(HOOK_ARRAY_WAIT, hook_array_wait);
        ppu_hook_set(HOOK_ARENA_WAIT, hook_arena_wait);
        ppu_hook_set(HOOK_TOKEN_TEST, hook_token_test);
    }
    if (env_int("PS3_D_HELP", 1)) inf2_dlogic_help = dlogic_help;   /* Track D's tick loop (above) */
    if (s_waitstat) waitstat_install();
    { const char* e = getenv("PS3_NATIVE_JOBS");
      fprintf(stderr, "[jobs] PS3_NATIVE_JOBS=%s: the job managers go to the native runner\n", e && *e ? e : "1 (default)"); }
}
