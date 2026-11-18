/* Force-included into every ENGINE translation unit (the runtime library and the port's engine
 * sources; never into the game module): the shared state of PS3_MODULE_VARS that lives in the
 * game module (kind R) is reached through ps3_mv_<name>, a pointer to the module's copy once
 * ps3_module_attach has bound it (before that, and with no module at all -- the toolkit's own
 * tests -- to the engine's own storage, which the module then takes over). So
 *     g_spu_fastch            is  (*ps3_mv_g_spu_fastch)
 *     g_ppu_st_pmap[i]        is  (*ps3_mv_g_ppu_st_pmap)[i]     (sizeof stays the array's)
 * and every declaration the engine's headers make (extern int g_spu_fastch;) declares the pointer
 * instead. The defining file writes PS3_MV_DEF(type, name, dims, init) where it had the
 * definition. (slim split, 2026-11-14; ps3emu/ps3_module.h) */
#ifndef PS3_MODULE_VARS_H
#define PS3_MODULE_VARS_H
#include <stdint.h>
#include "ps3emu/ps3_module.h"

#ifdef __cplusplus
extern "C" {
#endif
#define PS3_MV_DECL_R(type, name, dims) extern type (*ps3_mv_##name) dims;
#define PS3_MV_DECL_C(type, name, dims)
PS3_MODULE_VARS(PS3_MV_DECL_R, PS3_MV_DECL_C)
PS3_MODULE_HVARS(PS3_MV_DECL_R)
#undef PS3_MV_DECL_R
#undef PS3_MV_DECL_C
#ifdef __cplusplus
}
#endif

/* the engine's own storage (the original definition's type, dimensions and initial value) and
 * the pointer; at file scope, in place of the old definition (inside extern "C" in C++) */
#define PS3_MV_DEF(type, name, dims, init) \
    static type ps3_mv_own_##name dims init; \
    type (*ps3_mv_##name) dims = &ps3_mv_own_##name;

#define vm_inline_ok           (*ps3_mv_vm_inline_ok)
#define ppu_vm_size            (*ps3_mv_ppu_vm_size)
#define ppu_hle_inject_base    (*ps3_mv_ppu_hle_inject_base)
#define g_ppu_st_pmap          (*ps3_mv_g_ppu_st_pmap)
#define g_ppu_toc_ok           (*ps3_mv_g_ppu_toc_ok)
#define g_ppu_patch            (*ps3_mv_g_ppu_patch)
#define g_spu_patch            (*ps3_mv_g_spu_patch)
#define g_spu_stack_reset_hint (*ps3_mv_g_spu_stack_reset_hint)
#define g_spu_const_bi         (*ps3_mv_g_spu_const_bi)
#define g_spu_depth_max_pub    (*ps3_mv_g_spu_depth_max_pub)
#define g_spu_direct           (*ps3_mv_g_spu_direct)
#define g_spu_drain_inline     (*ps3_mv_g_spu_drain_inline)
#define g_spu_fastch           (*ps3_mv_g_spu_fastch)
#define g_spu_jobrec_mode      (*ps3_mv_g_spu_jobrec_mode)
#define g_spu_rdch_inline      (*ps3_mv_g_spu_rdch_inline)
#define g_spu_wrch_inline      (*ps3_mv_g_spu_wrch_inline)
#define g_spu_slot_verify      (*ps3_mv_g_spu_slot_verify)
#define g_spu_dma_lean         (*ps3_mv_g_spu_dma_lean)
#define g_spu_put_hook         (*ps3_mv_g_spu_put_hook)
#define g_spu_put_rsx_hook     (*ps3_mv_g_spu_put_rsx_hook)
#define g_spu_put_rsx_lo       (*ps3_mv_g_spu_put_rsx_lo)
#define g_spu_put_rsx_hi       (*ps3_mv_g_spu_put_rsx_hi)
#define g_spu_put_rsx_exact    (*ps3_mv_g_spu_put_rsx_exact)
#define g_vm_page_bitmap       (*ps3_mv_g_vm_page_bitmap)
#define g_barrier_sync_watch   (*ps3_mv_g_barrier_sync_watch)
#define g_ppu_icall_tbl        (*ps3_mv_g_ppu_icall_tbl)
#define g_spu_float_rz         (*ps3_mv_g_spu_float_rz)
#define g_spu_rz_ops           (*ps3_mv_g_spu_rz_ops)
#define g_spu_rz_bad           (*ps3_mv_g_spu_rz_bad)
#define g_spu_lifted_flags     (*ps3_mv_g_spu_lifted_flags)
#define g_spu_ls_probe         (*ps3_mv_g_spu_ls_probe)
#define g_spu_smc_watch        (*ps3_mv_g_spu_smc_watch)
#define g_wws_code_probe       (*ps3_mv_g_wws_code_probe)
#define g_wws_read_probe       (*ps3_mv_g_wws_read_probe)
#define g_spu_get_wide         (*ps3_mv_g_spu_get_wide)

/* The cross-fragment trampoline pointer is a thread-local of the module (TLS cannot cross a
 * module boundary): the engine reaches the calling thread's through the module's accessor,
 * cached per thread. */
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*ps3_tramp_fn)(void*);
ps3_tramp_fn* ps3_tramp_slot_slow(void);
/* constinit in C++: it is constant-initialised (zero), which spares every access clang's
 * on-demand TLS-init guard (the same as the lifted code's PPU_TLS_CONSTINIT) */
#if defined(__cplusplus) && defined(__cpp_constinit)
#  define PS3_MV_CONSTINIT constinit
#else
#  define PS3_MV_CONSTINIT
#endif
#if defined(_MSC_VER)
extern PS3_MV_CONSTINIT __declspec(thread) ps3_tramp_fn* ps3_tramp_cache;
#else
extern PS3_MV_CONSTINIT __thread ps3_tramp_fn* ps3_tramp_cache;
#endif
static inline ps3_tramp_fn* ps3_tramp_slot(void)
{
    ps3_tramp_fn* p = ps3_tramp_cache;
    return __builtin_expect(p != 0, 1) ? p : ps3_tramp_slot_slow();
}
#ifdef __cplusplus
}
#endif
#define g_trampoline_fn (*ps3_tramp_slot())

#endif /* PS3_MODULE_VARS_H */
