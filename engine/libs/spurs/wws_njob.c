/* wws_njob.c -- native job modules on the runner (Track B2). See wws_njob.h.
 *
 * The registry (the modules in native_jobs/), the descriptor decode, the game's services (guest memory with the
 * MFC's PUT and atomic semantics), the dispatch from wws_runner.c's run_one, the verify mode and the stats. */
#include "wws_njob.h"
#include "../../runtime/spu/spu_context.h"
#include "../../runtime/spu/spu_coherency.h"
#include "../../runtime/spu/spu_dma.h"
#include "../../runtime/platform/win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* vm_base;
extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);

/* ---- the registry ------------------------------------------------------------------------------ */
extern const wws_nmod wws_nmod_210, wws_nmod_208, wws_nmod_209, wws_nmod_212;
static const wws_nmod* const s_mods[] = { &wws_nmod_210, &wws_nmod_208, &wws_nmod_209, &wws_nmod_212 };

const wws_nmod* wws_nmod_find(int module)
{
    for (size_t i = 0; i < sizeof s_mods / sizeof s_mods[0]; i++)
        if (s_mods[i]->module == module && s_mods[i]->run) return s_mods[i];
    return NULL;
}

static int s_mode = -1, s_stats = 1;
static unsigned char s_on[256];
static void cfg(void)
{
    const char* e = getenv("PS3_NATIVE_CULL");
    int mode = 1;
    memset(s_on, 1, sizeof s_on);
    if (e && *e) {
        if (!strcmp(e, "0")) mode = 0;
        else if (!strcmp(e, "1")) mode = 1;
        else if (!strcmp(e, "2")) mode = 2;
        else {                                         /* a list of modules; "2:" in front = verify them */
            const char* p = e;
            if (!strncmp(p, "2:", 2)) { mode = 2; p += 2; }
            memset(s_on, 0, sizeof s_on);
            while (*p) {
                char* q; const long m = strtol(p, &q, 10);
                if (q == p) { p++; continue; }
                if (m >= 200 && m < 256) s_on[m] = 1;
                p = q;
            }
        }
    }
    const char* st = getenv("PS3_NATIVE_CULL_STATS");
    s_stats = !(st && *st == '0');
    __atomic_store_n(&s_mode, mode, __ATOMIC_RELEASE);
    char list[200] = ""; int n = 0;
    for (size_t i = 0; i < sizeof s_mods / sizeof s_mods[0]; i++)
        if (s_mods[i]->run && s_on[s_mods[i]->module])
            n += snprintf(list + n, sizeof list - (size_t)n, " %d", s_mods[i]->module);
    fprintf(stderr, "[ncull] native job modules (PS3_NATIVE_CULL=%s): %s%s\n", e && *e ? e : "1 (default)",
            mode == 0 ? "off" : mode == 2 ? "VERIFY against the lifted jobs:" : "on:", mode ? list : "");
}
int wws_nmod_mode(void)
{
    int m = __atomic_load_n(&s_mode, __ATOMIC_ACQUIRE);
    if (m < 0) { cfg(); m = s_mode; }
    return m;
}
int wws_nmod_enabled(int module) { return wws_nmod_mode() && module >= 0 && module < 256 && s_on[module]; }

/* ---- the descriptor ---------------------------------------------------------------------------- */
static inline uint32_t G32(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }

int wws_njob_decode(wws_njob* nj, const wws_job* j)
{
    const uint32_t len = (j->entry_hi >> 16) & 511;
    if (!len || len > 0x400 || (j->desc_ea & 15) || !mfc_ea_range_committed(j->desc_ea, len)) return 0;
    memset(nj->buf, 0, sizeof nj->buf);
    nj->params = NULL; nj->params_len = 0; nj->n_deps = 0;
    for (uint32_t o = 0; o + 8 <= len; o += 8) {
        const uint32_t F = G32(j->desc_ea + o), D = G32(j->desc_ea + o + 4);
        if (!F) continue;
        switch ((F >> 9) & 7) {
        case 1: {                                      /* ReserveBufSet: the layout the job assumes */
            const uint32_t set = (F >> 5) & 15;
            nj->buf[set].pages = (uint8_t)(D >> 16); nj->buf[set].nbufs = (uint8_t)(D >> 8);
            break;
        }
        case 3: break;                                 /* UnreserveBufSets: local store only */
        case 2: {                                      /* UseBuffer */
            const uint32_t set = (F >> 5) & 15, lb = F & 31;
            if (lb || (F & 0x10000u) || nj->buf[set].used || !nj->buf[set].pages) return 0;   /* a second buffer of a set: not taken */
            wws_nbuf* b = &nj->buf[set];
            b->used = 1; b->load = (F & 0x8000u) != 0; b->list = (F & 0x4000u) != 0; b->share = (F & 0x2000u) != 0;
            b->ea = b->load ? D & ~15u : 0; b->size = b->load ? ((F >> 18) & 0x3FFF) << 4 : 0;
            break;
        }
        case 4:                                        /* ReqDepDec */
            if (nj->n_deps >= 4) return 0;
            nj->dep_ea[nj->n_deps++] = D;
            break;
        case 5: {                                      /* RunJob: the parameters follow, 16-aligned */
            const uint32_t p = o + 8 + ((o + 8) & 8);
            if (p < len) { nj->params = vm_base + j->desc_ea + p; nj->params_len = len - p; }
            return 1;
        }
        default: return 0;
        }
    }
    return 0;
}

/* ---- the game's services ----------------------------------------------------------------------- */
static const uint8_t* g_read(wws_njob* nj, uint32_t ea, uint32_t size)
{
    (void)nj;
    if (!size || ea < 0x10000u || (uint64_t)ea + size > 0x100000000ull) return NULL;
    if (!mfc_ea_range_committed(ea, size)) return NULL;
    return vm_base + ea;
}
/* mfc_do_transfer_lean's PUT: its rules (a malformed transfer is not performed), the coherence protocol
 * (lines an SPU holds a reservation on are written under the line lock and their reservations broken),
 * the PUT hook (the RSX side's watch) */
static void g_put(wws_njob* nj, uint32_t ea, const uint8_t* src, uint32_t size, const uint8_t* dc)
{
    (void)nj; (void)dc;
    if (!size || size > 0x4000 || (size >= 16 && (size & 15)) || (ea & 15) || ea < 0x10000u) return;   /* the jobs PUT from 16-aligned LS */
    if (!mfc_ea_range_committed(ea, size)) return;
    uint8_t* g = vm_base + ea;
    const uint32_t a0 = ea & ~127u, a1 = (ea + size - 1u) & ~127u;
    int span_reserved = 0;
    for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) { span_reserved = 1; break; } if (a == a1) break; }
    if (span_reserved) {
        const uint64_t llm = spu_lockline_mask_range(ea, size);
        spu_lockline_lock_mask(llm);
        spu_put_copy(ea, g, src, size);
        for (uint32_t a = a0; ; a += 128u) { if (spu_coh_is_reserved(a)) spu_coh_notify_write_except(a, NULL); if (a == a1) break; }
        spu_lockline_unlock_mask(llm);
    } else {
        spu_put_copy(ea, g, src, size);
        for (uint32_t a = a0; ; a += 128u) {
            if (spu_coh_is_reserved(a)) {
                const uint64_t llm = spu_lockline_mask_ea(a);
                spu_lockline_lock_mask(llm);
                spu_coh_notify_write_except(a, NULL);
                spu_lockline_unlock_mask(llm);
            }
            if (a == a1) break;
        }
    }
    if (g_spu_put_hook) g_spu_put_hook(ea, size);
}
typedef struct { uint32_t off, v, old; } add_arg;
static void add_fn(uint8_t* line, void* a)
{
    add_arg* x = (add_arg*)a;
    uint32_t be; memcpy(&be, line + x->off, 4);
    x->old = __builtin_bswap32(be);
    be = __builtin_bswap32(x->old + x->v); memcpy(line + x->off, &be, 4);
}
/* the jobs' GETLLAR / add / PUTLLC loop as one lock-line update */
static uint32_t g_atomic_add(wws_njob* nj, uint32_t ea, uint32_t v)
{
    (void)nj;
    add_arg a = { ea & 127u & ~3u, v, 0 };
    wws_line_update(ea, add_fn, &a);
    return a.old;
}

/* verify mode: nothing is written; atomic adds see the current word plus what this job added before */
typedef struct cap_svc { uint32_t line[4], add[4]; int n; uint32_t nput; } cap_svc;
static void c_put(wws_njob* nj, uint32_t ea, const uint8_t* src, uint32_t size, const uint8_t* dc)
{
    (void)ea; (void)src; (void)size; (void)dc;
    ((cap_svc*)nj->svc)->nput++;
}
static uint32_t c_atomic_add(wws_njob* nj, uint32_t ea, uint32_t v)
{
    cap_svc* c = (cap_svc*)nj->svc;
    const uint32_t w = ea & ~3u;
    int k = 0;
    while (k < c->n && c->line[k] != w) k++;
    if (k == c->n && c->n < 4) { c->line[k] = w; c->add[k] = 0; c->n++; }
    const uint32_t cur = mfc_ea_range_committed(w, 4) ? G32(w) : 0;
    const uint32_t old = cur + (k < 4 ? c->add[k] : 0);
    if (k < 4) c->add[k] += v;
    return old;
}

/* ---- stats ------------------------------------------------------------------------------------- */
typedef struct mstat {
    volatile uint64_t jobs, ns, fb, why[32], cmp, bad, nat_ns, lift_ns, bad_code[8];
    volatile long warned;     /* first occurrence of each reason (bits) */
} mstat;
static mstat s_ms[256];
static volatile uint64_t s_next_print;
static uint64_t qpc_ns(void)
{
    static double s_scale;
    LARGE_INTEGER c;
    if (!s_scale) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_scale = 1e9 / (double)f.QuadPart; }
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * s_scale);
}
static const char* reason_name(const wws_nmod* m, int b)
{
    if (m && m->reasons) for (int q = 0; m->reasons[q]; q++) if (q == b) return m->reasons[q];
    return "?";
}
void wws_njob_stats_tick(void)
{
    if (!s_stats) return;
    const uint64_t now = qpc_ns();
    uint64_t nx = __atomic_load_n(&s_next_print, __ATOMIC_RELAXED);
    if (now < nx) return;
    if (!__atomic_compare_exchange_n(&s_next_print, &nx, now + 10000000000ull, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    if (!nx) return;                                   /* the first call only starts the clock */
    static uint64_t lj[256], lns[256], lfb[256], lcmp[256], lbad[256], lnat[256], llift[256];
    for (size_t i = 0; i < sizeof s_mods / sizeof s_mods[0]; i++) {
        const wws_nmod* m = s_mods[i];
        mstat* s = &s_ms[m->module];
        const uint64_t j = s->jobs, ns = s->ns, fb = s->fb, cmp = s->cmp, bad = s->bad, nat = s->nat_ns, lift = s->lift_ns;
        const int o = m->module;
        if (j == lj[o] && fb == lfb[o] && cmp == lcmp[o]) continue;
        char why[300] = ""; int w = 0;
        for (int b = 0; b < 32 && w < (int)sizeof why - 30; b++)
            if (s->why[b]) w += snprintf(why + w, sizeof why - (size_t)w, " %s:%llu", reason_name(m, b), (unsigned long long)s->why[b]);
        fprintf(stderr, "[ncull] %d %s: %llu jobs/s native, %.2f us/job; %llu/s declined (total by reason:%s)",
                o, m->name, (unsigned long long)((j - lj[o]) / 10), j > lj[o] ? (double)(ns - lns[o]) / 1e3 / (double)(j - lj[o]) : 0.0,
                (unsigned long long)((fb - lfb[o]) / 10), w ? why : " none");
        if (cmp != lcmp[o])
            fprintf(stderr, "; VERIFY %llu/s compared, %llu differ (total %llu of %llu), native %.2f us vs lifted %.2f us",
                    (unsigned long long)((cmp - lcmp[o]) / 10), (unsigned long long)(bad - lbad[o]),
                    (unsigned long long)bad, (unsigned long long)cmp,
                    (double)(nat - lnat[o]) / 1e3 / (double)(cmp - lcmp[o]), (double)(lift - llift[o]) / 1e3 / (double)(cmp - lcmp[o]));
        fprintf(stderr, "\n");
        lj[o] = j; lns[o] = ns; lfb[o] = fb; lcmp[o] = cmp; lbad[o] = bad; lnat[o] = nat; llift[o] = lift;
    }
}

/* ---- dispatch ---------------------------------------------------------------------------------- */
static SPU_THREAD_LOCAL wws_njob* t_nj;

int wws_njob_try(wws_worker* w, const wws_job* j, int module, wws_result* out)
{
    if (!wws_nmod_enabled(module)) return 0;
    const wws_nmod* m = wws_nmod_find(module);
    if (!m) return 0;
    mstat* st = &s_ms[module & 255];
    if (!t_nj) { t_nj = (wws_njob*)calloc(1, sizeof *t_nj); if (!t_nj) return 0; }
    wws_njob* nj = t_nj;
    memset(nj, 0, sizeof *nj);
    nj->module = module; nj->job = j;
    if (!wws_njob_decode(nj, j)) {
        __atomic_add_fetch(&st->fb, 1, __ATOMIC_RELAXED);
        if (!(__atomic_fetch_or(&st->warned, 1L << 31, __ATOMIC_RELAXED) & (1L << 31)))
            fprintf(stderr, "[ncull] WARNING %d %s: a descriptor the natives do not take (desc %08X, entry %08X): the lifted job runs\n",
                    module, m->name, j->desc_ea, j->entry_hi);
        return 0;
    }
    const uint32_t ndep = nj->n_deps; uint32_t dep[4]; memcpy(dep, nj->dep_ea, sizeof dep);
    const int verify = wws_nmod_mode() == 2;
    cap_svc cap; memset(&cap, 0, sizeof cap);
    nj->read = g_read;
    if (verify) { nj->put = c_put; nj->atomic_add = c_atomic_add; nj->svc = &cap; nj->capture = 1; }
    else { nj->put = g_put; nj->atomic_add = g_atomic_add; nj->svc = NULL; }
    const uint64_t t0 = qpc_ns();
    const int ran = m->run(nj);
    const uint64_t t1 = qpc_ns();
    if (ran != 1) {
        __atomic_add_fetch(&st->fb, 1, __ATOMIC_RELAXED);
        for (int b = 0; b < 32; b++) {
            if (!(nj->fallback >> b & 1)) continue;
            __atomic_add_fetch(&st->why[b], 1, __ATOMIC_RELAXED);
            if (!(__atomic_fetch_or(&st->warned, 1L << b, __ATOMIC_RELAXED) & (1L << b)))
                fprintf(stderr, "[ncull] WARNING %d %s: first decline for %s (ea 0x%08X size 0x%X op %u, desc %08X): the lifted job runs\n",
                        module, m->name, reason_name(m, b), nj->bad_ea, nj->bad_size, nj->bad_op, j->desc_ea);
        }
        wws_njob_stats_tick();
        if (!verify) return 0;
    }
    if (verify) {
        /* the lifted job now does the real work; the capture is compared with what it left */
        const uint64_t t2 = qpc_ns();
        wws_native_run(w, j, out);
        const uint64_t t3 = qpc_ns();
        if (ran == 1 && out->ok && m->verify) {
            uint32_t bl[WWS_NJOB_SETS];
            for (uint32_t s = 0; s < WWS_NJOB_SETS; s++) bl[s] = wws_worker_buffer_ls(w, s);
            const int code = m->verify(nj, wws_worker_ctx(w)->ls, bl);
            __atomic_add_fetch(&st->cmp, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&st->nat_ns, t1 - t0, __ATOMIC_RELAXED);
            __atomic_add_fetch(&st->lift_ns, t3 - t2, __ATOMIC_RELAXED);
            if (code) {
                const uint64_t nb = __atomic_add_fetch(&st->bad, 1, __ATOMIC_RELAXED);
                if (nb <= 10)
                    fprintf(stderr, "[ncull] VERIFY %d %s: job differs (code %d; desc %08X, %u puts captured)\n",
                            module, m->name, code, j->desc_ea, cap.nput);
            }
        }
        wws_njob_stats_tick();
        return 2;
    }
    __atomic_add_fetch(&st->jobs, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&st->ns, t1 - t0, __ATOMIC_RELAXED);
    memset(out, 0, sizeof *out);
    out->ok = 1; out->module = module;
    out->n_deps = ndep;
    for (uint32_t d = 0; d < ndep; d++) out->dep_ea[d] = dep[d];
    wws_njob_stats_tick();
    return 1;
}
