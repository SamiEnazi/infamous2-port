/* [shadow] The Lighting page's shadow-map resolution (included by
 * vk_overlay.cpp before vk_overlay_light.inc). A get / set pair any menu
 * layout can call, plus a ready row (combo, live mark, one-line hint).
 *
 *   Shadow quality  PS3 (1024) | High (2048) | Ultra (4096)
 *                   PS3RECOMP_SHADOW_RES=1|2|4, live (the draw engine's
 *                   rsx_shadow_res.inc: the sun-shadow maps drawn that many
 *                   times larger on each side; the game's shaders unchanged).
 *                   Unset = High. F3 (the Lighting page's compare) shows PS3.
 */
extern "C" int rsx_draw_engine_shadow_res_q(void);   /* rsx_draw_engine.h */
static const char* const kShadowQualityName[] = { "PS3 (1024)", "High (2048)", "Ultra (4096)" };
static const char* const kShadowQualityVal[]  = { "1", "2", "4" };
enum { SHADOW_QUALITY_N = 3 };
static const char kShadowQualityHint[] = "Sharper sun shadows from a larger shadow map. Small GPU cost.";

/* The scale in effect (the engine's value; quarter steps 4 / 8 / 16). */
static int shadow_quality_get(void)
{
    const int q = rsx_draw_engine_shadow_res_q();
    return q >= 16 ? 2 : q > 4 ? 1 : 0;
}
/* Live now (the engine's applier) and into the settings file. */
static void shadow_quality_set(int i)
{
    if (i < 0 || i >= SHADOW_QUALITY_N) return;
    ps3_setting_apply("PS3RECOMP_SHADOW_RES", kShadowQualityVal[i]);
    extra_set("PS3RECOMP_SHADOW_RES", kShadowQualityVal[i]);
}
static bool shadow_quality_live(void) { return ps3_setting_applier("PS3RECOMP_SHADOW_RES") != NULL; }

/* F3 compare: the PS3's maps while comparing, the choice back after (not
 * saved: the file keeps the choice). */
static char s_shq_saved[8];
static void shadow_quality_compare(bool ps3_look)
{
    if (ps3_look) {
        snprintf(s_shq_saved, sizeof s_shq_saved, "%s", kShadowQualityVal[shadow_quality_get()]);
        ps3_setting_apply("PS3RECOMP_SHADOW_RES", "1");
    } else if (s_shq_saved[0]) {
        ps3_setting_apply("PS3RECOMP_SHADOW_RES", s_shq_saved);
    }
}

static void shadow_quality_row(void)
{
    int c = shadow_quality_get();
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::Combo("Shadow quality", &c, kShadowQualityName, SHADOW_QUALITY_N)) shadow_quality_set(c);
    ImGui::SameLine();
    if (shadow_quality_live()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "live");
    else ImGui::TextColored(kGrey, "on restart");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("PS3RECOMP_SHADOW_RES");
    hint(kShadowQualityHint);
}
