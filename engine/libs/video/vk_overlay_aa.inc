/* [aa] The Graphics page's anti-aliasing choice and thin-lines switch
 * (included by vk_overlay.cpp). Each is a get / set pair any menu layout can
 * call, plus a ready row (combo or checkbox, mark, one-line hint).
 *
 *   Anti-aliasing  Off | FXAA | SMAA | SMAA + MSAA 2x | SMAA + MSAA 4x
 *                  post AA (PS3RECOMP_AA, live) and MSAA (PS3RECOMP_MSAA, read at
 *                  start: vk_msaa.inc). PS3RECOMP_MSAA=8 stays an env-only value;
 *                  the choice shows it as 4x.
 *   Thin lines     PS3RECOMP_ALPHA_AA on | off (read at start): alpha-tested
 *                  ropes, wires, fences and leaves keep their shape at a distance
 *                  (mip-scaled test) and, with MSAA, get alpha to coverage.
 */
static const char* const kAaChoiceName[] = { "Off", "FXAA (fast)", "SMAA (sharper)", "SMAA + MSAA 2x", "SMAA + MSAA 4x" };
enum { AA_CHOICES = 5 };
static const char kAaChoiceHint[] = "MSAA smooths thin lines and edges; costs GPU time, needs a restart.";
static const char kThinLinesHint[] = "Keeps ropes, wires and fences whole at a distance (restart).";

/* The choice in effect: post AA as it runs now, MSAA as the next start reads it. */
static int aa_choice_get(void)
{
    const char* e = getenv(kVar[V_MSAA]);
    const int ms = (e && *e && strcmp(e, "off")) ? atoi(e) : 0;
    if (ms >= 4) return 4;
    if (ms >= 2) return 3;
    const int a = vk_postfx_aa();
    return a == VK_POSTFX_AA_SMAA ? 2 : a == VK_POSTFX_AA_FXAA ? 1 : 0;
}
static void aa_choice_set(int i)
{
    static const char* const post[AA_CHOICES] = { "off", "fxaa", "smaa", "smaa", "smaa" };
    static const char* const ms[AA_CHOICES]   = { "off", "off", "off", "2", "4" };
    if (i < 0 || i >= AA_CHOICES) return;
    apply_setting(V_AA, post[i]);
    apply_setting(V_MSAA, ms[i]);
}
static bool thin_lines_get(void)
{
    const char* e = getenv(kVar[V_ALPHA_AA]);
    return e && (!strcmp(e, "on") || !strcmp(e, "1") || !strcmp(e, "mip") || !strcmp(e, "cov"));
}
static void thin_lines_set(bool on) { apply_setting(V_ALPHA_AA, on ? "on" : "off"); }

/* MSAA's part needs a restart once changed; the post AA part is live. */
static bool aa_msaa_changed(void)
{
    const char* now = getenv(kVar[V_MSAA]);
    return strcmp(now ? now : "", s_start[V_MSAA]) != 0;
}

static void aa_choice_row(void)
{
    int c = aa_choice_get();
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::Combo("Anti-aliasing", &c, kAaChoiceName, AA_CHOICES)) aa_choice_set(c);
    if (aa_msaa_changed()) restart_mark(V_MSAA, true);
    else live_mark(V_AA);
    hint(kAaChoiceHint);
}
static void thin_lines_row(void)
{
    bool on = thin_lines_get();
    if (ImGui::Checkbox("Thin lines at a distance", &on)) thin_lines_set(on);
    live_mark(V_ALPHA_AA);
    hint(kThinLinesHint);
}
