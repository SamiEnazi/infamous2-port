/* ---- (lowend) small graphics cards: the automatic choice and the presets ----
 * Included by vk_overlay.cpp after its page helpers (section, hint, extra_set,
 * apply_setting). The settings are kLowEnd's (top of vk_overlay.cpp).
 *
 * Automatic (vk_overlay_lowend_auto, from the Vulkan backend once it knows the
 * GPU, before anything reads these settings): a card with at most
 * LOWEND_VRAM_MB of its own memory (the 4 GB class: GTX 1650, RTX 3050 laptop,
 * RX 6500) or integrated graphics starts with the low-end values -- only for
 * the settings the player has not chosen (no variable set when the game
 * started, no line in the settings file; the launcher's PS3_OVERLAY_DEFAULTS
 * are not a choice). Nothing is written to the file. Windowed runs only (a
 * headless run is a test). PS3RECOMP_LOWEND=0: never, =1: as if the card were
 * small (tests). What the set saves was measured on an RTX 2070 Super (the
 * kLowEnd comment): ~20% of the GPU frame and ~0.25 GB; a 4 GB card itself
 * was not available.
 *
 * The Graphics page's "Low-end PC" / "Standard" buttons set all of them at
 * once, as the player's choice (live where the module takes it, saved). */

#define LOWEND_VRAM_MB 4608u                  /* 4.5 GB: a 4 GB card reports 4096 (some 3840..4095) */

static int s_le_auto = -1;                    /* -1 not decided, 0 standard, 1 low-end */
static unsigned s_le_mb;
static int s_le_integrated, s_le_changed;

static const char* le_std(int i) { return s_le_def[i][0] ? s_le_def[i] : kLowEnd[i].std_; }
/* the value in effect: the variable, else what the module uses without it */
static const char* le_now(int i)
{
    const char* e = getenv(kLowEnd[i].var);
    return (e && *e) ? e : kLowEnd[i].unset;
}
static bool le_same(int i, const char* a, const char* b)   /* "" = unset */
{
    return !strcmp(*a ? a : kLowEnd[i].unset, *b ? b : kLowEnd[i].unset);
}
static bool le_players(int i)
{
    if (overlay_off()) return getenv(kLowEnd[i].var) != NULL;   /* no preload: the environment is all there is */
    return s_le_src[i] == LE_ENV || s_le_src[i] == LE_FILE;
}
/* A [light] value straight into its module too: seeded before this ran, it
 * would not read the environment again. */
static void le_lightfx_store(const char* var, const char* v)
{
    for (int id = 0; id < PS3_LFX_COUNT; id++)
        if (!strcmp(var, ps3_lightfx_name(id))) { ps3_lightfx_seed(); ps3_lightfx_store(id, ps3_lightfx_parse(id, v)); }
}

extern "C" int vk_overlay_lowend_auto(unsigned vram_mb, int integrated, const char* gpu)
{
    const char* f = getenv("PS3RECOMP_LOWEND");
    int low;
    if (f && f[0] == '0' && !f[1]) low = 0;
    else if (f && f[0] == '1' && !f[1]) low = 1;
    else low = integrated || (vram_mb && vram_mb <= LOWEND_VRAM_MB);
    s_le_mb = vram_mb; s_le_integrated = integrated; s_le_auto = low;
    if (!low) {
        fprintf(stderr, "[lowend] %s, %u MB of video memory: the standard settings%s\n", gpu ? gpu : "GPU", vram_mb,
                f && f[0] == '0' && !f[1] ? " (PS3RECOMP_LOWEND=0)" : "");
        return 0;
    }
    char list[640];
    int k = 0, n = 0;
    list[0] = 0;
    for (int i = 0; i < LOWEND_N; i++) {
        const char* var = kLowEnd[i].var;
        const char* lo = kLowEnd[i].low;
        if (le_players(i)) {
            if (k < (int)sizeof list - 80) k += snprintf(list + k, sizeof list - (size_t)k, " %s=%s (yours, kept)", var, le_now(i));
            continue;
        }
        char was[24];
        snprintf(was, sizeof was, "%s", le_now(i));
        if (le_same(i, was, lo)) continue;
        ps3_setting_apply(var, lo);
        le_lightfx_store(var, lo);
        if (!strcmp(var, "PS3RECOMP_RENDER_SCALE")) vk_postfx_set_render_scale((float)atof(lo));
        const int v = var_index(var);        /* the value in effect at start: no "restart needed" mark */
        if (v >= 0) snprintf(s_start[v], sizeof s_start[v], "%s", lo);
        if (k < (int)sizeof list - 80) k += snprintf(list + k, sizeof list - (size_t)k, " %s=%s (was %s)", var, lo, was);
        n++;
    }
    s_le_changed = n;
    fprintf(stderr, "[lowend] %s, %u MB of video memory%s%s: low-end defaults for %d setting(s):%s\n",
            gpu ? gpu : "GPU", vram_mb, integrated ? " (integrated)" : "",
            f && f[0] == '1' && !f[1] ? " (PS3RECOMP_LOWEND=1)" : "", n, list[0] ? list : " none");
    return n;
}

/* At attach: say so once. */
static void lowend_attach(void)
{
    if (s_le_auto != 1 || s_le_changed <= 0) return;
    char b[160];
    if (s_le_integrated) snprintf(b, sizeof b, "Integrated graphics: lighter settings (F2 > Graphics > Low-end PC)");
    else snprintf(b, sizeof b, "%.1f GB graphics card: lighter settings (F2 > Graphics > Low-end PC)", (double)s_le_mb / 1024.0);
    notice_for(b, 8.0);
}

/* 1 = every setting has the low-end value, 2 = the standard one, 0 = neither */
static int lowend_state(void)
{
    bool lo = true, st = true;
    for (int i = 0; i < LOWEND_N; i++) {
        const char* now = le_now(i);
        lo = lo && le_same(i, now, kLowEnd[i].low);
        st = st && le_same(i, now, le_std(i));
    }
    return lo ? 1 : st ? 2 : 0;
}

static void lowend_preset(bool low)
{
    for (int i = 0; i < LOWEND_N; i++) {
        const char* var = kLowEnd[i].var;
        const char* v = low ? kLowEnd[i].low : le_std(i);
        const int k = var_index(var);
        if (k >= 0) apply_setting(k, v);     /* the internal resolution: saved, used at the next start */
        else { ps3_setting_apply(var, v); extra_set(var, v); }
    }
    s_ui_stale = true;
    const char* rs = getenv("PS3RECOMP_RENDER_SCALE");
    const bool restart = fabs((rs ? atof(rs) : 1.0) - (double)vk_postfx_render_scale()) > 0.01;
    char b[160];
    snprintf(b, sizeof b, "%s settings on%s", low ? "Low-end PC" : "Standard",
             restart ? " (the internal resolution changes at the next start)" : "");
    notice_for(b, 5.0);
}

extern "C" int rsx_vk_vram_now(unsigned* use_mb, unsigned* budget_mb, unsigned* card_mb);

static void lowend_section(void)
{
    section("Presets");
    const int st = lowend_state();
    if (ImGui::Button("Low-end PC")) lowend_preset(true);
    ImGui::SameLine();
    if (ImGui::Button("Standard")) lowend_preset(false);
    ImGui::SameLine();
    ImGui::TextColored(kGrey, "now: %s", st == 1 ? "Low-end PC" : st == 2 ? "Standard" : "your own mix");
    hint("Low-end PC: 720p, fire and smoke at the PS3's size, the PS3's ambient occlusion and shadow edges, and a "
         "texture cache that lets go of textures not seen for a while. Less GPU time and video memory: for 4 GB "
         "graphics cards, older laptops and integrated graphics. Standard: the port's usual look (1080p, full-size "
         "effects and ambient occlusion, soft shadows). Each button sets these on this page and the Lighting and "
         "Effects pages at once; you can still change any of them. The internal resolution changes at the next start. "
         "On a slow processor a 30 fps cap (Display page) also keeps the frame rate even.");
    static unsigned use, bud, card;            /* the driver's numbers, asked twice a second at most */
    static int have;
    static double asked = -1.0;
    const double t = now_s();
    if (asked < 0.0 || t - asked >= 0.5) { asked = t; have = rsx_vk_vram_now(&use, &bud, &card); }
    char b[320];
    if (card) {
        int k = snprintf(b, sizeof b, "This PC: %s, %.1f GB of video memory%s.", s_gpu_name[0] ? s_gpu_name : "graphics card",
                         (double)card / 1024.0, s_le_integrated ? " (integrated: shared with the system)" : "");
        if (s_le_auto == 1 && s_le_changed > 0 && k > 0 && k < (int)sizeof b)
            k += snprintf(b + k, sizeof b - (size_t)k, " %s, so the game started with the Low-end PC values for the "
                          "settings you had not set yourself.",
                          s_le_integrated ? "Integrated graphics" : card <= LOWEND_VRAM_MB ? "That is little"
                                                                   : "PS3RECOMP_LOWEND=1 asks for it");
        if (have && k > 0 && k < (int)sizeof b)
            snprintf(b + k, sizeof b - (size_t)k, " In use now: %.1f GB of the %.1f GB %s lets the game have.",
                     (double)use / 1024.0, (double)bud / 1024.0, kHostOs);
        hint(b);
    }
}
