/* Windows: the game module's import table. Every engine function and C library name the module
 * may call (ps3emu/ps3_module.h PS3_MODULE_IMPORTS, PS3_MODULE_LIBC) has
 *   __imp_<name>   a slot the engine fills when it binds the module (what a dllimport
 *                  declaration calls through: call [__imp_<name>]), and
 *   <name>         a stub that jumps through it (what a plain declaration calls),
 * exactly what an import library would have given, without one (and without naming the exe).
 * (slim split, 2026-11-14) */
#include <stdint.h>
#include "ps3emu/ps3_module.h"

#if defined(PS3_MODULE_PGO_GEN)
#  define PGO_RT(F) PS3_MODULE_PGO_RT(F)   /* the profile runtime's C library and kernel32 calls */
#else
#  define PGO_RT(F)
#endif
#define SLOT(n) void* __imp_##n = 0;
PS3_MODULE_IMPORTS(SLOT)
PS3_MODULE_LIBC(SLOT)
PGO_RT(SLOT)
#undef SLOT

/* each stub a COMDAT of its own, "any" (discard): the PGO profile runtime (clang_rt.profile, built
 * against the UCRT headers) carries its own inline fprintf / snprintf as COMDATs too, and two
 * such definitions merge where a plain one would be a duplicate symbol. Either one works: theirs
 * calls __stdio_common_vfprintf / __acrt_iob_func, which are engine slots as well. */
#define STUB(n) ".section .text$" #n ",\"xr\",discard," #n "\n.globl " #n "\n.p2align 4\n" #n ":\n  jmpq *__imp_" #n "(%rip)\n"
__asm__(".att_syntax prefix\n" PS3_MODULE_IMPORTS(STUB) PS3_MODULE_LIBC(STUB) PGO_RT(STUB) ".text\n");
#undef STUB

#define ROW(n) { #n, &__imp_##n },
const ps3_module_import ps3_module_import_table[] = { PS3_MODULE_IMPORTS(ROW) PS3_MODULE_LIBC(ROW) PGO_RT(ROW) };
#undef ROW
const uint32_t ps3_module_import_count = (uint32_t)(sizeof ps3_module_import_table / sizeof ps3_module_import_table[0]);
