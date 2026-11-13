/* The mouse camera's direct mode and the synthetic mouse (libs/input/pad_mouse_direct.h):
 * the claim holds while the title takes motion and lapses PMD_CLAIM_US after the
 * last take, dropping once; stick mode never holds; the tee (provider 2) gives the
 * stick every count and the title its own copy only while it holds the claim,
 * emptied once when the claim lapses; the mouse's stick axes; the script feeds
 * whole counts whose totals are exact, spread over a duration, from the anchor or
 * the start.
 *
 *   cc -std=gnu17 libs/input/tests/test_pad_mouse_direct.c -o /tmp/t && /tmp/t
 *   (Windows: clang -std=gnu17 libs\input\tests\test_pad_mouse_direct.c -o t.exe && t.exe)
 */
#include "../pad_mouse_direct.h"
#include <stdio.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static void test_claim(void)
{
    PmdClaim c = {0};
    int drop;
    /* never taken: the stick path, nothing to drop */
    CHECK(pmd_poll(&c, 1, 1000, &drop) == 0 && drop == 0);
    /* taken at 10 ms: held until 10 ms + PMD_CLAIM_US */
    pmd_take(&c, 10000);
    CHECK(pmd_poll(&c, 1, 10000, &drop) == 1 && drop == 0);
    CHECK(pmd_poll(&c, 1, 10000 + PMD_CLAIM_US - 1, &drop) == 1 && drop == 0);
    /* lapsed: the stick again, one drop, then no more drops */
    CHECK(pmd_poll(&c, 1, 10000 + PMD_CLAIM_US, &drop) == 0 && drop == 1);
    CHECK(pmd_poll(&c, 1, 10000 + PMD_CLAIM_US + 5000, &drop) == 0 && drop == 0);
    /* a camera at 60 fps keeps it across many polls */
    unsigned long long t = 1000000;
    int held = 0;
    for (int f = 0; f < 600; f++, t += 16667) { pmd_take(&c, t); held += pmd_poll(&c, 1, t + 2000, &drop); CHECK(drop == 0); }
    CHECK(held == 600);
    /* direct mode switched off while held: the stick at once, one drop */
    CHECK(pmd_poll(&c, 0, t, &drop) == 0 && drop == 1);
    CHECK(pmd_poll(&c, 0, t + 1, &drop) == 0 && drop == 0);
    /* stick mode: a take never holds the mouse, never drops */
    PmdClaim s = {0};
    for (int f = 0; f < 10; f++) { pmd_take(&s, 5000 + f); CHECK(pmd_poll(&s, 0, 5000 + f, &drop) == 0 && drop == 0); }
    /* a clock that goes back (another thread's earlier timestamp) does not hold */
    PmdClaim b = {0};
    pmd_take(&b, 50000);
    CHECK(pmd_poll(&b, 1, 40000, &drop) == 0);
}

static void test_tee(void)
{
    PmdTee t = {0};
    int32_t x, y;
    /* nobody took yet: everything to the stick, nothing to the title */
    pmd_tee_add(&t, 1, 1000, 160, -32);
    CHECK(t.stick_x == 160 && t.stick_y == -32 && t.title_x == 0 && t.title_y == 0);
    pmd_tee_take(&t.stick_x, &t.stick_y, &x, &y);
    CHECK(x == 160 && y == -32 && t.stick_x == 0 && t.stick_y == 0);
    /* a camera at 60 fps that takes: every count to both, totals equal; the stick's share is the pad
     * poll's (it polls between the camera's frames), the title's the camera's */
    unsigned long long now = 100000;
    long long st = 0, ti = 0, raw = 0;
    for (int f = 0; f < 120; f++, now += 16667) {
        const int32_t m1 = 16 * (f % 7), m2 = -16 * (f % 3);
        pmd_tee_add(&t, 1, now + 1000, m1, 0);          /* the pad poll drains ... */
        raw += m1;
        pmd_tee_take(&t.stick_x, &t.stick_y, &x, &y);  /* ... and takes the stick's share */
        st += x;
        pmd_take(&t.claim, now + 5000);                 /* the title's camera takes: drain, its share */
        pmd_tee_add(&t, 1, now + 5000, m2, 0);
        raw += m2;
        pmd_tee_take(&t.title_x, &t.title_y, &x, &y);
        ti += x;
    }
    pmd_tee_take(&t.stick_x, &t.stick_y, &x, &y);
    st += x;
    /* the first poll drained before the first take (frame 0: no motion yet): the title got every count */
    CHECK(st == raw);
    CHECK(ti == raw);
    /* the title stops taking (a menu): its share fills until the claim lapses, then is emptied once;
     * the stick keeps every count */
    const unsigned long long last_take = now - 16667 + 5000;
    pmd_tee_add(&t, 1, last_take + 1000, 320, 48);
    CHECK(t.title_x == 320 && t.title_y == 48 && t.stick_x == 320);
    pmd_tee_add(&t, 1, last_take + PMD_CLAIM_US, 16, 16);
    CHECK(t.title_x == 0 && t.title_y == 0);           /* lapsed: emptied, nothing new for the title */
    CHECK(t.stick_x == 336 && t.stick_y == 64);
    pmd_tee_add(&t, 1, last_take + PMD_CLAIM_US + 9000, 16, 0);
    CHECK(t.title_x == 0 && t.stick_x == 352);
    /* taking again: the title's share starts from what comes after the take */
    pmd_take(&t.claim, last_take + 1000000);
    pmd_tee_add(&t, 1, last_take + 1000000, 32, 0);
    CHECK(t.title_x == 32 && t.stick_x == 384);
    /* stick mode (direct off): the title never gets a share, even right after a take */
    PmdTee s = {0};
    pmd_take(&s.claim, 5000);
    pmd_tee_add(&s, 0, 5000, 64, 64);
    CHECK(s.title_x == 0 && s.title_y == 0 && s.stick_x == 64 && s.stick_y == 64);
    /* a share bigger than an int32 comes out clamped, not wrapped */
    PmdTee b = {0};
    b.stick_x = 0x200000000LL; b.stick_y = -0x200000000LL;
    pmd_tee_take(&b.stick_x, &b.stick_y, &x, &y);
    CHECK(x > 0x7FFF0000 && y < -0x7FFF0000 && b.stick_x == 0 && b.stick_y == 0);
}

static void test_axes(void)
{
    CHECK(pmd_mouse_axes(-1, -1, 128, 128) == 0);     /* the mouse is still */
    CHECK(pmd_mouse_axes(200, -1, 128, 128) == 1);    /* x only */
    CHECK(pmd_mouse_axes(-1, 20, 128, 128) == 2);     /* y only */
    CHECK(pmd_mouse_axes(0, 255, 128, 128) == 3);     /* both, full */
    CHECK(pmd_mouse_axes(200, 20, 0, 128) == 2);      /* a PS3_PAD_FILE x wins over the mouse */
    CHECK(pmd_mouse_axes(200, 20, 128, 255) == 1);
    /* hidden from the title's camera: the mouse's axes, and a centred other axis with them */
    CHECK(pmd_mouse_axes_hide(0, 128, 128) == 0);     /* the mouse is still: nothing hidden */
    CHECK(pmd_mouse_axes_hide(2, 128, 20) == 3);      /* mouse up, x centred: both */
    CHECK(pmd_mouse_axes_hide(1, 200, 128) == 3);     /* mouse right, y centred: both */
    CHECK(pmd_mouse_axes_hide(2, 0, 20) == 2);        /* a key or pad holds x left: x stays */
    CHECK(pmd_mouse_axes_hide(1, 200, 131) == 1);     /* a pad's y a hair off centre: its own */
    CHECK(pmd_mouse_axes_hide(3, 0, 255) == 3);
}

static void test_script(void)
{
    PmdScript s;
    CHECK(pmd_script_parse(&s, "w1:100:0, w2:-40:25:0.5,3:7:-7") == 3);
    CHECK(s.ev[0].world == 1 && s.ev[0].t == 1.0 && s.ev[0].dx == 100 && s.ev[0].dy == 0 && s.ev[0].dur == 0);
    CHECK(s.ev[1].world == 1 && s.ev[1].dx == -40 && s.ev[1].dy == 25 && s.ev[1].dur == 0.5);
    CHECK(s.ev[2].world == 0 && s.ev[2].t == 3.0 && s.ev[2].dx == 7 && s.ev[2].dy == -7);
    int32_t x, y;
    /* before the anchor: nothing, whatever the plain clock says (event 2 is plain: t=3) */
    CHECK(pmd_script_feed(&s, -1.0, 0.5, &x, &y) == -1 && x == 0 && y == 0);
    /* the instant event: all of it at once, in 1/16 units, once */
    CHECK(pmd_script_feed(&s, 1.0, 0.6, &x, &y) == 0 && x == 1600 && y == 0);
    CHECK(pmd_script_feed(&s, 1.5, 0.7, &x, &y) == -1 && x == 0 && y == 0);
    /* the spread event: whole counts as time passes, totals exact at the end */
    long long sx = 0, sy = 0;
    int started = 0;
    for (double tw = 2.0; tw <= 2.8; tw += 0.01) {
        const int i = pmd_script_feed(&s, tw, 1.0, &x, &y);
        if (i == 1) started++;
        CHECK(x % 16 == 0 && y % 16 == 0);
        CHECK(x <= 0 && y >= 0);
        sx += x; sy += y;
    }
    CHECK(started == 1);
    CHECK(sx == -40 * 16 && sy == 25 * 16);
    /* the plain-clock event at tp = 3 */
    CHECK(pmd_script_feed(&s, 3.5, 3.0, &x, &y) == 2 && x == 7 * 16 && y == -7 * 16);
    CHECK(pmd_script_feed(&s, 9.0, 9.0, &x, &y) == -1 && x == 0 && y == 0);
    /* halfway through a spread move: half of it, rounded down to whole counts */
    PmdScript h;
    pmd_script_parse(&h, "0:101:-33:1");
    pmd_script_feed(&h, 0.0, 0.0, &x, &y);
    CHECK(x == 0 && y == 0);
    pmd_script_feed(&h, 0.5, 0.5, &x, &y);
    CHECK(x == 50 * 16 && y == -16 * 16);
    pmd_script_feed(&h, 5.0, 5.0, &x, &y);
    CHECK(x == 51 * 16 && y == -17 * 16);
    /* junk ends the list where it starts */
    CHECK(pmd_script_parse(&h, "1:2:3,oops,4:5:6") == 1);
    CHECK(pmd_script_parse(&h, "") == 0);
}

int main(void)
{
    test_claim();
    test_tee();
    test_axes();
    test_script();
    printf("%s (%d failure(s))\n", s_fail ? "FAIL" : "PASS", s_fail);
    return s_fail != 0;
}
