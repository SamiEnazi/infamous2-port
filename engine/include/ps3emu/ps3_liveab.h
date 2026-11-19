/* ps3_liveab.h -- PS3_LIVEAB: an A/B inside one game run (bench only, default off; runtime/diag/ps3_liveab.c).
 *
 * PS3_LIVEAB="<switch>=<a>|<b>[,<switch>=<a>|<b>...][;block=<n>][;settle=<n>][;start=<n>][;blocks=<n>][;order=abba|ab]
 *             [;first=a|b]"
 * flips the named run-time switches between their A and B values in blocks while the game runs, and prints one
 * [liveab] line per block (fps over the block after its settle part, CPU ms per frame per thread group, GPU ms per
 * frame with PS3RECOMP_VK_GPU_PROF, the SMU's power and clocks, frame-time p99, draws), one per A/B pair and a
 * summary ("[liveab] done"). Counts <n> are seconds, or frames with an `f` suffix (block=1200f): frames make every
 * block cover the same stretch of guest time on the frame-stepped clock. The blocks start at the world mark
 * (PS3RECOMP_ENG_WORLD_MARK's file: the pad route's anchor) + start. work/bench/liveab.py reads the lines.
 *
 * Switches (each read through a cheap accessor: one load of g_ps3_lab[], -1 = not overridden):
 *   PS3RECOMP_VK_LIGHT_CULL   the light pass (0 original, 1 tiles, 2 check)
 *   PS3_PAD_THREAD_US         the controller thread's period (>= 250; only with PS3_PAD_SDL=1)
 *   PS3_CPU_PLACE_MAIN        off|x0|0, core|1, cpu|2 (re-pins the threads at the flip; needs PS3_CPU_PLACE=rsxcore)
 *   PS3_DBG_FLIP_DELAY_US     busy wait per flip on the flipping PPU thread (the main thread), us
 *   PS3_DBG_PRESENT_DELAY_US  busy wait per present on the RSX thread, us
 *   PS3_JOBDELAY_US           the delay of PS3_JOBDELAY's job (wws_runner.c), us
 *   PS3_OVERLAY_HUD           the performance HUD (F1) off / on (vk_overlay.cpp)
 *   null                      nothing (an A/A run: the method's own noise)
 */
#ifndef PS3EMU_PS3_LIVEAB_H
#define PS3EMU_PS3_LIVEAB_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS3_LAB_LIGHT_CULL = 0,
    PS3_LAB_PAD_US,
    PS3_LAB_PLACE_MAIN,
    PS3_LAB_FLIP_DELAY,
    PS3_LAB_PRESENT_DELAY,
    PS3_LAB_JOBDELAY_US,
    PS3_LAB_NULL,
    PS3_LAB_HUD,
    PS3_LAB_N
};
/* The in-run A/B's value of each switch; -1 = not overridden (the site uses its own setting). */
extern volatile int g_ps3_lab[PS3_LAB_N];
/* 1 until the first present has read PS3_LIVEAB, then 1 = on, 0 = off: the present path calls in only while 1 */
extern volatile int g_ps3_liveab_on;
/* GPU busy time, ns (vk_gprof_collect adds each batch's when PS3RECOMP_VK_GPU_PROF is on) */
extern volatile unsigned long long g_ps3_liveab_gpu_ns;
/* events a bench switch counts (PS3_JOBDELAY: delays made), shown per frame in the block lines */
extern volatile unsigned long long g_ps3_liveab_ev;
/* the presenting thread, once per presented frame (while g_ps3_liveab_on) */
void ps3_liveab_present(unsigned draws);

#ifdef __cplusplus
}
#endif
#endif /* PS3EMU_PS3_LIVEAB_H */
