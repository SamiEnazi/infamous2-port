/* The game module interface: the engine (a prebuilt exe) and the game module (the lifted
 * PPU/SPU code and the lifted firmware, compiled on the player's PC from their own disc into a
 * DLL / .so) meet here. (slim split, 2026-11-14)
 *
 *   engine exe  --  everything that is ours or third-party: runtime, HLE libraries, the Vulkan
 *                   backend, the port's main/cheats/GPU passes. Built and shipped by us.
 *   game module --  recompiled/, spu/, lle/, lle_spu/ of a lift, the native SPU rewrites,
 *                   spu_dispatch_mt.c and runtime/game_module/*.c. Built only on the player's PC.
 *
 * How they bind (the engine does it all, in ps3_module_attach, before any lifted code runs):
 *   1. the engine loads the module and calls its one export, ps3_game_module(), for the
 *      ps3_module_info below; abi and the layout words must equal the engine's;
 *   2. Windows: every engine function the module calls has a slot (__imp_<name>) in the module;
 *      the engine fills each by name from its own table (PS3_MODULE_IMPORTS). A name the engine
 *      does not have = refused. Linux: the dynamic linker binds them (the exe exports them).
 *   3. the shared state the lifted code reads on its hot paths (PS3_MODULE_VARS) lives in the
 *      MODULE, so the lifted code reaches it exactly as before (one RIP-relative access, the code
 *      is instruction for instruction what the single exe had). The engine reaches the module's
 *      copy through ps3_mv_<name> pointers (kind R), or keeps its own and copies it over at
 *      attach and in ps3_module_sync() (kind C: vm_base and the two coherency pointers, which are
 *      set once before the guest runs and read everywhere in the engine);
 *   4. the module's constructors (the lifted code's self-registration) run (Windows: init();
 *      Linux: at dlopen, with the imports already bound).
 * The engine works the same way with the module linked into the exe (PORT_SPLIT=0, the old
 * single exe): runtime/game_module/ps3_module_data.c then registers a static module.
 *
 * Versioning: PS3_MODULE_ABI changes whenever this file, a struct both sides share (ppu_context,
 * spu_context, func_entry, ...) or the meaning of an import changes. The layout words catch a
 * header the ABI number missed. kit = the engine source the module was built from (information
 * for the log; the updater rebuilds the module whenever the engine changes). */
#pragma once
#include <stdint.h>

#define PS3_MODULE_ABI 2   /* 2: track A (the lean-memory imports, g_ppu_icall_tbl) */

/* File names, next to the engine exe (PS3_GAME_MODULE=<path> overrides). */
#if defined(_WIN32)
#  define PS3_MODULE_FILE "Infamous2Game.dll"
#elif defined(__APPLE__)
#  define PS3_MODULE_FILE "libInfamous2Game.dylib"
#else
#  define PS3_MODULE_FILE "libInfamous2Game.so"
#endif
#define PS3_MODULE_ENTRY "ps3_game_module"

/* The shared state (see 3 above). PS3_MODULE_VARS(R, C): R(type, name, dims) = the module's
 * storage, the engine through ps3_mv_<name>; C(type, name, dims) = the engine's own variable,
 * copied into the module's. (A consumer uses name only with # and ##: the engine's
 * ps3_module_vars.h makes the R names macros.) */
#define PS3_MODULE_TOC_MAX   16384u   /* PPU_TOC_MAX_ENTRIES (ppu_loader.cpp) */
#define PS3_MODULE_SPU_PATCH 16       /* SPU_PATCH_MAX (spu_patch.h) */
typedef void (*ps3_mv_put_hook_fn)(uint32_t ea, uint32_t size);      /* g_spu_put_hook */
typedef int  (*ps3_mv_put_rsx_hook_fn)(uint32_t ea, uint32_t size);  /* g_spu_put_rsx_hook */
#define PS3_MODULE_VARS(R, C) \
    C(uint8_t*, vm_base, ) \
    C(int*, g_spu_coh_armed, ) \
    C(unsigned char*, g_spu_coh_bitmap, ) \
    R(uint32_t, ppu_vm_size, ) \
    R(int, vm_inline_ok, ) \
    R(uint32_t, ppu_hle_inject_base, ) \
    R(unsigned char, g_ppu_st_pmap, [1u << 20]) \
    R(unsigned char, g_ppu_toc_ok, [PS3_MODULE_TOC_MAX]) \
    R(volatile uint8_t, g_ppu_patch, [256]) \
    R(volatile uint8_t, g_spu_patch, [PS3_MODULE_SPU_PATCH]) \
    R(uint8_t, g_spu_stack_reset_hint, [1024]) \
    R(volatile int, g_spu_const_bi, ) \
    R(uint32_t, g_spu_depth_max_pub, ) \
    R(volatile int, g_spu_direct, ) \
    R(int, g_spu_drain_inline, ) \
    R(int, g_spu_fastch, ) \
    R(volatile int, g_spu_jobrec_mode, ) \
    R(int, g_spu_rdch_inline, ) \
    R(int, g_spu_wrch_inline, ) \
    R(int, g_spu_slot_verify, ) \
    R(volatile int, g_spu_dma_lean, ) \
    R(ps3_mv_put_hook_fn, g_spu_put_hook, ) \
    R(ps3_mv_put_rsx_hook_fn, g_spu_put_rsx_hook, ) \
    R(uint32_t, g_spu_put_rsx_lo, ) \
    R(uint32_t, g_spu_put_rsx_hi, ) \
    R(int, g_spu_put_rsx_exact, ) \
    R(unsigned char, g_vm_page_bitmap, [65536 / 8]) \
    R(uint32_t, g_barrier_sync_watch, ) \
    R(uint64_t, g_ppu_icall_tbl, [8192])
/* The same kind as R, for the diagnostic gates and mode flags the engine's SPU headers DEFINE
 * (selectany / weak: one merged copy per program in the single exe, so the lifted code and the
 * engine saw one variable): the module has the headers' definitions, the engine declares them
 * (the headers' PS3_MODULE_VARS_H branch) and defines its own storage in spu_channels.c.
 * g_spu_float_rz is set by the engine when an SPU thread switches MXCSR and read by every lifted
 * SPU float op; g_spu_lifted_flags is set by the lifted TUs' constructors and read by the engine. */
#define PS3_MODULE_HVARS(H) \
    H(int, g_spu_float_rz, ) \
    H(volatile long long, g_spu_rz_ops, ) \
    H(volatile long long, g_spu_rz_bad, ) \
    H(int, g_spu_lifted_flags, ) \
    H(int, g_spu_ls_probe, ) \
    H(int, g_spu_smc_watch, ) \
    H(int, g_wws_code_probe, ) \
    H(int, g_wws_read_probe, ) \
    H(int, g_spu_get_wide, )

/* The engine functions the module calls (Windows slots, Linux exports), and the C library
 * functions the lifted code calls (the module has no C library of its own on Windows: the
 * engine hands it its own). F(name). */
#define PS3_MODULE_IMPORTS(F) \
    F(lv2_syscall) F(ps3_hle_call) F(ps3_indirect_call) F(ps3_ms_now) \
    F(ppu_hook_register) F(ppu_patch_register) F(ppu_resv_line_note) F(ppu_stdcx64) F(ppu_stwcx32) \
    F(ppu_store_coh_notify) F(ppu_store_pm_recheck) F(ppu_timebase_now) F(ppu_toc_fold_register) \
    F(ppu_toc_write_hit) F(prx_static_register) \
    F(ppu_lean_mem_register) F(ppu_lean_st8) F(ppu_lean_st16) F(ppu_lean_st32) F(ppu_lean_st64) \
    F(vm_read8) F(vm_read16) F(vm_read32) F(vm_read64) \
    F(vm_write8) F(vm_write16) F(vm_write32) F(vm_write64) \
    F(spu_begin_image) F(spu_coh_is_reserved) F(spu_coh_notify_write_except) F(spu_drain_call) \
    F(spu_halt) F(spu_indirect_branch) F(spu_lockguard_check) F(spu_lockline_lock_mask) \
    F(spu_lockline_mask_ea) F(spu_lockline_mask_range) F(spu_lockline_unlock_mask) F(spu_lookup) \
    F(spu_native_atomic_add32) F(spu_native_dead_ls) F(spu_native_ea_ok) F(spu_native_entry) \
    F(spu_native_register) F(spu_native_replay_begin) F(spu_native_replay_end) \
    F(spu_overlay_register_region) F(spu_overlay_register_region_at) F(spu_overlay_register_region_reloc) \
    F(spu_overlay_register_sig) F(spu_patch_find) F(spu_patch_register) F(spu_rchcnt) F(spu_rdch) \
    F(spu_rdch_raw) F(spu_register_function) F(spu_register_stack_reset_entry) \
    F(spu_resident_slot_stale) F(spu_stop) F(spu_workload_register_img) F(spu_wrch) \
    F(spu_wrch_cmd_fast) F(spu_wrch_raw) F(spu_wrch_tagupd_fast)
/* What the module has from the C library and the OS (Windows: the engine's own; see
 * ps3_module_libc.c, which also names any extra the shim headers declare). */
#define PS3_MODULE_LIBC(F) \
    F(memcpy) F(memmove) F(memset) F(memcmp) F(strcmp) F(strncmp) F(strlen) F(strchr) F(strncpy) \
    F(getenv) F(atoi) F(strtol) F(strtoul) F(strtoull) F(malloc) F(calloc) F(realloc) F(free) \
    F(abort) F(exit) F(abs) F(_aligned_malloc) F(_aligned_free) \
    F(fprintf) F(printf) F(snprintf) F(vsnprintf) F(vfprintf) F(fwrite) F(fputs) F(fputc) F(puts) \
    F(fflush) F(fopen) F(fclose) F(__acrt_iob_func) \
    F(sqrt) F(sqrtf) F(exp2f) F(log2f) F(exp2) F(log2) F(expf) F(exp) F(ldexp) F(ldexpf) \
    F(frexp) F(frexpf) F(floor) F(floorf) F(ceil) F(ceilf) F(trunc) F(truncf) F(nearbyint) \
    F(nearbyintf) F(rint) F(rintf) F(fma) F(fmaf) F(fmin) F(fmax) F(fminf) F(fmaxf) F(copysign) \
    F(copysignf) F(fabs) F(fabsf) \
    F(VirtualAlloc) F(VirtualQuery)
/* Windows, a PGO-instrumented module (tools/build_module.py --pgo gen): what compiler-rt's
 * profile runtime (clang_rt.profile-x86_64.lib, linked into the module) needs besides the above. */
#define PS3_MODULE_PGO_RT(F) \
    F(CloseHandle) F(CreateEventW) F(CreateFileA) F(CreateFileMappingW) F(FlushViewOfFile) \
    F(GetComputerNameExW) F(GetLastError) F(GetNativeSystemInfo) F(GetOverlappedResult) F(LockFileEx) \
    F(MapViewOfFile) F(UnlockFile) F(UnmapViewOfFile) F(VirtualUnlock) F(WideCharToMultiByte) \
    F(__stdio_common_vfprintf) F(__stdio_common_vsprintf) F(_chsize) F(_errno) F(_fdopen) F(_fileno) \
    F(_get_osfhandle) F(_mkdir) F(_open_osfhandle) F(_putenv) F(atexit) F(fdopen) F(feof) F(fileno) \
    F(fread) F(fseek) F(ftell) F(getpid) F(open) F(strdup) F(strerror) F(strrchr)

/* The layout words: what both sides must agree on, from their own headers. */
#define PS3_MODULE_LAYOUT_WORDS 8

typedef struct ps3_module_import { const char* name; void** slot; } ps3_module_import;
typedef struct ps3_module_var { const char* name; void* addr; uint32_t size; uint32_t kind; } ps3_module_var;
#define PS3_MODULE_VAR_R 1u
#define PS3_MODULE_VAR_C 2u
#define PS3_MODULE_VAR_X 3u   /* measurement build (build_module.py --data-import): the module reaches the
                                 * ENGINE's storage through a pointer the engine fills (the dllimport form) */
/* = ppu_recomp.h's func_entry */
typedef struct ps3_module_func { uint64_t addr; void (*func)(void*); const char* name; } ps3_module_func;

typedef struct ps3_module_info {
    uint32_t abi;                              /* PS3_MODULE_ABI */
    uint32_t size;                             /* sizeof(ps3_module_info) */
    uint32_t layout[PS3_MODULE_LAYOUT_WORDS];  /* ps3_module_layout() */
    const char* kit;                           /* the engine source the module was built from */
    const char* build;                         /* compiler, flags summary, PGO state */
    const ps3_module_import* imports; uint32_t n_imports;   /* Windows: slots the engine fills */
    const ps3_module_var* vars;       uint32_t n_vars;
    const ps3_module_func* function_table;     /* the lift's function_table[] */
    const uint64_t* function_table_count;
    void (*init)(void);                        /* runs the module's constructors (once) */
    void** (*tramp_slot)(void);                /* the calling thread's g_trampoline_fn */
    int (*profile_dump)(void);                 /* PGO-instrumented module: write its counters; else 0 */
    const char* (*profile_path)(const char*);  /* PGO-instrumented module: set (non-NULL) / read the .profraw path */
} ps3_module_info;

typedef const ps3_module_info* (*ps3_module_entry_fn)(void);

#ifdef __cplusplus
extern "C" {
#endif
/* engine side (runtime/module/ps3_module_host.c) */
int  ps3_module_attach(void);                  /* 0 = bound; else a message was shown, exit */
void ps3_module_sync(void);                    /* kind C: the engine's values into the module's copies */
const ps3_module_info* ps3_module_current(void);
void ps3_module_register_static(const ps3_module_info* m);   /* the single exe (ps3_module_data.c) */
void* ps3_module_ppu_fn(uint32_t guest_addr);  /* a lifted PPU function by guest address (NULL: none) */
int  ps3_module_profile_dump(void);            /* PGO counters out, when the module is instrumented */
#ifdef __cplusplus
}
#endif
