/* The engine's layout words (ps3emu/ps3_module.h) from its own headers. (slim split, 2026-11-14) */
#include "../ppu/ppu_context.h"
#include "../spu/spu_context.h"
#include "ps3_module_layout.h"

void ps3_module_layout(uint32_t out[PS3_MODULE_LAYOUT_WORDS]) { ps3_module_layout_fill(out); }
