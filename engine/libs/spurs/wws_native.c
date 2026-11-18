/* wws_native.c -- the WWS job manager's per-job work in host C (Track B1). See wws_native.h.
 *
 * Read from the lifted policy module (work/port/spu/wwsjob_pm_at_008E5B00, disassembly of the image at
 * guest 0x8E5B00) and kept to its local-store formats, so the job module sees the local store it sees
 * under the policy module, and the C API below answers exactly what the module's API answers:
 *
 *   LS 0x0000        interrupt vector word (never taken here: the context's event mask is 0)
 *   LS 0x01C0/0x1D0  the kernel words the module copies into the job context
 *   LS 0x0A00..3880  the policy module image itself (its API table at 0x1360, data tables)
 *   LS 0x0C00        the job's command list (desc, `len` bytes); the job's parameters follow RunJob
 *   LS 0x0DF0        buffer sets: [3 job slots][16] u32 {firstPage, pagesPerBuf, reserved<<7|numBufs, firstBuf}
 *   LS 0x0EB0        buffers: [3][32] u64 {E0: free<<31|share<<30|wifd<<29|handed<<28|used<<27|tag<<22|size16<<8|stamp,
 *                                        E1: EA&~15 | set}
 *   LS 0x11B0        logical -> physical buffer map: [3][32] u8 (0xFF = none)
 *   LS 0x1230/0x1250/0x1270  page masks (256 LS pages of 1 KB, MSB first): used / shareable / reserved
 *   LS 0x1290        DMA tag mask (bit t = tag t taken; tags 0..7 belong to the manager)
 *   LS 0x12A0..0x1300 load / run / store slot state, LS 0x1330.. the job context the job's r3 points at:
 *                    +0 {spuNum, 0x1F, 0, spurs}, +0x10 {wid<<16|index, ...}, +0x20 the parameter pointer,
 *                    +0x30.. the API: +0x34 get buffer, +0x3C alloc tag, +0x40 free tag, +0x44 load next job
 *   LS 0x1440        in-flight records: [3] {ctrl|slot, end buffer<<16|shareable, index, deps} + 4 dep EAs
 *   LS 0x3E000       the module's stack canary 0x71077345; the job's stack pointer is 0x3F990
 *
 * Differences that are not visible to a job: one job at a time per worker (the module pipelines load /
 * run / store of three), so a double-buffered set always gets physical buffer 0, DMA tags start at 31 for
 * every job, and the next job is never claimed from inside LoadNextJob. A shareable buffer (the job code,
 * 218's rig) is kept between two jobs of a worker exactly when the module would keep it: same EA, same
 * pages, the new size not larger. */
#include "wws_native.h"
#include "../../runtime/spu/spu_context.h"
#include "../../runtime/spu/spu_coherency.h"
#include "../../runtime/spu/spu_jobrec.h"
#ifndef _WIN32
#include "../../runtime/platform/msvc_compat.h"   /* _aligned_malloc */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* vm_base;
extern u128 spu_rdch(spu_context* ctx, uint32_t channel);
extern void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
extern void spu_indirect_branch(spu_context* ctx);
extern void spu_drain_call(spu_context* ctx, uint32_t return_pc);
extern int  spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
extern void spu_begin_image(int image_id);
extern void spu_register_function(uint32_t addr, void (*fn)(spu_context*));
extern void (*g_spu_line_commit_hook)(uint32_t line);
extern int  spu_overlay_match_sig(const uint8_t hdr[16]);
extern void (*spu_find_function(int image_id, uint32_t addr))(spu_context*);
#define WWS_PM_IMAGE 3                /* the lifted policy module's image id (work/port/spu/spu_workloads.c) */

/* ---- local store, big-endian ------------------------------------------------------------- */
static inline uint32_t L32(const spu_context* c, uint32_t a)
{
    const uint8_t* p = c->ls + (a & SPU_LS_MASK);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void W32(spu_context* c, uint32_t a, uint32_t v)
{
    uint8_t* p = c->ls + (a & SPU_LS_MASK);
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void W16(spu_context* c, uint32_t a, uint32_t v)
{
    uint8_t* p = c->ls + (a & SPU_LS_MASK);
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void WQ(spu_context* c, uint32_t a, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    W32(c, a, w0); W32(c, a + 4, w1); W32(c, a + 8, w2); W32(c, a + 12, w3);
}
static inline void WSPLAT(spu_context* c, uint32_t a, uint32_t v) { WQ(c, a, v, v, v, v); }
static inline u128 U(uint32_t w0) { u128 v; memset(&v, 0, sizeof v); v._u32[0] = w0; return v; }
static inline void WR(spu_context* c, uint32_t ch, uint32_t v) { spu_wrch(c, ch, U(v)); }
static inline uint32_t SHL(uint32_t v, uint32_t n) { n &= 0x3F; return n < 32 ? v << n : 0; }   /* SPU shl */

enum {
    LS_CMDS = 0xC00, LS_SETS = 0xDF0, LS_BUFS = 0xEB0, LS_MAP = 0x11B0,
    LS_PG_USED = 0x1230, LS_PG_SHARE = 0x1250, LS_PG_RES = 0x1270, LS_TAGS = 0x1290,
    LS_LOADSTATE = 0x12A0, LS_JOBSTATE = 0x12B0, LS_CLAIMSLOT = 0x12C0, LS_LOADSLOT = 0x12D0,
    LS_RUNSLOT = 0x12E0, LS_STORESLOT = 0x12F0, LS_PREVSLOT = 0x1300, LS_SETPTR = 0x1310,
    LS_CODE = 0x1320, LS_JOBCTX = 0x1330, LS_JOBCTX1 = 0x1340, LS_PARAMS = 0x1350,
    LS_AUDIT = 0x1390, LS_RECCOPY = 0x13A0, LS_NEXTBUF = 0x13B0, LS_FREEBUFS = 0x13C0,
    LS_R4 = 0x13D0, LS_JOBCNT = 0x13E0, LS_CTRL = 0x13F0, LS_WID = 0x1400,
    LS_CLAIM_C = 0x1410, LS_CLAIM_E = 0x1420, LS_CLAIM_S = 0x1430, LS_REC = 0x1440,
    LS_STACK_TOP = 0x3F990, LS_CANARY = 0x3E000,
};

/* ---- page masks: 256 pages, page p = bit (7 - p%8) of byte p/8 ----------------------------- */
typedef struct { uint8_t b[32]; } pgmask;
static void pg_range(pgmask* m, uint32_t first, uint32_t n)
{
    memset(m, 0, sizeof *m);
    for (uint32_t p = first; p < first + n && p < 256; p++) m->b[p >> 3] |= (uint8_t)(0x80u >> (p & 7));
}
static int pg_overlap(const spu_context* c, uint32_t at, const pgmask* m)
{
    for (int i = 0; i < 32; i++) if (c->ls[at + i] & m->b[i]) return 1;
    return 0;
}
static void pg_or(spu_context* c, uint32_t at, const pgmask* m) { for (int i = 0; i < 32; i++) c->ls[at + i] |= m->b[i]; }
static void pg_clr(spu_context* c, uint32_t at, const pgmask* m) { for (int i = 0; i < 32; i++) c->ls[at + i] &= (uint8_t)~m->b[i]; }

/* ---- the worker ---------------------------------------------------------------------------- */
typedef struct cached_buf {   /* a shareable buffer the previous job of this worker left in its local store */
    uint32_t ea, size16, first_page, ppb, valid;
} cached_buf;

struct wws_worker {
    spu_context* ctx;
    spu_context* vctx;         /* PS3_NATIVE_JOBS_VERIFY: the lifted policy module stages the same job here */
    uint32_t spu_num;
    uint32_t pm_loaded;
    uint32_t job_count;        /* LS_JOBCNT */
    cached_buf cache[16];      /* by buffer set */
};

static volatile uint64_t s_jobs, s_reuse, s_loads, s_verified, s_verify_bad;
static int s_verify = -1;
void wws_native_verify_stats(uint64_t* verified, uint64_t* bad) { *verified = s_verified; *bad = s_verify_bad; }
void wws_native_stats(uint64_t* jobs, uint64_t* code_reuse, uint64_t* code_loads)
{
    if (jobs) *jobs = s_jobs;
    if (code_reuse) *code_reuse = s_reuse;
    if (code_loads) *code_loads = s_loads;
}

void wws_native_api_register(void);

wws_worker* wws_worker_create(uint32_t spu_num)
{
    wws_native_api_register();
    wws_worker* w = (wws_worker*)calloc(1, sizeof *w);
    spu_context* c = (spu_context*)_aligned_malloc(sizeof(spu_context), 64);
    if (!w || !c) { fprintf(stderr, "[wws] out of memory\n"); abort(); }
    spu_context_init(c, 0x7000u + spu_num);
    c->image_id = WWS_NATIVE_IMAGE;
    c->policy_mode = 0;
    c->event_mask = 0;                    /* no interrupts: every DMA here completes at once */
    w->ctx = c;
    w->spu_num = spu_num;
    return w;
}
void wws_worker_set_spu_num(wws_worker* w, uint32_t spu_num) { w->spu_num = spu_num; }
struct spu_context* wws_worker_ctx(wws_worker* w) { return w->ctx; }

/* What the policy module's start (0x2B60) leaves in the local store that a job can see. */
static void pm_template(wws_worker* w)
{
    spu_context* c = w->ctx;
    memcpy(c->ls + 0xA00, vm_base + WWS_PM_EA, WWS_PM_SIZE);
    WSPLAT(c, 0x0, 0x30014580u);                       /* the interrupt vector the module plants */
    WSPLAT(c, LS_CANARY, 0x71077345u);
    WSPLAT(c, LS_CLAIMSLOT, 0);
    WSPLAT(c, LS_PREVSLOT, 2);
    WSPLAT(c, 0x14A0, 0);
    WSPLAT(c, 0xD80, 0xB00); WSPLAT(c, 0xDD0, 0); WSPLAT(c, 0xDC0, 0); WSPLAT(c, 0xD90, 0); WSPLAT(c, 0xDB0, 0);
    WSPLAT(c, LS_STORESLOT, 0xFFFFFFFFu);
    WSPLAT(c, LS_RUNSLOT, 0xFFFFFFFFu);
    w->pm_loaded = 1;
}

/* ---- DMA through the context's MFC (the engine's own semantics: lists, overlay notes) ---- */
static void dma(spu_context* c, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag, uint32_t cmd)
{
    WR(c, MFC_LSA, lsa);
    WR(c, MFC_EAH, 0);
    WR(c, MFC_EAL, ea);
    WR(c, MFC_Size, size);
    WR(c, MFC_TagID, tag);
    WR(c, MFC_Cmd, cmd);
}
static void dma_wait_all(spu_context* c)
{
    WR(c, MFC_WrTagMask, 0xFFFFFFFFu);
    WR(c, MFC_WrTagUpdate, 2);
    (void)spu_rdch(c, MFC_RdTagStat);
}

/* ---- the command list (0x1AE8 and its handlers), for ONE job in load slot `L` -------------- */
typedef struct stage_state {
    uint32_t L, prev, code_lsa, params, set_ptr, done;
    const char* why;
} stage_state;

/* 0x1B50 kReserveBufSet */
static int cmd_reserve(wws_worker* w, stage_state* st, uint32_t F, uint32_t D)
{
    spu_context* c = w->ctx;
    const uint32_t set = (F >> 5) & 15;
    const uint32_t first = D >> 24, ppb = (D >> 16) & 0xFF, nb = (D >> 8) & 0xFF;
    const uint32_t next = L32(c, LS_NEXTBUF), freeb = L32(c, LS_FREEBUFS);
    pgmask m; pg_range(&m, first, ppb * nb);
    if (pg_overlap(c, LS_PG_RES, &m)) { st->why = "ReserveBufSet: pages already reserved"; return 0; }
    if (nb > freeb) { st->why = "ReserveBufSet: out of buffers"; return 0; }
    pg_or(c, LS_PG_RES, &m);
    W32(c, LS_SETS + st->L * 64 + set * 4, (D & 0xFFFFFF00u) | 0x8000u | (next & 0xFF));
    for (uint32_t i = 0; i < nb; i++) {                /* each buffer: free, owned by this set */
        W32(c, LS_BUFS + (next + i) * 8, 0x80000000u);
        W32(c, LS_BUFS + (next + i) * 8 + 4, set);
    }
    WSPLAT(c, LS_NEXTBUF, next + nb);
    WSPLAT(c, LS_FREEBUFS, freeb - nb);
    return 1;
}

/* 0x1C28 kUseBuffer (the paths this game's descriptors use: load / list load / no load, shareable) */
static int cmd_use(wws_worker* w, stage_state* st, uint32_t F, uint32_t D)
{
    spu_context* c = w->ctx;
    const uint32_t set = (F >> 5) & 15, lb = F & 31;
    if (F & 0x10000u) { st->why = "UseBuffer: the 'already placed' form"; return 0; }
    const uint32_t S = L32(c, LS_SETS + st->L * 64 + set * 4);
    if (!S) { st->why = "UseBuffer: set not reserved"; return 0; }
    const uint32_t first = S >> 24, ppb = (S >> 16) & 0xFF, nb = (S >> 8) & 0x7F, fbi = S & 0xFF;
    /* the least recently used free physical buffer: in a fresh row, the first free one */
    uint32_t p = 0xFF;
    for (uint32_t j = 0; j < nb; j++) if (L32(c, LS_BUFS + (fbi + j) * 8) & 0x80000000u) { p = j; break; }
    if (p == 0xFF) { st->why = "UseBuffer: no free buffer"; return 0; }
    const uint32_t page = first + ppb * p, ent = LS_BUFS + (fbi + p) * 8;
    const uint32_t size16 = (F >> 18) & 0x3FFF;
    const uint32_t e1 = (D & ~15u) | (L32(c, ent + 4) & 15);
    uint32_t e_size16 = size16;   /* a reused buffer keeps the size it was loaded with */
    if (F & 0x8000u) {
        const uint32_t tag = 1;                        /* kLoadJob_readBuffers */
        const uint32_t lsa = page << 10;
        cached_buf* cb = &w->cache[set];
        const int share = (F & 0x2000u) != 0;
        if (share && !(F & 0x4000u) && cb->valid && cb->ea == (D & ~15u) && cb->first_page == first &&
            cb->ppb == ppb && size16 <= cb->size16 && p == 0) {
            __atomic_add_fetch(&s_reuse, 1, __ATOMIC_RELAXED);   /* the module's shareable reuse: no load */
            e_size16 = cb->size16;
        } else {
            cb->valid = 0;
            if (F & 0x4000u) {                         /* a DMA list: fetch the list, then gather */
                const uint32_t list_bytes = size16 << 4;
                const uint32_t lls = ((page + ppb) << 10) - list_bytes - ((D & 7) << 4);
                dma(c, lls, D & ~15u, list_bytes, tag, 0x40);
                WR(c, MFC_LSA, lsa);
                WR(c, MFC_EAL, lls);
                WR(c, MFC_Cmd, 0x45);                  /* GETLB (size and tag as above) */
            } else {
                uint32_t left = size16 << 4, at = lsa, ea = D & ~15u;
                for (;;) {                             /* 16 KB pieces, as the module issues them */
                    const uint32_t n = left < 16384u ? left : 16384u;
                    dma(c, at, ea, n, tag, 0x40);
                    at += n; ea += n; left -= n;
                    if (n < 16384u) break;
                }
                if (set == 0) __atomic_add_fetch(&s_loads, 1, __ATOMIC_RELAXED);
            }
            if (share && !(F & 0x4000u)) {
                cb->ea = D & ~15u; cb->size16 = size16; cb->first_page = first; cb->ppb = ppb; cb->valid = (p == 0);
            }
        }
    } else {
        w->cache[set].valid = 0;                       /* the pages hold something else now */
    }
    /* any cached buffer of ANOTHER set whose pages this buffer covers is gone */
    for (uint32_t k = 0; k < 16; k++) {
        cached_buf* o = &w->cache[k];
        if (k == set || !o->valid) continue;
        if (o->first_page < page + ppb && page < o->first_page + o->ppb) o->valid = 0;
    }
    const uint32_t e0 = 0x08000000u | ((F & 0x3000u) << 17) | (e_size16 << 8) | (w->job_count & 0xFF);
    W32(c, ent, e0);
    W32(c, ent + 4, e1);
    c->ls[LS_MAP + fbi + lb] = (uint8_t)p;
    pgmask m; pg_range(&m, page, ppb);
    pg_or(c, LS_PG_USED, &m);
    if (F & 0x3000u) {
        pg_or(c, LS_PG_SHARE, &m);
        const uint32_t r = LS_REC + st->L * 32;
        W32(c, r + 4, L32(c, r + 4) | 0xFFFFu);
    }
    return 1;
}

/* 0x2158 kUnreserveBufSets */
static int cmd_unreserve(wws_worker* w, stage_state* st, uint32_t F, uint32_t D)
{
    spu_context* c = w->ctx;
    (void)F;
    for (uint32_t k = 0; k < 16; k++) {
        if (!(D & (1u << k))) continue;
        const uint32_t a = LS_SETS + st->L * 64 + k * 4, S = L32(c, a);
        const uint32_t first = S >> 24, ppb = (S >> 16) & 0xFF, nb = (S >> 8) & 0x7F;
        W32(c, a, S & ~0x8000u);
        pgmask m; pg_range(&m, first, nb * ppb);
        pg_clr(c, LS_PG_RES, &m);
    }
    return 1;
}

/* 0x21F8 kReqDepDec */
static int cmd_depdec(wws_worker* w, stage_state* st, uint32_t F, uint32_t D)
{
    spu_context* c = w->ctx;
    (void)F;
    const uint32_t r = LS_REC + st->L * 32, n = L32(c, r + 12);
    if (n >= 4) { st->why = "ReqDepDec: more than 4"; return 0; }
    W32(c, r + 16 + n * 4, D);
    W32(c, r + 12, n + 1);
    return 1;
}

/* 0x2244 kRunJob (its own set/buffer fields name the code buffer) */
static int cmd_run(wws_worker* w, stage_state* st, uint32_t F, uint32_t next_ptr)
{
    spu_context* c = w->ctx;
    const uint32_t set = (F >> 5) & 15, lb = F & 31;
    const uint32_t sa = LS_SETS + st->L * 64 + set * 4, S = L32(c, sa);
    const uint32_t first = S >> 24, ppb = (S >> 16) & 0xFF, fbi = S & 0xFF;
    const uint32_t p = c->ls[LS_MAP + fbi + lb];
    if (p == 0xFF) { st->why = "RunJob: code buffer not in use"; return 0; }
    st->code_lsa = (first + ppb * p) << 10;
    st->params = next_ptr + (next_ptr & 8);
    st->set_ptr = sa;
    return 1;
}

static int stage_commands(wws_worker* w, stage_state* st, uint32_t len)
{
    spu_context* c = w->ctx;
    for (uint32_t ptr = LS_CMDS; ptr + 8 <= LS_CMDS + len; ptr += 8) {
        const uint32_t F = L32(c, ptr), D = L32(c, ptr + 4);
        if (!F) continue;
        const uint32_t cmd = (F >> 9) & 7;
        int ok = 1;
        switch (cmd) {
        case 1: ok = cmd_reserve(w, st, F, D); break;
        case 2: ok = cmd_use(w, st, F, D); break;
        case 3: ok = cmd_unreserve(w, st, F, D); break;
        case 4: ok = cmd_depdec(w, st, F, D); break;
        case 5:
            ok = cmd_run(w, st, F, ptr + 8);
            if (ok) { st->done = 1; return 1; }        /* RunJob stays in place, the parameters follow it */
            break;
        default: st->why = "command list ends without RunJob"; return 0;
        }
        if (!ok) return 0;
        W32(c, ptr, 0); W32(c, ptr + 4, 0);           /* an executed command is zeroed */
    }
    st->why = "no RunJob in the command list";
    return 0;
}

/* ---- PS3_NATIVE_JOBS_VERIFY: the policy module's own command executor as the reference ------------ */
static void pm_exec_driver(spu_context* c)
{
    void (*fn)(spu_context*) = spu_find_function(WWS_PM_IMAGE, 0x1AE8);
    c->gpr[0] = U(0x3FF00u);
    c->pc = 0x1AE8;
    c->host_depth++;
    fn(c);
    spu_drain_call(c, 0x3FF00u);
    c->host_depth--;
    c->tramp_fn = 0;
}
static void verify_stage(wws_worker* w, const stage_state* st, const wws_job* j)
{
    spu_context* v = w->vctx;
    spu_context* c = w->ctx;
    if (!spu_find_function(WWS_PM_IMAGE, 0x1AE8)) { s_verify = 0; return; }
    v->gpr[3] = U(st->prev); v->gpr[4] = U(st->L); v->gpr[5] = U(LS_CMDS);
    v->gpr[1] = U(LS_STACK_TOP);
    v->image_id = WWS_PM_IMAGE; v->policy_mode = 1; v->event_mask = 0; v->int_enable = 0;
    const int halted = spu_run_with_halt(pm_exec_driver, v);
    /* compare what the job can see: its pages, the buffer tables, the job context words */
    static const struct { uint32_t lo, hi; const char* what; } R[] = {
        { 0x4000, LS_CANARY, "job pages" }, { LS_SETS, LS_SETS + 64, "buffer sets" },
        { LS_BUFS, LS_BUFS + 256, "buffers" }, { LS_MAP, LS_MAP + 32, "buffer map" },
        { LS_PG_USED, LS_PG_USED + 32, "used pages" }, { LS_PG_SHARE, LS_PG_SHARE + 32, "shareable pages" },
        { LS_PG_RES, LS_PG_RES + 32, "reserved pages" }, { LS_REC, LS_REC + 32, "record" },
        { LS_CODE, LS_CODE + 4, "code address" }, { LS_PARAMS, LS_PARAMS + 4, "parameter pointer" },
    };
    const char* bad = halted || v->gpr[3]._u32[0] != 1 ? "the policy module did not finish the commands" : NULL;
    uint32_t at = 0;
    for (size_t i = 0; i < sizeof R / sizeof R[0] && !bad; i++)
        for (uint32_t a = R[i].lo; a < R[i].hi; a++) {
            if (a >= 0x4000 && w->cache[0].valid && (a >> 10) >= w->cache[0].first_page &&
                (a >> 10) < w->cache[0].first_page + w->cache[0].ppb) continue;   /* a reused code buffer */
            if (c->ls[a] != v->ls[a]) { bad = R[i].what; at = a; break; }
        }
    __atomic_add_fetch(&s_verified, 1, __ATOMIC_RELAXED);
    if (bad) {
        const uint64_t n = __atomic_add_fetch(&s_verify_bad, 1, __ATOMIC_RELAXED);
        if (n <= 20)
            fprintf(stderr, "[wws] VERIFY: job %08X:%u #%u desc %08X: %s differ at LS 0x%05X (native %02X, policy module %02X)\n",
                    j->ctrl_ea, j->slot, j->index, j->desc_ea, bad, at, c->ls[at], v->ls[at]);
    }
}

/* ---- run one job --------------------------------------------------------------------------- */
static void job_driver(spu_context* c)
{
    const uint32_t entry = c->gpr[5]._u32[0];
    c->gpr[0] = U(WWS_JOB_RET_LS);
    c->int_enable = 1;
    c->pc = entry;
    c->host_depth++;
    spu_indirect_branch(c);
    spu_drain_call(c, WWS_JOB_RET_LS);
    c->host_depth--;
    c->tramp_fn = 0;
}

void wws_native_run(wws_worker* w, const wws_job* j, wws_result* out)
{
    spu_context* c = w->ctx;
    memset(out, 0, sizeof *out);
    if (!w->pm_loaded) pm_template(w);
    const uint32_t L = 0, prev = 2;
    stage_state st; memset(&st, 0, sizeof st); st.L = L; st.prev = prev;
    w->job_count++;
    /* the kernel words and the claim (0x2FB8): record, fresh rows for the load slot */
    WQ(c, 0x1C0, 0, j->spurs_ea, w->spu_num, 0x1F);
    WQ(c, 0x1D0, 0, WWS_PM_EA, 0, j->wid);
    WQ(c, LS_JOBCTX, w->spu_num, 0x1F, 0, j->spurs_ea);
    WQ(c, LS_WID, j->wid, 0, WWS_PM_EA, 0);
    WQ(c, LS_CTRL, j->ctrl_ea, 0, WWS_PM_EA, 0);
    WQ(c, LS_CLAIM_E, j->entry_hi, j->desc_ea, j->pair_hi, j->pair_lo);
    WQ(c, LS_CLAIM_C, j->index, 0, 0, 0);
    WQ(c, LS_CLAIM_S, j->slot, 0, 0, 0);
    WSPLAT(c, LS_LOADSLOT, L);
    WSPLAT(c, LS_CLAIMSLOT, (L + 1) % 3);
    WSPLAT(c, LS_NEXTBUF, L * 32);
    WSPLAT(c, LS_FREEBUFS, 32);
    WSPLAT(c, LS_JOBCNT, w->job_count);
    WQ(c, LS_REC + L * 32, j->ctrl_ea | j->slot, 0, j->index, 0);
    WQ(c, LS_REC + L * 32 + 16, 0, 0, 0, 0);
    memset(c->ls + LS_SETS + L * 64, 0, 64);
    memset(c->ls + LS_BUFS + L * 256, 0, 256);
    memset(c->ls + LS_MAP + L * 32, 0xFF, 32);
    memset(c->ls + LS_SETS + prev * 64, 0, 64);        /* no previous job to share buffers with */
    memset(c->ls + LS_BUFS + prev * 256, 0, 256);
    memset(c->ls + LS_MAP + prev * 32, 0xFF, 32);
    /* page masks: the manager's own pages (0..15 and the stack 248..255) */
    {
        pgmask base; memset(&base, 0, sizeof base);
        base.b[0] = base.b[1] = 0xFF; base.b[31] = 0xFF;
        memcpy(c->ls + LS_PG_USED, base.b, 32);
        memset(c->ls + LS_PG_SHARE, 0, 32);
        memcpy(c->ls + LS_PG_RES, base.b, 32);
    }
    WQ(c, LS_LOADSTATE, j->entry_hi, j->desc_ea, j->pair_hi, j->pair_lo);   /* kReadCommands */
    /* the command list */
    const uint32_t len = (j->entry_hi >> 16) & 511;
    if (!len || len > 0x400 || (j->desc_ea & 15)) { out->why = "bad entry"; return; }
    dma(c, LS_CMDS, j->desc_ea, len, 0, 0x40);
    WQ(c, LS_LOADSTATE, 2, 0, 0, 0);                   /* kExecuteCommands */
    if (s_verify < 0) { const char* e = getenv("PS3_NATIVE_JOBS_VERIFY"); s_verify = e && *e == '1'; }
    if (s_verify) {
        if (!w->vctx) {
            w->vctx = (spu_context*)_aligned_malloc(sizeof(spu_context), 64);
            spu_context_init(w->vctx, c->spu_id + 0x100);
        }
        memcpy(w->vctx->ls, c->ls, SPU_LS_SIZE);
    }
    if (!stage_commands(w, &st, len)) { out->why = st.why; return; }
    WSPLAT(c, LS_SETPTR, st.set_ptr);
    WQ(c, LS_CODE, st.code_lsa, 0, 0, 0);
    WSPLAT(c, LS_PARAMS, st.params);
    if (s_verify) verify_stage(w, &st, j);
    for (int k = 0; k < 4; k++) W32(c, LS_R4 + 4 * k, L32(c, LS_CLAIM_E + 4 * k) >> 28);
    dma_wait_all(c);
    /* ChangeLoadToRunJob (0x3148) */
    WSPLAT(c, LS_JOBSTATE, 1);
    WSPLAT(c, LS_LOADSLOT, 0xFFFFFFFFu);
    WSPLAT(c, LS_RUNSLOT, L);
    WQ(c, LS_LOADSTATE, 0, 0xFFFFFFFFu, 0, 0);
    memcpy(c->ls + LS_AUDIT, c->ls + 0xDC0, 16);
    memcpy(c->ls + LS_RECCOPY, c->ls + LS_REC + L * 32, 16);
    {
        const uint32_t r = LS_REC + L * 32;
        WQ(c, LS_JOBCTX1, (j->wid << 16) | L32(c, r + 8), L32(c, r + 12),
           0x5B00008Eu | L32(c, r), L32(c, r + 4));
    }
    const uint32_t entry = st.code_lsa + L32(c, st.code_lsa + 0x10);
    {
        const int ovl = spu_overlay_match_sig(c->ls + st.code_lsa);
        if (ovl >= 200 && ovl != c->resident_ovl) { c->resident_ovl = ovl; c->ib_gen++; }
        else if (ovl < 200) c->resident_ovl = 0;
    }
    out->module = c->resident_ovl;
    if (c->resident_ovl < 200) { out->why = "the job code is no known job module"; return; }
    /* the registers the job reads (and the ones the module leaves there for it) */
    c->gpr[1]._u32[0] = LS_STACK_TOP; c->gpr[1]._u32[1] = 0x1980; c->gpr[1]._u32[2] = 0; c->gpr[1]._u32[3] = 0;
    c->gpr[2]._u32[0] = 0x00010405u; c->gpr[2]._u32[1] = 0x08090C0Du; c->gpr[2]._u32[2] = 0x00010405u; c->gpr[2]._u32[3] = 0x08090C0Du;
    for (int k = 0; k < 4; k++) c->gpr[3]._u32[k] = LS_JOBCTX;
    for (int k = 0; k < 4; k++) c->gpr[4]._u32[k] = L32(c, LS_R4 + 4 * k);
    c->gpr[5] = U(entry);
    c->stop_code = 0;
    c->status = SPU_STATUS_RUNNING;
    const int halted = spu_run_with_halt(job_driver, c);
    if (halted || (c->pc & SPU_LS_MASK) != WWS_JOB_RET_LS) { out->why = "the job did not return"; return; }
    dma_wait_all(c);                                   /* the job's own PUTs */
    /* the job is done (0x3230): unreserve what is still reserved, retire its buffers (0x32E8) */
    WSPLAT(c, LS_RUNSLOT, 0xFFFFFFFFu);
    WSPLAT(c, LS_PREVSLOT, L);
    WSPLAT(c, LS_JOBSTATE, 0);
    for (uint32_t k = 0; k < 16; k++) {
        const uint32_t a = LS_SETS + L * 64 + k * 4, S = L32(c, a);
        if (!(S & 0x8000u)) continue;
        W32(c, a, S & ~0x8000u);
        pgmask m; pg_range(&m, S >> 24, ((S >> 8) & 0x7F) * ((S >> 16) & 0xFF));
        pg_clr(c, LS_PG_RES, &m);
    }
    for (uint32_t i = L * 32; i < L * 32 + 32; i++) {
        const uint32_t e0 = L32(c, LS_BUFS + i * 8);
        const uint32_t tag = (e0 >> 22) & 31;
        if ((e0 & 0x80000000u) || !(e0 & 0x08000000u) || !tag) continue;
        for (int k = 0; k < 4; k++) W32(c, LS_TAGS + 4 * k, L32(c, LS_TAGS + 4 * k) & ~SHL(1, tag));
    }
    WSPLAT(c, LS_STORESLOT, 0xFFFFFFFFu);
    {
        const uint32_t r = LS_REC + L * 32, n = L32(c, r + 12);
        out->n_deps = n > 4 ? 4 : n;
        for (uint32_t k = 0; k < out->n_deps; k++) out->dep_ea[k] = L32(c, r + 16 + 4 * k);
    }
    __atomic_add_fetch(&s_jobs, 1, __ATOMIC_RELAXED);
    out->ok = 1;
}

/* the local-store address of set `set`'s buffer after the last run (load slot 0, the only one the runner uses) */
uint32_t wws_worker_buffer_ls(wws_worker* w, uint32_t set)
{
    if (!w || set >= 16) return 0;
    spu_context* c = w->ctx;
    const uint32_t S = L32(c, LS_SETS + set * 4);
    if (!S) return 0;
    const uint32_t first = S >> 24, ppb = (S >> 16) & 0xFF, fbi = S & 0xFF;
    const uint32_t p = c->ls[LS_MAP + fbi];
    if (p == 0xFF) return 0;
    return ((first + ppb * p) & 0xFF) << 10;
}

/* ---- guest lock-line updates (as a successful PUTLLC) ---------------------------------------- */
void wws_line_update(uint32_t ea, void (*fn)(uint8_t* line, void* arg), void* arg)
{
    const uint32_t line = ea & ~127u;
    const uint64_t m = spu_lockline_mask_ea(line);
    spu_lockline_lock_mask(m);
    fn(vm_base + line, arg);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    spu_coh_notify_write(line);
    spu_lockline_unlock_mask(m);
    if (g_spu_line_commit_hook) g_spu_line_commit_hook(line);
}
typedef struct { uint32_t off, old; } cell_arg;
static void cell_dec_fn(uint8_t* line, void* a)
{
    cell_arg* ca = (cell_arg*)a;
    uint8_t* p = line + ca->off;
    const uint32_t v = ((uint32_t)p[0] << 8) | p[1];
    ca->old = v;
    const uint32_t n = (v - 1) & 0xFFFF;
    p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n;
}
uint32_t wws_cell_dec(uint32_t cell)
{
    cell_arg a = { cell & 127u, 0 };
    wws_line_update(cell, cell_dec_fn, &a);
    return a.old;
}

/* ---- the job-manager API the job calls (LS 0x1360.. table), in C on the same tables ---------- */
void spu_jobrec_api_note(spu_context* ctx, uint32_t api_pc, int exit);   /* spu_jobrec.c: PS3_JOBREPLAY_API=native */
#define API_IN(c, pc)  do { if (__builtin_expect(g_spu_jobrec_mode != 0, 0)) spu_jobrec_api_note((c), (pc), 0); } while (0)
#define API_OUT(c, pc) do { if (__builtin_expect(g_spu_jobrec_mode != 0, 0)) spu_jobrec_api_note((c), (pc), 1); } while (0)
static inline void api_ret(spu_context* c)
{
    c->pc = c->gpr[0]._u32[0];
    if (c->host_depth == 0) c->tramp_fn = spu_indirect_branch;
}

/* 0x2860: the highest free tag of each lane of the tag mask (lane 0 is the one used) */
static uint32_t tag_alloc(spu_context* c, uint32_t lanes[4])
{
    for (int k = 0; k < 4; k++) {            /* t = 31 - clz(~m); m |= 1 << t (SPU shl: a full lane stays) */
        const uint32_t m = L32(c, LS_TAGS + 4 * k);
        const uint32_t t = 31u - (uint32_t)(~m ? __builtin_clz(~m) : 32);
        lanes[k] = t;
        W32(c, LS_TAGS + 4 * k, m | SHL(1, t));
    }
    return lanes[0];
}

/* +0x34 (0x2EB0) GetBufferInfo(r4 set, r5 buffer, r6 allocate a tag): r3 = {set<<24|buf<<16|tag,
 * page<<24|pagesPerBuf<<8, EA, size} */
static void get_buffer_info(spu_context* c, uint32_t set, uint32_t buf, uint32_t f, uint32_t r[4])
{
    const uint32_t slot = L32(c, LS_RUNSLOT);
    const uint32_t S = L32(c, (LS_SETS + slot * 64 + set * 4) & SPU_LS_MASK);
    const uint32_t first = S >> 24, ppb = (S >> 16) & 0xFF, fbi = S & 0xFF;
    const uint32_t p = c->ls[(LS_MAP + fbi + buf) & SPU_LS_MASK];
    const uint32_t page = ((ppb & 0xFFFF) * (p & 0xFFFF) + first) & 0xFF;   /* mpya */
    uint32_t ent = (LS_BUFS + (fbi + p) * 8) & SPU_LS_MASK;
    uint32_t e0 = L32(c, ent), e1 = L32(c, ent + 4);
    if (e0 & 0x10000000u) {                            /* handed to the job being loaded: its row */
        const uint32_t ls2 = L32(c, LS_LOADSLOT);
        const uint32_t S2 = L32(c, LS_SETS + ls2 * 64 + set * 4);
        ent = LS_BUFS + ((S2 & 0xFF) + p) * 8;
        e0 = L32(c, ent); e1 = L32(c, ent + 4);
    }
    uint32_t tag = (e0 >> 22) & 31;
    if (f && !tag) {
        uint32_t lanes[4];
        tag = tag_alloc(c, lanes);
        e0 |= (tag & 31) << 22;
        W32(c, ent, e0);
    }
    r[0] = ((set & 0xFF) << 24) | ((buf & 0xFF) << 16) | (tag & 0xFFFF);
    r[1] = (page << 24) | ((ppb & 0xFF) << 8);
    r[2] = e1 & ~15u;
    r[3] = ((e0 >> 8) & 0x3FFF) << 4;
}
static void api_get_buffer(spu_context* c)
{
    uint32_t r[4];
    API_IN(c, 0x2EB0);
    get_buffer_info(c, c->gpr[4]._u32[0], c->gpr[5]._u32[0], c->gpr[6]._u32[0], r);
    for (int k = 0; k < 4; k++) c->gpr[3]._u32[k] = r[k];
    API_OUT(c, 0x2EB0);
    api_ret(c);
}

/* +0x38 (0x16E0) GetBufferInfos(r3 -> n quads {set, buf, allocate}, r4 n): each quad replaced by the answer */
static void api_get_buffers(spu_context* c)
{
    API_IN(c, 0x16E0);
    uint32_t at = c->gpr[3]._u32[0], n = c->gpr[4]._u32[0];
    do {
        uint32_t r[4];
        get_buffer_info(c, L32(c, at), L32(c, at + 4), L32(c, at + 8), r);
        WQ(c, at, r[0], r[1], r[2], r[3]);
        for (int k = 0; k < 4; k++) c->gpr[3]._u32[k] = r[k];
        at += 16;
    } while (--n);
    API_OUT(c, 0x16E0);
    api_ret(c);
}

/* +0x3C (0x1738) AllocTag: r3 = tag */
static void api_alloc_tag(spu_context* c)
{
    uint32_t lanes[4];
    API_IN(c, 0x1738);
    tag_alloc(c, lanes);
    for (int k = 0; k < 4; k++) c->gpr[3]._u32[k] = lanes[k];
    API_OUT(c, 0x1738);
    api_ret(c);
}

/* +0x40 (0x1740) FreeTag(r3) */
static void api_free_tag(spu_context* c)
{
    API_IN(c, 0x1740);
    for (int k = 0; k < 4; k++)
        W32(c, LS_TAGS + 4 * k, L32(c, LS_TAGS + 4 * k) & ~SHL(1, c->gpr[3]._u32[k]));
    API_OUT(c, 0x1740);
    api_ret(c);
}

/* +0x44 (0x1760) LoadNextJob: the job is done with its inputs. One job per worker: nothing to load;
 * the record's end-of-buffers index is set as the module sets it; r3 = the load state (none). */
static void api_load_next_job(spu_context* c)
{
    API_IN(c, 0x1760);
    WSPLAT(c, LS_JOBSTATE, 2);
    const uint32_t r = LS_REC + L32(c, LS_RUNSLOT) * 32;
    W16(c, r + 4, L32(c, LS_NEXTBUF) & 0xFFFF);
    for (int k = 0; k < 4; k++) c->gpr[3]._u32[k] = L32(c, LS_LOADSTATE + 4 * k);
    API_OUT(c, 0x1760);
    api_ret(c);
}

/* +0x30 (0x1670) ExecuteCommands, +0x48/+0x4C (0x1A58 / 0x1A78) FreeLogicalBuffer(s), +0x50 audit:
 * no job of this game calls them (work/native/TRACK_B1.md). */
static void api_unused(spu_context* c)
{
    fprintf(stderr, "[wws] FATAL: job module %d called the job-manager API at LS 0x%05X, which the native runner "
            "does not implement (set PS3_NATIVE_JOBS=0)\n", c->resident_ovl, c->pc & SPU_LS_MASK);
    fflush(stderr);
    abort();
}

void wws_native_api_register(void)
{
    extern void wws_runner_link(void);
    wws_runner_link();                 /* keeps libs/spurs/wws_runner.c (its constructor) in the link */
    static volatile long once = 0;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) return;
    spu_begin_image(WWS_NATIVE_IMAGE);
    spu_register_function(0x1670, api_unused);
    spu_register_function(0x2EB0, api_get_buffer);
    spu_register_function(0x16E0, api_get_buffers);
    spu_register_function(0x1738, api_alloc_tag);
    spu_register_function(0x1740, api_free_tag);
    spu_register_function(0x1760, api_load_next_job);
    spu_register_function(0x1A58, api_unused);
    spu_register_function(0x1A78, api_unused);
    spu_register_function(0x19F8, api_unused);
    spu_begin_image(0);
}

