/*
 * ps3recomp - Track C (native/gpu): the RSX thread off the critical path.
 *
 * One place for the track's switches, its startup line, its counters and its
 * correctness checks. Every optimisation of the track has a switch
 * PS3_RSX_<NAME>=0|1 (read once; default = the table in rsx_tc.c), the
 * engine prints one "[track-c]" line at start naming what is on, and
 * PS3_RSX_STATS=<s> prints the track's counters every <s> seconds.
 *
 * PS3_RSX_DIGEST: the draws, checked.
 *   =1 / =<file>: a line per frame (draws, an order-dependent hash of every
 *       draw's state and ranges) to <file> (default: stderr every 60 frames).
 *       The hash is taken where the draw engine starts a draw (sink_end), of
 *       the 3D register file, the transform program, the constant file, the
 *       draw's packets and the scalars the engine reads.
 *   =check: as 1, and wherever a switch of this track moves work between
 *       threads or skips work, the same hash is taken on both sides and
 *       compared draw by draw (e.g. the split: the walker's instance at the
 *       draw's END event, the engine's instance when it runs it). The first
 *       differences print as loud [track-c] MISMATCH lines; the counters say
 *       how many draws were compared.
 */
#ifndef PS3RECOMP_RSX_TC_H
#define PS3RECOMP_RSX_TC_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rsx_dispatch;

enum {
    TC_LBLWAIT = 0,   /* PS3_RSX_LBLWAIT: split walker waits for the exec thread before a semaphore acquire
                         only when a queued action writes THAT label (else: every queued action, as before) */
    TC_WALKON,        /* PS3_RSX_WALKON: split walker caught up / parked while the exec thread still works:
                         keep polling put / the park inside the drain instead of syncing at once */
    TC_SPLIT,         /* PS3_RSX_SPLIT: the walker / draw-engine split (PS3RECOMP_RSX_SPLIT wins when set) */
    TC_BT,            /* PS3_RSX_BT: the backend's draw work on the recording worker (PS3RECOMP_VK_BT wins when set) */
    TC_SPWAKE,        /* PS3_RSX_SPWAKE: the split's exec thread wakes a waiting walker only once it has
                         consumed what the walker waits for (before: a wake syscall at every draw's END while
                         the walker waited -- ~8% of the exec thread at the dock) */
    TC_BARBATCH,      /* PS3_RSX_BARBATCH: Vulkan barriers the GPU does not need are left out (exact rules, see
                         rsx_vulkan_backend.c vk_bb_*): a full barrier with nothing recorded since the last one,
                         a GENERAL->GENERAL image barrier right behind a full barrier, and the barriers between
                         consecutive target copies / clears of disjoint images. Default off on AMD (vendor 0x1002) */
    TC_FBF1X,         /* PS3_RSX_FBF1X: framebuffer fetch (the decals read their own targets in the pass, no
                         copies, no pass breaks) also at render scale 1.0, where it used to copy three whole
                         targets before every decal draw. Explicit PS3RECOMP_ENG_FBFETCH wins. Default on,
                         off on AMD (its copy path stays, the #27 history) */
    TC_GROWBUF,       /* PS3_RSX_GROWBUF: program memo copies keep their buffer (grow-only) instead of a heap
                         realloc per memo miss (~170 a frame at the dock: 1.3% of the RSX thread in the heap) */
    TC_FLIPFIRST,     /* PS3_RSX_FLIPFIRST [fix07]: the split walker never walks on past a flip request. A caught-up
                         or parked walker polls for new words while the exec thread works; the title asks for
                         the flip (the import) and THEN puts the next frame's words. When both had happened
                         by the time the walker looked (the poll found the new put before it looked at the
                         flip), it walked the next frame in the same drain and the present came a frame
                         late: 2-3 double-length frames a second at the dock at 1080p. Now the flip is looked
                         at first and again after new words were seen (the title stores the flip before it
                         moves put, so words seen with no flip pending belong to the frame being drawn) */
    TC_N
};

/* 0/1, read once (PS3_RSX_<NAME>; default from the table). */
int  rsx_tc(int id);
/* The startup line (once). */
void rsx_tc_banner(void);

/* PS3_RSX_DIGEST: 0 off, 1 log, 2 check. */
int  rsx_tc_digest_mode(void);
/* The state hash of a dispatcher instance (3D registers but the two upload
 * windows, the transform program, the constants, the draw scalars). */
u64  rsx_tc_state_hash(const struct rsx_dispatch* r);
/* Packets of the draw being built: fold (kind, a, b) into the pair hash of
 * that side (side 0 = the engine's instance, 1 = the split walker's). */
void rsx_tc_pair_reset(int side);
void rsx_tc_pair_note(int side, u32 kind, u32 a, u32 b);
u64  rsx_tc_pair_hash(int side);
/* PS3_RSX_DIGEST=check: the reference hash for the draw the engine runs next
 * (set by the split's exec thread around the END event; 0 = none). */
extern u64 g_rsx_tc_ref;
/* The engine starts a draw: its digest (log) and, with a reference hash from
 * the other side (check), the comparison. ref_valid 0 = no reference. */
void rsx_tc_draw(u64 h, int ref_valid, u64 ref);
/* A frame ended (present). */
void rsx_tc_frame(void);

/* Counters (PS3_RSX_STATS=<s>, default 0 = off; the check mode prints them
 * every 10 s anyway). */
enum {
    TCC_ACQ = 0,      /* split: semaphore acquires the walker met */
    TCC_ACQ_WAIT,     /* ... of them it had to wait for the exec thread */
    TCC_ACQ_SKIP,     /* ... it did not wait (LBLWAIT: no queued write of that label) */
    TCC_WAIT_US,      /* split: microseconds the walker waited for the exec thread (any reason) */
    TCC_SYNC,         /* split: full syncs (drain ends) */
    TCC_WALKON,       /* split: drains kept going by WALKON */
    TCC_WALKON_US,    /* ... microseconds spent polling there */
    TCC_WAKES,        /* split: wake syscalls the exec thread made for the walker */
    TCC_BAR_FULL,     /* PS3_RSX_BARBATCH: full barriers recorded */
    TCC_BAR_FULL_SKIP,/* ... left out (nothing recorded since the last one) */
    TCC_BAR_IMG,      /* image barriers recorded */
    TCC_BAR_IMG_SKIP, /* ... left out (GENERAL->GENERAL right behind a full barrier) */
    TCC_BAR_CHAIN,    /* copies / clears that shared the barrier before them (disjoint images) */
    TCC_DRAWS,        /* draws digested */
    TCC_CHECKED,      /* draws compared (check mode) */
    TCC_MISMATCH,     /* ... that differed */
    TCC_N
};
void rsx_tc_count(int c, u64 n);
/* Where the words of the draw being walked come from: 0 = the main ring,
 * 0x100 | (EA >> 24) = a CALLed list at that EA (the walker sets it at CALL /
 * RET; the split carries it to the engine with each END). The engine counts
 * its draws by it (rsx_tc_src_draw), the counter line names the top sources. */
extern u32 g_rsx_tc_src;
void rsx_tc_src_draw(u32 src);
/* ...the source of the draw the split's exec thread runs now (~0 = not split) */
extern u32 g_rsx_tc_src_exec;
void rsx_tc_tick(void);   /* the counter line when due (any thread; cheap when off) */

#ifdef __cplusplus
}
#endif
#endif
