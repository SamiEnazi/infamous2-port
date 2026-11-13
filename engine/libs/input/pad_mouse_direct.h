/*
 * pad_mouse_direct -- the mouse camera's direct mode and the synthetic mouse of
 * the tests (libs/input/cellPad.c; pure logic, tests/test_pad_mouse_direct.c).
 *
 * Direct mode (PS3_MOUSE_MODE=direct, [mouse] mode = "direct"): a title whose
 * camera code can take the mouse itself (inFAMOUS 2: the port's cheats.cpp hook
 * on the game's camera update) calls cellPad_host_mouse_direct_take() every
 * time its camera updates and turns the camera by the motion it gets back,
 * yaw += dx * k, pitch += dy * k. While it does that, the pad poll leaves the
 * motion alone and the mouse moves no stick: cellPad sees a "claim" that is at
 * most PMD_CLAIM_US old. When the title stops taking (a menu, a cutscene, the
 * map, a build without the hook), the claim lapses and the mouse is a right
 * stick again, as in "stick" mode; the motion that piled up meanwhile is
 * dropped once, so nothing lands on the stick as one big jump.
 *
 * The tee (GitHub #27, 2026-11-13): a title that also hides the mouse from
 * its own camera's stick read (cellPad_host_mouse_direct_provider(2): it
 * zeroes the right-stick axes the mouse drove, cellPad_host_mouse_stick_axes,
 * where that camera reads the stick) keeps the mouse a right stick for the
 * rest of the game while its camera turns by the motion: every count goes to
 * two shares, the pad poll's (the stick, exactly as in "stick" mode) and the
 * title's. So another camera that reads the stick turns as in "stick" mode,
 * and whatever waits for the stick to move (a tutorial prompt, the pad's own
 * "stick last moved" time and magnitude history) sees it move. The title's
 * share fills only while the claim is held and is emptied once when it
 * lapses, so the motion of a menu or a cutscene never turns the camera later.
 * Provider 1 (a title that only takes): the claim alone, as above.
 *
 * Synthetic mouse (PS3_MOUSE_SCRIPT, tests only: a harness window gets no real
 * mouse): "<t>:<dx>:<dy>[:<dur>],..." -- dx, dy mouse counts (pixels of raw
 * motion), at t seconds, or spread evenly over dur seconds from t. "w<t>" =
 * seconds after the file PAD_SCRIPT_WAIT appears (as PAD_SCRIPT). The counts
 * go where real motion goes (g_pad_mouse_dx/dy, 1/16 px), so both modes see
 * them the way they see a mouse.
 */
#ifndef PAD_MOUSE_DIRECT_H
#define PAD_MOUSE_DIRECT_H

#include <stdint.h>
#include <stdlib.h>

/* ---- the claim ----------------------------------------------------------- */
enum { PMD_CLAIM_US = 150000 };   /* a camera that updates at 7 fps or faster keeps it */

typedef struct {
    unsigned long long take_us;   /* when the title last took motion (0 = never) */
    int held;                     /* the last poll left the motion to the title */
} PmdClaim;

/* The title took motion at now_us. */
static inline void pmd_take(PmdClaim* c, unsigned long long now_us)
{
    c->take_us = now_us ? now_us : 1;
}
/* A pad poll at now_us with direct mode allowed (`on`): 1 = the title holds the
 * mouse (leave the motion, no stick); 0 = the stick path. *drop = 1 once, on
 * the first poll after a claim lapsed or direct mode went off: throw away the
 * motion that piled up meanwhile. */
static inline int pmd_poll(PmdClaim* c, int on, unsigned long long now_us, int* drop)
{
    *drop = 0;
    if (on && c->take_us && now_us >= c->take_us && now_us - c->take_us < PMD_CLAIM_US) {
        c->held = 1;
        return 1;
    }
    if (c->held) { c->held = 0; *drop = 1; }
    return 0;
}

/* ---- the tee (provider 2) ------------------------------------------------- */
typedef struct {
    PmdClaim claim;
    long long stick_x, stick_y;   /* 1/16 counts for the pad poll's stick */
    long long title_x, title_y;   /* 1/16 counts for the title's camera */
} PmdTee;

/* Raw motion (1/16 counts) drained at now_us, direct mode `on`: all of it to
 * the stick; to the title too while the claim is held (a take within
 * PMD_CLAIM_US); the title's share is emptied once when the claim lapses. */
static inline void pmd_tee_add(PmdTee* t, int on, unsigned long long now_us, int32_t dx, int32_t dy)
{
    int drop;
    const int feed = pmd_poll(&t->claim, on, now_us, &drop);
    if (drop) t->title_x = t->title_y = 0;
    t->stick_x += dx; t->stick_y += dy;
    if (feed) { t->title_x += dx; t->title_y += dy; }
}
/* Take a share (the stick's or the title's): its counts, the share emptied. */
static inline void pmd_tee_take(long long* x, long long* y, int32_t* ox, int32_t* oy)
{
    const long long lim = 0x7FFFFFF0LL;
    *ox = (int32_t)(*x > lim ? lim : *x < -lim ? -lim : *x);
    *oy = (int32_t)(*y > lim ? lim : *y < -lim ? -lim : *y);
    *x = *y = 0;
}
/* Which axes of port 0's right stick the mouse drove in a poll (bit 0 x,
 * bit 1 y): mouse axis mrx/mry (-1 = the mouse left it alone); a PS3_PAD_FILE
 * axis frx/fry off centre (128) wins over the mouse. */
static inline int pmd_mouse_axes(int mrx, int mry, int frx, int fry)
{
    return (mrx >= 0 && frx == 128 ? 1 : 0) | (mry >= 0 && fry == 128 ? 2 : 0);
}
/* The axes the title hides from its camera: the mouse's (`axes`, above) and, with
 * them, the other axis when nothing else moves it (rx / ry = the packet's final
 * values, 128 = centred). The game normalises the stick as a whole, so a centred
 * axis next to a deflected one reads a hair off centre (~0.004): left in, the
 * title's camera would see a stick that is not quite centred. */
static inline int pmd_mouse_axes_hide(int axes, int rx, int ry)
{
    if (!axes) return 0;
    return axes | (rx == 128 ? 1 : 0) | (ry == 128 ? 2 : 0);
}

/* ---- the synthetic mouse (tests) ----------------------------------------- */
enum { PMD_SCRIPT_MAX = 64 };

typedef struct {
    double t, dur;          /* seconds; dur 0 = all at once */
    int world;              /* t counts from the anchor file */
    double dx, dy;          /* counts */
    double fed_x, fed_y;    /* counts fed so far */
    int said;
} PmdEvent;

typedef struct {
    PmdEvent ev[PMD_SCRIPT_MAX];
    int n;
} PmdScript;

/* Parse "w<t>:<dx>:<dy>[:<dur>],..." (spaces or commas between events). */
static inline int pmd_script_parse(PmdScript* s, const char* p)
{
    s->n = 0;
    while (p && *p && s->n < PMD_SCRIPT_MAX) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        PmdEvent* e = &s->ev[s->n];
        e->world = 0; e->dur = 0; e->fed_x = e->fed_y = 0; e->said = 0;
        if (*p == 'w') { e->world = 1; p++; }
        char* q;
        e->t = strtod(p, &q);
        if (q == p || *q != ':') break;
        p = q + 1;
        e->dx = strtod(p, &q);
        if (q == p || *q != ':') break;
        p = q + 1;
        e->dy = strtod(p, &q);
        if (q == p) break;
        p = q;
        if (*p == ':') { p++; e->dur = strtod(p, &q); if (q == p) break; p = q; if (e->dur < 0) e->dur = 0; }
        s->n++;
    }
    return s->n;
}

/* Counts to add now: tw = seconds since the anchor (< 0 = not yet), tp = seconds
 * since the first poll. Whole counts only (the remainder stays for later), in
 * 1/16 units like the real motion (*ox, *oy). Returns the index of an event
 * that started now (for the log), else -1. */
static inline int pmd_script_feed(PmdScript* s, double tw, double tp, int32_t* ox, int32_t* oy)
{
    int started = -1;
    double ax = 0, ay = 0;
    for (int i = 0; i < s->n; i++) {
        PmdEvent* e = &s->ev[i];
        const double now = e->world ? tw : tp;
        if (now < 0 || now < e->t) continue;
        double f = e->dur > 0 ? (now - e->t) / e->dur : 1.0;
        if (f > 1.0) f = 1.0;
        /* whole counts due by now, minus those fed: the total is exact at the end */
        const double want_x = (double)(long long)(e->dx * f), want_y = (double)(long long)(e->dy * f);
        ax += want_x - e->fed_x; ay += want_y - e->fed_y;
        e->fed_x = want_x; e->fed_y = want_y;
        if (!e->said) { e->said = 1; started = i; }
    }
    *ox = (int32_t)(ax * 16.0);
    *oy = (int32_t)(ay * 16.0);
    return started;
}

#endif
