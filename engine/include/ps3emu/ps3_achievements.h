/*
 * ps3_achievements -- what the game does that an achievement service wants to
 * hear about: RecompLauncher's Recomp Achievements (libs/misc/
 * ps3_recomp_achievements.c, third_party/rl_achievements).
 *
 * The modules with something to report post an event here, from any thread:
 *   sceNpTrophy   a trophy unlocked now (NATIVE), or one the player already had
 *                 when the game started (EARLIER); and the title's trophy list
 *                 (ps3_ach_publish_defs)
 *   cellSaveData  a save was loaded (SAVE_LOADED)
 *   the Cheats page (ps3_cheats.h)  a cheat that changes play was used (CASUAL)
 * The reporter takes them once per frame, on the present thread, in the order
 * they were posted (so "save loaded" reaches the launcher before the unlocks
 * after it). Nothing listens unless the game was started by RecompLauncher; the
 * reporter then sets `off` and posting stops costing anything.
 *
 * Header-only shared state (as ps3_cheats.h / ps3_settings.h): a module that
 * posts needs no link dependency (its unit tests neither).
 */
#ifndef PS3EMU_PS3_ACHIEVEMENTS_H
#define PS3EMU_PS3_ACHIEVEMENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define PS3_ACH_SHARED __declspec(selectany)
#else
#  define PS3_ACH_SHARED __attribute__((weak))
#endif

enum {
    PS3_ACH_EV_NATIVE = 1,      /* id = the trophy id */
    PS3_ACH_EV_EARLIER,         /* id = a trophy unlocked before this session */
    PS3_ACH_EV_SAVE_LOADED,
    PS3_ACH_EV_CASUAL,          /* id = PS3_ACH_CASUAL_* */
};
enum { PS3_ACH_CASUAL_CHEATS = 0 };

typedef struct ps3_ach_def {
    uint32_t    id;
    const char* title;
    const char* description;
    uint32_t    gamerscore;     /* the Xbox-style weight the service converts to its points */
} ps3_ach_def;

enum { PS3_ACH_RING = 256 };

typedef struct {
    char               lock;
    volatile int       off;          /* no service: posts are dropped */
    volatile int       quiet;        /* the service shows its own unlock toast: hide the game's */
    unsigned           head, tail;   /* next write, next read */
    struct { int type; uint32_t id; } ev[PS3_ACH_RING];
    const ps3_ach_def* defs;         /* static storage, published once */
    int                n_defs;
} ps3_ach_state;

PS3_ACH_SHARED ps3_ach_state g_ps3_ach = { 0 };

static inline void ps3_ach_lock(void)   { while (__atomic_test_and_set(&g_ps3_ach.lock, __ATOMIC_ACQUIRE)) { } }
static inline void ps3_ach_unlock(void) { __atomic_clear(&g_ps3_ach.lock, __ATOMIC_RELEASE); }

static inline void ps3_ach_post(int type, uint32_t id)
{
    if (__atomic_load_n(&g_ps3_ach.off, __ATOMIC_ACQUIRE)) return;
    ps3_ach_lock();
    if (g_ps3_ach.head - g_ps3_ach.tail < PS3_ACH_RING) {     /* full: dropped */
        const unsigned i = g_ps3_ach.head++ % PS3_ACH_RING;
        g_ps3_ach.ev[i].type = type;
        g_ps3_ach.ev[i].id = id;
    }
    ps3_ach_unlock();
}

/* The reporter: the oldest event, 0 when none. */
static inline int ps3_ach_take(int* type, uint32_t* id)
{
    int got = 0;
    ps3_ach_lock();
    if (g_ps3_ach.tail != g_ps3_ach.head) {
        const unsigned i = g_ps3_ach.tail++ % PS3_ACH_RING;
        *type = g_ps3_ach.ev[i].type;
        *id = g_ps3_ach.ev[i].id;
        got = 1;
    }
    ps3_ach_unlock();
    return got;
}

/* `defs` must outlive the program. */
static inline void ps3_ach_publish_defs(const ps3_ach_def* defs, int n)
{
    ps3_ach_lock();
    g_ps3_ach.defs = defs;
    g_ps3_ach.n_defs = n;
    ps3_ach_unlock();
}

static inline int ps3_ach_quiet(void) { return __atomic_load_n(&g_ps3_ach.quiet, __ATOMIC_ACQUIRE); }

/* The reporter (libs/misc/ps3_recomp_achievements.c): once per presented frame,
 * from the present thread (libs/video/rsx_vulkan_backend.c). Connects on the first
 * call; PS3_RL_ACH=0 keeps it off. */
void ps3_rl_ach_frame(void);

#ifdef __cplusplus
}
#endif

#endif
