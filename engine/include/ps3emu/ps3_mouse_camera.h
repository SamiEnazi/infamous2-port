/*
 * ps3_mouse_camera -- the mouse camera's direct mode, for a title's own camera
 * code (libs/input/cellPad.c, libs/input/pad_mouse_direct.h).
 *
 * PS3_MOUSE_MODE=direct (the default; [mouse] mode = "direct" in input.toml):
 * a title that knows its camera (inFAMOUS 2: the port's cheats.cpp hook on the
 * game's camera update) turns it by the mouse's motion itself -- no stick in
 * between, so no speed cap and no stick acceleration -- and leaves everything
 * else the camera does to the game. Each time its camera updates it calls
 *
 *     float dx, dy, sens, aim_sens;
 *     if (cellPad_host_mouse_direct_take(&dx, &dy, &sens, &aim_sens) && (dx || dy))
 *         yaw += dx * k * (aiming ? aim_sens : sens), pitch += dy * k * (...);
 *
 * dx, dy = raw mouse counts since the last call (+x right, +y towards the
 * player), the player's invert settings applied. While those calls keep coming
 * the pad poll leaves the motion to the title and the mouse moves no stick;
 * when they stop (a menu, the map, a cutscene) the mouse is a right stick again
 * within PMD_CLAIM_US. A 0 return = the mouse is not the camera's now (stick
 * mode, mouse camera off, controller-only input, a menu open): leave the camera
 * to the game, nothing was taken.
 *
 * cellPad_host_mouse_direct_provider(1): the title has such a camera (the
 * menu's Mouse entry shows whether direct mode can work in this build).
 *
 * cellPad_host_mouse_direct_provider(2) (GitHub #27, the tee): the title also
 * hides the mouse from its own camera's stick read -- where that camera reads
 * the right stick it zeroes the axes cellPad_host_mouse_stick_axes() names
 * (bit 0 x, bit 1 y: the mouse drove them in the latest pad packet). The mouse
 * then stays the right stick for everything else in the game, exactly as in
 * stick mode (another camera, a tutorial waiting for the stick, the game's own
 * "stick last moved" bookkeeping), while that camera turns by the motion
 * alone; take() hands out the title's own share of the motion.
 */
#ifndef PS3EMU_PS3_MOUSE_CAMERA_H
#define PS3EMU_PS3_MOUSE_CAMERA_H
#define PS3_MOUSE_CAMERA_HAVE_TEE 1   /* cellPad_host_mouse_stick_axes, provider 2 */

#ifdef __cplusplus
extern "C" {
#endif

int   cellPad_host_mouse_direct_take(float* dx, float* dy, float* sens, float* aim_sens);
void  cellPad_host_mouse_direct_provider(int level);   /* 0 none, 1 takes, 2 takes + hides (the tee) */
int   cellPad_host_mouse_direct_ready(void);           /* the level */
int   cellPad_host_mouse_stick_axes(void);
int   cellPad_host_mouse_mode(void);                 /* 1 direct, 0 stick */
void  cellPad_host_set_mouse_mode(int direct);
float cellPad_host_mouse_aim_sens(void);
void  cellPad_host_set_mouse_aim_sens(float sens);

#ifdef __cplusplus
}
#endif

/* ---- the turn a title's camera hook owes its camera (pure; libs/input/tests/test_pad_mouse_direct.c) ----
 * A hook collects the mouse's motion as an owed turn (rad: owe_yaw, owe_pitch) and puts it into the camera's
 * target angles. Two ways (inFAMOUS 2's free camera, cheats.cpp hook_free_cam, GitHub #59):
 *   ps3_mouse_turn_whole  all of it now: the same angle per count for a slow move and a flick, nothing left
 *                         for later frames. Pitch is held in [lo, hi] (what is past a limit is gone, as for a
 *                         stick); an axis the game switched off (axes_off bit 0 yaw, bit 1 pitch) drops its turn.
 *   ps3_mouse_turn_lead   only as much as keeps the target within `lead` rad of the view the game has reached
 *                         (a camera whose view follows its target at a limited speed and forgets a target that
 *                         leads by more); the rest stays owed for the next frames. */
#define PS3_MOUSE_CAMERA_HAVE_TURN 1
static inline float ps3_mouse_wrap_pi(float a)
{
    const float pi = 3.14159265f, two_pi = 6.28318531f;
    while (a > pi) a -= two_pi;
    while (a < -pi) a += two_pi;
    return a;
}
static inline float ps3_mouse_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline void ps3_mouse_turn_whole(float* owe_yaw, float* owe_pitch, int axes_off, float lo, float hi,
                                        float* yaw, float* pitch)
{
    if (!(axes_off & 1)) *yaw = ps3_mouse_wrap_pi(*yaw + *owe_yaw);
    if (!(axes_off & 2)) *pitch = ps3_mouse_clampf(*pitch + *owe_pitch, lo, hi);
    *owe_yaw = *owe_pitch = 0.0f;
}
static inline void ps3_mouse_turn_lead(float* owe_yaw, float* owe_pitch, float view_yaw, float view_pitch,
                                       float lead, float lo, float hi, float* yaw, float* pitch)
{
    /* yaw: target + s within view +- lead (wrapped), towards the owed side only */
    const float ly = ps3_mouse_wrap_pi(*yaw - view_yaw), lp = *pitch - view_pitch;
    const float sy = *owe_yaw > 0.0f ? ps3_mouse_clampf(*owe_yaw, 0.0f, lead - ly > 0.0f ? lead - ly : 0.0f)
                                     : ps3_mouse_clampf(*owe_yaw, -lead - ly < 0.0f ? -lead - ly : 0.0f, 0.0f);
    const float sp = *owe_pitch > 0.0f ? ps3_mouse_clampf(*owe_pitch, 0.0f, lead - lp > 0.0f ? lead - lp : 0.0f)
                                       : ps3_mouse_clampf(*owe_pitch, -lead - lp < 0.0f ? -lead - lp : 0.0f, 0.0f);
    const float p1 = *pitch, p2 = ps3_mouse_clampf(p1 + sp, lo, hi);
    *yaw = ps3_mouse_wrap_pi(*yaw + sy);
    *pitch = p2;
    *owe_yaw -= sy;
    *owe_pitch -= sp;
    if (p2 != p1 + sp || (*owe_pitch > 0.0f && p2 >= hi) || (*owe_pitch < 0.0f && p2 <= lo))
        *owe_pitch = 0.0f;                     /* at a pitch limit: the rest is gone, as for a stick */
    if (*owe_yaw > -1e-7f && *owe_yaw < 1e-7f) *owe_yaw = 0.0f;
    if (*owe_pitch > -1e-7f && *owe_pitch < 1e-7f) *owe_pitch = 0.0f;
}

#endif
