/* The game module's layout words (ps3emu/ps3_module.h) from the lift's own ppu_recomp.h and the
 * kit's spu_context.h. (slim split, 2026-11-14) */
#include "ppu_recomp.h"
#include "spu_context.h"
#include "../module/ps3_module_layout.h"

void ps3_module_layout_lift(uint32_t out[PS3_MODULE_LAYOUT_WORDS]) { ps3_module_layout_fill(out); }
