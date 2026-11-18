/* ---- the Lighting page (agent/light, 2026-11-09; ps3_lightfx.h) -------------
 * Included by vk_overlay.cpp after its page helpers (section, hint, combo_w,
 * extra_set). Its settings are not in kVar: a change goes to the environment
 * and the owning module's applier (ps3_setting_apply: they are all live) and
 * into the settings file as one of its other lines (extra_set). The widgets
 * show the modules' current values (ps3_lightfx_get) every frame, so nothing
 * here can go stale. Every option off = the game as the PS3 drew it.
 *
 * F3 (with or without the menu): compare -- every option off at once, F3
 * again puts the player's choice back (not saved: the file keeps the choice).
 * [menu-e] F3 also takes the Effects page's two settings to the PS3's (effects at
 * the game's 512 x 288, glow at the PS3's size), and a held F3 (over 0.35 s) is a
 * peek: the look flips back when it is let go (vk_overlay_event). The rows are
 * vk_overlay_menu.inc's (page_lighting). */

static struct {
    bool comparing;                       /* F3: the PS3 look shown, the choice kept here */
    char saved[PS3_LFX_COUNT][16];
    char fx_saved[2][16];                 /* [menu-e] PS3RECOMP_FX_RES, PS3RECOMP_FX_GLOW */
} s_lfx;
static const char* const kLfxFx[2][2] = { { "PS3RECOMP_FX_RES", "1" }, { "PS3RECOMP_FX_GLOW", "2" } };   /* name, the PS3's */

static const char* lfx_off_value(int id) { return id == PS3_LFX_SSAO_STRENGTH ? "100" : "0"; }

/* The player's choice: live now, and in the file. */
static void lfx_choose(int id, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    ps3_setting_apply(ps3_lightfx_name(id), b);
    extra_set(ps3_lightfx_name(id), b);
}

/* 1 = flipped (0: nothing differs from the PS3 look) */
static bool lfx_compare_toggle(void)
{
    if (!s_lfx.comparing) {
        bool any = shadow_quality_get() != 0;   /* [shadow] maps above the PS3's size */
        for (int id = 0; id < PS3_LFX_COUNT; id++) {
            snprintf(s_lfx.saved[id], sizeof s_lfx.saved[id], "%d", ps3_lightfx_get(id));
            any = any || strcmp(s_lfx.saved[id], lfx_off_value(id)) != 0;
        }
        for (int k = 0; k < 2; k++) {
            const char* e = getenv(kLfxFx[k][0]);
            snprintf(s_lfx.fx_saved[k], sizeof s_lfx.fx_saved[k], "%s", e ? e : "");
            any = any || strcmp(e && *e ? e : "1", kLfxFx[k][1]) != 0;
        }
        if (!any) { vk_overlay_notice("All lighting and effects options are the PS3's -- nothing to compare"); return false; }
        for (int id = 0; id < PS3_LFX_COUNT; id++) ps3_setting_apply(ps3_lightfx_name(id), lfx_off_value(id));
        for (int k = 0; k < 2; k++) ps3_setting_apply(kLfxFx[k][0], kLfxFx[k][1]);
        shadow_quality_compare(true);   /* [shadow] the PS3's 1024x1024 maps too */
        s_lfx.comparing = true;
        vk_overlay_notice("PS3 look: lighting and effects (F3 = your settings)");
        fprintf(stderr, "[overlay] F3: the PS3 look (lighting + effects)\n");
    } else {
        for (int id = 0; id < PS3_LFX_COUNT; id++) ps3_setting_apply(ps3_lightfx_name(id), s_lfx.saved[id]);
        for (int k = 0; k < 2; k++) ps3_setting_apply(kLfxFx[k][0], s_lfx.fx_saved[k]);
        shadow_quality_compare(false);   /* [shadow] */
        s_lfx.comparing = false;
        vk_overlay_notice("Your settings (F3 = PS3 look)");
        fprintf(stderr, "[overlay] F3: your settings again\n");
    }
    return true;
}
