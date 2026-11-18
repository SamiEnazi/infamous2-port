/* The layout words of ps3emu/ps3_module.h, filled the same way on both sides from that side's own
 * headers: the includer has ppu_context (the engine: runtime/ppu/ppu_context.h; the module: the
 * lift's ppu_recomp.h) and spu_context (runtime/spu/spu_context.h) defined. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ps3emu/ps3_module.h"

static inline uint32_t ps3_module_fnv(uint32_t h, const char* s)
{
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

#define PS3_MODULE_NAME_STR(n) " " #n
#define PS3_MODULE_VAR_STR(type, name, dims) " " #type " " #name " " #dims
static inline void ps3_module_layout_fill(uint32_t out[PS3_MODULE_LAYOUT_WORDS])
{
    out[0] = (uint32_t)sizeof(ppu_context);
    out[1] = (uint32_t)offsetof(ppu_context, vrsave);
    out[2] = (uint32_t)sizeof(spu_context);
    out[3] = (uint32_t)offsetof(spu_context, tramp_fn);
    out[4] = (uint32_t)sizeof(ps3_module_func);
    out[5] = ps3_module_fnv(2166136261u, PS3_MODULE_VARS(PS3_MODULE_VAR_STR, PS3_MODULE_VAR_STR)
                                         PS3_MODULE_HVARS(PS3_MODULE_VAR_STR));
    out[6] = ps3_module_fnv(2166136261u, PS3_MODULE_IMPORTS(PS3_MODULE_NAME_STR) PS3_MODULE_LIBC(PS3_MODULE_NAME_STR));
    out[7] = (uint32_t)PS3_MODULE_ABI;
}
