/*
 * rsx_button_icons -- on-screen button prompts that follow the controller in
 * use (inFAMOUS 2: "Pressione (A) para continuar" with an Xbox pad).
 *
 * The title draws every button glyph of its prompts (the PUA characters
 * U+F080-U+F0A6 in its strings, menus and HUD alike) from one texture: a
 * 512x256 A8R8G8B8 atlas of white shapes with the symbol cut out, which the
 * draw engine uploads once. The engine hands each decoded upload of that
 * shape to rsx_button_icons_upload(), which recognises the atlas by the
 * pixels of two of its glyphs and, unless the mode is PlayStation, redraws
 * the button glyphs in place: same boxes, same white-with-cut-out style, so
 * the font's layout does not change. Nothing of the game's files is touched,
 * and the replacement art is drawn here procedurally (simple shapes and a
 * small stroke font), no artwork is copied from anywhere.
 *
 * Modes (PS3_BUTTON_ICONS, the in-game menu, PS3_TOGGLE_FILE):
 *   auto         (default) the device last used: an Xbox-style controller
 *                (Xbox 360/One/Series and anything else SDL does not call a
 *                PlayStation or Nintendo pad) shows Xbox glyphs, a DualShock /
 *                DualSense / Switch pad the original ones, the keyboard and
 *                mouse their keys. Before any input: the controller that is
 *                connected, else the keyboard (headless: PlayStation).
 *   playstation  the original atlas, untouched.
 *   xbox         A/B/X/Y, LB/RB/LT/RT, View/Menu, LS/RS.
 *   keyboard     the keys (and mouse buttons) bound to each PS3 control in
 *                the input map (PS3_INPUT_MAP, the Controls tab).
 * The Xbox and keyboard glyphs follow the bindings: a control rebound to
 * another controller button or key shows that one.
 *
 * The title also has eight button pictures outside the atlas (the context-
 * action diamond, the powers menu's tabs): see "the separate button pictures"
 * below and in rsx_button_icons.c. They follow the same mode and bindings.
 *
 * Cost: rsx_button_icons_frame() once per frame (a few loads, and while the
 * mode is not PlayStation a 600-byte hash of the input map); a texture upload
 * of exactly 512x256 A8R8G8B8 is checked against the atlas (a hash of ~3000
 * bytes); when the effective mode or the bindings change the atlas alone is
 * re-uploaded, with no restart.
 *
 * Threads: _frame and _upload run on the RSX thread; the setters on any.
 */
#ifndef PS3RECOMP_RSX_BUTTON_ICONS_H
#define PS3RECOMP_RSX_BUTTON_ICONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS3_ICONS_AUTO = 0,
    PS3_ICONS_PLAYSTATION = 1,
    PS3_ICONS_XBOX = 2,
    PS3_ICONS_KEYBOARD = 3,
};

/* Once per frame: resolves the effective mode and returns a generation that
 * changes whenever the atlas has to be drawn differently. */
uint32_t rsx_button_icons_frame(void);

/* A decoded upload's first level (RGBA8 rows, R,G,B,A bytes). `fmt` is the
 * NV4097 texture format byte. Returns 1 when this is the button atlas (and
 * redraws its glyphs for the current mode unless that is PlayStation), 0 for
 * anything else (untouched). */
int rsx_button_icons_upload(uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels,
                            uint8_t* rgba, uint32_t row_bytes);

/* The setting (PS3_ICONS_*): the in-game menu sets it for the session; -1
 * goes back to PS3_BUTTON_ICONS / the toggle file. */
void ps3_button_icons_set_mode(int mode);
int  ps3_button_icons_mode_setting(void);
/* What auto resolves to right now (PS3_ICONS_PLAYSTATION/XBOX/KEYBOARD). */
int  ps3_button_icons_effective(void);

/* For tests and tools: redraw an atlas image for `mode` (an explicit mode,
 * not auto) with the given input map (NULL = the defaults' look). */
void rsx_button_icons_draw(uint8_t* rgba, uint32_t row_bytes, int mode,
                           const void* pad_input_map, int mouse_camera);

/* ---- the separate button pictures -----------------------------------------
 * Eight button images of the title's own, outside the atlas (the context-action
 * diamond, the powers menu's tabs, ...): rsx_button_icons.c has the list. The
 * engine hands each 2D DXT1/DXT5 upload of their sizes (32x32, 64x64) to
 * rsx_button_icons_image(), which knows them by size, format and a hash of
 * their level-0 blocks; for a mode other than PlayStation it uploads the
 * picture rsx_button_icons_image_draw() makes instead (RGBA8, 4x the size,
 * its own mips), and redraws it on the same generation as the atlas. */
enum {
    PS3_BTNIMG_R2 = 1, PS3_BTNIMG_R1, PS3_BTNIMG_L1,
    PS3_BTNIMG_CROSS, PS3_BTNIMG_TRIANGLE, PS3_BTNIMG_SQUARE, PS3_BTNIMG_CIRCLE,
    PS3_BTNIMG_DPAD_DOWN,
    PS3_BTNIMG_COUNT = PS3_BTNIMG_DPAD_DOWN
};

/* `blocks` = level 0 as stored (`bytes` of it), `fmt` the NV4097 format byte.
 * Returns the picture (PS3_BTNIMG_*), or 0 for anything else. */
int rsx_button_icons_image(uint32_t fmt, uint32_t w, uint32_t h,
                           const uint8_t* blocks, uint32_t bytes);
/* The size of its replacement (RGBA8, rows tightly packed). */
void rsx_button_icons_image_size(int image, uint32_t* w, uint32_t* h);
/* The replacement for the current mode and bindings, into rgba (that size).
 * Returns 0 when the original stays: PlayStation mode, or nothing the mode
 * shows is bound to the picture's control. */
int rsx_button_icons_image_draw(int image, uint8_t* rgba);
/* For tests and tools: the same for an explicit mode and map. */
int rsx_button_icons_image_render(int image, int mode, const void* pad_input_map,
                                  int mouse_camera, uint8_t* rgba);

#ifdef __cplusplus
}
#endif

#endif
