/* Windows, the game module's lifted PPU/firmware TUs (tools/build_module.py force-includes it): the
 * engine functions the lifted code calls most often go straight through their import slot -- one
 * indirect call, call [__imp_<name>], as a dllimport call would -- instead of the stub in
 * ps3_module_imports.c and its extra jmp. Each name is made the slot itself (an object-like
 * macro): the lifted code's own declaration, extern "C" void ps3_indirect_call(ppu_context*),
 * then declares the slot with the function's type, and every call through it is one indirect
 * call. Only names the lifted code declares extern (so the declaration stays a declaration).
 * (slim split, 2026-11-14) */
#pragma once
#define ps3_indirect_call (*__imp_ps3_indirect_call)
#define ps3_hle_call      (*__imp_ps3_hle_call)
#define lv2_syscall       (*__imp_lv2_syscall)
#define ppu_timebase_now  (*__imp_ppu_timebase_now)
