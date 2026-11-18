/* Game module side of ps3emu/ps3_module.h: the storage of the shared state the lifted code reads
 * on its hot paths, the module information and the one export. Compiled into the game module
 * with the lifted code (never into the engine's runtime library); in the single exe
 * (PS3_MODULE_STATIC) it registers itself with the engine instead of being exported.
 * (slim split, 2026-11-14) */
#include <stddef.h>
#include <stdint.h>
#include "ps3emu/ps3_module.h"   /* nothing else: the lifted headers declare some of these const */
void ps3_module_layout_lift(uint32_t out[PS3_MODULE_LAYOUT_WORDS]);   /* ps3_module_layout.c */

#if defined(_WIN32)
#  define PS3_MODULE_EXPORT __declspec(dllexport)
#  define PS3_MODULE_TLS    __declspec(thread)
#else
#  define PS3_MODULE_EXPORT __attribute__((visibility("default")))
#  define PS3_MODULE_TLS    __thread __attribute__((tls_model("initial-exec")))
#endif

/* ---- the storage (zero here: the engine hands over its values when it binds) ----------------
 * Arrays on their own cache lines (the store page map was, in the engine: every PPU store reads
 * it); the scalars together, as they are read together and written once. */
#if defined(PS3_MODULE_STATIC)
#  define DEF_C(type, name, dims) extern type name dims;   /* the engine's own variables, in the same exe */
#else
#  define DEF_C(type, name, dims) type name dims;
#endif
#define PS3_DIMS_ALIGN(dims) __attribute__((aligned(sizeof(#dims) > 1 ? 64 : 8)))
#if defined(PS3_MODULE_DATA_IMPORT)
/* the measurement build (build_module.py --data-import): no storage here, a pointer to the engine's
 * for each (the generated ps3_xdata.h, force-included everywhere, declares them and makes every
 * name (*ps3_x_<name>): the code a dllimport of the engine's variables would be) */
#  undef DEF_C
#  define DEF_R(type, name, dims) __typeof__(ps3_x_##name) ps3_x_##name = 0;
#  define DEF_C(type, name, dims) __typeof__(ps3_x_##name) ps3_x_##name = 0;
#else
#  define DEF_R(type, name, dims) type name dims PS3_DIMS_ALIGN(dims);
#endif
PS3_MODULE_VARS(DEF_R, DEF_C)
#if defined(PS3_MODULE_DATA_IMPORT)
PS3_MODULE_HVARS(DEF_R)
#else
/* the engine headers' own (selectany) definitions, in every TU that includes them */
#  define DECL_H(type, name, dims) extern type name dims;
PS3_MODULE_HVARS(DECL_H)
#  undef DECL_H
#endif
#undef DEF_R
#undef DEF_C

/* the cross-fragment trampoline pointer (ppu_recomp.h's PPU_THREAD_LOCAL g_trampoline_fn):
 * constant-initialised, so no TLS guard is ever needed for it */
PS3_MODULE_TLS void (*g_trampoline_fn)(void*) = 0;
static void** tramp_slot(void) { return (void**)&g_trampoline_fn; }

/* ---- the lift's function table ------------------------------------------------------------- */
extern const ps3_module_func function_table[];   /* = func_entry (ppu_recomp.h; this file does not see it) */
extern const uint64_t function_table_count;

/* ---- what the module tells the engine -------------------------------------------------------- */
#if defined(PS3_MODULE_DATA_IMPORT)
#  define ROW_R(type, name, dims) { #name, (void*)&ps3_x_##name, 0u, PS3_MODULE_VAR_X },
#  define ROW_C ROW_R
#else
#  define ROW_R(type, name, dims) { #name, (void*)&name, (uint32_t)sizeof(name), PS3_MODULE_VAR_R },
#  define ROW_C(type, name, dims) { #name, (void*)&name, (uint32_t)sizeof(name), PS3_MODULE_VAR_C },
#endif
static const ps3_module_var s_vars[] = { PS3_MODULE_VARS(ROW_R, ROW_C) PS3_MODULE_HVARS(ROW_R) };
#undef ROW_R
#undef ROW_C

#if defined(_WIN32) && !defined(PS3_MODULE_STATIC)
/* ps3_module_imports.c: the slots of every engine function and C library name */
extern const ps3_module_import ps3_module_import_table[];
extern const uint32_t ps3_module_import_count;
void ps3_module_run_ctors(void);   /* ps3_module_crt_win.c */
#  define MODULE_IMPORTS ps3_module_import_table
#  define MODULE_NIMPORTS ps3_module_import_count
#  define MODULE_INIT ps3_module_run_ctors
#else
#  define MODULE_IMPORTS 0
#  define MODULE_NIMPORTS 0
#  define MODULE_INIT 0      /* Linux: the dynamic linker ran the constructors; single exe: the CRT did */
#endif

#if defined(PS3_MODULE_PGO_GEN)
int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char*);
const char* __llvm_profile_get_filename(void);
static int profile_dump(void) { return __llvm_profile_write_file(); }
static const char* profile_path(const char* p) { if (p) __llvm_profile_set_filename(p); return __llvm_profile_get_filename(); }
#  define MODULE_PGO "PGO-instrumented"
#  define MODULE_PDUMP profile_dump
#  define MODULE_PPATH profile_path
#else
#  if defined(PS3_MODULE_PGO_USE)
#    define MODULE_PGO "PGO-optimised"
#  else
#    define MODULE_PGO "no PGO"
#  endif
#  define MODULE_PDUMP 0
#  define MODULE_PPATH 0
#endif
#ifndef PS3_MODULE_KIT
#  define PS3_MODULE_KIT "unknown"
#endif
#define MODULE_STR2(x) #x
#define MODULE_STR(x) MODULE_STR2(x)

static ps3_module_info s_info = {
    PS3_MODULE_ABI, (uint32_t)sizeof(ps3_module_info), { 0 },
    PS3_MODULE_KIT,
    "clang " __clang_version__ ", " MODULE_PGO,
    MODULE_IMPORTS, 0,
    s_vars, (uint32_t)(sizeof s_vars / sizeof s_vars[0]),
    function_table, &function_table_count,
    MODULE_INIT, tramp_slot, MODULE_PDUMP, MODULE_PPATH,
};

static const ps3_module_info* module_info(void)
{
    if (!s_info.layout[0]) {
        ps3_module_layout_lift(s_info.layout);
        s_info.n_imports = MODULE_NIMPORTS;
    }
    return &s_info;
}

#if defined(PS3_MODULE_STATIC)
__attribute__((constructor)) static void ps3_module_static_register(void)
{
    ps3_module_register_static(module_info());
}
#else
PS3_MODULE_EXPORT const ps3_module_info* ps3_game_module(void) { return module_info(); }
#endif
