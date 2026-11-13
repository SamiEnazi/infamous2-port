/*
 * ps3_title_view -- what the title's own code knows about the picture being
 * made, for the renderer: is it the player's camera (gameplay) or a cutscene /
 * scripted camera? A renderer alone cannot tell (both draw the same passes).
 *
 * The title (inFAMOUS 2: the port's cheats.cpp hooks on the game's camera
 * modes) marks, on the guest thread, each time one of its camera modes runs:
 *
 *     ps3_title_view_mark(1);   // a gameplay camera (free camera, aiming, ...)
 *     ps3_title_view_mark(0);   // any other camera (cutscenes, scripted shots)
 *
 * and the renderer asks, once a frame:
 *
 *     int g = ps3_title_view_gameplay(now_ms);  // 1 gameplay, 0 not, -1 the title says nothing
 *
 * Gameplay = a gameplay camera ran within the last PS3_TITLE_VIEW_STALE_MS and
 * no other camera ran within the last PS3_TITLE_VIEW_SETTLE_MS (inFAMOUS 2's
 * opening cutscene hands the free camera ~25 frames between two shots: still a
 * cutscene); a cutscene that runs none of the title's camera modes reads "not
 * gameplay" once the marks go stale. Times are
 * ps3_ms_now() (libs/video/cellGcmSys.c), passed in, so this header needs no
 * link dependency.
 *
 * Users: PS3RECOMP_DOF=play (rsx_draw_engine.c): the game's depth of field off
 * in gameplay, kept in cutscenes.
 *
 * Header-only shared state (one object, as in ps3_settings.h).
 */
#ifndef PS3EMU_PS3_TITLE_VIEW_H
#define PS3EMU_PS3_TITLE_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define PS3_TITLE_VIEW_SHARED __declspec(selectany)
#else
#  define PS3_TITLE_VIEW_SHARED __attribute__((weak))
#endif

enum { PS3_TITLE_VIEW_STALE_MS = 200, PS3_TITLE_VIEW_SETTLE_MS = 300 };

typedef struct {
    volatile unsigned long long play_ms;    /* the last gameplay camera frame (0 = never) */
    volatile unsigned long long other_ms;   /* the last other camera frame */
    volatile int provider;                  /* 1 once the title has marked anything */
} ps3_title_view_state;

PS3_TITLE_VIEW_SHARED ps3_title_view_state g_ps3_title_view = { 0, 0, 0 };

/* The title: one of its camera modes ran now (`now_ms` = ps3_ms_now()). */
static inline void ps3_title_view_mark(int gameplay, unsigned long long now_ms)
{
    if (gameplay) __atomic_store_n(&g_ps3_title_view.play_ms, now_ms, __ATOMIC_RELAXED);
    else          __atomic_store_n(&g_ps3_title_view.other_ms, now_ms, __ATOMIC_RELAXED);
    if (!g_ps3_title_view.provider) __atomic_store_n(&g_ps3_title_view.provider, 1, __ATOMIC_RELEASE);
}

/* The title has said anything at all (a build whose title knows its cameras). */
static inline int ps3_title_view_provided(void)
{
    return __atomic_load_n(&g_ps3_title_view.provider, __ATOMIC_ACQUIRE);
}

/* 1 = the player's camera now, 0 = another one (or none recently), -1 = the
 * title never said anything. */
static inline int ps3_title_view_gameplay(unsigned long long now_ms)
{
    if (!ps3_title_view_provided()) return -1;
    const unsigned long long p = __atomic_load_n(&g_ps3_title_view.play_ms, __ATOMIC_RELAXED);
    const unsigned long long o = __atomic_load_n(&g_ps3_title_view.other_ms, __ATOMIC_RELAXED);
    if (!p || (now_ms > p && now_ms - p > (unsigned long long)PS3_TITLE_VIEW_STALE_MS)) return 0;
    if (o && (o >= p || p - o < (unsigned long long)PS3_TITLE_VIEW_SETTLE_MS)) return 0;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
