/* jobrunner_harness.c -- the native job runner (engine/libs/spurs/wws_runner.c) without the game.
 *
 * The runner is linked as it is; everything around it is a stand-in: guest memory is a plain buffer, a job
 * "runs" by counting itself, and this file plays the game's side of the job manager (init 0x38B10C, kick
 * 0x38B644, swap 0x38AE0C, the slot reset 0x54C6D0) the way work/native/B1_CONTRACT.md describes it, calling
 * the runner through the hooks it registers (flush, token wait, token test, array wait).
 *
 *     jobrunner_harness <scenario>       exit 0 = pass, 1 = a job ran twice or never, 3 = a wait hung
 *
 * Scenarios (tests/jobrunner/test_jobrunner.py runs them, with the runner's switches in the environment):
 *   stream         one manager, 20000 jobs over many swaps, token waits in between: the plain path
 *   reinit_same    the hang of "Powering Up Flood Town": a manager that swapped to array 1 is destroyed and
 *                  created again at the same address with the same workload id and the same two arrays, then
 *                  97 jobs are kicked into array 0 and the game waits for token 11
 *   reinit_poll    the same, the game polling the token test instead of waiting
 *   reinit_noswap  the old instance never swapped, so the new one also has the same sequence byte, active
 *                  array and table version: the first 50 entries of the new instance must still run
 *   reinit_inplace the init runs over the old object (not zeroed first)
 *   prio           (deck, s1) two managers at once for PS3_NATIVE_JOBS_PRIO: the "main" thread (guest thread id 1)
 *                  streams 20000 jobs with token waits, a second thread (id 8) 6000 jobs of its own manager with
 *                  polled token tests -- the two priority classes; every job must run once, no wait may hang
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../engine/libs/spurs/wws_native.h"
#include "../../engine/libs/spurs/wws_njob.h"
#include "../../engine/runtime/ppu/ppu_context.h"
#include "../../engine/runtime/platform/win32_compat.h"
#include "../../engine/include/ps3emu/ppu_hooks.h"

#ifdef _MSC_VER
#define HARNESS_TLS __declspec(thread)
#else
#include <unistd.h>
#define HARNESS_TLS __thread
#endif

/* ---- what the runner links against ---------------------------------------------------------- */
uint8_t* vm_base;
#define VM_SIZE   (64u << 20)
#define CODE_EA   0x00100000u
#define DESC_BASE 0x01000000u          /* job n's descriptor: DESC_BASE + 128 n */
#define MAX_JOBS  200000u

static volatile long s_ran[MAX_JOBS];
static SRWLOCK s_line = SRWLOCK_INIT;

int spu_overlay_match_sig(const uint8_t hdr[16]) { (void)hdr; return 212; }
void wws_native_verify_stats(uint64_t* v, uint64_t* bad) { *v = 0; *bad = 0; }
void wws_native_stats(uint64_t* jobs, uint64_t* reuse, uint64_t* loads) { if (jobs) *jobs = 0; if (reuse) *reuse = 0; if (loads) *loads = 0; }
void spurs_kernel_notify(uint32_t ea) { (void)ea; }
int  ps3_host_cores(void) { return 6; }
void ps3_host_place_self(int role) { (void)role; }
void (*volatile inf2_dlogic_help)(uint32_t mgr, uint32_t token);
volatile uint64_t inf2_dlogic_help_jobs, inf2_dlogic_help_ns, inf2_dlogic_help_left;

struct wws_worker { int x; };
wws_worker* wws_worker_create(uint32_t spu_num) { (void)spu_num; return (wws_worker*)calloc(1, sizeof(wws_worker)); }
void wws_worker_set_spu_num(wws_worker* w, uint32_t n) { (void)w; (void)n; }
int  wws_njob_try(wws_worker* w, const wws_job* j, int module, wws_result* out) { (void)w; (void)j; (void)module; (void)out; return 0; }
void wws_native_run(wws_worker* w, const wws_job* job, wws_result* out)
{
    (void)w;
    const uint32_t n = (job->desc_ea - DESC_BASE) / 128;
    if (n < MAX_JOBS) __atomic_add_fetch(&s_ran[n], 1, __ATOMIC_SEQ_CST);
    for (volatile int i = 0; i < 2000; i++) { }       /* a little work, so jobs overlap */
    memset(out, 0, sizeof *out);
    out->ok = 1; out->module = 212;
}
void wws_line_update(uint32_t ea, void (*fn)(uint8_t* line, void* arg), void* arg)
{
    AcquireSRWLockExclusive(&s_line);
    fn(vm_base + (ea & ~127u), arg);
    ReleaseSRWLockExclusive(&s_line);
}
static void dec_fn(uint8_t* line, void* a)
{
    uint32_t* x = (uint32_t*)a; uint8_t* p = line + x[0];
    const uint32_t v = ((uint32_t)p[0] << 8) | p[1];
    x[1] = v; p[0] = (uint8_t)((v - 1) >> 8); p[1] = (uint8_t)(v - 1);
}
uint32_t wws_cell_dec(uint32_t cell) { uint32_t x[2] = { cell & 127u, 0 }; wws_line_update(cell, dec_fn, x); return x[1]; }

static struct { uint32_t addr; ppu_hook_fn fn; } s_hooks[32];
static int s_nhooks;
int ppu_hook_set(uint32_t addr, ppu_hook_fn fn) { s_hooks[s_nhooks].addr = addr; s_hooks[s_nhooks].fn = fn; s_nhooks++; return 1; }
void ppu_hook_call(ppu_context* ctx, ppu_hook_body body) { (void)ctx; (void)body; fprintf(stderr, "harness: a hook ran the game's body\n"); exit(2); }
static HARNESS_TLS uint64_t s_tid;    /* the guest thread id the hooks see (0; the prio scenario: 1 = main, 8) */
static uint64_t hook(uint32_t addr, uint32_t r3, uint32_t r4)
{
    static HARNESS_TLS ppu_context* ctx;
    if (!ctx) ctx = (ppu_context*)calloc(1, sizeof *ctx);
    for (int i = 0; i < s_nhooks; i++) if (s_hooks[i].addr == addr) {
        ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->thread_id = s_tid;
        s_hooks[i].fn(ctx, NULL);
        return ctx->gpr[3];
    }
    fprintf(stderr, "harness: no hook at %08X (is PS3_NATIVE_JOBS off?)\n", addr); exit(2);
}

/* ---- guest memory --------------------------------------------------------------------------- */
static uint32_t R32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void W32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void W16(uint32_t ea, uint32_t v) { vm_base[ea] = (uint8_t)(v >> 8); vm_base[ea + 1] = (uint8_t)v; }
static uint32_t R16(uint32_t ea) { return ((uint32_t)vm_base[ea] << 8) | vm_base[ea + 1]; }

/* ---- the game's side of a manager (B1_CONTRACT.md) -------------------------------------------- */
#define CAP 128u                       /* entries per array (module 212's manager: a swap at 126) */
#define H(mgr, s) ((mgr) + 164 + 28 * (s))
static void slot_reset(uint32_t mgr, uint32_t s)          /* 0x54C6D0 */
{
    const uint32_t q = mgr + 0x40 + 16 * s;
    AcquireSRWLockExclusive(&s_line);
    for (uint32_t o = 4; o <= 0xE; o += 2) W16(q + o, 0xFFFF);
    W16(q + 2, 0); W16(q, 0);
    ReleaseSRWLockExclusive(&s_line);
}
static void mgr_init(uint32_t mgr, uint32_t base0, uint32_t base1, uint32_t wid, int zero_first)   /* 0x38B10C */
{
    if (zero_first) memset(vm_base + mgr, 0, 384);         /* a new object at the old address */
    W32(mgr + 128, mgr); W32(mgr + 132, 0x00200000u); W32(mgr + 136, wid);
    for (uint32_t s = 0; s < 2; s++) {
        const uint32_t base = s ? base1 : base0;
        W32(H(mgr, s) + 0, base); W32(H(mgr, s) + 4, CAP); W32(H(mgr, s) + 8, 0);
        memset(vm_base + base, 0, 8 * CAP);
        slot_reset(mgr, s);
    }
    const uint8_t seq = (uint8_t)(vm_base[mgr + 260] + 1);
    W32(mgr + 256, 0);
    vm_base[mgr + 184] = seq; vm_base[mgr + 260] = seq;
    W32(mgr + 8, 0); W32(mgr + 0xC, R32(mgr + 0xC) - 1);
}
static void flush(uint32_t mgr) { hook(0x0038AC78u, mgr, 0); }
static void array_wait(uint32_t mgr, uint32_t s) { hook(0x0038AA14u, mgr, s); }
static void push(uint32_t mgr, uint32_t s, uint32_t hi, uint32_t lo)
{
    const uint32_t base = R32(H(mgr, s)), w = R32(H(mgr, s) + 8);
    W32(base + 8 * w + 4, lo); W32(base + 8 * w, hi);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    W32(H(mgr, s) + 8, w + 1);
}
static void swap(uint32_t mgr)                             /* 0x38AE0C */
{
    flush(mgr);
    const uint32_t cur = R32(mgr + 256), o = 1 - cur;
    array_wait(mgr, o);
    const uint8_t seq = (uint8_t)(vm_base[mgr + 260] + 1);
    vm_base[mgr + 260] = seq; vm_base[H(mgr, o) + 20] = seq;
    W32(H(mgr, o) + 8, 0); memset(vm_base + R32(H(mgr, o)), 0, 8 * CAP);
    slot_reset(mgr, o);
    push(mgr, cur, 4, o);                                  /* the marker */
    W32(mgr + 256, o);
}
static uint32_t s_next_job;
static uint32_t s_tok_ea = 0x00300000u;
/* kick 0x38B644: returns the EA of a token {pool slot, seq, u16 index} */
static uint32_t kick(uint32_t mgr)
{
    uint32_t cur = R32(mgr + 256);
    if (R32(H(mgr, cur) + 8) >= CAP - 2) { swap(mgr); cur = R32(mgr + 256); }
    const uint32_t n = __atomic_fetch_add(&s_next_job, 1, __ATOMIC_SEQ_CST), desc = DESC_BASE + 128 * n;
    W32(desc, 0x8400u); W32(desc + 4, CODE_EA);            /* UseBuffer, set 0, load: the job's code */
    push(mgr, cur, (8u << 16) | 1, desc);
    const uint32_t tok = __atomic_fetch_add(&s_tok_ea, 4, __ATOMIC_SEQ_CST);   /* (prio: two kicking threads) */
    vm_base[tok] = 0; vm_base[tok + 1] = vm_base[mgr + 260]; W16(tok + 2, R32(H(mgr, cur) + 8));
    return tok;
}
static void token_wait(uint32_t mgr, uint32_t tok) { hook(0x0038B43Cu, mgr, tok); }
static int  token_test(uint32_t mgr, uint32_t tok) { return (int)hook(0x0038ABF8u, mgr, tok); }
static void mgr_reset(uint32_t mgr) { array_wait(mgr, 0); array_wait(mgr, 1); }   /* 0x38BA78 */

/* ---- the scenarios ---------------------------------------------------------------------------- */
static const char* s_stage = "start";
static DWORD WINAPI watchdog(LPVOID a)
{
    Sleep((DWORD)(uintptr_t)a);
    fprintf(stderr, "harness: HANG at stage '%s'\n", s_stage); fflush(stderr);
    _exit(3);
    return 0;
}
static int check(uint32_t from, uint32_t to, const char* what)
{
    uint32_t never = 0, twice = 0;
    for (uint32_t n = from; n < to; n++) { const long r = s_ran[n]; if (!r) never++; else if (r > 1) twice++; }
    printf("%s: jobs %u..%u: %u never ran, %u ran more than once\n", what, from, to, never, twice);
    return never || twice;
}
#define MGR   0x00B02600u
#define ARR_A 0x00400000u
#define ARR_B 0x00401000u

/* (s1) prio: the second thread -- its own manager, polled token tests (as Edge's kicker polls Edge's) */
#define MGR2  0x00B02A00u
#define ARR_C 0x00402000u
#define ARR_D 0x00403000u
static volatile long s_second_done;
static DWORD WINAPI second_thread(LPVOID a)
{
    (void)a;
    s_tid = 8;
    for (uint32_t i = 0; i < 6000; i++) {
        const uint32_t t = kick(MGR2);
        if (i % 3 == 2) flush(MGR2);
        if (i % 10 == 9) { flush(MGR2); while (!token_test(MGR2, t)) Sleep(0); }
    }
    flush(MGR2); mgr_reset(MGR2);
    __atomic_store_n(&s_second_done, 1, __ATOMIC_SEQ_CST);
    return 0;
}

/* an instance that kicks `n` jobs, flushing every `every`, and is then waited for */
static void run_instance(uint32_t n, uint32_t every)
{
    for (uint32_t i = 0; i < n; i++) { kick(MGR); if (i % every == every - 1) flush(MGR); }
    flush(MGR);
    mgr_reset(MGR);
}
/* the new instance of the bug report: 97 jobs, a wait for token 11, then for all of it */
static int second_instance(int poll)
{
    const uint32_t first = s_next_job;
    uint32_t tok11 = 0;
    for (uint32_t i = 0; i < 97; i++) { const uint32_t t = kick(MGR); if (i == 10) tok11 = t; }
    flush(MGR);
    s_stage = "the new instance's wait for token 11";
    if (poll) { while (!token_test(MGR, tok11)) Sleep(0); }
    else token_wait(MGR, tok11);
    s_stage = "the new instance's drain";
    mgr_reset(MGR);
    printf("new instance: words +2 = %u, u32[+8] = %u\n", R16(MGR + 0x40 + 2), R32(MGR + 8));
    return check(first, s_next_job, "new instance");
}

int main(int argc, char** argv)
{
    const char* sc = argc > 1 ? argv[1] : "";
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)8000, 0, NULL);
    int bad = 0;
    if (!strcmp(sc, "stream")) {
        mgr_init(MGR, ARR_A, ARR_B, 10, 1);
        s_stage = "stream";
        uint32_t toks[64]; int nt = 0;
        for (uint32_t i = 0; i < 20000; i++) {
            const uint32_t t = kick(MGR);
            if (i % 7 == 0) flush(MGR);
            if (i % 13 == 0) { toks[nt++] = t; if (nt == 8) { flush(MGR); for (int k = 0; k < nt; k++) token_wait(MGR, toks[k]); nt = 0; } }
        }
        flush(MGR); mgr_reset(MGR);
        bad = check(0, s_next_job, "stream");
    } else if (!strcmp(sc, "reinit_same") || !strcmp(sc, "reinit_poll")) {
        /* 73 swaps: array 0 holds 126 jobs + the marker under sequence byte 0x49, the claim is in array 1 */
        mgr_init(MGR, ARR_A, ARR_B, 10, 1);
        s_stage = "the old instance";
        run_instance(126 * 73 + 30, 5);
        printf("old instance: seq of array 0 = %02X, widx %u, u32[+8] = %u\n", vm_base[H(MGR, 0) + 20], R32(H(MGR, 0) + 8), R32(MGR + 8));
        bad = check(0, s_next_job, "old instance");
        mgr_init(MGR, ARR_A, ARR_B, 10, 1);                /* destroyed, created again: same address, wid, arrays */
        bad |= second_instance(!strcmp(sc, "reinit_poll"));
    } else if (!strcmp(sc, "reinit_noswap") || !strcmp(sc, "reinit_inplace")) {
        mgr_init(MGR, ARR_A, ARR_B, 10, 1);
        s_stage = "the old instance";
        run_instance(!strcmp(sc, "reinit_noswap") ? 50 : 200, 5);
        bad = check(0, s_next_job, "old instance");
        mgr_init(MGR, ARR_A, ARR_B, 10, !strcmp(sc, "reinit_noswap"));
        bad |= second_instance(0);
    } else if (!strcmp(sc, "prio")) {
        mgr_init(MGR, ARR_A, ARR_B, 10, 1);
        mgr_init(MGR2, ARR_C, ARR_D, 11, 1);
        s_stage = "prio: two managers, two threads";
        s_tid = 1;                                         /* this thread is the guest's main thread */
        flush(MGR);                                        /* the runner sees the main manager first */
        CreateThread(NULL, 0, second_thread, NULL, 0, NULL);
        uint32_t toks[64]; int nt = 0;
        for (uint32_t i = 0; i < 20000; i++) {
            const uint32_t t = kick(MGR);
            if (i % 2 == 0) flush(MGR);
            if (i % 13 == 0) { toks[nt++] = t; if (nt == 8) { flush(MGR); for (int k = 0; k < nt; k++) token_wait(MGR, toks[k]); nt = 0; } }
        }
        flush(MGR); mgr_reset(MGR);
        s_stage = "prio: the second thread's jobs";
        while (!__atomic_load_n(&s_second_done, __ATOMIC_SEQ_CST)) Sleep(1);
        bad = check(0, s_next_job, "prio (both managers)");
    } else {
        fprintf(stderr, "usage: jobrunner_harness stream|reinit_same|reinit_poll|reinit_noswap|reinit_inplace|prio\n");
        return 2;
    }
    fflush(stdout);
    printf(bad ? "FAIL\n" : "PASS\n");
    fflush(stdout);
    _exit(bad ? 1 : 0);
}
