/* cellPad's controller thread: the title's pad calls never wait on SDL.
 *
 * SDL_GameControllerUpdate runs SDL's device detection, and opening a
 * Bluetooth controller is a handshake that takes several hundred ms. Both
 * used to run inside cellPadGetInfo2 / cellPadGetData, on the game's main
 * thread (0.84 s, 0.73 s and 0.55 s freezes in one play session, each time a
 * controller came back). They run on cellPad's controller thread now; a pad
 * call copies its newest snapshot.
 *
 * The fake SDL of test_pad_hotplug.c, made slow: an open takes 400 ms, one
 * update (device detection) 300 ms, a rumble write 200 ms. Checked: no pad
 * call takes longer than CALL_LIMIT_MS through all of it, a button reaches
 * the title within a few ms, a controller switched off and on is opened
 * again into its port, a second one is flagged on port 1, rumble arrives,
 * and cellPadEnd closes everything.
 *
 *   cc -std=gnu17 -I include -isystem /opt/homebrew/include \
 *      -isystem /opt/homebrew/include/SDL2 libs/input/tests/test_pad_thread.c \
 *      -lpthread -o /tmp/test_pad_thread && /tmp/test_pad_thread
 *   (Windows, MinGW gcc: add -DPS3RECOMP_PAD_USE_SDL2 and SDL2's include
 *    folders, and give it sys/mman.h, setenv and unsetenv.)
 */
#define main test_pad_hotplug_main
#include "test_pad_hotplug.c"
#undef main

#define CALL_LIMIT_MS 50.0   /* far above a snapshot copy, far below the fake drivers' waits */

static double s_call_max_ms;
static void timed(unsigned long long t0)
{
    const double ms = (double)(pad_now_us() - t0) / 1000.0;
    if (ms > s_call_max_ms) s_call_max_ms = ms;
    if (ms >= CALL_LIMIT_MS) { fprintf(stderr, "a pad call took %.1f ms\n", ms); abort(); }
}
/* One "frame" of the title: cellPadGetInfo2, then cellPadGetData of a port. */
static unsigned frame(u32 port, u32* status)
{
    unsigned long long t0 = pad_now_us();
    assert(cellPadGetInfo2((CellPadInfo2*)(uintptr_t)0x1000) == CELL_OK);
    timed(t0);
    if (status) {
        const uint8_t* b = vm_base + 0x1000 + 12 + port * 4;
        *status = (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
    }
    t0 = pad_now_us();
    assert(cellPadGetData(port, (CellPadData*)(uintptr_t)0x2000) == CELL_OK);
    timed(t0);
    const uint8_t* b = vm_base + 0x2000 + 4 + CELL_PAD_BTN_OFFSET_DIGITAL2 * 2;
    return (unsigned)b[0] << 8 | b[1];
}
/* Frames, 2 ms apart, until cond() holds; ms it took, or -1 after max_ms. */
static double frames_until(int (*cond)(unsigned d2, u32 status), u32 port, int max_ms)
{
    const unsigned long long t0 = pad_now_us();
    for (;;) {
        u32 st = 0;
        const unsigned d2 = frame(port, &st);
        const double ms = (double)(pad_now_us() - t0) / 1000.0;
        if (cond(d2, st)) return ms;
        if (ms > max_ms) return -1.0;
        usleep(2000);
    }
}
static FakePad* s_want;
static int c_cross(unsigned d2, u32 st)   { (void)st; return (d2 & CROSS_BIT) != 0; }
static int c_circle(unsigned d2, u32 st)  { (void)st; return (d2 & CIRCLE_BIT) != 0; }
static int c_opened(unsigned d2, u32 st)  { (void)d2; (void)st; return s_want->opens == 1; }
static int c_closed(unsigned d2, u32 st)  { (void)d2; (void)st; return s_want->closed; }
static int c_changed(unsigned d2, u32 st) { (void)d2; return st == (CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES); }
static int c_rumble(unsigned d2, u32 st)  { (void)d2; (void)st; return s_want->rumble_lo == 65535 && s_want->rumble_hi == 65535; }
static int c_never(unsigned d2, u32 st)   { (void)d2; (void)st; return 0; }

int main(void)
{
    unsetenv("PS3_PAD_SDL");
    unsetenv("PS3_INPUT_MAP");
    unsetenv("PS3_INPUT_MODE");
    unsetenv("PS3_RUMBLE");
    unsetenv("PS3_RUMBLE_STRENGTH");
    unsetenv("PS3_PAD_THREAD_US");
    setenv("PS3_PAD_THREAD", "1", 1);     /* also where it is not the default (macOS) */
    setenv("PS3_PAD_SLOW_LOG", "1", 1);   /* the slow passes are named on stderr */
    setenv("PS3_KBM", "0", 1);            /* no keyboard or mouse of the machine in a test */
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);

    /* A controller is on at start and its open takes 400 ms: cellPadInit and
     * the first frames do not wait for it. */
    s_fake_open_ms = 400;
    FakePad* a = fake_new(1, 0xA5);
    unsigned long long t0 = pad_now_us();
    assert(cellPadInit(4) == CELL_OK);
    timed(t0);
    assert(s_sdl_thr_on);
    s_want = a;
    double ms = frames_until(c_opened, 0, 3000);
    assert(ms >= 0);
    printf("start: controller opened %.0f ms after cellPadInit, slowest pad call so far %.2f ms\n", ms, s_call_max_ms);

    /* A button: at the title within a few ms (one thread pass + one poll). */
    usleep(20000);
    a->buttons = 1u << SDL_CONTROLLER_BUTTON_A;
    t0 = pad_now_us();
    while (!(frame(0, NULL) & CROSS_BIT)) {      /* frames back to back: no sleep's own granularity */
        assert(pad_now_us() - t0 < 500000ull);
        sched_yield();
    }
    ms = (double)(pad_now_us() - t0) / 1000.0;
    printf("button -> title: %.2f ms\n", ms);
    assert(ms < 30.0);
    a->buttons = 0;

    /* Switched off, and back as a new device; SDL's detection takes 300 ms
     * inside one update and the open 400 ms more. The title keeps getting
     * its frames. */
    a->present = 0;
    s_want = a;
    assert(frames_until(c_closed, 0, 3000) >= 0);
    FakePad* a2 = fake_new(7, 0xA5);
    s_fake_update_once_ms = 300;
    s_want = a2;
    ms = frames_until(c_opened, 0, 5000);
    assert(ms >= 300.0);                     /* the fake drivers did take their time ... */
    printf("reconnect: opened after %.0f ms, slowest pad call so far %.2f ms\n", ms, s_call_max_ms);
    a2->buttons = 1u << SDL_CONTROLLER_BUTTON_A;
    assert(frames_until(c_cross, 0, 500) >= 0);   /* ... and it plays, on port 0 again */
    a2->buttons = 0;
    assert(g_ps3_pad_icons.connected == 1 + SDL_CONTROLLER_TYPE_PS5);

    /* A second controller: port 1, flagged once. */
    FakePad* b = fake_new(9, 0xB6);
    assert(frames_until(c_changed, 1, 5000) >= 0);
    { u32 st = 0; frame(1, &st); assert(st == CELL_PAD_STATUS_CONNECTED); }
    assert(a2->opens == 1 && b->opens == 1);
    b->buttons = 1u << SDL_CONTROLLER_BUTTON_B;
    assert(frames_until(c_circle, 1, 500) >= 0);
    b->buttons = 0;
    { char names[4][128];
      assert(cellPad_host_controller_names(names, 4) == 2 && !strcmp(names[0], "Fake DualSense")); }

    /* Rumble: the write to the device takes 200 ms; cellPadSetActDirect
     * returns at once and the motors get it. */
    s_fake_open_ms = 0;
    s_fake_rumble_ms = 200;
    CellPadActParam* act = (CellPadActParam*)(vm_base + 0x3000);
    act->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] = 1;
    act->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] = 255;
    t0 = pad_now_us();
    assert(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x3000) == CELL_OK);
    timed(t0);
    s_want = a2;
    assert(frames_until(c_rumble, 0, 3000) >= 0);
    act->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] = 0;
    act->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] = 0;
    assert(cellPadSetActDirect(0, (CellPadActParam*)(uintptr_t)0x3000) == CELL_OK);
    frames_until(c_never, 0, 500);
    assert(a2->rumble_lo == 0 && a2->rumble_hi == 0);
    s_fake_rumble_ms = 0;

    assert(cellPadEnd() == CELL_OK);
    assert(!s_sdl_thr_on && a2->closed && b->closed);
    printf("test_pad_thread: ok (slowest pad call %.2f ms; limit %.0f ms)\n", s_call_max_ms, CALL_LIMIT_MS);
    return 0;
}
