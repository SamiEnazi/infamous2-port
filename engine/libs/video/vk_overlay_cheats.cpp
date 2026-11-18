/*
 * vk_overlay_cheats -- the in-game menu's Cheats page (vk_overlay.cpp shows it as
 * its last tab when the title registered cheats: include/ps3emu/ps3_cheats.h).
 * The page (vk_overlay_menu.inc, [menu-e]) lists the registered cheats by group;
 * every change applies at once through the cheat's own apply callback
 * (vk_overlay_cheat_change). "Remember cheats" (default on) keeps the ones
 * that are on in the menu's settings file and sets them again at the next start, with
 * a notice naming them; off = all off at every start.
 */
#include "../../include/ps3emu/ps3_cheats.h"

#include <stdio.h>
#include <string.h>

/* vk_overlay.cpp: the settings file was changed (saved soon after); a notice in a corner. */
extern "C" void vk_overlay_settings_changed(void);
extern "C" void vk_overlay_notice_for(const char* text, double secs);

/* "Remember cheats" (default on): the cheats that can be remembered (ps3_cheat_rememberable)
 * are kept in the menu's settings file as PS3_CHEAT_<ID>=<value> lines and set again when the
 * window opens; PS3_CHEATS_REMEMBER=0 in the file = off (nothing kept, nothing set). */
static bool s_remember = true;

/* [menu-e] The page itself is the menu's (vk_overlay_menu.inc builds a row per cheat from
 * g_ps3_cheats); what a change does stays here. */
static ps3_cheat* s_failed;   /* the last cheat whose apply said no */

/* The menu changed a cheat to `v` (an ACTION: pressed, with `v` its amount): apply it, log it,
 * tell the achievement service, keep it in the settings file. 1 = applied. */
extern "C" int vk_overlay_cheat_change(ps3_cheat* c, double v)
{
    if (!c || c->unavailable) return 0;
    c->value = v;
    const int ok = c->apply ? c->apply(c) : 0;
    fprintf(stderr, "[cheats] menu: %s = %g%s\n", c->id, c->value, ok ? "" : " (not applied)");
    ps3_cheat_note_use(c, ok);   /* RecompLauncher: later unlocks are Casual */
    s_failed = ok ? (s_failed == c ? NULL : s_failed) : c;
    if (s_remember && ps3_cheat_rememberable(c)) vk_overlay_settings_changed();
    return ok;
}
extern "C" int vk_overlay_cheat_failed(const ps3_cheat* c) { return c && s_failed == c; }
extern "C" void vk_overlay_cheats_refresh(void) { if (g_ps3_cheats.refresh) g_ps3_cheats.refresh(); }
extern "C" void vk_overlay_cheats_set_remember(int on)
{
    s_remember = on != 0;
    vk_overlay_settings_changed();
    fprintf(stderr, "[cheats] menu: remember cheats = %d\n", s_remember ? 1 : 0);
}
extern "C" int vk_overlay_cheats_count(void) { return ps3_cheats_count(); }
extern "C" int vk_overlay_cheats_remember(void) { return s_remember ? 1 : 0; }

/* The menu's timed test script: "cheat=id=value" (vk_overlay.cpp script_poll); id "remember"
 * is the Remember cheats switch. */
extern "C" int ps3_overlay_cheat_set(const char* id, double v)
{
    if (!strcmp(id, "remember")) {
        s_remember = v != 0.0;
        vk_overlay_settings_changed();
        fprintf(stderr, "[cheats] remember = %d\n", s_remember ? 1 : 0);
        return 1;
    }
    ps3_cheat* c = ps3_cheat_find(id);
    if (!c) { fprintf(stderr, "[cheats] no cheat '%s'\n", id); return 0; }
    const int ok = ps3_cheat_set(c, v);
    if (s_remember && ps3_cheat_rememberable(c)) vk_overlay_settings_changed();
    return ok;
}

/* The settings file's lines this page owns (the file writer skips them in the lines it keeps). */
extern "C" int vk_overlay_cheats_owns_key(const char* k)
{
    return !strcmp(k, "PS3_CHEATS_REMEMBER") || !strncmp(k, "PS3_CHEAT_", 10);
}

/* The settings file (vk_overlay.cpp settings_save): the switch when off, else the remembered
 * cheats that are on (ps3_cheat_is_on). */
extern "C" void vk_overlay_cheats_write(FILE* f)
{
    if (!s_remember) { fprintf(f, "PS3_CHEATS_REMEMBER=0\n"); fprintf(stderr, "[cheats] remembered: off\n"); return; }
    char name[64], line[256] = "";
    size_t n = 0;
    for (int i = 0; i < ps3_cheats_count(); i++) {
        const ps3_cheat* c = g_ps3_cheats.e[i];
        if (!ps3_cheat_rememberable(c) || !ps3_cheat_is_on(c) || c->unavailable) continue;
        ps3_cheat_env_name(c, name, sizeof name);
        fprintf(f, "%s=%g\n", name, c->value);
        if (n < sizeof line - 40) n += (size_t)snprintf(line + n, sizeof line - n, " %s=%g", c->id, c->value);
    }
    fprintf(stderr, "[cheats] remembered:%s\n", n ? line : " (none)");
}

/* The window opened (vk_overlay_attach; the file was read into the environment before main):
 * set the remembered cheats again and say which are on, for a few seconds. Their apply
 * callbacks only switch things on; the title's hooks act once the game is in play. */
extern "C" void vk_overlay_cheats_attach(void)
{
    __atomic_store_n(&g_ps3_cheats.notice, &vk_overlay_notice_for, __ATOMIC_RELEASE);   /* ps3_cheats_notice */
    if (!ps3_cheats_count()) return;
    if (g_ps3_cheats.refresh) g_ps3_cheats.refresh();
    { const char* e = getenv("PS3_CHEATS_REMEMBER"); s_remember = !(e && e[0] == '0'); }
    if (!s_remember) return;
    char name[64], on[200] = "";
    size_t n = 0;
    int k = 0;
    for (int i = 0; i < ps3_cheats_count(); i++) {
        ps3_cheat* c = g_ps3_cheats.e[i];
        if (!ps3_cheat_rememberable(c)) continue;
        ps3_cheat_env_name(c, name, sizeof name);
        const char* e = getenv(name);
        if (!e || !*e) continue;
        if (!ps3_cheat_set(c, atof(e)) || !ps3_cheat_is_on(c)) continue;
        if (c->kind == PS3_CHEAT_CHOICE && c->choices && (int)c->value < c->n_choices)
            n += (size_t)snprintf(on + n, n < sizeof on ? sizeof on - n : 0, "%s%s: %s", k ? ", " : "", c->name,
                                  c->choices[(int)c->value]);
        else if (c->kind == PS3_CHEAT_TOGGLE)
            n += (size_t)snprintf(on + n, n < sizeof on ? sizeof on - n : 0, "%s%s", k ? ", " : "", c->name);
        else
            n += (size_t)snprintf(on + n, n < sizeof on ? sizeof on - n : 0, "%s%s: %g", k ? ", " : "", c->name, c->value);
        k++;
    }
    if (k) {
        char msg[240];
        snprintf(msg, sizeof msg, "Cheats on: %s", on);
        vk_overlay_notice_for(msg, 8.0);
        fprintf(stderr, "[cheats] %s (remembered; F2 -> Cheats)\n", msg);
    }
}

/* F7 (vk_overlay.cpp vk_overlay_event, menu open or closed): the first cheat marked PS3_CHEAT_HOTKEY
 * (an ACTION) is pressed, as its button on the page would be; its apply says what happened. Runs on
 * the window's event thread, not the menu's: no refresh here (`unavailable` is as the window's open
 * or the page's last draw left it), the apply must be safe on any thread. */
extern "C" void vk_overlay_cheats_hotkey(void)
{
    for (int i = 0; i < ps3_cheats_count(); i++) {
        ps3_cheat* c = g_ps3_cheats.e[i];
        if (c->kind != PS3_CHEAT_ACTION || !(c->flags & PS3_CHEAT_HOTKEY)) continue;
        if (c->unavailable) {
            char msg[160];
            snprintf(msg, sizeof msg, "F7 (%s): %s", c->name, c->unavailable);
            vk_overlay_notice_for(msg, 4.0);
            fprintf(stderr, "[cheats] F7: %s unavailable (%s)\n", c->id, c->unavailable);
            return;
        }
        fprintf(stderr, "[cheats] F7: %s\n", c->id);
        ps3_cheat_set(c, c->value);
        return;
    }
    fprintf(stderr, "[cheats] F7: no quick-key cheat in this build\n");
}
