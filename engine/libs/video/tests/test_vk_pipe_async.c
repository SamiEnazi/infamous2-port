/*
 * (stutter) vk_pipe_async.inc without a GPU: the pipeline workers, the waits,
 * steals and skips of a draw, releases while a worker is on a pipeline, the
 * recipe file (round trip, aging, damage) and the prewarm -- against a
 * stand-in for the Vulkan backend whose shader modules and pipelines are
 * counted handles that take random time to make.
 *
 * Checked: no variant is built twice for one pipeline, nothing is built from
 * a destroyed module, every module and pipeline is destroyed exactly once, a
 * draw that may not skip always gets its pipeline, a draw that may skip gets
 * it in the end, async off builds every variant on the backend's thread, and
 * a prewarmed pipeline is made from exactly what the draw would have made it
 * from (same SPIR-V words, same state).
 *
 * Built and run by test_vk_pipe_async_win.py. Exit 0 = every check passed.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#endif
#include "rsx_draw_engine.h"
#include "rsx_shader_spirv.h"

/* ---- the backend's pieces the .inc uses ---- */
#define VK_LOG(...)  (s_quiet ? (void)0 : (void)fprintf(stderr, "[RSX vulkan] " __VA_ARGS__))
static int s_quiet = 1;
#define VK_SC_VERSION      "ps3recomp-vk-spirv-2"
#define VK_SC_PATH         1024u
#define VK_SC_SAVE_EVERY   64u
#define VK_SC_SAVE_GAP_NS  2000000000ull
#define VK_SC_IDLE_NS      2000000000ull
#define VK_SC_SEED_A       14695981039346656037ull
#define VK_SC_SEED_B       0x9E3779B97F4A7C15ull
#define VK_SPV_WORDS       (256u * 1024u)
#define VK_ENG_TOPOLOGIES  5
#define VK_ENG_MAX_PIPE    512
#ifdef _WIN32
#define VK_SC_SEP '\\'
#else
#define VK_SC_SEP '/'
#endif
typedef struct { u64 a, b; } vk_sc_key;
rsx_tex_stats g_rsx_tstat;
static const char kVkScCanaryVS[] = "canary";

static u64 ps3_ft_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (u64)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}
static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static struct {
    int on, fp_done;
    u32 frag_rcp, pc_new, warned;
    u64 fp;
    VkPipelineCache pc;
    u64 pc_last_new, pc_last_save;
    unsigned long long hits, misses, spv_ns, pipes, pipe_ns, saves;
    char dir[VK_SC_PATH];
} s_sc;
static struct { int on; u32* spv; char log[4096]; } s_g;
static struct { VkDevice device; VkFormat depth_format; } s_vk;
static struct { int fbf_in; VkPipelineLayout pl; } s_ropt;
static int s_vk_indep_blend = 1, s_vk_depth_clamp_ok = 1;
static struct { VkRenderPass rp[RSX_BE_MAX_COLOR_TARGETS + 1]; } s_e2;

typedef struct vk_eng_pipe {
    int used;
    VkShaderModule vs, fs;
    rsx_be_render_state rs;
    u32 nslots, stride, rt_count;
    VkPipeline variant[VK_ENG_TOPOLOGIES][4];
    u32 cube_mask, vol_mask;
    u32 sblend;
    VkShaderModule fs_neg;
    VkPipeline variant_neg[VK_ENG_TOPOLOGIES][4];
    u32 fbf_mask;
    u8 fbf_att[16];
    u8 res_wpos;
    u8 fbf_in;
    u8 key_ok;
    vk_sc_key key[3];
    volatile u32 pa_xl, pa_fail, pa_job;
    char* pa_text[3];
    volatile u64 pa_want, pa_done;
    u32 pa_prev, pa_next;
} vk_eng_pipe;
static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];

/* ---- the stand-in driver: counted handles ---- */
#define MAXH 400000u
static volatile int  s_mod_alive[MAXH], s_pipe_alive[MAXH];
static u64           s_mod_words[MAXH];          /* hash of the SPIR-V a module was made from */
static u64           s_pipe_sig[MAXH];           /* what a pipeline was built from            */
static volatile long s_next_mod = 1, s_next_pipe = 1;
static volatile long s_fail;
static volatile long s_bg_builds, s_main_builds, s_bg_xlate;
static volatile long s_conc, s_conc_max;
static unsigned long s_main_tid;
static int s_cost_us = 300;                      /* a build's random cost: 0..2x this */

#define CHECK(c, ...) do { if (!(c)) { __atomic_add_fetch(&s_fail, 1, __ATOMIC_RELAXED); \
    fprintf(stderr, "[FAIL] %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static unsigned long tid_now(void)
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return (unsigned long)pthread_self();
#endif
}
static u32 rnd_state = 12345u;
static u32 rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }
static _Thread_local u32 t_rnd = 777u;
static u32 trnd(void) { t_rnd = t_rnd * 1664525u + 1013904223u + (u32)tid_now(); return t_rnd >> 8; }
static void spin_us(u32 us)
{
    const u64 end = ps3_ft_now() + (u64)us * 1000u;
    while (ps3_ft_now() < end) {}
}
static u32 hid(const void* h) { return (u32)((uintptr_t)h >> 4); }

static VkResult VKAPI_CALL m_create_module(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a, VkShaderModule* out)
{
    (void)d; (void)a;
    const long id = __atomic_fetch_add(&s_next_mod, 1, __ATOMIC_RELAXED);
    if ((u32)id >= MAXH) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s_mod_words[id] = vk_fnv1a64(ci->pCode, ci->codeSize, 1);
    __atomic_store_n(&s_mod_alive[id], 1, __ATOMIC_RELEASE);
    *out = (VkShaderModule)(uintptr_t)((u64)id << 4);
    return VK_SUCCESS;
}
static void VKAPI_CALL m_destroy_module(VkDevice d, VkShaderModule m, const VkAllocationCallbacks* a)
{
    (void)d; (void)a;
    const u32 id = hid(m);
    CHECK(id && id < MAXH && __atomic_load_n(&s_mod_alive[id], __ATOMIC_ACQUIRE) == 1, "module %u destroyed twice or never made", id);
    if (id < MAXH) __atomic_store_n(&s_mod_alive[id], 2, __ATOMIC_RELEASE);
}
static VkResult VKAPI_CALL m_create_pipes(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* gci,
                                          const VkAllocationCallbacks* a, VkPipeline* out)
{
    (void)d; (void)c; (void)a; (void)n;
    const long now = __atomic_add_fetch(&s_conc, 1, __ATOMIC_ACQ_REL);
    long m = __atomic_load_n(&s_conc_max, __ATOMIC_RELAXED);
    while (now > m && !__atomic_compare_exchange_n(&s_conc_max, &m, now, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    spin_us(trnd() % (2u * (u32)s_cost_us + 1u));
    const long id = __atomic_fetch_add(&s_next_pipe, 1, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&s_conc, 1, __ATOMIC_ACQ_REL);
    if ((u32)id >= MAXH) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s_pipe_sig[id] = (u64)(uintptr_t)gci->pNext;    /* the stand-in builder's signature */
    __atomic_store_n(&s_pipe_alive[id], 1, __ATOMIC_RELEASE);
    *out = (VkPipeline)(uintptr_t)((u64)id << 4);
    return VK_SUCCESS;
}
static void VKAPI_CALL m_destroy_pipe(VkDevice d, VkPipeline p, const VkAllocationCallbacks* a)
{
    (void)d; (void)a;
    const u32 id = hid(p);
    CHECK(tid_now() == s_main_tid, "a pipeline destroyed off the backend's thread");
    CHECK(id && id < MAXH && __atomic_load_n(&s_pipe_alive[id], __ATOMIC_ACQUIRE) == 1, "pipeline %u destroyed twice or never made", id);
    if (id < MAXH) __atomic_store_n(&s_pipe_alive[id], 2, __ATOMIC_RELEASE);
}
static PFN_vkCreateShaderModule      pvkCreateShaderModule = m_create_module;
static PFN_vkDestroyShaderModule     pvkDestroyShaderModule = m_destroy_module;
static PFN_vkCreateGraphicsPipelines pvkCreateGraphicsPipelines = m_create_pipes;
static PFN_vkDestroyPipeline         pvkDestroyPipeline = m_destroy_pipe;

/* ---- the shader cache's helpers (as in rsx_vulkan_backend.c) ---- */
static int vk_sc_join(char* out, size_t cap, const char* dir, const char* name)
{
    const int r = snprintf(out, cap, "%s%c%s", dir, VK_SC_SEP, name);
    return (r < 0 || (size_t)r >= cap) ? -1 : 0;
}
static int vk_sc_spv_path(char* out, size_t cap, const char* dir, vk_sc_key k)
{
    char name[48];
    snprintf(name, sizeof name, "%016llx%016llx.spv", (unsigned long long)k.a, (unsigned long long)k.b);
    return vk_sc_join(out, cap, dir, name);
}
static int vk_sc_write_file(const char* path, const void* head, size_t nhead, const void* body, size_t nbody)
{
    char tmp[VK_SC_PATH];
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)tid_now());
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = (!nhead || fwrite(head, 1, nhead, f) == nhead) && (!nbody || fwrite(body, 1, nbody, f) == nbody);
    if (fclose(f) != 0) ok = 0;
#ifdef _WIN32
    if (ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) return 0;
#else
    if (ok && rename(tmp, path) == 0) return 0;
#endif
    remove(tmp);
    return -1;
}
static int vk_sc_read_into(const char* path, void* buf, size_t cap, size_t* got)
{
    *got = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    const size_t n = fread(buf, 1, cap, f);
    const int bad = ferror(f) || (n == cap && fgetc(f) != EOF);
    fclose(f);
    if (bad) return -1;
    *got = n;
    return 0;
}
static u8* vk_sc_read_all(const char* path, size_t* size)
{
    *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    long end = -1;
    if (fseek(f, 0, SEEK_END) == 0) end = ftell(f);
    u8* buf = (end > 0 && fseek(f, 0, SEEK_SET) == 0) ? (u8*)malloc((size_t)end) : NULL;
    if (buf && fread(buf, 1, (size_t)end, f) != (size_t)end) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *size = (size_t)end;
    return buf;
}
static int vk_sc_spirv_ok(const u32* w, size_t bytes)
{
    if (bytes % 4u || bytes < 24u) return 0;
    const size_t n = bytes / 4u;
    if (w[0] != 0x07230203u || w[3] == 0u || w[4] != 0u) return 0;
    u32 last = 0;
    for (size_t i = 5; i < n;) {
        const u32 wc = w[i] >> 16;
        if (wc == 0u || wc > n - i) return 0;
        last = w[i] & 0xFFFFu;
        i += wc;
    }
    return last == 56u;
}
static u64 vk_sc_blob_hash(const u8* p, size_t n)
{
    u64 h = VK_SC_SEED_A ^ (u64)n;
    size_t i = 0;
    for (; i + 8u <= n; i += 8u) { u64 v; memcpy(&v, p + i, sizeof v); h = (h ^ v) * 1099511628211ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    h ^= h >> 32; h *= 0xD6E8FEB86659FD93ull; h ^= h >> 32;
    return h;
}
static u32 vk_sc_le32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static int vk_sc_async_on(void) { return 1; }
static void vk_sc_fingerprint(u32* w, u32 n) { (void)w; (void)n; s_sc.fp_done = 1; }
static long s_stores;
/* commits land here: the .spv for the next run's prewarm */
static void vk_sc_store(vk_sc_key key, const u32* words, u32 n)
{
    CHECK(tid_now() == s_main_tid, "a translation stored off the backend's thread");
    char path[VK_SC_PATH];
    if (vk_sc_spv_path(path, sizeof path, s_sc.dir, key) == 0 && vk_sc_write_file(path, NULL, 0, words, (size_t)n * 4u) == 0)
        s_stores++;
}
static void vk_sc_save_async(u32 why) { (void)why; s_sc.pc_new = 0; s_sc.pc_last_save = ps3_ft_now(); }
static void vk_sc_maybe_stats(const char* when) { (void)when; }
static int vk_mrt_mode(void) { return 1; }
static VkRenderPass vk_eng_render_pass(u32 n)
{
    CHECK(tid_now() == s_main_tid, "a render pass made off the backend's thread");
    if (n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (!s_e2.rp[n]) s_e2.rp[n] = (VkRenderPass)(uintptr_t)(0x1000u + n * 16u);
    return s_e2.rp[n];
}
static VkCompareOp vk_gcm_compare(u32 f) { return (f >= 0x0200u && f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) { case 0x0000: return VK_STENCIL_OP_ZERO; case 0x1E01: return VK_STENCIL_OP_REPLACE; default: return VK_STENCIL_OP_KEEP; }
}

/* glslang's stand-in: valid-looking SPIR-V whose ids carry the text's hash */
static int s_xl_us = 400;
int rsx_hlsl_to_spirv(const char* hlsl, int stage, u32* words, u32 max_words, u32* out_words, char* log, u32 log_size)
{
    (void)stage;
    *out_words = 0;
    if (strstr(hlsl, "FAIL")) { snprintf(log, log_size, "stand-in failure"); return -1; }
    if (tid_now() != s_main_tid) __atomic_add_fetch(&s_bg_xlate, 1, __ATOMIC_RELAXED);
    spin_us(trnd() % (2u * (u32)s_xl_us + 1u));
    const u64 h = vk_fnv1a64(hlsl, strlen(hlsl), 7);
    const u32 w[11] = { 0x07230203u, 0x00010000u, 0, 100, 0,
                        (5u << 16) | 54u, 1, (u32)(h | 1u), 0, (u32)(h >> 32), (1u << 16) | 56u };
    if (max_words < 11) return -1;
    memcpy(words, w, sizeof w);
    *out_words = 11;
    return 0;
}

/* ---- the backend functions the .inc calls (as in rsx_vulkan_backend.c, the
 * build itself a stand-in that checks its inputs) ---- */
typedef struct {
    VkShaderModule vs, fs;
    const rsx_be_render_state* rs;
    u32 nslots, stride, rt_count, sblend, fbf_in, topo_idx, vk_idx, neg;
} vk_ev_in;
static VkResult vk_pa_create_bg(const VkGraphicsPipelineCreateInfo* gci, VkPipeline* out);
static VkResult vk_sc_create_pipeline(const VkGraphicsPipelineCreateInfo* gci, VkPipeline* out, u32 site)
{
    (void)site;
    CHECK(tid_now() == s_main_tid, "vk_sc_create_pipeline off the backend's thread");
    s_sc.pipes++;
    return pvkCreateGraphicsPipelines(s_vk.device, s_sc.pc, 1, gci, NULL, out);
}
static u64 sig_of(const vk_ev_in* in)
{
    u64 h = vk_fnv1a64(&s_mod_words[hid(in->vs)], 8, 3);
    h = vk_fnv1a64(&s_mod_words[hid(in->fs)], 8, h);
    rsx_be_render_state rs = *in->rs;
    rs.alpha_test_enable = rs.alpha_func = rs.alpha_ref_raw = rs.alpha_ref_format = rs.rt_fp16 = 0;
    h = vk_fnv1a64(&rs, sizeof rs, h);
    const u32 v[8] = { in->nslots, in->stride, in->rt_count, in->sblend, in->fbf_in, in->topo_idx, in->vk_idx, in->neg };
    return vk_fnv1a64(v, sizeof v, h) | 1u;
}
static VkResult vk_eng_variant_build(const vk_ev_in* in, VkPipeline* out, int bg)
{
    *out = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (!bg) rp = vk_eng_render_pass(in->rt_count);
    else if (in->rt_count >= 1 && in->rt_count <= RSX_BE_MAX_COLOR_TARGETS) rp = s_e2.rp[in->rt_count];
    if (!rp || !in->vs || !in->fs || in->topo_idx >= VK_ENG_TOPOLOGIES || in->vk_idx > 3u) return VK_ERROR_INITIALIZATION_FAILED;
    CHECK(__atomic_load_n(&s_mod_alive[hid(in->vs)], __ATOMIC_ACQUIRE) == 1, "built from a dead vertex module %u", hid(in->vs));
    CHECK(__atomic_load_n(&s_mod_alive[hid(in->fs)], __ATOMIC_ACQUIRE) == 1, "built from a dead fragment module %u", hid(in->fs));
    if (bg) __atomic_add_fetch(&s_bg_builds, 1, __ATOMIC_RELAXED); else s_main_builds++;
    VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .pNext = (const void*)(uintptr_t)sig_of(in), .renderPass = rp };
    const VkResult r = bg ? vk_pa_create_bg(&gci, out) : vk_sc_create_pipeline(&gci, out, 0);
    if (r != VK_SUCCESS) *out = VK_NULL_HANDLE;
    return r;
}
static void vk_ev_from(const vk_eng_pipe* P, u32 topo_idx, u32 vk_idx, int neg, vk_ev_in* in)
{
    in->vs = __atomic_load_n(&P->vs, __ATOMIC_ACQUIRE);
    in->fs = neg ? __atomic_load_n(&P->fs_neg, __ATOMIC_ACQUIRE) : __atomic_load_n(&P->fs, __ATOMIC_ACQUIRE);
    in->rs = &P->rs;
    in->nslots = P->nslots; in->stride = P->stride; in->rt_count = P->rt_count;
    in->sblend = P->sblend; in->fbf_in = P->fbf_in;
    in->topo_idx = topo_idx; in->vk_idx = vk_idx; in->neg = neg ? 1u : 0u;
}
static VkPipeline vk_eng_variant_now(vk_eng_pipe* P, u32 topo_idx, u32 vk_idx, int neg, VkPipeline* slotp)
{
    if (neg && !P->fs_neg) return VK_NULL_HANDLE;
    VkRenderPass rp = vk_eng_render_pass(P->rt_count);
    if (!rp) return VK_NULL_HANDLE;
    vk_ev_in in;
    vk_ev_from(P, topo_idx, vk_idx, neg, &in);
    VkPipeline p = VK_NULL_HANDLE;
    if (vk_eng_variant_build(&in, &p, 0) != VK_SUCCESS) p = VK_NULL_HANDLE;
    __atomic_store_n(slotp, p, __ATOMIC_RELEASE);
    return p;
}
static VkPipeline vk_pa_variant(vk_eng_pipe* P, u32 topo_idx, u32 vk_idx, int neg, VkPipeline* slotp);
static VkPipeline vk_eng_variant_ex(vk_eng_pipe* P, u32 topo_idx, u32 vk_idx, int neg)
{
    VkPipeline* slotp = neg ? &P->variant_neg[topo_idx][vk_idx] : &P->variant[topo_idx][vk_idx];
    const VkPipeline have = __atomic_load_n(slotp, __ATOMIC_ACQUIRE);
    if (have) return have;
    return vk_pa_variant(P, topo_idx, vk_idx, neg, slotp);
}

#include "../vk_pipe_async.inc"

/* ---- the scenario ---- */
static u32 s_gen[VK_ENG_MAX_PIPE];

static vk_sc_key key_of(const char* text, int stage)
{
    vk_sc_key k = { vk_fnv1a64(text, strlen(text), VK_SC_SEED_A) ^ (u64)stage, vk_fnv1a64(text, strlen(text), VK_SC_SEED_B) };
    return k;
}
/* a translation on the backend's thread, stored in the cache as the real one is */
static void make_module(VkShaderModule* m, const char* text, int stage)
{
    u32 w[16], n = 0;
    if (rsx_hlsl_to_spirv(text, stage, w, 16, &n, s_g.log, sizeof s_g.log) == 0) {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n * 4u, .pCode = w };
        pvkCreateShaderModule(s_vk.device, &ci, NULL, m);
        vk_sc_store(key_of(text, stage), w, n);
    }
}
/* A pipeline as vk_eng_pipeline_create_body leaves it: modules of the stages the
 * cache had, the others deferred (texts), the keys for the recipes. */
static void make_pipe(u32 slot, u32 seed, int defer, int fail)
{
    vk_eng_pipe* P = &s_epipe[slot];
    memset(P, 0, sizeof *P);
    char vs[64], fs[64];
    snprintf(vs, sizeof vs, "vs %u", seed % 37u);          /* shared vertex programs */
    snprintf(fs, sizeof fs, "fs %u%s", seed, fail ? " FAIL" : "");
    memset(&P->rs, 0, sizeof P->rs);
    P->rs.depth_test = (seed % 3u) != 0;                    /* two thirds may skip */
    P->rs.depth_func = 0x0203u;
    P->rs.blend_enable = seed & 1u;
    P->rs.color_mask = 0x01010101u;
    if (seed % 11u == 0) { P->rs.stencil_enable = 1; P->rs.s_write_mask = 0xFF; P->rs.s_zpass = 0x1E01u; }  /* a mark */
    P->rs.alpha_ref_raw = trnd();                           /* not part of a recipe */
    P->nslots = 1 + seed % 5u; P->stride = 16u * P->nslots; P->rt_count = 1 + seed % 3u;
    P->sblend = (seed % 17u == 0) ? 1u : 0u;
    P->key[0] = key_of(vs, 0); P->key[1] = key_of(fs, 1); P->key_ok = 3;
    if (P->sblend) { char fn[80]; snprintf(fn, sizeof fn, "%s neg", fs); P->key[2] = key_of(fn, 1); P->key_ok |= 4; make_module(&P->fs_neg, fn, 1); }
    if (defer && !P->sblend && vk_pa_xlate_on()) {
        P->pa_text[0] = vk_pa_strdup(vs); P->pa_text[1] = vk_pa_strdup(fs);
        P->pa_xl = 3;
    } else {
        make_module(&P->vs, vs, 0);
        make_module(&P->fs, fs, 1);
        if (!P->fs) P->pa_fail = 1;
    }
    P->used = 1;
    s_gen[slot]++;
    if (P->pa_xl) vk_pa_queue(P);
}
/* vk_eng_pipeline_release */
static void release_pipe(u32 slot)
{
    vk_eng_pipe* P = &s_epipe[slot];
    if (!P->used) return;
    vk_pa_forget(P);
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
        for (u32 k = 0; k < 4; k++) {
            if (P->variant[t][k]) pvkDestroyPipeline(s_vk.device, P->variant[t][k], NULL);
            if (P->variant_neg[t][k]) pvkDestroyPipeline(s_vk.device, P->variant_neg[t][k], NULL);
        }
    if (P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
    if (P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
    if (P->fs_neg) pvkDestroyShaderModule(s_vk.device, P->fs_neg, NULL);
    memset(P, 0, sizeof *P);
}
/* One draw: what vk_eng_draw_ex_body does with the variant. */
static long s_draw_null_unskippable, s_draw_null_skippable;
static int draw(u32 slot, u32 t, u32 k)
{
    vk_eng_pipe* P = &s_epipe[slot];
    if (!P->used) return 0;
    VkPipeline p = vk_eng_variant_ex(P, t, k, 0);
    if (p) {
        const u32 id = hid(p);
        CHECK(__atomic_load_n(&s_pipe_alive[id], __ATOMIC_ACQUIRE) == 1, "a draw got a dead pipeline");
        if (P->sblend) { VkPipeline n = vk_eng_variant_ex(P, t, k, 1); CHECK(n != VK_NULL_HANDLE, "no negative half"); }
        return 1;
    }
    if (P->pa_fail) return 0;
    if (vk_pa_skippable(P, 0)) s_draw_null_skippable++; else s_draw_null_unskippable++;
    return 0;
}
static void reset_all(const char* dir, int mode)
{
    vk_pa_stop();
    for (u32 s = 0; s < VK_ENG_MAX_PIPE; s++) release_pipe(s);
    vk_pa_join(5000);
    CHECK(s_pa.nrun == 0 || !s_pa.started, "a worker did not stop");
    while (__atomic_load_n(&s_pw.saving, __ATOMIC_ACQUIRE)) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    free(s_pw.rec); free(s_pw.set); free(s_pw.map); free(s_pw.order); free(s_pw.mods);
    while (s_pa.commits) { vk_pa_commit* c = s_pa.commits; s_pa.commits = c->next; free(c); }
    memset(&s_pa, 0, sizeof s_pa);
    memset(&s_pw, 0, sizeof s_pw);
    s_sc.on = 1; s_sc.fp_done = 0; s_sc.pc_new = 0;
    snprintf(s_sc.dir, sizeof s_sc.dir, "%s", dir);
    static char m[8];
    snprintf(m, sizeof m, "%d", mode);
#ifdef _WIN32
    SetEnvironmentVariableA("PS3RECOMP_VK_PIPE_ASYNC", m);
    _putenv_s("PS3RECOMP_VK_PIPE_ASYNC", m);
#else
    setenv("PS3RECOMP_VK_PIPE_ASYNC", m, 1);
#endif
}

static int count_alive(volatile int* a, long n)
{
    int c = 0;
    for (long i = 1; i < n && i < (long)MAXH; i++) c += a[i] == 1;
    return c;
}

/* A play session: pipelines appear in bursts ("camera cuts"), draws ask for
 * variants, some pipelines are released and their slots reused. */
static void session(int mode, int frames, int release, long* skipped_out, long* main_builds_out)
{
    const long mb0 = s_main_builds;
    u32 live = 0;
    for (int f = 0; f < frames; f++) {
        vk_pa_frame();
        if (f % 7 == 0) {                                   /* a cut: new pipelines */
            for (u32 i = 0; i < 12u && live < VK_ENG_MAX_PIPE; i++, live++)
                make_pipe(live, live * 2654435761u % 100000u, (live % 2u) == 0, live % 97u == 50u);
        }
        for (u32 d = 0; d < 40u && live; d++) {
            const u32 s = rnd() % live;
            draw(s, 3u + rnd() % 2u, rnd() % 4u);
        }
        if (release && f % 13 == 5 && live > 20u) {         /* a release (+ the slot made again) */
            const u32 s = rnd() % live;
            release_pipe(s);
            make_pipe(s, s * 7919u + (u32)f, 1, 0);
        }
    }
    /* a later frame: every pipeline of a draw that may skip is ready by now */
    for (int tries = 0; tries < 2000; tries++) {
        vk_pa_frame();
        int pending = 0;
        for (u32 s = 0; s < live; s++) {
            vk_eng_pipe* P = &s_epipe[s];
            if (!P->used || P->pa_fail) continue;
            for (u32 t = 3; t < 5; t++) for (u32 k = 0; k < 4; k++)
                if ((__atomic_load_n(&P->pa_want, __ATOMIC_ACQUIRE) >> vk_pa_bit(t, k, 0)) & 1u)
                    pending += vk_eng_variant_ex(P, t, k, 0) == VK_NULL_HANDLE;
        }
        if (!pending) break;
        if (tries == 1999) CHECK(0, "%d variants never became ready", pending);
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    vk_pw_save();
    *skipped_out = (long)s_pa.skip_n;
    *main_builds_out = s_main_builds - mb0;
    (void)mode;
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    s_main_tid = tid_now();
    s_vk.device = (VkDevice)(uintptr_t)0x10;
    s_vk.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    s_g.on = 1;
    s_g.spv = (u32*)malloc(VK_SPV_WORDS * 4u);
    s_ropt.pl = (VkPipelineLayout)(uintptr_t)0x20;
    char dir[512];
    const char* base = getenv("TEST_DIR");
    snprintf(dir, sizeof dir, "%s", base && *base ? base : ".");
    char rcp[600];
    snprintf(rcp, sizeof rcp, "%s%cpipelines.rcp", dir, VK_SC_SEP);
    remove(rcp);
    if (getenv("TEST_VERBOSE")) s_quiet = 0;

    /* 1. async, empty cache: draws that may not skip always draw; the others
     *    are late at most; nothing built twice; releases while workers run */
    reset_all(dir, 1);
    long skipped = 0, mainb = 0;
    session(1, 300, 1, &skipped, &mainb);
    printf("[INFO] async: %ld draws skipped, %ld built on the backend thread, %ld on workers, %ld translations on "
           "workers, max %ld builds at once, %llu draws waited (worst %.2f ms)\n",
           skipped, mainb, s_bg_builds, s_bg_xlate, s_conc_max, s_pa.wait_n, (double)s_pa.wait_worst / 1e6);
    CHECK(s_draw_null_unskippable == 0, "%ld draws that may not skip got no pipeline", s_draw_null_unskippable);
    CHECK(s_bg_builds > 0, "no worker built anything");
    CHECK(s_bg_xlate > 0, "no worker translated anything");
    CHECK(s_pa.nrun >= 1, "no worker running");
    printf("%s async session\n", s_fail ? "[FAIL]" : "[PASS]");
    const long fail_after_1 = s_fail;

    /* the variants of run 1, by signature, for the prewarm check */
    vk_pw_save();
    const u32 recipes_1 = s_pw.nrec;
    CHECK(recipes_1 > 50u, "only %u recipes recorded", recipes_1);
    /* 2. async off: every variant built on the backend's thread, no skip;
     *    the recipes of run 1 are prewarmed meanwhile (workers) */
    const long bg0 = s_bg_builds;
    reset_all(dir, 0);
    s_draw_null_unskippable = s_draw_null_skippable = 0;
    rnd_state = 999u;
    session(0, 150, 0, &skipped, &mainb);
    CHECK(skipped == 0, "async off skipped %ld draws", skipped);
    CHECK(s_draw_null_unskippable == 0 && s_draw_null_skippable == 0, "async off: draws without a pipeline");
    printf("[INFO] async off: %ld built on the backend thread, %llu prewarm hits, %llu of %u recipes prewarmed\n",
           mainb, s_pa.pw_hit, s_pw.built, s_pw.npw);
    CHECK(s_pw.npw == recipes_1, "prewarm planned %u of %u recipes", s_pw.npw, recipes_1);
    (void)bg0;
    printf("%s async off + prewarm\n", s_fail != fail_after_1 ? "[FAIL]" : "[PASS]");
    const long fail_after_2 = s_fail;

    /* 3. the prewarm makes what the draw makes: run 1's sessions again, same
     *    seeds; every prewarmed pipeline a draw takes has the signature the draw
     *    would have built (checked in vk_pw_take's taker below) */
    reset_all(dir, 1);
    rnd_state = 12345u;
    /* wait for the prewarm, then replay */
    vk_pa_start();
    for (int i = 0; i < 20000 && !s_pw.done; i++) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    CHECK(s_pw.done, "the prewarm did not finish");
    printf("[INFO] prewarm: %llu built, %llu failed, %llu missing shaders of %u\n", s_pw.built, s_pw.failed, s_pw.missing, s_pw.npw);
    CHECK(s_pw.built > 0, "nothing prewarmed");
    const long main0 = s_main_builds;
    u32 live = 0, hits_same = 0, hits_diff = 0;
    for (int f = 0; f < 60; f++) {
        vk_pa_frame();
        if (f % 7 == 0)
            for (u32 i = 0; i < 12u && live < VK_ENG_MAX_PIPE; i++, live++)
                make_pipe(live, live * 2654435761u % 100000u, 0, live % 97u == 50u);
        for (u32 s = 0; s < live; s++) {
            vk_eng_pipe* P = &s_epipe[s];
            if (!P->used || P->pa_fail) continue;
            for (u32 t = 3; t < 5; t++) for (u32 k = 0; k < 4; k++) {
                const unsigned long long before = s_pa.pw_hit;
                VkPipeline p = vk_eng_variant_ex(P, t, k, 0);
                if (p && s_pa.pw_hit != before) {
                    vk_ev_in in; vk_ev_from(P, t, k, 0, &in);
                    if (s_pipe_sig[hid(p)] == sig_of(&in)) hits_same++; else hits_diff++;
                }
            }
        }
    }
    printf("[INFO] replay: %u prewarm pipelines taken with the draw's own inputs, %u with other inputs, %ld built here\n",
           hits_same, hits_diff, s_main_builds - main0);
    CHECK(hits_same > 50u, "few prewarm hits (%u)", hits_same);
    CHECK(hits_diff == 0u, "%u prewarmed pipelines differ from what the draw would build", hits_diff);
    printf("%s prewarm = live\n", s_fail != fail_after_2 ? "[FAIL]" : "[PASS]");
    const long fail_after_3 = s_fail;

    /* 4. the recipe file: aging and damage */
    vk_pw_save();
    {
        size_t n = 0;
        u8* f = vk_sc_read_all(rcp, &n);
        CHECK(f && n > VK_PW_HEADER, "no recipe file");
        if (f && n > VK_PW_HEADER) {
            const u32 cnt = vk_sc_le32(f + 20), run = vk_sc_le32(f + 24);
            /* every record 31 or 32 runs old */
            for (u32 i = 0; i < cnt; i++) {
                vk_pw_rec r; memcpy(&r, f + VK_PW_HEADER + (size_t)i * sizeof r, sizeof r);
                r.last_run = run - ((i & 1u) ? 31u : 32u) + 1u;   /* the next run is run + 1 */
                memcpy(f + VK_PW_HEADER + (size_t)i * sizeof r, &r, sizeof r);
            }
            const u64 h = vk_sc_blob_hash(f + VK_PW_HEADER, n - VK_PW_HEADER);
            memcpy(f + 32, &h, sizeof h);
            vk_sc_write_file(rcp, NULL, 0, f, n);
            reset_all(dir, 1);
            vk_pa_start();
            const u32 loaded = s_pw.nrec;
            CHECK(loaded == cnt, "loaded %u of %u", loaded, cnt);
            s_pw.dirty = 1;
            vk_pw_save();
            size_t n2 = 0;
            u8* f2 = vk_sc_read_all(rcp, &n2);
            const u32 kept = f2 ? vk_sc_le32(f2 + 20) : 0;
            CHECK(kept == cnt / 2u, "aging kept %u of %u (want %u)", kept, cnt, cnt / 2u);
            printf("[INFO] aging: %u of %u recipes kept (used 31 runs ago kept, 32 dropped)\n", kept, cnt);
            if (f2 && n2 > VK_PW_HEADER + 10u) {
                f2[VK_PW_HEADER + 7] ^= 0x5Au;               /* damage */
                vk_sc_write_file(rcp, NULL, 0, f2, n2);
                reset_all(dir, 1);
                vk_pa_start();
                CHECK(s_pw.nrec == 0, "a damaged file gave %u recipes", s_pw.nrec);
            }
            free(f2);
        }
        free(f);
    }
    printf("%s recipe file\n", s_fail != fail_after_3 ? "[FAIL]" : "[PASS]");

    /* 5. teardown: every module and pipeline destroyed exactly once */
    vk_pa_stop();
    for (u32 s = 0; s < VK_ENG_MAX_PIPE; s++) release_pipe(s);
    const int mods = count_alive(s_mod_alive, s_next_mod), pipes = count_alive(s_pipe_alive, s_next_pipe);
    CHECK(mods == 0, "%d modules never destroyed", mods);
    CHECK(pipes == 0, "%d pipelines never destroyed", pipes);
    printf("%s teardown (%ld modules, %ld pipelines made)\n", (mods || pipes) ? "[FAIL]" : "[PASS]",
           s_next_mod - 1, s_next_pipe - 1);
    printf(s_fail ? "FAILED (%ld)\n" : "ALL PASSED\n", s_fail);
    fflush(stdout);
#ifdef _WIN32
    ExitProcess(s_fail ? 1u : 0u);                         /* no atexit: the workers are stopped */
#else
    _exit(s_fail ? 1 : 0);
#endif
}
