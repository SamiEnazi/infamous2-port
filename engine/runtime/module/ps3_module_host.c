/* Engine side of the game module interface (ps3emu/ps3_module.h): finds the game module, checks
 * that it fits this engine, binds it, runs its constructors. (slim split, 2026-11-14)
 *
 * Where the module is: PS3_GAME_MODULE=<path>, else PS3_MODULE_FILE next to the engine exe. A
 * module linked into the exe (the single-exe build) registers itself instead
 * (ps3_module_register_static) and wins. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3_module.h"
#include "ps3_module_vars.h"   /* (force-included as well; the guard makes it once) */

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <unistd.h>
#  include <limits.h>
#endif

/* ---- the kind-C variables: the engine's own ---------------------------------------------- */
extern uint8_t* vm_base;
extern int* const           g_spu_coh_armed;
extern unsigned char* const g_spu_coh_bitmap;

/* ---- the function table (the scaffold's function_table / function_table_count) -------------- */
static const ps3_module_func s_no_functions[1] = { { 0, 0, 0 } };
static const uint64_t s_no_function_count = 0;
const ps3_module_func* ps3_mod_function_table = s_no_functions;
const uint64_t* ps3_mod_function_table_count = &s_no_function_count;

/* ---- the trampoline slot: the module's thread-local; the engine's own until a module binds -- */
static void** s_engine_tramp_slot(void)
{
#if defined(_MSC_VER)
    static __declspec(thread) void* t_slot;
#else
    static __thread void* t_slot;
#endif
    return &t_slot;
}
static void** (*s_tramp_get)(void) = s_engine_tramp_slot;
#if defined(_MSC_VER)
__declspec(thread) ps3_tramp_fn* ps3_tramp_cache;
#else
__thread ps3_tramp_fn* ps3_tramp_cache;
#endif
ps3_tramp_fn* ps3_tramp_slot_slow(void)
{
    return ps3_tramp_cache = (ps3_tramp_fn*)s_tramp_get();
}

/* ---- the engine's names the module may import ----------------------------------------------- */
typedef struct { const char* name; void* addr; } eng_sym;
#define ENG_DECL(n) extern void n(void);
PS3_MODULE_IMPORTS(ENG_DECL)
#undef ENG_DECL
#define ENG_ROW(n) { #n, (void*)&n },
static const eng_sym s_engine_syms[] = { PS3_MODULE_IMPORTS(ENG_ROW) { 0, 0 } };
#undef ENG_ROW
const void* ps3_module_libc_lookup(const char* name);   /* ps3_module_libc.c */

static const void* engine_symbol(const char* name)
{
    for (const eng_sym* s = s_engine_syms; s->name; ++s)
        if (!strcmp(s->name, name)) return s->addr;
    return ps3_module_libc_lookup(name);
}

/* ---- the kind-R pointers and the kind-C variables, by name ------------------------------------ */
typedef struct { const char* name; void** engine_ptr; void* engine_var; uint32_t size; uint32_t kind; } eng_var;
#define VAR_ROW_R(type, name, dims) { #name, (void**)&ps3_mv_##name, 0, (uint32_t)sizeof(*ps3_mv_##name), PS3_MODULE_VAR_R },
#define VAR_ROW_C(type, name, dims) { #name, 0, (void*)&name, (uint32_t)sizeof(name), PS3_MODULE_VAR_C },
static const eng_var s_engine_vars[] = { PS3_MODULE_VARS(VAR_ROW_R, VAR_ROW_C) PS3_MODULE_HVARS(VAR_ROW_R) { 0, 0, 0, 0, 0 } };
#undef VAR_ROW_R
#undef VAR_ROW_C

/* ---- layout: what this engine's headers say ------------------------------------------------ */
void ps3_module_layout(uint32_t out[PS3_MODULE_LAYOUT_WORDS]);   /* ps3_module_layout.c (shared) */

static const ps3_module_info* s_mod;
static const ps3_module_info* s_static_mod;
static char s_why[1024];

void ps3_module_register_static(const ps3_module_info* m) { s_static_mod = m; }
const ps3_module_info* ps3_module_current(void) { return s_mod; }

static const ps3_module_var* module_var(const ps3_module_info* m, const char* name)
{
    for (uint32_t i = 0; i < m->n_vars; ++i)
        if (m->vars[i].name && !strcmp(m->vars[i].name, name)) return &m->vars[i];
    return 0;
}

void ps3_module_sync(void)
{
    const ps3_module_info* m = s_mod;
    if (!m) return;
    for (const eng_var* e = s_engine_vars; e->name; ++e) {
        if (e->kind != PS3_MODULE_VAR_C) continue;
        const ps3_module_var* v = module_var(m, e->name);
        if (v && v->kind == PS3_MODULE_VAR_C && v->addr && v->addr != e->engine_var) memcpy(v->addr, e->engine_var, e->size);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static int check_and_bind(const ps3_module_info* m, int is_static)
{
    char* w = s_why;
    size_t left = sizeof s_why;
#define WHY(...) do { int _n = snprintf(w, left, __VA_ARGS__); if (_n > 0 && (size_t)_n < left) { w += _n; left -= (size_t)_n; } } while (0)
    if (!m) { WHY("it has no module information"); return -1; }
    if (m->abi != PS3_MODULE_ABI || m->size != sizeof(ps3_module_info)) {
        WHY("it was built for interface %u (size %u), this engine is interface %u (size %u)",
            m->abi, m->size, (unsigned)PS3_MODULE_ABI, (unsigned)sizeof(ps3_module_info));
        return -1;
    }
    uint32_t mine[PS3_MODULE_LAYOUT_WORDS];
    ps3_module_layout(mine);
    for (int i = 0; i < PS3_MODULE_LAYOUT_WORDS; ++i)
        if (mine[i] != m->layout[i]) {
            WHY("its shared structures differ from this engine's (layout word %d: %u, engine %u)", i, m->layout[i], mine[i]);
            return -1;
        }
    /* 1. imports (a module without slots -- Linux, the single exe -- has none) */
    int missing = 0;
    for (uint32_t i = 0; i < m->n_imports; ++i) {
        const void* a = engine_symbol(m->imports[i].name);
        if (!a) { if (missing++ < 8) WHY("%sengine has no %s", missing > 1 ? ", " : "", m->imports[i].name); continue; }
        *m->imports[i].slot = (void*)a;
    }
    if (missing) { WHY(" (%d missing)", missing); return -1; }
    /* 2. the shared state: every engine variable must have the module's counterpart */
    for (const eng_var* e = s_engine_vars; e->name; ++e)
        if (e->kind == PS3_MODULE_VAR_R && !*e->engine_ptr) {   /* a definition left without PS3_MV_DEF */
            WHY("engine bug: ps3_mv_%s has no storage", e->name);
            return -1;
        }
    for (const eng_var* e = s_engine_vars; e->name; ++e) {
        const ps3_module_var* v = module_var(m, e->name);
        if (!v || (v->kind != PS3_MODULE_VAR_X && (v->size != e->size || v->kind != e->kind))) {
            WHY("shared variable %s %s", e->name, !v ? "is missing" : "differs");
            return -1;
        }
        if (v->kind == PS3_MODULE_VAR_X) {
            /* the measurement build: the module reaches the engine's own storage */
            *(void**)v->addr = e->kind == PS3_MODULE_VAR_R ? *e->engine_ptr : e->engine_var;
        } else if (e->kind == PS3_MODULE_VAR_R) {
            /* the module takes over: what the engine stored so far (its constructors, the
             * single exe's lifted constructors), then the engine points at the module's copy */
            if (v->addr != *e->engine_ptr) {
                /* g_spu_lifted_flags: the lifted TUs' constructors set their bits in the module's
                 * copy, and on Linux they ran at dlopen, before this: keep them */
                int keep = !strcmp(e->name, "g_spu_lifted_flags") ? *(int*)v->addr : 0;
                memcpy(v->addr, *e->engine_ptr, e->size);
                if (keep) *(int*)v->addr |= keep;
                __atomic_store_n(e->engine_ptr, v->addr, __ATOMIC_SEQ_CST);
            }
        } else if (v->addr && v->addr != e->engine_var) {
            memcpy(v->addr, e->engine_var, e->size);
        }
    }
    /* 3. the function table and the trampoline slot */
    if (!m->function_table || !m->function_table_count || !m->tramp_slot) { WHY("it has no function table"); return -1; }
    ps3_mod_function_table = m->function_table;
    ps3_mod_function_table_count = m->function_table_count;
    s_tramp_get = m->tramp_slot;
    ps3_tramp_cache = 0;   /* this thread asks the module again */
    s_mod = m;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* 4. the module's constructors (the lifted code's self-registration) */
    if (m->init) m->init();
    fprintf(stderr, "[module] %s game module bound: %llu lifted PPU functions, %u imports, %u shared variables; "
                    "kit %s; %s\n", is_static ? "linked-in" : "loaded",
            (unsigned long long)*m->function_table_count, m->n_imports, m->n_vars,
            m->kit ? m->kit : "?", m->build ? m->build : "");
    {   /* [next] one line: every default that is on in this build, with the switch that turns it off
         * ("-" = unset, the default; settings.ini lines the game exports later are not seen here) */
#define NV(n) (getenv(n) && *getenv(n) ? getenv(n) : "-")
        fprintf(stderr, "[next] defaults on: lean lift (PS3_MODULE_ABI %u); native job runner (PS3_NATIVE_JOBS=%s, 0 = SPURS); "
                        "native cull 208/209/210 (PS3_NATIVE_CULL=%s); native anim 218 (PS3_NATIVE_ANIM=%s); "
                        "native world tick/poll/sort (PS3_D=%s, PS3_D_TICK=%s); RSX thread split (PS3_RSX_SPLIT=%s, not when cores are scarce); "
                        "barrier batching (PS3_RSX_BARBATCH=%s, off on AMD); framebuffer fetch at 1.0 (PS3_RSX_FBF1X=%s, off on AMD or with MSAA); "
                        "sun shadow map 2x (PS3RECOMP_SHADOW_RES=%s, low-end preset 1x); off by default: MSAA (PS3RECOMP_MSAA=%s)\n",
                (unsigned)PS3_MODULE_ABI, NV("PS3_NATIVE_JOBS"), NV("PS3_NATIVE_CULL"), NV("PS3_NATIVE_ANIM"), NV("PS3_D"),
                NV("PS3_D_TICK"), NV("PS3_RSX_SPLIT"), NV("PS3_RSX_BARBATCH"), NV("PS3_RSX_FBF1X"),
                NV("PS3RECOMP_SHADOW_RES"), NV("PS3RECOMP_MSAA"));
#undef NV
    }
    return 0;
#undef WHY
}

static void fail_message(const char* path)
{
    char msg[1600];
    snprintf(msg, sizeof msg,
             "The game module could not be used:\n\n%s\n\n%s\n\n"
             "It is built on this PC from your own copy of the game. Run Setup again "
             "(or F2 > Update in the game) to build it for this version.",
             path ? path : "(linked in)", s_why);
    fprintf(stderr, "[module] FAILED: %s: %s\n", path ? path : "(linked in)", s_why);
    fflush(stderr);
#if defined(_WIN32)
    const char* quiet = getenv("PS3_NO_MSGBOX");
    if (!(quiet && *quiet == '1')) MessageBoxA(NULL, msg, "inFAMOUS 2 port", MB_OK | MB_ICONERROR);
#endif
}

#if !defined(_WIN32)
/* Linux: the module's thread-locals are initial-exec (the lifted code reads g_trampoline_fn after
 * every call: one load, as in the single exe), and an initial-exec .so must fit the static TLS
 * block glibc sets aside at start for libraries dlopen'd later -- 512 bytes by default; the SPU
 * natives keep ~155 KB of per-thread scratch. So: ask glibc for room
 * (glibc.rtld.optional_static_tls) and start this process again, once, with the same arguments.
 * PS3_STATIC_TLS_RETRY marks the second start. */
static void static_tls_restart(void)
{
    if (getenv("PS3_STATIC_TLS_RETRY")) return;
    char tun[1024];
    const char* old = getenv("GLIBC_TUNABLES");
    snprintf(tun, sizeof tun, "%s%sglibc.rtld.optional_static_tls=1048576", old && *old ? old : "", old && *old ? ":" : "");
    setenv("GLIBC_TUNABLES", tun, 1);
    setenv("PS3_STATIC_TLS_RETRY", "1", 1);
    static char buf[65536];
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    static char* argv[512];
    int argc = 0;
    for (size_t i = 0; i < n && argc < 511; i += strlen(buf + i) + 1) argv[argc++] = buf + i;
    argv[argc] = 0;
    fprintf(stderr, "[module] the game module needs more static TLS than glibc's default: starting again with %s\n", tun);
    fflush(stderr);
    fflush(stdout);
    execv("/proc/self/exe", argv);
}
#endif

int ps3_module_attach(void)
{
    if (s_mod) return 0;
    if (s_static_mod) {
        if (check_and_bind(s_static_mod, 1) == 0) return 0;
        fail_message(0);
        return -1;
    }
    const char* env = getenv("PS3_GAME_MODULE");
#if defined(_WIN32)
    wchar_t wpath[MAX_PATH * 2];
    if (env && *env) {
        MultiByteToWideChar(CP_UTF8, 0, env, -1, wpath, (int)(sizeof wpath / sizeof wpath[0]));
    } else {
        DWORD n = GetModuleFileNameW(NULL, wpath, (DWORD)(sizeof wpath / sizeof wpath[0]));
        while (n && wpath[n - 1] != L'\\' && wpath[n - 1] != L'/') --n;
        wpath[n] = 0;
        MultiByteToWideChar(CP_UTF8, 0, PS3_MODULE_FILE, -1, wpath + n, (int)(sizeof wpath / sizeof wpath[0]) - (int)n);
    }
    char upath[MAX_PATH * 4];
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, upath, (int)sizeof upath, NULL, NULL);
    /* the module's own folder first for anything it loads (it loads nothing today) */
    HMODULE h = LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        DWORD e = GetLastError();
        snprintf(s_why, sizeof s_why, e == ERROR_MOD_NOT_FOUND ? "the file is not there (error %lu)"
                                                              : "Windows could not load it (error %lu)", (unsigned long)e);
        fail_message(upath);
        return -1;
    }
    ps3_module_entry_fn entry = (ps3_module_entry_fn)(void*)GetProcAddress(h, PS3_MODULE_ENTRY);
#else
    char upath[PATH_MAX + 64];
    if (env && *env) {
        snprintf(upath, sizeof upath, "%s", env);
    } else {
        ssize_t n = readlink("/proc/self/exe", upath, PATH_MAX);
        if (n < 0) n = 0;
        while (n && upath[n - 1] != '/') --n;
        upath[n] = 0;
        snprintf(upath + n, sizeof upath - (size_t)n, "%s", PS3_MODULE_FILE);
    }
    void* h = dlopen(upath, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(s_why, sizeof s_why, "%s", dlerror());
        if (strstr(s_why, "static TLS")) static_tls_restart();   /* returns only when it cannot */
        fail_message(upath);
        return -1;
    }
    ps3_module_entry_fn entry = (ps3_module_entry_fn)dlsym(h, PS3_MODULE_ENTRY);
#endif
    if (!entry) {
        snprintf(s_why, sizeof s_why, "it has no %s entry (not a game module)", PS3_MODULE_ENTRY);
        fail_message(upath);
        return -1;
    }
    if (check_and_bind(entry(), 0) != 0) {
        fail_message(upath);
        return -1;
    }
    return 0;
}

/* ---- lookups for the port's own code (cheats, the GPU passes) ------------------------------- */
void* ps3_module_ppu_fn(uint32_t guest_addr)
{
    const ps3_module_func* t = ps3_mod_function_table;
    const uint64_t n = *ps3_mod_function_table_count;
    uint64_t lo = 0, hi = n;   /* the lifter writes the table in address order; fall back to a scan */
    while (lo < hi) {
        const uint64_t mid = (lo + hi) / 2;
        if (t[mid].addr < guest_addr) lo = mid + 1; else hi = mid;
    }
    if (lo < n && t[lo].addr == guest_addr) return (void*)t[lo].func;
    for (uint64_t k = 0; k < n; ++k)
        if (t[k].addr == guest_addr) return (void*)t[k].func;
    return 0;
}

int ps3_module_profile_dump(void)
{
    return s_mod && s_mod->profile_dump ? s_mod->profile_dump() : -1;
}
