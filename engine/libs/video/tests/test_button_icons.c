/*
 * rsx_button_icons: the mode, the generation the draw engine re-uploads the
 * prompt atlas on, and the glyph drawing (no game data: the atlas itself is
 * recognised by its pixels, which only the player's copy has, so here only a
 * look-alike is checked to be left alone).
 *
 *   cc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *      -o /tmp/test_button_icons libs/video/tests/test_button_icons.c \
 *      libs/video/rsx_button_icons.c -lm && /tmp/test_button_icons
 */
#include "rsx_button_icons.h"
#pragma GCC diagnostic ignored "-Wunused-function"   /* the parser half of the header */
#include "../../input/pad_input_map.h"
#include "../../input/pad_remap.h"                 /* [remap] the menu's rebinding rules */
#include "../../../include/ps3emu/ps3_pad_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static PadInputMap s_test_map;
static int s_test_mouse_cam = 1;
static void test_map(void* out) { memcpy(out, &s_test_map, sizeof s_test_map); }
static int test_mouse_cam(void) { return s_test_mouse_cam; }

static uint8_t img[256][512][4], ref[256][512][4];

/* Alpha in a box (inclusive), summed. */
static long alpha_sum(int x0, int y0, int x1, int y1)
{
    long s = 0;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) s += img[y][x][3];
    return s;
}
static int box_differs(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        if (memcmp(&img[y][x0], &ref[y][x0], (size_t)(x1 - x0 + 1) * 4)) return 1;
    return 0;
}

/* ---- the separate button pictures ------------------------------------------
 * The pictures themselves are the player's (i2.core.xpps), so recognition is
 * only checked against them when PS3_BUTTON_PICTURES_DIR names a folder with
 * their level-0 blocks taken from his copy (r2.bin r1.bin l1.bin cross.bin
 * triangle.bin square.bin circle.bin dpad_down.bin); without it, that anything
 * else is not taken for one, and what each mode draws. */
static uint8_t pic[256 * 256 * 4], pic2[256 * 256 * 4];

static const uint8_t* px_at(const uint8_t* p, int n, int x, int y_display)
{
    return p + ((size_t)(n - 1 - y_display) * (size_t)n + (size_t)x) * 4u;   /* stored bottom-up */
}
/* Dark opaque texels in a display-space box. */
static int ink_in(const uint8_t* p, int n, int x0, int y0, int x1, int y1)
{
    int c = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        const uint8_t* t = px_at(p, n, x, y);
        if (t[3] > 200 && t[0] < 60) c++;
    }
    return c;
}

static void test_pictures(const PadInputMap* def)
{
    /* sizes: 4x the originals */
    uint32_t w, h;
    for (int i = PS3_BTNIMG_R2; i <= PS3_BTNIMG_CIRCLE; i++) { rsx_button_icons_image_size(i, &w, &h); CHECK(w == 128 && h == 128); }
    rsx_button_icons_image_size(PS3_BTNIMG_DPAD_DOWN, &w, &h); CHECK(w == 256 && h == 256);
    rsx_button_icons_image_size(0, &w, &h); CHECK(w == 0);

    /* nothing else is taken for one: other contents, sizes, formats */
    static uint8_t blk[4096];
    for (int i = 0; i < 4096; i++) blk[i] = (uint8_t)(i * 7 + 3);
    CHECK(rsx_button_icons_image(0x86, 32, 32, blk, 512) == 0);
    CHECK(rsx_button_icons_image(0x88, 32, 32, blk, 1024) == 0);
    CHECK(rsx_button_icons_image(0x88, 64, 64, blk, 4096) == 0);
    CHECK(rsx_button_icons_image(0x85, 32, 32, blk, 4096) == 0);
    CHECK(rsx_button_icons_image(0x88, 32, 64, blk, 4096) == 0);
    CHECK(rsx_button_icons_image(0x88, 32, 32, blk, 100) == 0);   /* short */

    /* the player's own pictures, when given */
    const char* dir = getenv("PS3_BUTTON_PICTURES_DIR");
    if (dir && *dir) {
        static const char* const names[] = { "r2", "r1", "l1", "cross", "triangle", "square", "circle", "dpad_down" };
        for (int i = 0; i < PS3_BTNIMG_COUNT; i++) {
            char path[1024]; snprintf(path, sizeof path, "%s/%s.bin", dir, names[i]);
            FILE* f = fopen(path, "rb");
            CHECK(f != NULL);
            if (!f) continue;
            const size_t n = fread(blk, 1, sizeof blk, f); fclose(f);
            const uint32_t sz = i == PS3_BTNIMG_DPAD_DOWN - 1 ? 64u : 32u, fmt = i < 3 ? 0x86u : 0x88u;
            CHECK(rsx_button_icons_image(fmt, sz, sz, blk, (uint32_t)n) == i + 1);
            CHECK(rsx_button_icons_image(fmt | 0x20u, sz, sz, blk, (uint32_t)n) == i + 1);   /* LN/UN bits ignored */
            blk[n / 2] ^= 1;
            CHECK(rsx_button_icons_image(fmt, sz, sz, blk, (uint32_t)n) == 0);              /* one bit off: not it */
        }
        printf("test_button_icons: the player's pictures recognised\n");
    }

    /* PlayStation: the originals stay */
    for (int i = 1; i <= PS3_BTNIMG_COUNT; i++)
        CHECK(rsx_button_icons_image_render(i, PS3_ICONS_PLAYSTATION, def, 1, pic) == 0);

    /* Every picture, both modes: transparent texels black (drawn inverted, as
     * the powers menu does, anything else would show as a box). */
    for (int mode = PS3_ICONS_XBOX; mode <= PS3_ICONS_KEYBOARD; mode++)
        for (int i = 1; i <= PS3_BTNIMG_COUNT; i++) {
            memset(pic, 0x5A, sizeof pic);
            if (!rsx_button_icons_image_render(i, mode, def, 1, pic)) { CHECK(mode == PS3_ICONS_XBOX && i == PS3_BTNIMG_DPAD_DOWN); continue; }
            rsx_button_icons_image_size(i, &w, &h);
            int bad = 0;
            for (uint32_t k = 0; k < w * h; k++) if (!pic[k * 4 + 3] && (pic[k * 4] | pic[k * 4 + 1] | pic[k * 4 + 2])) bad++;
            CHECK(bad == 0);
            if (i <= PS3_BTNIMG_L1) {   /* the boxes stay opaque squares */
                int clear = 0;
                for (uint32_t k = 0; k < w * h; k++) clear += pic[k * 4 + 3] != 255;
                CHECK(clear == 0);
            } else if (i <= PS3_BTNIMG_CIRCLE) {   /* the discs' corners stay clear */
                CHECK(px_at(pic, 128, 1, 1)[3] == 0 && px_at(pic, 128, 126, 126)[3] == 0);
            }
        }

    /* keyboard: Square is F; on T instead the picture changes, and it is
     * stored bottom-up: the T's bar is in the upper half on screen */
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_SQUARE, PS3_ICONS_KEYBOARD, def, 1, pic));
    PadInputMap t = *def;
    memset(t.key[PIM_SQUARE], 0, sizeof t.key[PIM_SQUARE]);
    pim_key_set(t.key[PIM_SQUARE], 0x11);   /* T */
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_SQUARE, PS3_ICONS_KEYBOARD, &t, 1, pic2));
    CHECK(memcmp(pic, pic2, 128 * 128 * 4) != 0);
    CHECK(ink_in(pic2, 128, 44, 34, 56, 50) > 50 && ink_in(pic2, 128, 44, 34, 56, 50) > 3 * ink_in(pic2, 128, 44, 78, 56, 94));
    CHECK(px_at(pic2, 128, 64, 64)[3] == 255);                  /* a key, opaque in the middle */
    /* a control with nothing on the keyboard keeps the original */
    PadInputMap none = *def;
    memset(none.key[PIM_TRIANGLE], 0, sizeof none.key[PIM_TRIANGLE]); none.mouse[PIM_TRIANGLE] = 0;
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_TRIANGLE, PS3_ICONS_KEYBOARD, &none, 1, pic) == 0);

    /* R1: the left mouse button with the mouse camera on, the E key without */
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_R1, PS3_ICONS_KEYBOARD, def, 1, pic));
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_R1, PS3_ICONS_KEYBOARD, def, 0, pic2));
    CHECK(memcmp(pic, pic2, 128 * 128 * 4) != 0);
    CHECK(ink_in(pic, 128, 44, 30, 62, 50) > 150);             /* the left button filled */
    CHECK(ink_in(pic, 128, 68, 30, 84, 50) < 60);              /* the right one not */

    /* xbox: Cross shows A on a white disc; rebound to Y it shows Y */
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_CROSS, PS3_ICONS_XBOX, def, 1, pic));
    CHECK(px_at(pic, 128, 64, 20)[0] == 255 && px_at(pic, 128, 64, 20)[3] == 255);
    PadInputMap y = *def; y.pad[PIM_CROSS] = 1u << 3;
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_CROSS, PS3_ICONS_XBOX, &y, 1, pic2));
    CHECK(memcmp(pic, pic2, 128 * 128 * 4) != 0);
    PadInputMap unb = *def; unb.pad[PIM_CIRCLE] = 0;
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_CIRCLE, PS3_ICONS_XBOX, &unb, 1, pic) == 0);

    /* the D-pad: kept on the pad's own D-pad down; on D-pad up, the up arm is
     * the grey one; on the keyboard (2) a black key with a grey face */
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_DPAD_DOWN, PS3_ICONS_XBOX, def, 1, pic) == 0);
    PadInputMap up = *def; up.pad[PIM_DOWN] = 1u << 11;
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_DPAD_DOWN, PS3_ICONS_XBOX, &up, 1, pic));
    { const uint8_t* a = px_at(pic, 256, 128, 60); CHECK(a[3] == 255 && a[0] > 90 && a[0] < 110); }    /* up arm: grey */
    { const uint8_t* a = px_at(pic, 256, 128, 196); CHECK(a[3] == 255 && a[0] < 10); }                 /* down arm: black */
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_DPAD_DOWN, PS3_ICONS_KEYBOARD, def, 1, pic));
    { const uint8_t* a = px_at(pic, 256, 60, 60); CHECK(a[3] == 255 && a[0] > 90 && a[0] < 110); }     /* the key's grey face */
    CHECK(px_at(pic, 256, 4, 4)[3] == 0);

    /* the live path: the current mode and bindings (rsx_button_icons_frame) */
    g_ps3_pad_icons.source = 1 + 7;                             /* a DualSense: PlayStation */
    ps3_button_icons_set_mode(-1);
    s_test_map = *def;
    rsx_button_icons_frame();
    CHECK(rsx_button_icons_image_draw(PS3_BTNIMG_SQUARE, pic) == 0);
    g_ps3_pad_icons.source = 0;                                 /* the keyboard */
    rsx_button_icons_frame();
    CHECK(rsx_button_icons_image_draw(PS3_BTNIMG_SQUARE, pic) == 1);
    CHECK(rsx_button_icons_image_render(PS3_BTNIMG_SQUARE, PS3_ICONS_KEYBOARD, def, 1, pic2));
    CHECK(!memcmp(pic, pic2, 128 * 128 * 4));
    s_test_map = t;                                             /* rebound in the menu: drawn with the new key */
    rsx_button_icons_frame();
    CHECK(rsx_button_icons_image_draw(PS3_BTNIMG_SQUARE, pic) == 1);
    CHECK(memcmp(pic, pic2, 128 * 128 * 4) != 0);
    s_test_map = *def;
}

int main(void)
{
    setenv("PS3RECOMP_METAL_HEADLESS", "1", 1);
    unsetenv("PS3_BUTTON_ICONS");
    pim_defaults(&s_test_map);
    g_ps3_pad_icons.input_map = test_map;
    g_ps3_pad_icons.mouse_camera = test_mouse_cam;

    /* auto, nothing used yet, headless: the original glyphs */
    uint32_t g0 = rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_PLAYSTATION);
    CHECK(rsx_button_icons_frame() == g0);                         /* nothing changed: same generation */

    /* an Xbox One pad pressed (1 + SDL_CONTROLLER_TYPE_XBOXONE) */
    g_ps3_pad_icons.source = 1 + 2;
    uint32_t g1 = rsx_button_icons_frame();
    CHECK(g1 != g0 && ps3_button_icons_effective() == PS3_ICONS_XBOX);
    /* a DualSense (PS5 = 7) */
    g_ps3_pad_icons.source = 1 + 7;
    uint32_t g2 = rsx_button_icons_frame();
    CHECK(g2 != g1 && ps3_button_icons_effective() == PS3_ICONS_PLAYSTATION);
    /* the keyboard */
    g_ps3_pad_icons.source = 0;
    rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_KEYBOARD);
    /* a rebinding changes the keyboard glyphs: a new generation */
    uint32_t g3 = rsx_button_icons_frame();
    pim_key_set(s_test_map.key[PIM_CROSS], 0x0C);   /* Q */
    CHECK(rsx_button_icons_frame() != g3);
    /* nothing used, a controller connected: that controller */
    g_ps3_pad_icons.source = -1; g_ps3_pad_icons.connected = 1 + 1;   /* Xbox 360 */
    rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_XBOX);
    /* the menu's choice wins over auto, and -1 hands it back */
    ps3_button_icons_set_mode(PS3_ICONS_KEYBOARD);
    rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_KEYBOARD);
    CHECK(ps3_button_icons_mode_setting() == PS3_ICONS_KEYBOARD);
    ps3_button_icons_set_mode(-1);
    rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_XBOX);

    /* Anything that is not the atlas is left alone, whatever the mode. */
    for (int y = 0; y < 256; y++) for (int x = 0; x < 512; x++) {
        img[y][x][0] = img[y][x][1] = img[y][x][2] = 240; img[y][x][3] = (uint8_t)((x ^ y) & 0xFF);
    }
    memcpy(ref, img, sizeof img);
    CHECK(rsx_button_icons_upload(0x85, 512, 256, 1, &img[0][0][0], 2048) == 0);
    CHECK(!memcmp(img, ref, sizeof img));
    CHECK(rsx_button_icons_upload(0x86, 512, 256, 1, &img[0][0][0], 2048) == 0);   /* DXT1 */
    CHECK(rsx_button_icons_upload(0x85, 256, 256, 1, &img[0][0][0], 1024) == 0);

    /* Xbox glyphs: drawn inside the button boxes only. */
    memset(img, 0, sizeof img);
    memcpy(ref, img, sizeof img);
    PadInputMap def; pim_defaults(&def);
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_XBOX, &def, 1);
    CHECK(alpha_sum(57, 178, 94, 215) > 255L * 300);      /* the cross box: a disc */
    CHECK(alpha_sum(241, 8, 290, 41) > 255L * 600);       /* L1: a bumper */
    CHECK(img[196][62][1] > img[196][62][0] + 60 && img[196][62][3] == 255);   /* A: a green disc */
    CHECK(alpha_sum(3, 34, 234, 79) == 0);                 /* the logo's area is not touched */
    CHECK(alpha_sum(468, 54, 504, 147) == 0);              /* the walking man, the flag, the check */
    CHECK(alpha_sum(105, 176, 147, 218) == 0);             /* the D-pad stays the original */

    /* Keyboard glyphs follow the bindings: cross on Q differs from Space. */
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &def, 1);
    memcpy(ref, img, sizeof img);
    PadInputMap q = def;
    memset(q.key[PIM_CROSS], 0, sizeof q.key[PIM_CROSS]);
    pim_key_set(q.key[PIM_CROSS], 0x0C);
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &q, 1);
    CHECK(box_differs(57, 178, 94, 215));
    CHECK(!box_differs(419, 57, 456, 94));                 /* circle: same key, same glyph */
    /* R1 is the left mouse button with the mouse camera on, the O key without */
    memcpy(ref, img, sizeof img);
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &q, 0);
    CHECK(box_differs(359, 8, 408, 41));

    /* A controller rebinding shows the button pressed: cross on Y. */
    memset(img, 0, sizeof img);
    PadInputMap y = def; y.pad[PIM_CROSS] = 1u << 3;
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_XBOX, &y, 1);
    CHECK(img[196][62][0] > img[196][62][2] + 60 && img[196][62][1] > img[196][62][2] + 60);   /* Y: yellow */

    /* [remap] After a change in F2 > Controls (pad_remap.h's rules): the prompt is the new
     * key's, exactly as for a map that had it from the start, and the swapped key moves too. */
    {
        PadInputMap r = def;
        const PrmInput e = { PRM_KEY, 0x0E };              /* Cross's Space cell -> E (R1's): Swap */
        CHECK(prm_assign(&r, PIM_CROSS, e, 0x31) == 1u << PIM_R1);
        PadInputMap direct = def;
        memset(direct.key[PIM_CROSS], 0, sizeof direct.key[PIM_CROSS]);
        memset(direct.key[PIM_R1], 0, sizeof direct.key[PIM_R1]);
        pim_key_set(direct.key[PIM_CROSS], 0x0E);
        pim_key_set(direct.key[PIM_R1], 0x31);
        static uint8_t a[256][512][4];
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &r, 0);
        memcpy(a, img, sizeof img);
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &direct, 0);
        CHECK(!memcmp(a, img, sizeof img));                 /* the same atlas */
        memcpy(ref, img, sizeof img);
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &def, 0);
        CHECK(box_differs(57, 178, 94, 215));               /* cross: E, not Space */
        CHECK(box_differs(359, 8, 408, 41));                /* R1: Space, not E (mouse camera off: keys) */
        CHECK(!box_differs(419, 57, 456, 94));              /* circle: untouched */
        /* Start rebound to keypad Enter alone still shows ENTER (Enter twins) */
        PadInputMap s = def;
        const PrmInput kpe = { PRM_KEY, 0x4C };
        prm_clear_keys(&s, PIM_START);
        prm_assign(&s, PIM_START, kpe, -1);
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &s, 0);
        memcpy(a, img, sizeof img);
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &def, 0);
        CHECK(!memcmp(a, img, sizeof img));
        /* the live path: the map the menu set is what the next frame hashes */
        ps3_button_icons_set_mode(PS3_ICONS_KEYBOARD);
        s_test_map = def;
        const uint32_t g4 = rsx_button_icons_frame();
        s_test_map = r;
        const uint32_t g5 = rsx_button_icons_frame();
        CHECK(g5 != g4);
        CHECK(rsx_button_icons_frame() == g5);              /* and stays until the next change */
        ps3_button_icons_set_mode(-1);
    }

    test_pictures(&def);

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("test_button_icons: ok\n");
    return 0;
}
