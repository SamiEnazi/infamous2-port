/*
 * ps3recomp - (stutter) engine pipelines off the backend's thread, and built
 * before they are needed. Included once by rsx_vulkan_backend.c, right after
 * vk_eng_variant_ex.
 *
 * Why (player report, v0.4.x, 2026-11-12: "it stutters every time the camera
 * switches angle in a cutscene, and whenever something loads"). Every new
 * material on screen is a new engine pipeline: the RSX programs decompiled to
 * HLSL, HLSL -> SPIR-V through glslang (1-4 ms; skipped when the .spv is in the
 * shader cache), and at its first draw a vkCreateGraphicsPipelines -- 0.05 ms
 * when the driver has it cached, 10-50 ms when it does not. All of it ran on
 * the thread that records the draws, so a camera cut that shows 50-180 new
 * materials froze the frame:
 *   - the player's first session (RTX 3060, empty cache): 2,280 pipelines,
 *     vkCreateGraphicsPipelines 24.3 s in total (10.6 ms each), glslang 3.0 s;
 *   - his second session, pipelines.bin (32 MB) loaded: still 969 creations,
 *     1.64 s (87 shaders the cache never had: new places, other settings);
 *   - Sami's own sessions (warm caches): frames of 505 ms (133 pipelines in
 *     353 ms + 37 shaders in 52 ms), 302 ms (7 pipelines, 278 ms: cold ones),
 *     223 ms, 151 ms ... at camera cuts, the hitch catcher's stack in
 *     vk_sc_create_pipeline -> nvgpucomp64.
 * With a frame cap of 30, every one of those over 33 ms is a visible stutter.
 *
 * What this does (each part has its own switch, read once at the first engine
 * pipeline; =0 is the old behaviour):
 *
 *   PS3RECOMP_VK_PIPE_ASYNC=1 (default)  pipeline workers. A variant a draw
 *       needs that is not built yet is built by a worker. A draw that can be
 *       left out for a frame without leaving garbage behind -- it is depth-
 *       tested (not ALWAYS), writes no stencil, and is not a signed-blend or
 *       framebuffer-fetch draw: the scene's objects, shadows, decals,
 *       particles -- waits for it at most PS3RECOMP_VK_PIPE_WAIT_US (default
 *       2000) per frame in all, and is otherwise skipped until the worker is
 *       done (an object appears a frame or two late instead of the frame
 *       freezing). Every other draw (full-screen passes, the HUD, post
 *       effects, stencil marks) waits for its pipeline, built right here as
 *       before. With the driver's cache warm a pipeline takes ~0.05-0.5 ms,
 *       so the budget covers a whole camera cut and nothing is skipped; only
 *       pipelines the driver has never seen are late.
 *       The same workers translate the HLSL of a pipeline whose shaders are
 *       not in the shader cache (PS3RECOMP_VK_PIPE_ASYNC_XLATE=0: glslang on
 *       this thread, as before).
 *       =2: skip any draw whose pipeline is not ready (diagnostics only).
 *   PS3RECOMP_VK_PIPE_PREWARM=1 (default)  every pipeline variant this
 *       backend builds is recorded as a recipe -- the two shader cache keys and
 *       every value vk_eng_variant_build reads -- in <shader cache>/
 *       pipelines.rcp. At the next start (the first engine pipeline, at the
 *       title screen) the workers build them all again, in the order they
 *       were first seen, at below-normal priority; a draw then finds its
 *       variant ready and only takes it. The .spv files they read stay in
 *       memory so the engine's own translations of those shaders do not go to
 *       the disk either. A recipe not used for 32 runs is dropped.
 *       PS3RECOMP_VK_PIPE_RECIPES=0: neither recorded nor read.
 *   PS3RECOMP_VK_PIPE_THREADS=<n>  workers (default: logical CPUs / 4, 1..4).
 *
 * Same pipelines either way: a worker, the prewarm and the draw thread build
 * through the one vk_eng_variant_build from the same values, through the same
 * VkPipelineCache (internally synchronised). The picture of a frame differs
 * only while a skipped draw's pipeline is being built.
 *
 * Threads. The backend's own thread (the engine's, or with PS3RECOMP_VK_BT
 * the recording worker -- never both at once) owns everything it did before;
 * the workers touch a pipeline only through the pa_* fields under s_pa_mx,
 * store module and variant handles with release, and never destroy a
 * pipeline, a render pass or a layout (vkDestroyPipeline drains the recording
 * ring: the backend thread's job). A release waits for the pipeline's job
 * (vk_pa_forget); shutdown stops the workers before anything they use goes.
 *
 * Stats: the "pipelines:" line after every "shader cache:" line -- draws that
 * waited for a pipeline (count, total, worst), draws skipped, worker and
 * prewarm counts, and the session's texture uploads (count, total, worst
 * single, worst frame).
 */

/* ---- platform ---- */
#ifdef _WIN32
static SRWLOCK            s_pa_mx = SRWLOCK_INIT;         /* queue, pipeline job state, commits */
static CONDITION_VARIABLE s_pa_cv = CONDITION_VARIABLE_INIT;
static SRWLOCK            s_pw_mx = SRWLOCK_INIT;         /* recipes, prewarm modules */
static SRWLOCK            s_pw_save_mx = SRWLOCK_INIT;    /* one pipelines.rcp writer */
static void vk_pa_lock(void)   { AcquireSRWLockExclusive(&s_pa_mx); }
static void vk_pa_unlock(void) { ReleaseSRWLockExclusive(&s_pa_mx); }
static void vk_pa_wait(void)   { SleepConditionVariableSRW(&s_pa_cv, &s_pa_mx, INFINITE, 0); }
static void vk_pa_wake(int all) { if (all) WakeAllConditionVariable(&s_pa_cv); else WakeConditionVariable(&s_pa_cv); }
static void vk_pw_lock(void)   { AcquireSRWLockExclusive(&s_pw_mx); }
static void vk_pw_unlock(void) { ReleaseSRWLockExclusive(&s_pw_mx); }
static void vk_pa_yield(void)  { SwitchToThread(); }
extern int ps3_host_cpus(void);   /* (perf60) runtime/platform/host_place.c: the CPUs this process may use (its affinity mask) */
static u32  vk_pa_cpus(void)   { const int n = ps3_host_cpus(); return n > 0 ? (u32)n : 1u; }
#else
static pthread_mutex_t s_pa_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_pa_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_pw_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_pw_save_mx = PTHREAD_MUTEX_INITIALIZER;
static void vk_pa_lock(void)   { pthread_mutex_lock(&s_pa_mx); }
static void vk_pa_unlock(void) { pthread_mutex_unlock(&s_pa_mx); }
static void vk_pa_wait(void)   { pthread_cond_wait(&s_pa_cv, &s_pa_mx); }
static void vk_pa_wake(int all) { if (all) pthread_cond_broadcast(&s_pa_cv); else pthread_cond_signal(&s_pa_cv); }
static void vk_pw_lock(void)   { pthread_mutex_lock(&s_pw_mx); }
static void vk_pw_unlock(void) { pthread_mutex_unlock(&s_pw_mx); }
static void vk_pa_yield(void)  { sched_yield(); }
static u32  vk_pa_cpus(void)   { const long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (u32)n : 1u; }
#endif

static char* vk_pa_strdup(const char* s)
{
    const size_t n = strlen(s) + 1u;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

#define VK_PA_MAX_THREADS 8u

/* A translation a worker finished: stored into the shader cache by the
 * backend's thread (vk_pa_frame), which owns the cache's memory table. */
typedef struct vk_pa_commit { struct vk_pa_commit* next; vk_sc_key key; u32 n; u32 w[]; } vk_pa_commit;

static struct {
    int  cfg;                       /* switches read                                  */
    int  mode, xlate, prewarm, recipes;
    u32  nthreads;
    u64  wait_ns;                   /* PS3RECOMP_VK_PIPE_WAIT_US                      */
    int  skipw;                     /* PS3RECOMP_VK_PIPE_SKIP_STENCIL                 */
    int  skipb;                     /* PS3RECOMP_VK_PIPE_SKIP_BLEND                   */
    u64  hold_t0;                   /* backend thread: the first pass left out since the last whole frame */
    unsigned long long hold_cls[5]; /* passes left out, by vk_pa_hold_class           */
    u64  hold_ns;                   /* PS3RECOMP_VK_PIPE_HOLD_MS (0 = off)            */
    int  hold_flag;                 /* backend thread: this frame left a pass out     */
    u64  last_shown;                /* backend thread: when the last whole frame went */
    u32  hold_run;                  /* frames held since then                         */
    unsigned long long hold_frames, hold_runs, hold_ns_sum, hold_worst, hold_skips;
    int  started;                   /* vk_pa_start ran                                */
    volatile int quit;
    volatile int nrun;              /* workers alive                                  */
    int  atexit_set;
    u64  spent_ns;                  /* backend thread: this frame's waiting so far    */
    u32  qhead, qtail;              /* the queue: slot + 1, linked through pa_prev/next */
    vk_pa_commit* commits;
    /* stats: backend thread (plain) */
    unsigned long long wait_n, wait_ns_sum, wait_worst, skip_n, inline_n, pw_hit;
    unsigned long long tex_last_ns, tex_frame_worst;
    /* stats: workers (atomic) */
    unsigned long long bg_pipes, bg_pipe_ns, bg_pipe_worst, bg_xl, bg_xl_ns;
    volatile u32 bg_pc_new;         /* worker pipelines the driver had to build (> 1 ms) */
} s_pa;

/* ---- recipes and the prewarm ---- */
#define VK_PW_MAGIC     "PS3RVKRC"
#define VK_PW_VERSION   1u
#define VK_PW_HEADER    48u
#define VK_PW_KEEP_RUNS 32u
typedef struct {
    vk_sc_key vs, fs;               /* the .spv files of the two modules (fs_neg's for neg) */
    rsx_be_render_state rs;
    u32 nslots, stride, rt_count, sblend, fbf_in, topo, vk_idx, neg;
    u32 last_run;                   /* the run that last asked for it                 */
    u64 ctx;                        /* vk_pw_ctx() when it was recorded               */
} vk_pw_rec;                        /* the file's record, as is (its size is in the header) */
/* a recipe the prewarm builds: 0 queued, 1 being built, 2 ready, 3 failed or
 * its shaders are gone, 4 claimed by a draw (built there), 5 taken by a draw */
enum { VK_PW_QUEUED = 0, VK_PW_BUILDING, VK_PW_READY, VK_PW_FAILED, VK_PW_CLAIMED, VK_PW_TAKEN };
typedef struct { u64 h; VkPipeline pipe; volatile int st; u32 rec; } vk_pw_ent;
typedef struct { vk_sc_key k; VkShaderModule m; u32* w; u32 n; volatile int st; } vk_pw_mod;  /* st 0 empty..3 */
typedef struct { u64 h; u32 idx; u32 pad; } vk_pw_slot;     /* idx: rec index + 1, 0 = empty */

static struct {
    u64        ctx;                 /* this run's device + switch context             */
    u32        run;
    vk_pw_rec* rec; u32 nrec, caprec;
    vk_pw_slot* set; u32 setcap, setn;                     /* recipe hash -> rec index + 1 */
    int        dirty;
    u64        last_new;
    /* the prewarm (built at start, never resized) */
    vk_pw_ent* map; u32 mapcap;
    u32*       order; u32 npw;      /* map entries in build order                     */
    u32        next, busy;          /* under s_pa_mx                                  */
    int        done, said;
    vk_pw_mod* mods; u32 modcap;
    u64        t0, t1;
    unsigned long long built, failed, missing, built_ns;
    volatile int saving;
} s_pw;

static u64 vk_pw_mix(u64 h, const void* d, size_t n) { return vk_fnv1a64(d, n, h); }

/* What else decides how a recipe builds: features, switches, formats. */
static u64 vk_pw_ctx(void)
{
    u64 h = VK_SC_SEED_A;
    const u32 v[8] = { VK_PW_VERSION, (u32)s_vk_indep_blend, (u32)vk_mrt_mode(), (u32)s_vk.depth_format,
                       (u32)s_ropt.fbf_in, s_ropt.pl ? 1u : 0u, (u32)s_vk_depth_clamp_ok, (u32)sizeof(rsx_be_render_state) };
    h = vk_pw_mix(h, VK_SC_VERSION, strlen(VK_SC_VERSION));
    return vk_pw_mix(h, v, sizeof v);
}

/* The recipe's identity: everything vk_eng_variant_build reads (the render
 * state without the alpha reference fields and the FP16 flag, which it does
 * not), and the context. */
static u64 vk_pw_hash(const vk_pw_rec* r)
{
    rsx_be_render_state rs = r->rs;               /* all u32: no padding inside */
    rs.alpha_test_enable = rs.alpha_func = rs.alpha_ref_raw = rs.alpha_ref_format = 0;
    rs.rt_fp16 = 0;
    const u32 v[8] = { r->nslots, r->stride, r->rt_count, r->sblend, r->fbf_in, r->topo, r->vk_idx, r->neg };
    u64 h = vk_pw_mix(VK_SC_SEED_B, &r->vs, sizeof r->vs);
    h = vk_pw_mix(h, &r->fs, sizeof r->fs);
    h = vk_pw_mix(h, &rs, sizeof rs);
    h = vk_pw_mix(h, v, sizeof v);
    h = vk_pw_mix(h, &r->ctx, sizeof r->ctx);
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return h ? h : 1u;
}

static int vk_pw_from(const vk_eng_pipe* P, u32 t, u32 k, int neg, vk_pw_rec* r)
{
    const u8 need = (u8)(1u | (neg ? 4u : 2u));
    if ((P->key_ok & need) != need) return 0;
    memset(r, 0, sizeof *r);
    r->vs = P->key[0];
    r->fs = P->key[neg ? 2 : 1];
    r->rs = P->rs;
    r->nslots = P->nslots; r->stride = P->stride; r->rt_count = P->rt_count;
    r->sblend = P->sblend; r->fbf_in = P->fbf_in;
    r->topo = t; r->vk_idx = k; r->neg = neg ? 1u : 0u;
    r->ctx = s_pw.ctx;
    return 1;
}

/* s_pw.set: recipe hash -> index into s_pw.rec + 1. Under s_pw_mx. */
static u32* vk_pw_set_slot(u64 h)
{
    if (!s_pw.setcap) return NULL;
    for (u32 i = (u32)(h ^ (h >> 31)) & (s_pw.setcap - 1u);; i = (i + 1u) & (s_pw.setcap - 1u)) {
        if (!s_pw.set[i].idx || s_pw.set[i].h == h) { s_pw.set[i].h = h; return &s_pw.set[i].idx; }
    }
}
static int vk_pw_set_grow(void)
{
    if ((s_pw.setn + 1u) * 2u <= s_pw.setcap) return 0;
    const u32 ncap = s_pw.setcap ? s_pw.setcap * 2u : 4096u;
    vk_pw_slot* ns = (vk_pw_slot*)calloc(ncap, sizeof *ns);
    if (!ns) return -1;
    vk_pw_slot* old = s_pw.set; const u32 ocap = s_pw.setcap;
    s_pw.set = ns; s_pw.setcap = ncap;
    for (u32 i = 0; i < ocap; i++)
        if (old[i].idx) *vk_pw_set_slot(old[i].h) = old[i].idx;
    free(old);
    return 0;
}
/* Add a recipe. fresh = recorded now: this run's (a known one is marked used
 * by this run instead); else read from the file, with its own last_run. */
static void vk_pw_add_locked(const vk_pw_rec* r, u64 h, int fresh)
{
    if (vk_pw_set_grow()) return;
    u32* slot = vk_pw_set_slot(h);
    if (*slot) {
        vk_pw_rec* o = &s_pw.rec[*slot - 1u];
        if (fresh && o->last_run != s_pw.run) { o->last_run = s_pw.run; s_pw.dirty = 1; }
        return;
    }
    if (s_pw.nrec == s_pw.caprec) {
        const u32 ncap = s_pw.caprec ? s_pw.caprec * 2u : 1024u;
        vk_pw_rec* nr = (vk_pw_rec*)realloc(s_pw.rec, (size_t)ncap * sizeof *nr);
        if (!nr) return;
        s_pw.rec = nr; s_pw.caprec = ncap;
    }
    s_pw.rec[s_pw.nrec] = *r;
    if (fresh) {
        s_pw.rec[s_pw.nrec].last_run = s_pw.run;
        s_pw.dirty = 1;
        s_pw.last_new = ps3_ft_now();
    }
    *slot = ++s_pw.nrec;
    s_pw.setn++;
}
/* Record the variant P just got (any thread). */
static void vk_pw_note(const vk_eng_pipe* P, u32 t, u32 k, int neg)
{
    if (!s_pa.recipes || !s_sc.on) return;
    vk_pw_rec r;
    if (!vk_pw_from(P, t, k, neg, &r)) return;
    const u64 h = vk_pw_hash(&r);
    vk_pw_lock();
    vk_pw_add_locked(&r, h, 1);
    vk_pw_unlock();
}

static int vk_pw_path(char* out, size_t cap) { return vk_sc_join(out, cap, s_sc.dir, "pipelines.rcp"); }

/* pipelines.rcp: VK_PW_MAGIC, u32 version, record size, render-state size,
 * count, run, 0, u64 hash of the records, u64 0 -- then the records. */
static void vk_pw_load(void)
{
    char path[VK_SC_PATH];
    size_t size = 0;
    s_pw.run = 1;
    if (vk_pw_path(path, sizeof path) != 0) return;
    u8* f = vk_sc_read_all(path, &size);
    const char* why = NULL;
    if (!f) why = "none yet";
    else if (size < VK_PW_HEADER || memcmp(f, VK_PW_MAGIC, 8) != 0) why = "not a recipe file";
    else {
        const u32 ver = vk_sc_le32(f + 8), rsz = vk_sc_le32(f + 12), ssz = vk_sc_le32(f + 16);
        const u32 n = vk_sc_le32(f + 20), run = vk_sc_le32(f + 24);
        u64 h; memcpy(&h, f + 32, sizeof h);
        if (ver != VK_PW_VERSION || rsz != (u32)sizeof(vk_pw_rec) || ssz != (u32)sizeof(rsx_be_render_state))
            why = "another version";
        else if ((size - VK_PW_HEADER) != (size_t)n * sizeof(vk_pw_rec) ||
                 vk_sc_blob_hash(f + VK_PW_HEADER, size - VK_PW_HEADER) != h)
            why = "damaged";
        else {
            s_pw.run = run + 1u;
            vk_pw_lock();
            for (u32 i = 0; i < n; i++) {
                vk_pw_rec r;
                memcpy(&r, f + VK_PW_HEADER + (size_t)i * sizeof r, sizeof r);
                vk_pw_add_locked(&r, vk_pw_hash(&r), 0);  /* not used by this run yet */
            }
            vk_pw_unlock();
        }
    }
    free(f);
    if (why) VK_LOG("pipelines: recipes %s (%s)\n", why, path);
    else     VK_LOG("pipelines: %u recipes from %s (run %u)\n", s_pw.nrec, path, s_pw.run);
}

/* The recipes used in the last VK_PW_KEEP_RUNS runs, through a temporary file. */
static void vk_pw_save(void)
{
    if (!s_pa.recipes || !s_sc.on || !s_pw.dirty) return;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_pw_save_mx);
#else
    pthread_mutex_lock(&s_pw_save_mx);
#endif
    vk_pw_lock();
    u8* buf = s_pw.dirty && s_pw.nrec ? (u8*)malloc((size_t)s_pw.nrec * sizeof(vk_pw_rec)) : NULL;
    u32 n = 0;
    if (buf) {
        for (u32 i = 0; i < s_pw.nrec; i++)
            if (s_pw.run - s_pw.rec[i].last_run < VK_PW_KEEP_RUNS)
                memcpy(buf + (size_t)n++ * sizeof(vk_pw_rec), &s_pw.rec[i], sizeof(vk_pw_rec));
        s_pw.dirty = 0;
    }
    const u32 run = s_pw.run;
    vk_pw_unlock();
    if (buf) {
        u8 head[VK_PW_HEADER];
        memset(head, 0, sizeof head);
        memcpy(head, VK_PW_MAGIC, 8);
        const u32 hv[5] = { VK_PW_VERSION, (u32)sizeof(vk_pw_rec), (u32)sizeof(rsx_be_render_state), n, run };
        memcpy(head + 8, hv, sizeof hv);
        const u64 h = vk_sc_blob_hash(buf, (size_t)n * sizeof(vk_pw_rec));
        memcpy(head + 32, &h, sizeof h);
        char path[VK_SC_PATH];
        if (vk_pw_path(path, sizeof path) != 0 ||
            vk_sc_write_file(path, head, sizeof head, buf, (size_t)n * sizeof(vk_pw_rec)) != 0) {
            if (!(s_sc.warned & 4u)) VK_LOG("pipelines: cannot write the recipes (%s)\n", path);
            s_sc.warned |= 4u;
        }
        free(buf);
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_pw_save_mx);
#else
    pthread_mutex_unlock(&s_pw_save_mx);
#endif
}
#ifdef _WIN32
static DWORD WINAPI vk_pw_save_thread(LPVOID u) { (void)u; vk_pw_save(); __atomic_store_n(&s_pw.saving, 0, __ATOMIC_RELEASE); return 0; }
#else
static void* vk_pw_save_thread(void* u) { (void)u; vk_pw_save(); __atomic_store_n(&s_pw.saving, 0, __ATOMIC_RELEASE); return NULL; }
#endif
static void vk_pw_save_async(void)
{
    if (__atomic_exchange_n(&s_pw.saving, 1, __ATOMIC_ACQ_REL)) return;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, vk_pw_save_thread, NULL, 0, NULL);
    if (h) { CloseHandle(h); return; }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, vk_pw_save_thread, NULL) == 0) { pthread_detach(t); return; }
#endif
    vk_pw_save();
    __atomic_store_n(&s_pw.saving, 0, __ATOMIC_RELEASE);
}

static vk_pw_ent* vk_pw_find(u64 h)
{
    if (!s_pw.mapcap) return NULL;
    for (u32 i = (u32)(h ^ (h >> 29)) & (s_pw.mapcap - 1u);; i = (i + 1u) & (s_pw.mapcap - 1u)) {
        if (!s_pw.map[i].h) return NULL;
        if (s_pw.map[i].h == h) return &s_pw.map[i];
    }
}

/* The map of what to prewarm: this context's recipes, in recorded order. */
static void vk_pw_plan(void)
{
    u32 n = 0;
    for (u32 i = 0; i < s_pw.nrec; i++) n += s_pw.rec[i].ctx == s_pw.ctx;
    if (!n) return;
    u32 cap = 1024u;
    while (cap < n * 2u) cap *= 2u;
    u32 mcap = 1024u;
    while (mcap < n * 4u) mcap *= 2u;
    s_pw.map = (vk_pw_ent*)calloc(cap, sizeof *s_pw.map);
    s_pw.order = (u32*)malloc((size_t)n * sizeof *s_pw.order);
    s_pw.mods = (vk_pw_mod*)calloc(mcap, sizeof *s_pw.mods);
    if (!s_pw.map || !s_pw.order || !s_pw.mods) {
        free(s_pw.map); free(s_pw.order); free(s_pw.mods);
        s_pw.map = NULL; s_pw.order = NULL; s_pw.mods = NULL;
        return;
    }
    s_pw.mapcap = cap; s_pw.modcap = mcap;
    for (u32 i = 0; i < s_pw.nrec; i++) {
        if (s_pw.rec[i].ctx != s_pw.ctx) continue;
        const u64 h = vk_pw_hash(&s_pw.rec[i]);
        u32 j = (u32)(h ^ (h >> 29)) & (cap - 1u);
        while (s_pw.map[j].h && s_pw.map[j].h != h) j = (j + 1u) & (cap - 1u);
        if (s_pw.map[j].h) continue;                       /* the same recipe twice */
        s_pw.map[j].h = h; s_pw.map[j].rec = i; s_pw.map[j].st = VK_PW_QUEUED;
        s_pw.order[s_pw.npw++] = j;
    }
}

/* The prewarm's module for one .spv (read once, shared by its recipes); its
 * words stay for the engine's own lookups (vk_pw_spv_get). Any thread. */
static VkShaderModule vk_pw_module(vk_sc_key k, u32* buf)
{
    if (!s_pw.modcap || (!k.a && !k.b)) return VK_NULL_HANDLE;
    vk_pw_lock();
    u32 i = (u32)(k.a ^ (k.b >> 7)) & (s_pw.modcap - 1u);
    u32 probes = 0;
    while (s_pw.mods[i].st && !(s_pw.mods[i].k.a == k.a && s_pw.mods[i].k.b == k.b)) {
        i = (i + 1u) & (s_pw.modcap - 1u);
        if (++probes == s_pw.modcap) { vk_pw_unlock(); return VK_NULL_HANDLE; }
    }
    vk_pw_mod* m = &s_pw.mods[i];
    for (;;) {
        if (m->st == 0) { m->k = k; m->st = 1; break; }   /* this thread reads it */
        if (m->st == 2) { const VkShaderModule r = m->m; vk_pw_unlock(); return r; }
        if (m->st == 3) { vk_pw_unlock(); return VK_NULL_HANDLE; }
        vk_pw_unlock(); vk_pa_yield(); vk_pw_lock();       /* another worker is reading it */
    }
    vk_pw_unlock();
    char path[VK_SC_PATH];
    size_t got = 0;
    VkShaderModule mod = VK_NULL_HANDLE;
    u32* w = NULL;
    if (vk_sc_spv_path(path, sizeof path, s_sc.dir, k) == 0 &&
        vk_sc_read_into(path, buf, (size_t)VK_SPV_WORDS * 4u, &got) == 0 && vk_sc_spirv_ok(buf, got)) {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = got, .pCode = buf };
        if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &mod) != VK_SUCCESS) mod = VK_NULL_HANDLE;
        if (mod && (w = (u32*)malloc(got)) != NULL) memcpy(w, buf, got);
    }
    vk_pw_lock();
    m->m = mod; m->w = w; m->n = (u32)(got / 4u);
    m->st = mod ? 2 : 3;
    vk_pw_unlock();
    return mod;
}

/* (vk_sc_probe) the prewarm already read this .spv: its words, handed over
 * once (the session's memory table keeps them from then on). Backend thread. */
static int vk_pw_spv_get(vk_sc_key k, u32* words, u32 max_words, u32* out_words)
{
    if (!s_pw.modcap) return 0;
    int hit = 0;
    vk_pw_lock();
    u32 i = (u32)(k.a ^ (k.b >> 7)) & (s_pw.modcap - 1u);
    for (u32 probes = 0; s_pw.mods[i].st && probes < s_pw.modcap; probes++, i = (i + 1u) & (s_pw.modcap - 1u)) {
        vk_pw_mod* m = &s_pw.mods[i];
        if (m->k.a != k.a || m->k.b != k.b) continue;
        if (m->st == 2 && m->w && m->n && m->n <= max_words) {
            memcpy(words, m->w, (size_t)m->n * 4u);
            *out_words = m->n;
            if (vk_sc_async_on()) { free(m->w); m->w = NULL; }   /* vk_sc_mem_put keeps them now */
            hit = 1;
        }
        break;
    }
    vk_pw_unlock();
    return hit;
}

/* One prewarm entry, on a worker. */
static void vk_pw_build(u32 j, u32* buf)
{
    vk_pw_ent* e = &s_pw.map[j];
    int q = VK_PW_QUEUED;
    if (!__atomic_compare_exchange_n(&e->st, &q, VK_PW_BUILDING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;                                            /* a draw claimed it first */
    vk_pw_rec r;
    vk_pw_lock();
    r = s_pw.rec[e->rec];                                  /* rec may be reallocated by a note */
    vk_pw_unlock();
    const u64 t0 = ps3_ft_now();
    const VkShaderModule vs = vk_pw_module(r.vs, buf);
    const VkShaderModule fs = vs ? vk_pw_module(r.fs, buf) : VK_NULL_HANDLE;
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && fs) {
        const vk_ev_in in = { vs, fs, &r.rs, r.nslots, r.stride, r.rt_count, r.sblend, r.fbf_in,
                              r.topo, r.vk_idx, r.neg };
        if (vk_eng_variant_build(&in, &p, 1) != VK_SUCCESS) p = VK_NULL_HANDLE;
        if (p) __atomic_add_fetch(&s_pw.built, 1ull, __ATOMIC_RELAXED);
        else   __atomic_add_fetch(&s_pw.failed, 1ull, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&s_pw.missing, 1ull, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&s_pw.built_ns, ps3_ft_now() - t0, __ATOMIC_RELAXED);
    e->pipe = p;
    __atomic_store_n(&e->st, p ? VK_PW_READY : VK_PW_FAILED, __ATOMIC_RELEASE);
}

/* The modules are only needed while pipelines are created from them. */
static void vk_pw_drop_modules(void)
{
    for (u32 i = 0; i < s_pw.modcap; i++)
        if (s_pw.mods[i].st == 2 && s_pw.mods[i].m) {
            pvkDestroyShaderModule(s_vk.device, s_pw.mods[i].m, NULL);
            s_pw.mods[i].m = VK_NULL_HANDLE;
        }
}

/* ---- the queue (under s_pa_mx) ---- */
static u32 vk_pa_idx(const vk_eng_pipe* P) { return (u32)(P - s_epipe); }
static void vk_pa_enqueue_locked(vk_eng_pipe* P)
{
    const u32 me = vk_pa_idx(P) + 1u;
    P->pa_next = 0;
    P->pa_prev = s_pa.qtail;
    if (s_pa.qtail) s_epipe[s_pa.qtail - 1u].pa_next = me; else s_pa.qhead = me;
    s_pa.qtail = me;
    P->pa_job = 1;
    vk_pa_wake(0);
}
static void vk_pa_unqueue_locked(vk_eng_pipe* P);
/* (freeze) to the front: a frame is being held for this one */
static void vk_pa_enqueue_front_locked(vk_eng_pipe* P)
{
    const u32 me = vk_pa_idx(P) + 1u;
    if (P->pa_job == 1) vk_pa_unqueue_locked(P);
    P->pa_prev = 0;
    P->pa_next = s_pa.qhead;
    if (s_pa.qhead) s_epipe[s_pa.qhead - 1u].pa_prev = me; else s_pa.qtail = me;
    s_pa.qhead = me;
    P->pa_job = 1;
    vk_pa_wake(0);
}
static void vk_pa_unqueue_locked(vk_eng_pipe* P)
{
    if (P->pa_prev) s_epipe[P->pa_prev - 1u].pa_next = P->pa_next; else s_pa.qhead = P->pa_next;
    if (P->pa_next) s_epipe[P->pa_next - 1u].pa_prev = P->pa_prev; else s_pa.qtail = P->pa_prev;
    P->pa_prev = P->pa_next = 0;
}

static u32 vk_pa_bit(u32 t, u32 k, int neg) { return (neg ? 20u : 0u) + t * 4u + k; }

static void vk_pa_commit_push(vk_sc_key key, const u32* w, u32 n)
{
    vk_pa_commit* c = (vk_pa_commit*)malloc(sizeof *c + (size_t)n * 4u);
    if (!c) return;
    c->key = key; c->n = n;
    memcpy(c->w, w, (size_t)n * 4u);
    vk_pa_lock();
    c->next = s_pa.commits;
    s_pa.commits = c;
    vk_pa_unlock();
}

/* A worker-built pipeline: vk_sc_create_pipeline's counters, kept apart (the
 * backend thread folds the "the driver had to build it" ones into the
 * pipeline-cache saves at the next present). */
static VkResult vk_pa_create_bg(const VkGraphicsPipelineCreateInfo* gci, VkPipeline* out)
{
    const u64 t0 = ps3_ft_now();
    vk_pipe_test_cold();                       /* (perf60) test aid */
    const VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_sc.pc, 1, gci, NULL, out);
    const u64 d = ps3_ft_now() - t0;
    __atomic_add_fetch(&s_pa.bg_pipes, 1ull, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_pa.bg_pipe_ns, d, __ATOMIC_RELAXED);
    unsigned long long w = __atomic_load_n(&s_pa.bg_pipe_worst, __ATOMIC_RELAXED);
    while (d > w && !__atomic_compare_exchange_n(&s_pa.bg_pipe_worst, &w, d, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (r == VK_SUCCESS && s_sc.pc && d > 1000000ull) __atomic_add_fetch(&s_pa.bg_pc_new, 1u, __ATOMIC_RELAXED);
    return r;
}

/* A pipeline's job: the translations it owes, then every variant asked of it
 * until none is left. On a worker (bg = 1) or, stolen, on the backend thread. */
static void vk_pa_run(vk_eng_pipe* P, u32* spv, char* log, u32 log_size, int bg)
{
    const u32 xl = __atomic_load_n(&P->pa_xl, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < 2u; i++) {
        if (!(xl & (1u << i))) continue;
        char* text = P->pa_text[i];
        P->pa_text[i] = NULL;
        const int stage = i ? RSX_SHADER_STAGE_FRAGMENT : RSX_SHADER_STAGE_VERTEX;
        u32 n = 0;
        const u64 t0 = ps3_ft_now();
        int ok = text && rsx_hlsl_to_spirv(text, stage, spv, VK_SPV_WORDS, &n, log, log_size) == 0 && n;
        const u64 d = ps3_ft_now() - t0;
        if (bg) { __atomic_add_fetch(&s_pa.bg_xl, 1ull, __ATOMIC_RELAXED); __atomic_add_fetch(&s_pa.bg_xl_ns, d, __ATOMIC_RELAXED); }
        else    s_sc.spv_ns += d;
        VkShaderModule m = VK_NULL_HANDLE;
        if (ok) {
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = (size_t)n * 4u, .pCode = spv };
            ok = pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) == VK_SUCCESS;
        }
        if (!ok) {
            VK_LOG("engine: %s translation failed%s: %s\n", i ? "fragment" : "vertex",
                   bg ? " (pipeline worker)" : "", text ? log : "no memory");
            __atomic_store_n(&P->pa_fail, 1u, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(i ? &P->fs : &P->vs, m, __ATOMIC_RELEASE);
            if (P->key_ok & (1u << i)) vk_pa_commit_push(P->key[i], spv, n);
        }
        free(text);
    }
    __atomic_store_n(&P->pa_xl, 0u, __ATOMIC_RELEASE);
    for (;;) {
        vk_pa_lock();
        const u64 todo = P->pa_want & ~P->pa_done;
        if (!todo) { P->pa_job = 0; vk_pa_unlock(); return; }
        vk_pa_unlock();
        for (u32 b = 0; b < 40u; b++) {
            if (!(todo & (1ull << b))) continue;
            const int neg = b >= 20u;
            const u32 t = (b % 20u) / 4u, k = b % 4u;
            VkPipeline* slotp = neg ? &P->variant_neg[t][k] : &P->variant[t][k];
            if (!__atomic_load_n(slotp, __ATOMIC_ACQUIRE) && !__atomic_load_n(&P->pa_fail, __ATOMIC_ACQUIRE)) {
                vk_ev_in in;
                vk_ev_from(P, t, k, neg, &in);
                VkPipeline p = VK_NULL_HANDLE;
                if (in.vs && in.fs && vk_eng_variant_build(&in, &p, bg) == VK_SUCCESS && p) {
                    __atomic_store_n(slotp, p, __ATOMIC_RELEASE);
                    vk_pw_note(P, t, k, neg);
                } else {
                    static int said;
                    if (said++ < 8) VK_LOG("engine: vkCreateGraphicsPipelines failed%s\n", bg ? " (pipeline worker)" : "");
                }
            }
            __atomic_fetch_or(&P->pa_done, 1ull << b, __ATOMIC_ACQ_REL);
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI vk_pa_worker(LPVOID arg)
#else
static void* vk_pa_worker(void* arg)
#endif
{
    (void)arg;
    u32* spv = (u32*)malloc((size_t)VK_SPV_WORDS * 4u);
    char log[2048];
    int prio = 1;                                         /* 1 below normal, 0 normal */
    vk_pa_lock();
    while (spv && !s_pa.quit) {
        if (s_pa.qhead) {
            vk_eng_pipe* P = &s_epipe[s_pa.qhead - 1u];
            vk_pa_unqueue_locked(P);
            P->pa_job = 2;
            vk_pa_unlock();
#ifdef _WIN32
            if (prio) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL); prio = 0; }
#endif
            vk_pa_run(P, spv, log, sizeof log, 1);
            vk_pa_lock();
            continue;
        }
        if (s_pw.next < s_pw.npw) {
            const u32 j = s_pw.order[s_pw.next++];
            s_pw.busy++;
            vk_pa_unlock();
#ifdef _WIN32
            if (!prio) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); prio = 1; }
#endif
            vk_pw_build(j, spv);
            vk_pa_lock();
            if (--s_pw.busy == 0 && s_pw.next >= s_pw.npw && !s_pw.done) {
                s_pw.done = 1;
                s_pw.t1 = ps3_ft_now();
                vk_pa_unlock();
                vk_pw_drop_modules();                     /* every prewarm pipeline is made */
                vk_pa_lock();
            }
            continue;
        }
        vk_pa_wait();
    }
    __atomic_sub_fetch(&s_pa.nrun, 1, __ATOMIC_ACQ_REL);
    vk_pa_unlock();
    free(spv);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void vk_pa_cfg_read(void)
{
    if (s_pa.cfg) return;
    s_pa.cfg = 1;
    const char* e = getenv("PS3RECOMP_VK_PIPE_ASYNC");
    s_pa.mode = e && *e ? atoi(e) : 1;
    e = getenv("PS3RECOMP_VK_PIPE_ASYNC_XLATE");
    s_pa.xlate = s_pa.mode && !(e && *e == '0');
    e = getenv("PS3RECOMP_VK_PIPE_RECIPES");
    s_pa.recipes = !(e && *e == '0');
    e = getenv("PS3RECOMP_VK_PIPE_PREWARM");
    s_pa.prewarm = s_pa.recipes && !(e && *e == '0');
    e = getenv("PS3RECOMP_VK_PIPE_WAIT_US");
    s_pa.wait_ns = (u64)((e && *e) ? atoi(e) : 2000) * 1000ull;
    e = getenv("PS3RECOMP_VK_PIPE_SKIP_STENCIL");
    s_pa.skipw = !(e && *e == '0');
    e = getenv("PS3RECOMP_VK_PIPE_SKIP_BLEND");
    s_pa.skipb = !(e && *e == '0');
    e = getenv("PS3RECOMP_VK_PIPE_HOLD_MS");
    { int ms = (e && *e) ? atoi(e) : 300;
      if (ms < 0) ms = 0;
      if (ms > 5000) ms = 5000;
      s_pa.hold_ns = s_pa.mode == 1 ? (u64)ms * 1000000ull : 0; }
    e = getenv("PS3RECOMP_VK_PIPE_THREADS");
    u32 n = (e && *e) ? (u32)atoi(e) : vk_pa_cpus() / 4u;
    if (!(e && *e) && n > 4u) n = 4u;
    if (n < 1u) n = 1u;
    if (n > VK_PA_MAX_THREADS) n = VK_PA_MAX_THREADS;
    s_pa.nthreads = n;
}
static void vk_pa_start(void);
static int vk_pa_xlate_on(void)
{
    vk_pa_start();
    return s_pa.xlate && __atomic_load_n(&s_pa.nrun, __ATOMIC_ACQUIRE) > 0;
}

static void vk_pa_atexit(void);
/* At the first engine pipeline (the device, layouts and render state are up). */
static void vk_pa_start(void)
{
    if (s_pa.started || !s_vk.device || !s_g.on) return;
    s_pa.started = 1;
    vk_pa_cfg_read();
    (void)vk_mrt_mode();                                  /* its static, before a worker reads it */
    if (s_pa.mode || s_pa.prewarm) {
        /* glslang's process init (and the cache's fingerprint) on this thread,
         * before any worker translates */
        if (!s_sc.fp_done && s_sc.on) vk_sc_fingerprint(s_g.spv, VK_SPV_WORDS);
        else if (!s_sc.on) { u32 n = 0; (void)rsx_hlsl_to_spirv(kVkScCanaryVS, RSX_SHADER_STAGE_VERTEX, s_g.spv,
                                                                 VK_SPV_WORDS, &n, s_g.log, sizeof s_g.log); }
        for (u32 n = 1; n <= RSX_BE_MAX_COLOR_TARGETS; n++) (void)vk_eng_render_pass(n);
    }
    s_pw.ctx = vk_pw_ctx();
    s_pw.run = 1;
    if (s_pa.recipes && s_sc.on) vk_pw_load();
    if (s_pa.prewarm && s_sc.on) vk_pw_plan();
    s_pw.t0 = ps3_ft_now();
    if (!s_pw.npw) s_pw.done = 1;
    const u32 want = (s_pa.mode || s_pw.npw) ? s_pa.nthreads : 0u;
    for (u32 i = 0; i < want; i++) {
        __atomic_add_fetch(&s_pa.nrun, 1, __ATOMIC_ACQ_REL);   /* before it runs: it counts itself out */
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 4u << 20, vk_pa_worker, NULL, CREATE_SUSPENDED, NULL);
        if (!h) { __atomic_sub_fetch(&s_pa.nrun, 1, __ATOMIC_ACQ_REL); break; }
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
        ResumeThread(h);
        CloseHandle(h);
#else
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 4u << 20);
        const int rc = pthread_create(&t, &at, vk_pa_worker, NULL);
        pthread_attr_destroy(&at);
        if (rc != 0) { __atomic_sub_fetch(&s_pa.nrun, 1, __ATOMIC_ACQ_REL); break; }
        pthread_detach(t);
#endif
    }
    if (!s_pa.atexit_set) { s_pa.atexit_set = 1; atexit(vk_pa_atexit); }
    VK_LOG("pipelines: %d worker(s) (PS3RECOMP_VK_PIPE_THREADS); async %s (PS3RECOMP_VK_PIPE_ASYNC=%d, wait "
           "budget %llu us/frame, translations %s); prewarm %s (%u recipes)\n",
           s_pa.nrun, s_pa.mode ? "on" : "off", s_pa.mode, (unsigned long long)(s_pa.wait_ns / 1000u),
           s_pa.xlate ? "on the workers" : "here", s_pa.prewarm ? "on" : "off", s_pw.npw);
    VK_LOG("pipelines: objects that write stencil may be left out a frame: %s (PS3RECOMP_VK_PIPE_SKIP_STENCIL); "
           "blended draws without a depth test too: %s (PS3RECOMP_VK_PIPE_SKIP_BLEND); "
           "a frame missing a pass is held back for at most %llu ms (PS3RECOMP_VK_PIPE_HOLD_MS)\n",
           s_pa.skipw ? "yes" : "no", s_pa.skipb ? "yes" : "no", (unsigned long long)(s_pa.hold_ns / 1000000ull));
}

/* Draws that can be left out a frame without leaving garbage behind. */
static int vk_pa_skippable(const vk_eng_pipe* P, int neg)
{
    if (s_pa.mode < 1 || neg || P->sblend || P->fbf_in || P->fbf_mask) return 0;
    if (s_pa.mode >= 2) return 1;
    const rsx_be_render_state* rs = &P->rs;
    /* (freeze) A blended draw without a depth test -- the HUD, a glow, a light
     * added onto the frame -- lays itself over what its target already holds
     * this frame: left out, that is still a whole picture without it. A
     * "blend" of ONE, ZERO replaces what is there (a full-screen pass) and
     * waits like one. PS3RECOMP_VK_PIPE_SKIP_BLEND=0: all of them wait. */
    if (s_pa.skipb && (!rs->depth_test || vk_gcm_compare(rs->depth_func) == VK_COMPARE_OP_ALWAYS) &&
        rs->blend_enable && (rs->color_mask & 0x00FFFFFFu) && !(rs->stencil_enable && rs->s_write_mask) &&
        !(vk_gcm_blend_factor(rs->sf_rgb) == VK_BLEND_FACTOR_ONE && vk_gcm_blend_factor(rs->df_rgb) == VK_BLEND_FACTOR_ZERO))
        return 1;
    if (!rs->depth_test || vk_gcm_compare(rs->depth_func) == VK_COMPARE_OP_ALWAYS) return 0;
    /* (freeze) An object that also marks the stencil -- depth-tested, writing
     * colour: 40% of this game's pipelines, its whole G-buffer -- is as absent
     * for a frame as any other object: no colour, no depth, no mark. Only a
     * mark without colour (a light volume's) has to be there for the pass that
     * reads it. PS3RECOMP_VK_PIPE_SKIP_STENCIL=0: any stencil write waits. */
    if (s_pa.skipw && (rs->color_mask & 0x00FFFFFFu)) return 1;
    if (rs->stencil_enable && rs->s_write_mask &&
        (vk_gcm_stencil_op(rs->s_fail) != VK_STENCIL_OP_KEEP || vk_gcm_stencil_op(rs->s_zfail) != VK_STENCIL_OP_KEEP ||
         vk_gcm_stencil_op(rs->s_zpass) != VK_STENCIL_OP_KEEP ||
         (rs->stencil_two_sided &&
          (vk_gcm_stencil_op(rs->bs_fail) != VK_STENCIL_OP_KEEP || vk_gcm_stencil_op(rs->bs_zfail) != VK_STENCIL_OP_KEEP ||
           vk_gcm_stencil_op(rs->bs_zpass) != VK_STENCIL_OP_KEEP))))
        return 0;
    return 1;
}

/* (freeze) A draw that may not be left out (a full-screen pass, the HUD, a
 * stencil mark) used to wait for its pipeline right here: 30 ms each on a
 * driver that has never seen it, one after the other, on the thread the whole
 * frame waits for -- 71 of them in the first frame of the world. Now it is
 * left out like any other, its pipeline goes to the FRONT of the workers'
 * queue, and the frame it belongs to is submitted but not shown: the window
 * keeps the last whole picture while the game, the sound and the input run
 * on, and the workers build in parallel. A picture is never held for longer
 * than PS3RECOMP_VK_PIPE_HOLD_MS (default 300; 0 = off, the old waiting):
 * past that the draws wait as before, so a frame always completes.
 * Only frames that end in vk_present_async are held (vk_pa_hold_ok). */
static int vk_pa_may_hold(void)
{
    if (!s_pa.hold_ns || !vk_pa_hold_ok || !s_pa.last_shown) return 0;
    /* from the first pass left out, not from the last picture: a frame that is
     * long by itself (the first of the world: 640 textures) has its passes
     * built by the workers all the same */
    return !s_pa.hold_t0 || ps3_ft_now() - s_pa.hold_t0 < s_pa.hold_ns;
}
/* why a draw may not be left out: 0 opaque without depth test, 1 blended
 * without depth test, 2 depth ALWAYS, 3 a stencil mark, 4 signed blend /
 * framebuffer fetch */
static u32 vk_pa_hold_class(const vk_eng_pipe* P, int neg)
{
    const rsx_be_render_state* rs = &P->rs;
    if (neg || P->sblend || P->fbf_in || P->fbf_mask) return 4u;
    if (!rs->depth_test) return rs->blend_enable ? 1u : 0u;
    if (vk_gcm_compare(rs->depth_func) == VK_COMPARE_OP_ALWAYS) return 2u;
    return 3u;
}
/* At the present: 1 = do not show this frame. */
static int vk_pa_hold_take(void)
{
    const u64 now = ps3_ft_now();
    if (s_pa.hold_flag) {
        s_pa.hold_flag = 0;
        s_pa.hold_frames++;
        s_pa.hold_run++;
        return 1;
    }
    if (s_pa.hold_run) {
        const u64 d = s_pa.last_shown ? now - s_pa.last_shown : 0;
        s_pa.hold_runs++;
        s_pa.hold_ns_sum += d;
        if (d > s_pa.hold_worst) s_pa.hold_worst = d;
        if (d >= 50000000ull)
            VK_LOG("pipelines: HOLD the picture stood for %.1f ms (%u frames went on behind it)\n",
                   (double)d / 1e6, s_pa.hold_run);
        s_pa.hold_run = 0;
    }
    s_pa.hold_t0 = 0;
    s_pa.last_shown = now;
    return 0;
}
static void vk_pa_enqueue_front_locked(vk_eng_pipe* P);

/* A draw waited t0..now for its pipeline. */
static void vk_pa_waited(u64 t0)
{
    const u64 d = ps3_ft_now() - t0;
    s_pa.spent_ns += d;
    s_pa.wait_n++;
    s_pa.wait_ns_sum += d;
    if (d > s_pa.wait_worst) s_pa.wait_worst = d;
}

/* The prewarm's variant for this draw: 1 = *out is it; 0 = none (build it).
 * A recipe a worker is still building is waited for (a draw that may skip:
 * within the frame's budget, else -1 = skip it this frame). */
static int vk_pw_take(vk_eng_pipe* P, u32 t, u32 k, int neg, u64 t0, VkPipeline* out)
{
    if (!s_pw.mapcap) return 0;
    vk_pw_rec r;
    if (!vk_pw_from(P, t, k, neg, &r)) return 0;
    vk_pw_ent* e = vk_pw_find(vk_pw_hash(&r));
    if (!e) return 0;
    vk_pw_lock();
    if (s_pw.rec[e->rec].last_run != s_pw.run) {          /* used by this run: kept another 32 */
        s_pw.rec[e->rec].last_run = s_pw.run;
        s_pw.dirty = 1;
        s_pw.last_new = ps3_ft_now();
    }
    vk_pw_unlock();
    int st = __atomic_load_n(&e->st, __ATOMIC_ACQUIRE);
    if (st == VK_PW_QUEUED &&
        __atomic_compare_exchange_n(&e->st, &st, VK_PW_CLAIMED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;                                          /* not started: built for the draw instead */
    if (st == VK_PW_BUILDING) {
        const int skippable = vk_pa_skippable(P, neg);
        while ((st = __atomic_load_n(&e->st, __ATOMIC_ACQUIRE)) == VK_PW_BUILDING) {
            if (skippable && s_pa.spent_ns + (ps3_ft_now() - t0) >= s_pa.wait_ns) return -1;
            vk_pa_yield();
        }
    }
    if (st == VK_PW_READY &&
        __atomic_compare_exchange_n(&e->st, &st, VK_PW_TAKEN, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        *out = e->pipe;
        return 1;
    }
    return 0;
}

/* Run a pipeline's queued job here (no worker took it). */
static void vk_pa_steal(vk_eng_pipe* P)
{
    vk_pa_lock();
    if (P->pa_job != 1) { vk_pa_unlock(); return; }
    vk_pa_unqueue_locked(P);
    P->pa_job = 3;
    vk_pa_unlock();
    vk_pa_run(P, s_g.spv, s_g.log, sizeof s_g.log, 0);
}

/* vk_eng_variant_ex: a variant not built yet. Backend thread. */
static VkPipeline vk_pa_variant(vk_eng_pipe* P, u32 t, u32 k, int neg, VkPipeline* slotp)
{
    const u64 t0 = ps3_ft_now();
    VkPipeline r = VK_NULL_HANDLE;
    vk_pa_start();
    const int pw = vk_pw_take(P, t, k, neg, t0, &r);
    if (pw == 1) {
        __atomic_store_n(slotp, r, __ATOMIC_RELEASE);
        s_pa.pw_hit++;
        s_pa.spent_ns += ps3_ft_now() - t0;
        return r;
    }
    if (pw < 0) { s_pa.spent_ns += ps3_ft_now() - t0; s_pa.skip_n++; return VK_NULL_HANDLE; }
    if (!s_pa.mode || __atomic_load_n(&s_pa.nrun, __ATOMIC_ACQUIRE) <= 0) {
        /* the old way: built here, now (a job left queued -- no worker -- first) */
        if (P->pa_job == 1) vk_pa_steal(P);
        while (__atomic_load_n(&P->pa_job, __ATOMIC_ACQUIRE)) vk_pa_yield();   /* a stopping worker */
        if (P->pa_fail) return VK_NULL_HANDLE;
        r = vk_eng_variant_now(P, t, k, neg, slotp);
        vk_pa_waited(t0);
        if (r) vk_pw_note(P, t, k, neg);
        return r;
    }
    if (__atomic_load_n(&P->pa_fail, __ATOMIC_ACQUIRE)) return VK_NULL_HANDLE;
    const u64 bit = 1ull << vk_pa_bit(t, k, neg);
    const int skippable = vk_pa_skippable(P, neg);
    const int hold = !skippable && vk_pa_may_hold();      /* (freeze) left out, the frame not shown */
    vk_pa_lock();
    if (__atomic_load_n(&P->pa_done, __ATOMIC_ACQUIRE) & bit) {      /* tried: it failed */
        vk_pa_unlock();
        return __atomic_load_n(slotp, __ATOMIC_ACQUIRE);
    }
    if (!P->pa_job && !skippable && !hold) {              /* nothing in flight: the old way */
        vk_pa_unlock();
        r = vk_eng_variant_now(P, t, k, neg, slotp);
        s_pa.inline_n++;
        vk_pa_waited(t0);
        if (r) vk_pw_note(P, t, k, neg);
        return r;
    }
    P->pa_want |= bit;
    if (hold) { if (P->pa_job <= 1) vk_pa_enqueue_front_locked(P); }
    else if (!P->pa_job) vk_pa_enqueue_locked(P);
    vk_pa_unlock();
    if (!skippable && !hold) vk_pa_steal(P);              /* still queued: run it here */
    for (;;) {
        if (__atomic_load_n(&P->pa_done, __ATOMIC_ACQUIRE) & bit) {
            r = __atomic_load_n(slotp, __ATOMIC_ACQUIRE);
            vk_pa_waited(t0);
            return r;
        }
        if ((skippable || hold) && s_pa.spent_ns + (ps3_ft_now() - t0) >= s_pa.wait_ns) break;
        vk_pa_yield();
    }
    s_pa.spent_ns += ps3_ft_now() - t0;
    if (hold) {
        s_pa.hold_flag = 1;
        s_pa.hold_skips++;
        s_pa.hold_cls[vk_pa_hold_class(P, neg)]++;
        if (!s_pa.hold_t0) s_pa.hold_t0 = ps3_ft_now();
    }
    else s_pa.skip_n++;
    return VK_NULL_HANDLE;
}

/* pipeline_create_body: P's deferred translations to the workers. */
static void vk_pa_queue(vk_eng_pipe* P)
{
    vk_pa_start();
    (void)vk_eng_render_pass(P->rt_count);               /* the workers only read render passes */
    if (__atomic_load_n(&s_pa.nrun, __ATOMIC_ACQUIRE) <= 0) {
        P->pa_job = 3;
        vk_pa_run(P, s_g.spv, s_g.log, sizeof s_g.log, 0);
        return;
    }
    vk_pa_lock();
    if (!P->pa_job) vk_pa_enqueue_locked(P);
    vk_pa_unlock();
}

/* pipeline_release: no job of P's left anywhere. */
static void vk_pa_forget(vk_eng_pipe* P)
{
    if (s_pa.started) {
        for (;;) {
            vk_pa_lock();
            if (P->pa_job == 1) { vk_pa_unqueue_locked(P); P->pa_job = 0; }
            const u32 j = P->pa_job;
            vk_pa_unlock();
            if (!j) break;
            vk_pa_yield();                                /* a worker finishes it */
        }
    }
    free(P->pa_text[0]); free(P->pa_text[1]);
    P->pa_text[0] = P->pa_text[1] = NULL;
    P->pa_xl = 0;
}

/* The workers' translations into the shader cache. Backend thread. */
static void vk_pa_store_commits(void)
{
    vk_pa_lock();
    vk_pa_commit* c = s_pa.commits;
    s_pa.commits = NULL;
    vk_pa_unlock();
    while (c) {
        vk_pa_commit* nx = c->next;
        vk_sc_store(c->key, c->w, c->n);
        free(c);
        c = nx;
    }
}

/* At every present (vk_sc_tick), on the backend thread. */
static void vk_pa_frame(void)
{
    s_pa.spent_ns = 0;
    { const unsigned long long d = g_rsx_tstat.ns - s_pa.tex_last_ns;  /* the frame's texture work */
      s_pa.tex_last_ns = g_rsx_tstat.ns;
      if (d > s_pa.tex_frame_worst) s_pa.tex_frame_worst = d; }
    if (!s_pa.started) return;
    if (s_pa.commits) vk_pa_store_commits();
    const u32 nb = __atomic_exchange_n(&s_pa.bg_pc_new, 0u, __ATOMIC_ACQ_REL);
    if (nb) {                                            /* worker pipelines count towards a save */
        const u64 now = ps3_ft_now();
        s_sc.pc_new += nb;
        s_sc.pc_last_new = now;
        if (s_sc.pc_new >= VK_SC_SAVE_EVERY && now - s_sc.pc_last_save >= VK_SC_SAVE_GAP_NS) {
            vk_sc_save_async(0);
            vk_sc_maybe_stats("periodic");
        }
    }
    if (s_pw.done && !s_pw.said && s_pw.npw) {
        s_pw.said = 1;
        VK_LOG("pipelines: prewarm built %llu of %u recipes in %.1f s (%.0f ms of worker time; %llu failed, %llu "
               "without their shaders)\n", s_pw.built, s_pw.npw, (double)(s_pw.t1 - s_pw.t0) / 1e9,
               (double)s_pw.built_ns / 1e6, s_pw.failed, s_pw.missing);
    }
    if (s_pw.dirty && s_pw.last_new && ps3_ft_now() - s_pw.last_new >= VK_SC_IDLE_NS) {
        s_pw.last_new = 0;                                /* a burst of new recipes ended */
        vk_pw_save_async();
    }
}

/* The stats line (after every "shader cache:" line). */
static void vk_pa_stats(void)
{
    VK_LOG("pipelines: %llu draws waited for one (%.0f ms, worst %.1f ms), %llu skipped while one was built; "
           "workers %llu pipelines (%.0f ms, worst %.1f ms) + %llu translations (%.0f ms); prewarm %llu of %u "
           "ready, %llu used; textures: %llu uploads, %.0f ms, worst %.1f ms, worst frame %.1f ms\n",
           s_pa.wait_n, (double)s_pa.wait_ns_sum / 1e6, (double)s_pa.wait_worst / 1e6, s_pa.skip_n,
           s_pa.bg_pipes, (double)s_pa.bg_pipe_ns / 1e6, (double)s_pa.bg_pipe_worst / 1e6,
           s_pa.bg_xl, (double)s_pa.bg_xl_ns / 1e6, s_pw.built, s_pw.npw, s_pa.pw_hit,
           g_rsx_tstat.n, (double)g_rsx_tstat.ns / 1e6, (double)g_rsx_tstat.worst_ns / 1e6,
           (double)s_pa.tex_frame_worst / 1e6);
    if (s_pa.hold_frames)
        VK_LOG("pipelines: %llu frames not shown for a missing pass (%llu draws), in %llu holds of the picture "
               "(%.0f ms in all, longest %.1f ms); the draws: %llu opaque full-screen, %llu blended, %llu depth "
               "ALWAYS, %llu stencil marks, %llu signed blend or framebuffer fetch\n",
               s_pa.hold_frames, s_pa.hold_skips, s_pa.hold_runs,
               (double)s_pa.hold_ns_sum / 1e6, (double)s_pa.hold_worst / 1e6, s_pa.hold_cls[0], s_pa.hold_cls[1],
               s_pa.hold_cls[2], s_pa.hold_cls[3], s_pa.hold_cls[4]);
}

/* Stop the workers: each finishes the job it is in. */
static void vk_pa_join(u32 ms)
{
    vk_pa_lock();
    s_pa.quit = 1;
    vk_pa_wake(1);
    vk_pa_unlock();
    for (u32 i = 0; i < ms && __atomic_load_n(&s_pa.nrun, __ATOMIC_ACQUIRE) > 0; i++) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
}
/* exit() with the game still running (the quit thread): no worker inside the
 * driver while the process ends, and the recipes on disk. */
static void vk_pa_atexit(void)
{
    vk_pa_join(500);
    vk_pw_save();
}
/* Clean shutdown, before anything the workers use goes (vk_eng_shutdown,
 * vk_sc_close): the workers stopped, their translations stored, the prewarm's
 * unused pipelines and modules destroyed. */
static void vk_pa_stop(void)
{
    if (!s_pa.started) return;
    vk_pa_join(10000);
    if (__atomic_load_n(&s_pa.nrun, __ATOMIC_ACQUIRE) > 0) {
        VK_LOG("pipelines: a worker did not stop -- its objects are left to the driver\n");
        return;
    }
    vk_pa_store_commits();
    for (u32 i = 0; i < s_pw.mapcap; i++) {
        if (s_pw.map[i].h && s_pw.map[i].st == VK_PW_READY && s_pw.map[i].pipe)
            pvkDestroyPipeline(s_vk.device, s_pw.map[i].pipe, NULL);
        s_pw.map[i].pipe = VK_NULL_HANDLE;
        s_pw.map[i].st = VK_PW_FAILED;
    }
    vk_pw_drop_modules();
    for (u32 i = 0; i < s_pw.modcap; i++) { free(s_pw.mods[i].w); s_pw.mods[i].w = NULL; }
}
