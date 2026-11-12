/*
 * ps3_recomp_achievements -- the game's own trophies as RecompLauncher
 * achievements (Recomp Achievements, third_party/rl_achievements; its
 * docs/SPEC.md). PS3 trophies are "native" achievements there: an unlock is
 * reported with its trophy id, the hosted list for the title says what each id
 * is worth (the platinum is left out of it).
 *
 * The events come from ps3_achievements.h (sceNpTrophy, cellSaveData, the Cheats
 * page); ps3_rl_ach_frame drains them once per frame on the present thread and
 * pumps the launcher's channel:
 *   NATIVE       rl_ach_native(id)          a trophy unlocked in this session
 *   EARLIER      rl_ach_native_earlier(id)  one the player had before (Casual)
 *   SAVE_LOADED  rl_ach_save_loaded()       unlocks right after it are Casual
 *   CASUAL       rl_ach_casual("cheats")    once a session, the first cheat used
 * plus, once, the trophy names (rl_ach_native_define, read from the game's own
 * TROPHY.TRP), and the launcher's "quiet" back into g_ps3_ach.quiet (the game's
 * own trophy notice is then hidden: the launcher shows one).
 *
 * Started without RecompLauncher (no RL_SESSION), or with PS3_RL_ACH=0, it does
 * nothing: posts are switched off on the first frame. PS3_RL_GAME_ID /
 * PS3_RL_VERSION, when set, go into the hello (rl_ach_set_info).
 */
#include "ps3emu/ps3_achievements.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PS3RECOMP_RL_ACH)
#include "rl_achievements.h"
#endif

static int s_state;          /* 0 = first frame not seen, 1 = connected, -1 = off */
static int s_defs_sent;
static int s_casual_sent;

static void drop_all(void)
{
    int type;
    uint32_t id;
    while (ps3_ach_take(&type, &id)) { }
}

void ps3_rl_ach_frame(void)
{
    if (s_state < 0) return;
#if defined(PS3RECOMP_RL_ACH)
    if (s_state == 0) {
        const char* sw = getenv("PS3_RL_ACH");
        const char* session = getenv("RL_SESSION");
        if ((sw && !strcmp(sw, "0")) || !session || !*session) {
            s_state = -1;
        } else {
            const char* game = getenv("PS3_RL_GAME_ID");
            const char* ver = getenv("PS3_RL_VERSION");
            if ((game && *game) || (ver && *ver)) rl_ach_set_info(game ? game : "", ver ? ver : "");
            s_state = rl_ach_init("ps3", NULL, NULL) ? 1 : -1;
            fprintf(stderr, "[achievements] RecompLauncher: %s\n",
                    s_state > 0 ? "connected (trophies are reported)" : "no connection (not reported)");
        }
        if (s_state < 0) {
            __atomic_store_n(&g_ps3_ach.off, 1, __ATOMIC_RELEASE);
            drop_all();
            return;
        }
    }
    if (!s_defs_sent) {
        ps3_ach_lock();
        const ps3_ach_def* d = g_ps3_ach.defs;
        const int n = g_ps3_ach.n_defs;
        ps3_ach_unlock();
        if (d && n > 0) {
            for (int i = 0; i < n; i++) rl_ach_native_define(d[i].id, d[i].title, d[i].description, d[i].gamerscore);
            s_defs_sent = 1;
        }
    }
    int type;
    uint32_t id;
    while (ps3_ach_take(&type, &id)) {
        switch (type) {
        case PS3_ACH_EV_NATIVE:      rl_ach_native(id); break;
        case PS3_ACH_EV_EARLIER:     rl_ach_native_earlier(id); break;
        case PS3_ACH_EV_SAVE_LOADED: rl_ach_save_loaded(); break;
        case PS3_ACH_EV_CASUAL:
            if (!s_casual_sent) {
                rl_ach_casual("cheats");
                s_casual_sent = 1;
                fprintf(stderr, "[achievements] a cheat was used: unlocks from now on are Casual\n");
            }
            break;
        default: break;
        }
    }
    rl_ach_frame();
    __atomic_store_n(&g_ps3_ach.quiet, rl_ach_quiet_toasts() ? 1 : 0, __ATOMIC_RELEASE);
#else
    s_state = -1;
    __atomic_store_n(&g_ps3_ach.off, 1, __ATOMIC_RELEASE);
    drop_all();
    (void)s_defs_sent; (void)s_casual_sent;
#endif
}
