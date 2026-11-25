/*
 * vk_overlay_menu.inc -- the F2 settings menu, design "E" (picked 2026-11-16:
 * "it's a setting for gamers, no need for all the text"). Included by vk_overlay.cpp
 * after every page helper (light, update, lowend, aa, remap, mouse).
 *
 *   The game stays visible: a dark panel on the left (780 of 1280) fades into it.
 *   A slim icon rail picks the page (the last icon: About); the header names it. One row per setting: the
 *   label left; right a controller-style "< value >" with dots for its place in the
 *   list, a switch, a slider or a button. A card bottom right over the game: the
 *   focused setting's name, ONE short line, its GPU cost (0-3 bars), when it applies
 *   ("Instant" / "After restart" / "Restart needed") and at most a line or two of
 *   live status (the GPU and its memory, the display's Hz, the language in use, ...).
 *   Footer: L1/R1 pages, F3 the PS3 look, Quit game, Resume.
 *   Compare: on a post-processing row (anti-aliasing, sharpening, upscaling, tone,
 *   clarity) a "PS3 | YOURS" line over the game, dragged with the card's slider, the
 *   mouse or L2/R2: vk_postfx_set_compare (the post passes and the tone pass leave the
 *   left of it as it came). Scene settings (shadows, AO, effects) say "Hold F3".
 *
 * Rows are built every frame by the page functions with mrow_* one-liners; a new
 * option is one line. menu_census (at attach) builds every page once and names, loudly,
 * any setting of kVar (or of kMenuExtras) that has no row.
 *
 * Input: the menu's own navigation; ImGui's stays on for the popups only (the panel's
 * windows are NoNav). D-pad / left stick / arrows: rows (up, down) and values (left,
 * right); Cross / Enter / Space: switch, press, next value; Circle / Esc: back (a
 * sub-page) or close; L1 / R1 (Q / E, Tab): pages, wrapping; L2 / R2 ([ / ]): move the
 * compare line; Square / Delete: clear a remap cell; Triangle: Quit game. Mouse: a row
 * under a moving mouse takes the focus; click the arrows, the value, the switch, the
 * slider, a button, the rail. Layout in 720-line units (S()): the panel keeps its
 * proportion at any window size and the game shows on the right (ultrawide: more of it).
 */
#include "../../include/ps3emu/ps3_cheats.h"
#include <ctype.h>
#include <functional>

extern "C" {
int  vk_overlay_cheat_change(ps3_cheat* c, double v);   /* vk_overlay_cheats.cpp */
int  vk_overlay_cheat_failed(const ps3_cheat* c);
void vk_overlay_cheats_refresh(void);
void vk_overlay_cheats_set_remember(int on);
}

/* ---- rows ----------------------------------------------------------------- */
enum { RK_CHOICE, RK_TOGGLE, RK_SLIDER, RK_ACTION, RK_HEAD, RK_INFO };   /* RK_INFO: a line to read (About), vtext at its right */
enum { WH_NONE, WH_INSTANT, WH_RESTART, WH_PENDING };
enum { CMP_NONE, CMP_SPLIT, CMP_F3 };
enum { SUB_NONE, SUB_REMAP, SUB_ZIP, SUB_NOTES };

struct MRow {
    std::string id, label;
    int kind = RK_ACTION;
    std::vector<std::string> opts;       /* choice */
    int cur = 0;                         /* choice index; -1 = a value not in the list (vtext) */
    float v = 0, mn = 0, mx = 1, step = 1;   /* slider; an action's amount */
    bool bipolar = false, has_amount = false;
    std::string vtext;                   /* value shown (slider, custom choice, amount, an action's state) */
    bool on = false;                     /* toggle */
    std::string button;                  /* action */
    bool disabled = false, indent = false, save_tag = false, compact = false;   /* compact: a list's line (About's names) */
    std::string desc, status, note;      /* the card: one line, live status, a second note */
    bool status_warn = false;
    ImU32 head_col = 0;                  /* RK_HEAD: its colour (0 = the muted grey) */
    int cost = -1, when = WH_INSTANT, cmp = CMP_NONE;
    bool env = false;
    uint64_t vars = 0;                   /* the kVar settings it shows (census) */
    const char* extra = NULL;            /* a setting outside kVar (census, logs) */
    ImTextureData* pic = NULL;           /* a mod's preview picture (the card) */
    std::function<void(int)> set;        /* choice: index; toggle: 0 / 1 */
    std::function<void(float)> setf;     /* slider value; action amount */
    std::function<void()> press;         /* action */

    MRow& d(const char* s) { desc = s ? s : ""; return *this; }
    MRow& d(const std::string& s) { desc = s; return *this; }
    MRow& st(const std::string& s, bool warn = false) { status = s; status_warn = warn; return *this; }
    MRow& nt(const std::string& s) { note = s; return *this; }
    MRow& c(int k) { cost = k; return *this; }
    MRow& cm(int m) { cmp = m; return *this; }
    MRow& wh(int w) { when = w; return *this; }
    MRow& off(bool dis) { disabled = disabled || dis; return *this; }
    MRow& ind(bool i = true) { indent = i; return *this; }
    MRow& var(int k) { if (k >= 0 && k < V_N) { vars |= 1ull << k; if (s_env_set[k]) env = true; } return *this; }
    /* a kVar setting a module may take live: "Instant", or read at start: "After restart" /
     * "Restart needed" once changed (today's live / on restart / restart needed marks) */
    MRow& live(int k)
    {
        var(k);
        if (ps3_setting_applier(kVar[k])) { when = WH_INSTANT; return *this; }
        const char* now = getenv(kVar[k]);
        when = strcmp(now ? now : "", s_start[k]) ? WH_PENDING : WH_RESTART;
        return *this;
    }
    MRow& start(int k) { var(k); const char* now = getenv(kVar[k]); when = strcmp(now ? now : "", s_start[k]) ? WH_PENDING : WH_RESTART; return *this; }
    MRow& ex(const char* name) { extra = name; when = ps3_setting_applier(name) ? WH_INSTANT : WH_RESTART; return *this; }
};
typedef std::vector<MRow> Rows;

static std::string sfmt(const char* fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    return b;
}

static MRow& mrow(Rows& R, const char* id, const std::string& label, int kind)
{
    R.emplace_back();
    MRow& r = R.back();
    r.id = id; r.label = label; r.kind = kind;
    return r;
}
static MRow& mrow_choice(Rows& R, const char* id, const std::string& label, std::vector<std::string> opts, int cur,
                         std::function<void(int)> set)
{
    MRow& r = mrow(R, id, label, RK_CHOICE);
    r.opts = std::move(opts); r.cur = cur; r.set = std::move(set);
    return r;
}
static MRow& mrow_toggle(Rows& R, const char* id, const std::string& label, bool on, std::function<void(bool)> set)
{
    MRow& r = mrow(R, id, label, RK_TOGGLE);
    r.on = on;
    r.set = [set](int v) { set(v != 0); };
    return r;
}
/* `text`: the value as shown ("" = the number with `unit`) */
static MRow& mrow_slider(Rows& R, const char* id, const std::string& label, float v, float mn, float mx, float step,
                         const std::string& text, std::function<void(float)> set)
{
    MRow& r = mrow(R, id, label, RK_SLIDER);
    r.v = v; r.mn = mn; r.mx = mx; r.step = step; r.vtext = text; r.setf = std::move(set);
    r.bipolar = mn < 0.0f && mx > 0.0f;
    return r;
}
static MRow& mrow_action(Rows& R, const char* id, const std::string& label, const std::string& button, std::function<void()> press)
{
    MRow& r = mrow(R, id, label, RK_ACTION);
    r.button = button; r.press = std::move(press);
    return r;
}
static MRow& mrow_info(Rows& R, const char* id, const std::string& label, const std::string& value = std::string())
{
    MRow& r = mrow(R, id, label, RK_INFO);
    r.vtext = value; r.when = WH_NONE;
    return r;
}
static MRow& mrow_head(Rows& R, const char* id, const std::string& label, ImU32 col = 0)
{
    MRow& r = mrow(R, id, label, RK_HEAD);
    r.head_col = col;
    return r;
}

/* ---- state ------------------------------------------------------------------ */
static int    s_sub;                 /* SUB_* of the page (Controls: remap, Mods: zip, Update: notes) */
static int    s_sub_want = -1;       /* a sub-page to switch to at the next frame */
static int    s_fi;                  /* the focus: -1 the pills / Back, 0..n-1 a row, n the footer */
static std::string s_fkey;           /* the focused row's id (kept when the list changes) */
static std::string s_focus_req;      /* the timed script's focus=<id> */
static std::string s_page_key[16];   /* each page's last focus */
static int    s_pill, s_foot = 1;    /* which pill / footer button has the focus */
static int    s_rm_col;              /* remap grid: the focused column (0..3) */
static bool   s_scroll_focus;        /* keep the focused row in view (after a key / pad move) */
static float  s_split = 0.45f;       /* the compare line: 0 = the panel's edge .. 1 = the window's */
static bool   s_split_drag, s_line_drag;   /* the card's slider / the line itself, held by the mouse */
static std::string s_drag_id;        /* the slider being dragged with the mouse */
static float  s_card_top;            /* the card's top (the notices sit above it) */
static bool   s_census;              /* menu_census: build every row, touch nothing slow */
static uint64_t s_census_vars;
static std::vector<std::function<void()>> s_ops;   /* row changes, run after the frame's drawing */
#define s_font_semi s_font_semi_load   /* Segoe UI Semibold (the values), else the bold face (imgui_setup) */
static char   s_nav_script[8];       /* the timed script's nav=<key>: one simulated press per frame */
static int    s_nav_script_n;
static char   s_zip_res[2400];       /* the last zip install's report */
static int    s_zip_kind;

static const char* const kTabName[TAB_ABOUT + 1] = { "Display", "Graphics", "Audio", "Controls", "HUD", "Lighting",
                                                     "Effects", "Mods", "Update", "Cheats", "About" };

static void menu_sub(int sub)
{
    if (sub == s_sub) return;
    fprintf(stderr, "[overlay] menu: %s %s\n", kTabName[tab_page(s_tab)], sub == SUB_REMAP ? "> Buttons and keys" : sub == SUB_ZIP ? "> Install from zip"
                                                               : sub == SUB_NOTES ? "> What's new" : "< back");
    const int from = s_sub;
    s_sub = sub;
    s_fi = 0; s_fkey.clear(); s_scroll_focus = true; s_rm_col = 0;
    if (sub == SUB_ZIP) ps3_game_mods_zips_refresh();
    if (sub == SUB_NONE) s_fkey = from == SUB_REMAP ? "remap" : from == SUB_ZIP ? "zip" : from == SUB_NOTES ? "notes" : "";
}
static void menu_set_tab(int t)
{
    if (t < 0 || t >= tab_count()) return;
    if (t != s_tab || s_sub) {
        if (!s_sub) s_page_key[s_tab & 15] = s_fkey;
        if (s_sub) { remap_cancel(); s_sub = SUB_NONE; }
        fprintf(stderr, "[overlay] menu: page %s\n", kTabName[tab_page(t)]);
        s_tab = t;
        s_fkey = s_page_key[t & 15];
        s_fi = 0;
        s_scroll_focus = true;
    }
}

/* set_open(0): no compare line left on the picture, the page itself next time */
static void menu_closed(void)
{
    vk_postfx_set_compare(0.0f);
    s_split_drag = s_line_drag = false;
    s_drag_id.clear();
    if (s_sub) { s_sub = SUB_NONE; s_fkey.clear(); }
    s_card_top = 0.0f;
}

/* The timed script (PS3RECOMP_OVERLAY_MENU_AT): sub=remap|zip|notes|none, focus=<row id>
 * ("#pills", "#footer" too), nav=up|down|left|right|ok|back|l1|r1 (one press, as the pad's),
 * split=<0..1> (the compare line). */
static void menu_script(const char* w)
{
    if (!SDL_strncasecmp(w, "sub=", 4)) {
        const char* s = w + 4;
        s_sub_want = !SDL_strcasecmp(s, "remap") ? SUB_REMAP : !SDL_strcasecmp(s, "zip") ? SUB_ZIP : !SDL_strcasecmp(s, "notes") ? SUB_NOTES : SUB_NONE;
        if (s_sub_want == SUB_REMAP) s_tab_want = 3;
        else if (s_sub_want == SUB_ZIP) s_tab_want = TAB_MODS;
        else if (s_sub_want == SUB_NOTES) s_tab_want = TAB_UPDATE;
    } else if (!SDL_strncasecmp(w, "focus=", 6)) s_focus_req = w + 6;
    else if (!SDL_strncasecmp(w, "nav=", 4)) { snprintf(s_nav_script, sizeof s_nav_script, "%s", w + 4); s_nav_script_n = 1; }
    else if (!SDL_strncasecmp(w, "split=", 6)) { const float f = (float)atof(w + 6); s_split = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f; }
    set_open(1);
}

/* Esc, Circle: a question's Cancel, a sub-page's Back, else close. */
static bool menu_modal(void);
static void menu_escape(void)
{
    if (s_quit_ask) { s_quit_cancel = true; return; }
    if (s_restart_ask) { s_restart_cancel = true; return; }
    if (s_cf_ask) { s_cf_cancel = true; return; }
    if (s_upd_ask) { s_upd_cancel = true; return; }
    if (remap_escape() || remap_capturing()) return;
    if (s_sub) { s_sub_want = SUB_NONE; return; }
    set_open(0);
}

/* ---- colours and drawing helpers ---------------------------------------------- */
static ImU32 rgba(int r, int g, int b, float a = 1.0f) { return IM_COL32(r, g, b, (int)(a * 255.0f + 0.5f)); }
#define C_TEXT     rgba(0xe9, 0xee, 0xf3)
#define C_LABEL    rgba(0xcf, 0xd8, 0xe1)
#define C_MUTED    rgba(0x7f, 0x91, 0xa3)
#define C_MUTED2   rgba(0x8f, 0xa1, 0xb3)
#define C_LINE     rgba(0x3a, 0x47, 0x56)
#define C_TRACK    rgba(0x34, 0x42, 0x52)
#define C_BAROFF   rgba(0x2a, 0x36, 0x44)
#define C_DESC     rgba(0xb8, 0xc4, 0xcf)
#define C_AMBER    rgba(0xf2, 0xb4, 0x5a)
#define C_DARK     rgba(0x06, 0x12, 0x1c)
static ImU32 acc(float a = 1.0f) { return vk_ov_u32(vk_ov_theme().accent, a); }
static ImU32 acc_soft(void) { return acc(vk_ov_theme_red() ? 0.16f : 0.14f); }
static ImU32 dim(ImU32 c, bool d) { return d ? (c & 0x00FFFFFFu) | ((((c >> 24) & 255u) * 45u / 100u) << 24) : c; }

static ImFont* f_reg(void) { return ImGui::GetIO().Fonts->Fonts[0]; }
static ImFont* f_bold(void) { ImFont* b = vk_ov_theme_bold(); return b ? b : f_reg(); }
static ImFont* f_semi(void) { return s_font_semi ? s_font_semi : f_bold(); }
static float tw(ImFont* f, float px, const char* t) { return f->CalcTextSizeA(S(px), FLT_MAX, 0.0f, t).x; }
static void txt(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 col, const char* t) { dl->AddText(f, S(px), p, col, t); }
/* "Barlow Condensed": the bold face drawn narrower (its vertices squeezed) */
static float tw_cond(float px, const char* t, float sx = 0.84f) { return tw(f_bold(), px, t) * sx; }
static void txt_cond(ImDrawList* dl, float px, ImVec2 p, ImU32 col, const char* t, float sx = 0.84f)
{
    const int v0 = dl->VtxBuffer.Size;
    dl->AddText(f_bold(), S(px), p, col, t);
    for (int i = v0; i < dl->VtxBuffer.Size; i++) dl->VtxBuffer[i].pos.x = p.x + (dl->VtxBuffer[i].pos.x - p.x) * sx;
}
/* letter-spaced caps ("SETTINGS") */
static float txt_spaced(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 col, const char* t, float sp)
{
    float x = p.x;
    for (const char* c = t; *c; c++) {
        if (dl) dl->AddText(f, S(px), ImVec2(x, p.y), col, c, c + 1);
        x += f->CalcTextSizeA(S(px), FLT_MAX, 0.0f, c, c + 1).x + S(sp);
    }
    return x - p.x;
}
/* the text in at most w pixels: smaller (to 80%), then cut with "..." */
static void txt_fit(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 col, const char* t, float w, float align = 0.0f)
{
    float size = px, width = tw(f, size, t);
    if (width > w) { size = px * (w / width > 0.8f ? w / width : 0.8f); width = tw(f, size, t); }
    if (width <= w + 0.5f) {
        dl->AddText(f, S(size), ImVec2(p.x + (w - width) * align, p.y + (S(px) - S(size)) * 0.5f), col, t);
        return;
    }
    std::string s = t;
    while (!s.empty() && tw(f, size, (s + "...").c_str()) > w) s.pop_back();
    s += "...";
    dl->AddText(f, S(size), ImVec2(p.x, p.y + (S(px) - S(size)) * 0.5f), col, s.c_str());
}
static void rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float r) { dl->AddRectFilled(a, b, col, S(r)); }
static bool hov(ImVec2 a, ImVec2 b) { return ImGui::IsMouseHoveringRect(a, b, false); }

/* ---- the rail's icons: the mockup's 24-unit SVG strokes ------------------------- */
static void icon(ImDrawList* dl, int which, ImVec2 c, float size, ImU32 col)
{
    const float k = size / 24.0f, th = 2.0f * k;
    auto P = [&](float x, float y) { return ImVec2(c.x + (x - 12.0f) * k, c.y + (y - 12.0f) * k); };
    auto line = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), col, th); };
    auto poly = [&](std::initializer_list<float> xy, bool closed) {
        ImVec2 p[16]; int n = 0;
        for (auto it = xy.begin(); it != xy.end() && n < 16; it += 2) p[n++] = P(*it, *(it + 1));
        dl->AddPolyline(p, n, col, closed ? ImDrawFlags_Closed : 0, th);
    };
    switch (which) {
    case 0:   /* display */
        dl->AddRect(P(3, 4), P(21, 16), col, 2.0f * k, 0, th);
        line(8, 20, 16, 20); line(12, 16, 12, 20);
        break;
    case 1:   /* graphics: layers */
        poly({ 12, 3, 21, 8, 12, 13, 3, 8 }, true);
        poly({ 3, 13, 12, 18, 21, 13 }, false);
        break;
    case 2:   /* audio */
        poly({ 4, 9, 4, 15, 8, 15, 13, 19, 13, 5, 8, 9 }, true);
        dl->PathArcTo(P(13.354f, 12), 4.0f * k, -0.848f, 0.848f, 8); dl->PathStroke(col, 0, th);
        dl->PathArcTo(P(13.708f, 12), 8.0f * k, -0.848f, 0.848f, 12); dl->PathStroke(col, 0, th);
        break;
    case 3:   /* controls: a controller */
        dl->AddRect(P(2, 8), P(22, 17), col, 4.5f * k, 0, th);
        line(7, 11, 7, 14); line(5.5f, 12.5f, 8.5f, 12.5f);
        dl->AddCircleFilled(P(16.5f, 11.8f), 1.1f * k, col); dl->AddCircleFilled(P(18.5f, 13.6f), 1.1f * k, col);
        break;
    case 4:   /* HUD: a graph */
        poly({ 3, 17, 8, 11, 12, 15, 16, 8, 21, 17 }, false);
        line(3, 21, 21, 21);
        break;
    case 5:   /* lighting: a sun */
        dl->AddCircle(P(12, 12), 4.0f * k, col, 16, th);
        line(12, 2, 12, 4); line(12, 20, 12, 22); line(2, 12, 4, 12); line(20, 12, 22, 12);
        line(4.9f, 4.9f, 6.3f, 6.3f); line(17.7f, 17.7f, 19.1f, 19.1f); line(4.9f, 19.1f, 6.3f, 17.7f); line(17.7f, 6.3f, 19.1f, 4.9f);
        break;
    case 6:   /* effects: a flame */
        dl->PathLineTo(P(12, 3));
        dl->PathBezierCubicCurveTo(P(14, 7), P(18, 8), P(18, 13));
        dl->PathArcTo(P(12, 13), 6.0f * k, 0.0f, 3.14159f, 12);
        dl->PathBezierCubicCurveTo(P(6, 10), P(8, 9), P(8, 6));
        dl->PathBezierCubicCurveTo(P(10, 7), P(11, 9), P(11, 11));
        dl->PathBezierCubicCurveTo(P(12, 9), P(12, 6), P(12, 3));
        dl->PathStroke(col, ImDrawFlags_Closed, th);
        break;
    case 7:   /* mods: a piece */
        poly({ 9, 3, 15, 3, 15, 7, 19, 7, 19, 13, 15, 13, 15, 17, 9, 17, 9, 13, 5, 13, 5, 7, 9, 7 }, true);
        break;
    case 8:   /* update: a download */
        line(12, 4, 12, 15); poly({ 7, 10, 12, 15, 17, 10 }, false); line(4, 20, 20, 20);
        break;
    case 9:   /* cheats: a bolt */
        poly({ 13, 2, 4, 14, 11, 14, 10, 22, 19, 10, 12, 10 }, true);
        break;
    default:  /* about: an "i" in a circle */
        dl->AddCircle(P(12, 12), 9.0f * k, col, 24, th);
        line(12, 11, 12, 17);
        dl->AddCircleFilled(P(12, 7.6f), 1.3f * k, col);
        break;
    }
}

/* ---- the pages' rows ------------------------------------------------------------ */
static const char* const kCapShort[] = { "30", "60", "72", "120", "144", "Unlimited" };
static std::vector<std::string> vs(std::initializer_list<const char*> l) { std::vector<std::string> v; for (const char* s : l) v.push_back(s); return v; }
static std::vector<std::string> vs(const char* const* a, int n) { std::vector<std::string> v; for (int i = 0; i < n; i++) v.push_back(a[i]); return v; }

static void page_display(Rows& R)
{
    { MRow& r = mrow_choice(R, "fps", "Frame rate cap", vs(kCapShort, 6), s_ui.cap, [](int i) {
          s_ui.cap = i; char b[16]; snprintf(b, sizeof b, "%d", kCap[i]); apply_setting(V_FPS_CAP, b); });
      if (s_ui.cap < 0) r.vtext = sfmt("%d (custom)", s_ui.cap_custom);
      r.d("60 is the target. Higher caps suit 120 Hz+ screens.").c(0).live(V_FPS_CAP);
      if (s_refresh_hz > 0) r.st(sfmt("This display: %d Hz.", s_refresh_hz)); }
    mrow_choice(R, "present", "Present mode", vs({ "Vsync", "Mailbox", "Immediate" }), s_ui.present,
                [](int i) { s_ui.present = i; apply_setting(V_PRESENT, kPresent[i]); })
        .d("Mailbox: low input lag, no tearing. Immediate may tear.").c(0).live(V_PRESENT);
    mrow_toggle(R, "full", "Fullscreen", s_ui.fullscreen, [](bool on) { s_ui.fullscreen = on; apply_setting(V_FULLSCREEN, on ? "1" : "0"); })
        .d("Borderless. Also Alt+Enter or F11.").c(0).live(V_FULLSCREEN);
    mrow_toggle(R, "capture", "Capture-friendly window", s_ui.capture, [](bool on) { s_ui.capture = on; apply_setting(V_CAPTURE, on ? "1" : "0"); })
        .d("Lets Lossless Scaling and OBS grab the game.").c(0).live(V_CAPTURE);
    { MRow& r = mrow_choice(R, "aspect", "Aspect ratio", vs({ "16:9", "Auto", "21:9", "32:9", "16:10" }), s_ui.aspect,
                            [](int i) { s_ui.aspect = i; apply_setting(V_ASPECT, kAspect[i]); });
      if (s_ui.aspect < 0) { const char* e = getenv(kVar[V_ASPECT]); r.vtext = e ? e : "?"; }
      r.d("Auto follows your screen. Ultrawide shows more of the city.").c(1).live(V_ASPECT);
      SDL_Window* w = (SDL_Window*)s_vk.sdl_window;
      int ww = 0, wh = 0; if (w && !s_census) SDL_GetWindowSize(w, &ww, &wh);
      const float rs = vk_postfx_render_scale(), ka = vk_postfx_aspect_k();
      r.st(sfmt("Now %s: window %d x %d, game image %u x %u.", ka > 1.001f || ka < 0.999f ? "wide" : "16:9", ww, wh,
                (unsigned)((float)s_vk.render_width * rs * ka + 0.5f), (unsigned)((float)s_vk.render_height * rs + 0.5f))); }
    { const bool have = ps3_setting_applier(kVar[V_FOV]) != NULL;
      MRow& r = mrow_slider(R, "fov", "Field of view", (float)s_ui.fov, (float)kFovMin, (float)kFovMax, 5.0f, sfmt("%d%%", s_ui.fov),
                            [](float v) { s_ui.fov = (int)v; char b[16]; snprintf(b, sizeof b, "%d", s_ui.fov); apply_setting(V_FOV, b); });
      r.d("100% is the game's own camera.").c(0).live(V_FOV).off(!have);
      const double vv = kFovGameV * s_ui.fov / 100.0;
      const double hh = 2.0 * atan(tan(vv * 0.5) * (16.0 / 9.0) * (double)vk_postfx_aspect_k());
      r.st(have ? sfmt("The free camera now sees %.0f\xC2\xB0 across, %.0f\xC2\xB0 up and down.", hh * 57.29578, vv * 57.29578)
                : std::string("Not in this build (its lift has no camera hooks)."), !have); }
    mrow_choice(R, "theme", "Menu theme", vs({ "Hero", "Infamous" }), vk_ov_theme_red(),
                [](int i) { apply_setting(V_THEME, i ? "infamous" : "hero"); })
        .d("Blue or red menu colours.").c(0).live(V_THEME);
}

/* (lowend) the Graphics presets' card: this PC */
static std::string lowend_status(void)
{
    if (s_census) return "";
    static unsigned use, bud, card;
    static int have;
    static double asked = -1.0;
    const double t = now_s();
    if (asked < 0.0 || t - asked >= 0.5) { asked = t; have = rsx_vk_vram_now(&use, &bud, &card); }
    if (!card) return s_gpu_name[0] ? std::string(s_gpu_name) : std::string();
    std::string s = sfmt("%s, %.1f GB%s", s_gpu_name[0] ? s_gpu_name : "GPU", (double)card / 1024.0, s_le_integrated ? " (shared)" : "");
    if (have) s += sfmt(" \xC2\xB7 %.1f of %.1f GB in use", (double)use / 1024.0, (double)bud / 1024.0);
    return s;
}

static void page_graphics(Rows& R)
{
    { MRow& r = mrow_choice(R, "res", "Internal resolution", vs({ "720p", "1080p", "1440p", "4K" }), s_ui.scale,
                            [](int i) { s_ui.scale = i; apply_setting(V_RENDER_SCALE, kScale[i]); });
      const float rs = vk_postfx_render_scale();
      r.d("The game's resolution: what it is drawn at. Lower = faster, higher = sharper.").c(3).live(V_RENDER_SCALE)
       .st(sfmt("Now %u x %u. A restart is needed to see a change.", (unsigned)(1280.0f * rs * vk_postfx_aspect_k() + 0.5f), (unsigned)(720.0f * rs + 0.5f))); }
    /* [aa] feat/aa: one choice for post AA + MSAA, and the thin-lines switch (vk_overlay_aa.inc) */
    { const int a = aa_choice_get();
      MRow& r = mrow_choice(R, "aa", "Anti-aliasing", vs(kAaChoiceName, AA_CHOICES), a, [](int i) { aa_choice_set(i); });
      r.d(kAaChoiceHint).c(a >= 3 ? 2 : 1).cm(a < 3 ? CMP_SPLIT : CMP_NONE).var(V_MSAA);
      if (aa_msaa_changed()) r.var(V_AA).wh(WH_PENDING); else r.live(V_AA); }
    mrow_toggle(R, "thin", "Thin lines", thin_lines_get(), [](bool on) { thin_lines_set(on); }).d(kThinLinesHint).c(1).live(V_ALPHA_AA);
    mrow_slider(R, "sharpen", "Sharpening", s_ui.sharpen * 100.0f, 0.0f, 100.0f, 5.0f,
                s_ui.sharpen > 0.0f ? sfmt("%.0f%%", s_ui.sharpen * 100.0f) : std::string("Off"),
                [](float v) { s_ui.sharpen = v / 100.0f; char b[16]; snprintf(b, sizeof b, "%.2f", s_ui.sharpen); apply_setting(V_SHARPEN, b); })
        .d("Crisper textures after scaling.").c(0).cm(CMP_SPLIT).live(V_SHARPEN);
    mrow_choice(R, "aniso", "Texture filtering", vs({ "Off", "2x", "4x", "8x", "16x" }), s_ui.aniso,
                [](int i) { s_ui.aniso = i; apply_setting(V_ANISO, kAniso[i]); })
        .d("Keeps floors and streets sharp into the distance.").c(1).live(V_ANISO);
    { MRow& r = mrow_choice(R, "up", "Upscaling", vs({ "FSR 1", "CAS", "Bilinear" }), s_ui.upscale,
                            [](int i) { s_ui.upscale = i; apply_setting(V_UPSCALE, kUp[i]); });
      int ww = 0, wh = 0;   /* the swapchain's pixels (the window's size x its scale) */
      if (!s_census) { const ImGuiIO& io = ImGui::GetIO();
                       ww = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x + 0.5f); wh = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y + 0.5f); }
      const float rs = vk_postfx_render_scale();
      const bool used = ww > (int)(1280.0f * rs * vk_postfx_aspect_k() + 0.5f) && wh > (int)(720.0f * rs + 0.5f);
      r.d("Used when the window is bigger than the game image.").c(1).cm(CMP_SPLIT).live(V_UPSCALE)
       .st(used ? "In use now: the window is bigger than the game image." : "Not in use now: the window is not bigger than the game image."); }
    { const int slot = spu_patch_find("mesh_trim");
      mrow_choice(R, "trim", "Mesh trimming", vs({ "Render res", "Off", "PS3 720p" }), s_ui.mesh_trim, [](int i) {
          s_ui.mesh_trim = i; apply_setting(V_MESH_TRIM, i == 1 ? "0" : "1"); apply_setting(V_EDGE_VP, i == 2 ? "0" : "1"); })
          .d("Skips triangles too small to see.").c(2).live(V_MESH_TRIM).var(V_EDGE_VP).off(slot < 0)
          .st(slot < 0 ? "Not in this build (no tools/spu_patches.txt in its lift)." : s_ui.mesh_trim == 2 ? "Leaves holes above 720p." : "", slot < 0 || s_ui.mesh_trim == 2); }
    mrow_choice(R, "dof", "Depth of field", vs({ "On", "Cutscenes", "Off" }), s_ui.dof, [](int i) { s_ui.dof = i; apply_setting(V_DOF, kDof[i]); })
        .d("The game's background blur.").c(1).live(V_DOF)
        .st(s_ui.dof == 1 && !s_census && !ps3_title_view_provided() ? "No game camera yet: off everywhere until you play." : "");
    mrow_toggle(R, "edge", "Edge blur", s_ui.edge_blur, [](bool on) { s_ui.edge_blur = on; apply_setting(V_EDGE_BLUR, on ? "1" : "0"); })
        .d("The game's soft screen edges.").c(0).live(V_EDGE_BLUR);
}

/* ---- Audio: volume and the languages (the lists are this disc's) ---- */
static void lang_opts(int v, char (*list)[24], int n, Rows& R, const char* id, const char* label, const char* desc)
{
    char cur[48];
    lang_choice(v, cur, sizeof cur);
    std::vector<std::string> o; std::vector<std::string> val;
    o.push_back("As game"); val.push_back("same");
    int sel = lang_unset(v, cur) ? 0 : -1;
    for (const auto& k : kLang)
        if (lang_has(list, n, k.name)) { if (!SDL_strcasecmp(cur, k.name)) sel = (int)o.size(); o.push_back(k.brief); val.push_back(k.name); }
    for (int i = 0; i < n; i++) {
        bool known = false;
        for (const auto& k : kLang) known = known || !SDL_strcasecmp(k.name, list[i]);
        if (!known) { if (!SDL_strcasecmp(cur, list[i])) sel = (int)o.size(); o.push_back(list[i]); val.push_back(list[i]); }
    }
    MRow& r = mrow_choice(R, id, label, o, sel, [v, val](int i) { lang_set(v, val[(size_t)i].c_str()); });
    if (sel < 0) r.vtext = sfmt("%s (not on disc)", lang_brief(cur));
    r.d(desc).c(0).var(v).wh(lang_changed(v) ? WH_PENDING : WH_RESTART);
}

static void page_audio(Rows& R)
{
    if (!s_census) lang_ui_load();
    mrow_slider(R, "vol", "Master volume", (float)s_ui.volume, 0.0f, 100.0f, 5.0f, sfmt("%d%%", s_ui.volume),
                [](float v) { s_ui.volume = (int)v; cellAudio_host_set_volume(s_ui.volume); set_value(V_VOLUME, "%d", s_ui.volume); })
        .d("All game sound.").c(0).var(V_VOLUME);
    {   /* PS3_LANG: Automatic (the Windows display language) or one of this disc's */
        char cur[48];
        lang_choice(V_LANG, cur, sizeof cur);
        const bool autom = lang_unset(V_LANG, cur);
        std::vector<std::string> o; std::vector<std::string> code;
        o.push_back(s_lang.host[0] ? sfmt("Auto (%s)", lang_brief(lang_code_name(s_lang.host) ? lang_code_name(s_lang.host) : "english"))
                                   : std::string("Automatic"));
        code.push_back("auto");
        int sel = autom ? 0 : -1;
        for (const auto& k : kLang) {
            if (s_lang.ntext && !lang_has(s_lang.text, s_lang.ntext, k.name)) continue;
            if (!autom && !SDL_strcasecmp(cur, k.code)) sel = (int)o.size();
            o.push_back(k.brief); code.push_back(k.code);
        }
        MRow& r = mrow_choice(R, "lang", "Game language", o, sel, [code](int i) { lang_set(V_LANG, code[(size_t)i].c_str()); });
        if (sel < 0) r.vtext = lang_code_label(cur);
        r.d("Menus, subtitles and dub follow it.").c(0).var(V_LANG).wh(lang_changed(V_LANG) ? WH_PENDING : WH_RESTART);
        /* the voices' fallback when this disc has no dub of that language, else what the session runs with */
        char vcur[48];
        lang_choice(V_VOICE_LANG, vcur, sizeof vcur);
        const char* gname = autom ? (s_lang.host[0] ? lang_code_name(s_lang.host) : "english") : lang_code_name(cur);
        const char* fb = lang_unset(V_VOICE_LANG, vcur) ? lang_dub_fallback(gname) : NULL;
        if (fb) r.st(sfmt("No %s dub on this disc: the voices stay %s.", lang_brief(gname), lang_brief(fb)), true);
        else if (!s_census) {
            const int told = ps3_sysutil_language_in_use();
            char va[24], vg[24], ta[24], tg[24];
            ps3_lang_in_use(0, va, vg, sizeof va);
            ps3_lang_in_use(1, ta, tg, sizeof ta);
            const char* tn = lang_id_name(told);
            r.st(sfmt("Now: told %s, voice %s, text %s.", told == 3 ? "Spanish" : told == 7 ? "Portuguese" : tn ? lang_brief(tn) : "-",
                      vg[0] ? lang_brief(vg) : "-", tg[0] ? lang_brief(tg) : "-"));
        }
    }
    lang_opts(V_VOICE_LANG, s_lang.voice, s_lang.nvoice, R, "voice", "Voice", "Dialogue and movie voices. Only dubs on your disc.");
    if (s_lang.nvoice == 1) R.back().st(sfmt("This disc has one dub: %s.", lang_brief(s_lang.voice[0])));
    lang_opts(V_TEXT_LANG, s_lang.text, s_lang.ntext, R, "text", "Text", "Menus, HUD and subtitles.");
    if (!s_lang.ntext && !s_census) R.back().st("No language files found on this disc.", true);
}

static void page_controls(Rows& R)
{
    if (!s_census) pads_refresh(now_s());
    { static const char* const names[] = { "both", "controller", "keyboard" };
      std::string pads;
      for (int i = 0; i < s_n_pads; i++) pads += sfmt("%s%s", i ? ", " : "", s_pads[i]);
      const char* e = getenv("PS3_PAD_SDL");
      mrow_choice(R, "input", "Input", vs({ "Both", "Controller", "Keyboard" }), s_ui.input_mode,
                  [](int i) { s_ui.input_mode = i; cellPad_host_set_input_mode(i); set_value(V_INPUT_MODE, "%s", names[i]); })
          .d("Play with a controller, keyboard and mouse, or both.").c(0).var(V_INPUT_MODE)
          .st(s_n_pads ? "Controllers: " + pads : std::string(e && e[0] == '0' ? "Controller input is off for this run (PS3_PAD_SDL=0)."
                                                                             : "No controller connected (one switched on is picked up).")); }
    { static const char* const names[] = { "auto", "playstation", "xbox", "keyboard" };
      const int eff = s_census ? 0 : ps3_button_icons_effective();
      mrow_choice(R, "icons", "Button icons", vs({ "Auto", "PlayStation", "Xbox", "Keyboard" }), s_ui.button_icons,
                  [](int i) { s_ui.button_icons = i; ps3_button_icons_set_mode(i); set_value(V_BUTTON_ICONS, "%s", names[i]); })
          .d("Which button pictures the game shows.").c(0).var(V_BUTTON_ICONS)
          .st(sfmt("Showing now: %s.", eff == 2 ? "Xbox" : eff == 3 ? "keyboard and mouse" : "PlayStation")); }
    mrow_toggle(R, "rumble", "Vibration", s_ui.rumble, [](bool on) {
        s_ui.rumble = on;
        cellPad_host_set_rumble_strength(on ? s_ui.rumble_strength : 0);
        set_value(V_RUMBLE, "%d", on ? 1 : 0);
        if (on) set_value(V_RUMBLE_STRENGTH, "%d", s_ui.rumble_strength); })
        .d("Controller rumble.").c(0).var(V_RUMBLE);
    mrow_slider(R, "rumbleS", "Vibration strength", (float)s_ui.rumble_strength, 10.0f, 200.0f, 10.0f, sfmt("%d%%", s_ui.rumble_strength),
                [](float v) { s_ui.rumble_strength = (int)v; cellPad_host_set_rumble_strength(s_ui.rumble_strength); set_value(V_RUMBLE_STRENGTH, "%d", s_ui.rumble_strength); })
        .d("100% is the game's.").c(0).var(V_RUMBLE_STRENGTH).off(!s_ui.rumble);
    /* [mouse] the mouse camera (cellPad's setters; PS3_MOUSE_* in the file's other lines) */
    { int on = 0, ix = 0, iy = 0;
      float sens = 1.0f;
      cellPad_host_mouse_settings(&on, &sens, &ix, &iy);
      const float aim = cellPad_host_mouse_aim_sens();
      const int pick = cellPad_host_mouse_mode() ? 1 : 0;
      const bool ready = cellPad_host_mouse_direct_ready() != 0;
      mrow_choice(R, "mouse", "Mouse camera", vs({ "Like a stick", "Direct" }), pick,
                  [](int i) { cellPad_host_set_mouse_mode(i); extra_set("PS3_MOUSE_MODE", i ? "direct" : "stick"); })
          .d("Direct turns the camera by how far the mouse moves.").c(0).ex("PS3_MOUSE_MODE")
          .st(!on ? "The mouse camera is off (PS3_MOUSE=0 or input.toml)." : pick && !ready ? "Direct needs this build's camera hook: like a stick until then." : "",
              !on || (pick && !ready));
      mrow_slider(R, "sens", "Mouse sensitivity", sens * 100.0f, 10.0f, 500.0f, 10.0f, sfmt("%.0f%%", sens * 100.0f), [ix, iy](float v) {
          cellPad_host_set_mouse_settings(v / 100.0f, ix, iy); char b[16]; snprintf(b, sizeof b, "%.2f", v / 100.0f); extra_set("PS3_MOUSE_SENS", b); })
          .d("Camera speed.").c(0).ex("PS3_MOUSE_SENS");
      mrow_slider(R, "aim", "Aiming sensitivity", aim * 100.0f, 10.0f, 500.0f, 10.0f, sfmt("%.0f%%", aim * 100.0f), [](float v) {
          cellPad_host_set_mouse_aim_sens(v / 100.0f); char b[16]; snprintf(b, sizeof b, "%.2f", v / 100.0f); extra_set("PS3_MOUSE_AIM_SENS", b); })
          .d("Camera speed while aiming (Direct).").c(0).ex("PS3_MOUSE_AIM_SENS").off(!pick); }
    { char path[600] = "";
      if (!s_census) cellPad_host_input_map_path(path, sizeof path);
      mrow_action(R, "remap", "Buttons and keys", "Remap", [] { s_sub_want = SUB_REMAP; })
          .d("Change any button or key.")
          .st(s_rm.status[0] ? std::string(s_rm.status) : path[0] ? sfmt("Saved to %s.", path) : std::string("For this session only (PS3_INPUT_MAP is empty)."),
              s_rm.status_bad); }
}

static void page_hud(Rows& R)
{
    std::string live, cpu;
    if (s_stats.valid) {
        live = sfmt("%.0f fps \xC2\xB7 %.1f ms", s_stats.fps, s_stats.avg_ms);
        if (s_stats.low1_fps > 0) live += sfmt(" \xC2\xB7 1%% low %.0f", s_stats.low1_fps);
        live += sfmt(" \xC2\xB7 max %.1f ms", s_stats.max_ms);
    } else live = "Measuring...";
    { CpuSample cs; { std::lock_guard<std::mutex> g(s_cpu_lock); cs = s_cpu; }
#ifdef _WIN32
      if (cs.valid) cpu = sfmt("Now: CPU %.0f%%, RSX %.0f%%, busiest other thread %.0f%%.", cs.proc_pct, cs.rsx_pct, cs.top_pct);
#else
      if (cs.valid) cpu = sfmt("Now: CPU %.1f cores.", cs.cores);
#endif
    }
    mrow_toggle(R, "hud", "Performance HUD (F1)", s_hud != 0, [](bool on) { set_hud(on); })
        .d("FPS and frame times while you play.").c(0).var(V_HUD).st("Now: " + live).nt(s_gpu_name[0] ? sfmt("GPU: %s (Vulkan).", s_gpu_name) : "");
    mrow_choice(R, "corner", "Corner", vs({ "Top left", "Top right", "Bottom left", "Bottom right" }), s_hud_corner,
                [](int i) { s_hud_corner = i; set_value(V_HUD_CORNER, "%d", i); })
        .d("Where the HUD sits.").c(0).var(V_HUD_CORNER);
    mrow_slider(R, "hudop", "Background opacity", s_hud_opacity * 100.0f, 0.0f, 100.0f, 5.0f, sfmt("%.0f%%", s_hud_opacity * 100.0f),
                [](float v) { s_hud_opacity = v / 100.0f; set_value(V_HUD_OPACITY, "%.2f", s_hud_opacity); })
        .d("How solid the HUD panel is.").c(0).var(V_HUD_OPACITY);
    mrow_toggle(R, "graph", "Frame time graph", s_hud_graph, [](bool on) { s_hud_graph = on; set_value(V_HUD_GRAPH, "%d", on ? 1 : 0); })
        .d("A line that shows hitches.").c(0).var(V_HUD_GRAPH);
    mrow_toggle(R, "cpu", "CPU use", s_hud_cpu, [](bool on) { s_hud_cpu = on; set_value(V_HUD_CPU, "%d", on ? 1 : 0); })
        .d("Per-thread load.").c(0).var(V_HUD_CPU).st(cpu);
}

static void page_lighting(Rows& R)
{
    const bool gpu_light = env_on("PS3RECOMP_ENG_GPU_LIGHT");
    const bool gpu_ssao = gpu_light && env_on("PS3RECOMP_ENG_GPU_SSAO");
    const bool cmp = s_lfx.comparing;
    const char* const need = "Needs the GPU lighting path (the launcher's -SpuLighting turns it off).";
    const std::string showing = "Showing the PS3 look: press F3 for yours.";
    auto lrow = [&](MRow& r, int id, bool ok) -> MRow& {
        r.ex(ps3_lightfx_name(id)).off(cmp || !ok);
        if (cmp) r.st(showing, true); else if (!ok) r.st(need, true);
        return r;
    };
    lrow(mrow_choice(R, "soft", "Shadow softness", vs({ "PS3", "Soft", "Softer", "Softest" }), ps3_lightfx_get(PS3_LFX_SOFT_SHADOWS),
                     [](int i) { lfx_choose(PS3_LFX_SOFT_SHADOWS, i); }).d("Softer shadow edges.").c(1).cm(CMP_F3), PS3_LFX_SOFT_SHADOWS, true);
    /* [shadow] feat/shadows: the sun-shadow map's size (vk_overlay_shadow.inc, PS3RECOMP_SHADOW_RES 1 / 2 / 4) */
    mrow_choice(R, "shadowres", "Shadow quality", vs(kShadowQualityName, SHADOW_QUALITY_N), shadow_quality_get(),
                [](int i) { shadow_quality_set(i); })
        .d(kShadowQualityHint).c(2).cm(CMP_F3).ex("PS3RECOMP_SHADOW_RES").off(cmp).st(cmp ? showing : std::string(), cmp);
    lrow(mrow_choice(R, "ao", "Ambient occlusion", vs({ "PS3", "Full", "Full, high" }), ps3_lightfx_get(PS3_LFX_SSAO),
                     [](int i) { lfx_choose(PS3_LFX_SSAO, i); }).d("Soft darkening in corners and under objects.").c(2).cm(CMP_F3), PS3_LFX_SSAO, gpu_ssao);
    { const int v = ps3_lightfx_get(PS3_LFX_SSAO_STRENGTH);
      lrow(mrow_slider(R, "aoS", "AO strength", (float)v, 0.0f, 300.0f, 5.0f, sfmt("%d%%", v), [](float x) { lfx_choose(PS3_LFX_SSAO_STRENGTH, (int)x); })
           .d("100% is the game's.").c(0).cm(CMP_F3), PS3_LFX_SSAO_STRENGTH, gpu_ssao); }
    lrow(mrow_toggle(R, "dither", "Smooth light gradients", ps3_lightfx_get(PS3_LFX_LIGHT_DITHER) != 0,
                     [](bool on) { lfx_choose(PS3_LFX_LIGHT_DITHER, on ? 1 : 0); }).d("Removes banding in skies and light.").c(0).cm(CMP_F3),
         PS3_LFX_LIGHT_DITHER, gpu_light);
    static const struct { int id; const char* key; const char* label; const char* desc; } kTone[] = {
        { PS3_LFX_TONE_SHADOWS, "tsh", "Shadows", "Lift or lower the darks." },
        { PS3_LFX_TONE_MIDTONES, "tmid", "Midtones", "Lift or lower the middle." },
        { PS3_LFX_TONE_HIGHLIGHTS, "thi", "Highlights", "Lift or lower the brights." },
        { PS3_LFX_CLARITY, "clar", "Clarity", "Local contrast, more depth." } };
    for (const auto& t : kTone) {
        const int v = ps3_lightfx_get(t.id), id = t.id;
        lrow(mrow_slider(R, t.key, t.label, (float)v, -100.0f, 100.0f, 5.0f, v ? sfmt("%+d", v) : std::string("Off"),
                         [id](float x) { lfx_choose(id, (int)x); }).d(t.desc).c(0).cm(CMP_SPLIT), t.id, true);
    }
    mrow_action(R, "ps3look", "PS3 look (F3)", cmp ? "Yours" : "Show", [] { lfx_compare_toggle(); })
        .d("Flips lighting and effects to the PS3 look. Hold F3 to peek.").st(cmp ? showing : "", cmp);
    mrow_action(R, "ps3reset", "Reset to the PS3 look", "Reset", [] {
        for (int id = 0; id < PS3_LFX_COUNT; id++) lfx_choose(id, atoi(lfx_off_value(id)));
        shadow_quality_set(0);   /* [shadow] the PS3's 1024x1024 maps */
        vk_overlay_notice("Lighting: every option back to the PS3 look"); })
        .d("Every lighting option back to the game's own.").off(cmp);
}

static void page_effects(Rows& R)
{
    const bool cmp = s_lfx.comparing;
    { static const char* const kVal[] = { "1", "2", "full" };
      const char* e = getenv("PS3RECOMP_FX_RES");
      int cur = 0;
      if (e && (!strcmp(e, "full") || atof(e) >= 2.5)) cur = 2; else if (e && atof(e) > 1.0) cur = 1;
      mrow_choice(R, "fxres", "Effects resolution", vs({ "As game", "High", "Full" }), cur, [](int i) { fx_set("PS3RECOMP_FX_RES", kVal[i]); })
          .d("Fire, smoke and particles.").c(2).ex("PS3RECOMP_FX_RES").cm(CMP_F3).off(cmp)
          .st(cmp ? "Showing the PS3 look: press F3 for yours." : "", cmp); }
    { static const char* const kVal[] = { "1", "2", "0" };
      const char* e = getenv("PS3RECOMP_FX_GLOW");
      int cur = 0;
      if (e && e[0] == '2' && !e[1]) cur = 1; else if (e && e[0] == '0' && !e[1]) cur = 2;
      mrow_choice(R, "glow", "Glow and blur", vs({ "PS3 smooth", "PS3", "Render res" }), cur, [](int i) { fx_set("PS3RECOMP_FX_GLOW", kVal[i]); })
          .d("Bloom around lights and powers.").c(1).ex("PS3RECOMP_FX_GLOW").cm(CMP_F3).off(cmp)
          .st(cmp ? "Showing the PS3 look: press F3 for yours." : "", cmp); }
}

/* the zip installs' report, once per new one (a new mod may be in the list: the pictures again) */
static void zip_result_poll(void)
{
    static unsigned seen;
    const unsigned seq = ps3_game_mods_zip_seq();
    if (seq == seen) return;
    seen = seq;
    char res[2400];
    const int kind = ps3_game_mods_zip_result(res, sizeof res);
    if (strcmp(res, s_zip_res)) {
        snprintf(s_zip_res, sizeof s_zip_res, "%s", res);
        if (kind > 0) pics_release_all();
        if (kind) fprintf(stderr, "[overlay] menu: zip install %s: %s\n", kind > 0 ? "done" : "failed", res);
    }
    s_zip_kind = kind;
}
static std::string first_line(const char* s, size_t max = 160)
{
    std::string r;
    for (; *s && *s != '\n' && r.size() < max; s++) r += *s;
    return r;
}

static void page_mods(Rows& R)
{
    const char* dir = ps3_game_mods_dir();
    const bool have = dir && *dir;
    if (!s_census) zip_result_poll();
    { const char* e = getenv(kVar[V_GAME_MODS]);
      const bool use = !(e && e[0] == '0' && !e[1]);
      const bool restart = have && !s_census && ps3_game_mods_restart_needed();
      mrow_toggle(R, "mods", "Use mods", use, [](bool on) { apply_setting(V_GAME_MODS, on ? "1" : "0"); })
          .d("Turn all game mods on or off.").start(V_GAME_MODS).off(!have)
          .st(!have ? "No mods folder: start with \"Play inFAMOUS 2\" (or set PS3_GAME_MODS_DIR)."
                    : restart ? "The mods differ from this session: they apply after a restart." : "", !have || restart);
      if (restart && ps3_host_can_restart())
          mrow_action(R, "restart", "Restart to apply", "Restart", [] { s_restart_ask = 1; })
              .d("Starts the game again with the mods as set (back at the title screen).").wh(WH_PENDING); }
    mrow_action(R, "folder", "Mods folder", "Open", [] {
#ifdef _WIN32
                    ps3_game_mods_open_folder();
#endif
                })
        .d("Drop mods here, one folder each.").st(have ? std::string(dir) : std::string("No mods folder."))
#ifndef _WIN32
        .off(true)
#endif
        .off(!have);
    mrow_action(R, "scan", "Look for new mods", "Scan", [] { ps3_game_mods_rescan(); pics_release_all(); })
        .d("Reads the mods folder again.").off(!have);
    mrow_action(R, "zip", "Install from a zip", "Open", [] { s_sub_want = SUB_ZIP; })
        .d("Mod zips in Downloads and the mods folder. Only mod files are taken out.").off(!have)
        .st(s_zip_kind ? first_line(s_zip_res) : std::string(), s_zip_kind < 0);
    const int n = have && !s_census ? ps3_game_mods_count() : 0;
    if (have && !s_census) mrow_head(R, "#mods", n ? sfmt("Your mods (%d)", n) : std::string("No mods yet: install one from a zip"));
    for (int i = 0; i < n; i++) {
        ps3_game_mod m;
        if (!ps3_game_mods_get(i, &m)) continue;
        const bool ver = m.version[0] && strcmp(m.version, "unknown");
        const bool who = m.author[0] && strcmp(m.author, "unknown");
        std::string by = sfmt("%s%s%s%s", ver ? "v" : "", ver ? m.version : "", who ? (ver ? " by " : "By ") : "", who ? m.author : "");
        const bool good = m.active && m.on && m.next;
        const bool bad = !m.for_game || !strncmp(m.status, "Not used", 8) || (m.on && !m.next && !m.live);
        MRow& r = mrow_toggle(R, ("mod:" + std::string(m.folder)).c_str(), m.name, m.on != 0, [i](bool on) { ps3_game_mods_set_on(i, on ? 1 : 0); if (on) mods_conflict_open(i, true); });
        r.d(by.empty() ? sfmt("Folder: %s", m.folder) : by).st(m.status, bad).off(!m.for_game)
         .wh(m.live ? WH_INSTANT : (m.on != 0) != (m.active != 0) ? WH_PENDING : WH_RESTART);
        if (m.live) r.nt(sfmt("Live mod: %d texture patch(es), %d xpps patch(es), on and off at once", m.n_tex, m.n_xpps));
        else r.nt(sfmt("%d file(s), %d texture patch(es), %d xpps patch(es)%s", m.n_files, m.n_tex, m.n_xpps, good ? ", in use now" : ""));
        r.pic = pic_for(i, m);
        if (m.conflicts && m.on && m.for_game)
            mrow_action(R, ("conflict:" + std::string(m.folder)).c_str(), "Combine with the mods it conflicts with", "Combine",
                        [i] { mods_conflict_open(i, false); })
                .d(sfmt("It and %s change the same bytes in %d place(s) of the game files: combine them to use both.", m.conflicts_with, m.conflicts))
                .ind().wh(m.live ? WH_INSTANT : WH_RESTART);
        else if (m.merged_with[0] && m.on && m.for_game)
            mrow_action(R, ("merged:" + std::string(m.folder)).c_str(), "Combined", "Change",
                        [i] { mods_conflict_open(i, false); })
                .d(sfmt("Used together with %s. Change whose bytes are used where both change the same place.", m.merged_with)).ind()
                .wh(m.live ? WH_INSTANT : WH_RESTART);
        if (m.clash && m.on)
            mrow_action(R, ("prefer:" + std::string(m.folder)).c_str(), "Use this one instead", "Use", [i] { ps3_game_mods_prefer(i); })
                .d(m.prefer).ind().wh(WH_RESTART);
        if (m.n_modules && m.for_game) {   /* its options: switches, a choice per one-at-a-time group */
            ps3_game_mod_option o[64];
            int got = 0;
            for (int k = 0; k < m.n_modules && k < 64; k++) if (ps3_game_mods_option(i, k, &o[got])) got++;
            for (int k = 0; k < got; k++) {
                if (o[k].dropdown || o[k].group_id[0]) continue;
                const int kk = k;
                mrow_toggle(R, sfmt("opt:%s:%s", m.folder, o[k].id).c_str(), o[k].name, o[k].picked != 0,
                            [i, kk](bool on) { ps3_game_mods_pick(i, kk, on ? 1 : 0); }).d(sfmt("An option of %s.", m.name)).ind().wh(WH_RESTART);
            }
            for (int k = 0; k < got; k++) {
                if (!o[k].dropdown && !o[k].group_id[0]) continue;
                bool first = true;
                for (int j = 0; j < k; j++) if ((o[j].dropdown || o[j].group_id[0]) && !strcmp(o[j].group_id, o[k].group_id)) first = false;
                if (!first) continue;
                const char* gname = o[k].group[0] ? o[k].group : "Options";
                if (o[k].dropdown) {
                    std::vector<std::string> names; std::vector<int> idx; int sel = -1;
                    for (int j = k; j < got; j++)
                        if (!strcmp(o[j].group_id, o[k].group_id)) { if (o[j].picked && sel < 0) sel = (int)names.size(); names.push_back(o[j].name); idx.push_back(j); }
                    if (sel < 0) sel = 0;
                    mrow_choice(R, sfmt("grp:%s:%s", m.folder, o[k].group_id).c_str(), gname, names, sel,
                                [i, idx](int c) { ps3_game_mods_pick(i, idx[(size_t)c], 1); })
                        .d(o[k].single ? "One at a time: they all change the same texture." : "Pick one.").ind().wh(WH_RESTART);
                } else {
                    for (int j = k; j < got; j++) {
                        if (strcmp(o[j].group_id, o[k].group_id)) continue;
                        const int jj = j;
                        mrow_toggle(R, sfmt("opt:%s:%s", m.folder, o[j].id).c_str(), sfmt("%s: %s", gname, o[j].name), o[j].picked != 0,
                                    [i, jj](bool on) { ps3_game_mods_pick(i, jj, on ? 1 : 0); }).d(sfmt("An option of %s.", m.name)).ind().wh(WH_RESTART);
                    }
                }
            }
        }
    }
    mrow_head(R, "#packs", "Texture packs");
    { static char packs[16][64];
      static int npacks = -1;
      static double listed;
      const double t = now_s();
      if (!s_census && (npacks < 0 || t - listed > 2.0)) { npacks = fx_pack_list(packs, 16); listed = t; }
      const char* e = getenv("PS3_MODS");
      std::vector<std::string> o; o.push_back("Off");
      int cur = (e && *e) ? -1 : 0;
      for (int i = 0; i < npacks; i++) { if (e && !SDL_strcasecmp(e, packs[i])) cur = i + 1; o.push_back(packs[i]); }
      unsigned np = 0, nf = 0, na = 0; int dumping = 0;
      if (!s_census) rsx_texpack_status(&np, &nf, &na, &dumping);
      std::vector<std::string> names(o);
      MRow& r = mrow_choice(R, "pack", "Texture pack", o, cur, [names](int i) { fx_set("PS3_MODS", i ? names[(size_t)i].c_str() : ""); });
      if (cur < 0) r.vtext = e;
      r.d("Replacement textures from the packs folder.").c(1).ex("PS3_MODS")
       .st(np ? sfmt("%u file(s) in use, %u upload(s) replaced.", nf, na) : std::string("No pack in use."));
      const char* pd = rsx_texpack_dir();
      mrow_action(R, "reload", "Reload pack files", "Reload", [] { const char* p = getenv("PS3_MODS"); fx_set("PS3_MODS", p ? p : ""); })
          .d("Picks up files you changed or added.")
          .st(pd && *pd ? sfmt("Packs folder: %s", pd) : std::string("No packs folder: make work\\texpacks or set PS3_MODS_DIR."), !(pd && *pd));
      mrow_toggle(R, "dump", "Dump textures", dumping != 0, [](bool on) { ps3_setting_apply("PS3_MODS_DUMP", on ? "1" : "0"); })
          .d("Saves the game's textures to make a pack. Slows the game; off at the next start.").ex("PS3_MODS_DUMP")
          .st(dumping ? "Writing to the packs folder's _dump." : ""); }
}

static void page_zip(Rows& R)
{
    zip_result_poll();
    char what[200] = "";
    float prog = 0.0f;
    const bool busy = ps3_game_mods_zip_busy(what, sizeof what, &prog) != 0;
    if (busy) mrow_head(R, "#busy", prog > 0.0f ? sfmt("%s... %.0f%%", what, prog * 100.0f) : sfmt("%s...", what), acc());
    else if (s_zip_kind) mrow_head(R, "#res", first_line(s_zip_res, 90), s_zip_kind > 0 ? rgba(0x73, 0xd9, 0x73) : C_AMBER);
    const int n = ps3_game_mods_zip_count();
    for (int k = 0; k < n; k++) {
        ps3_game_mod_zip z;
        if (!ps3_game_mods_zip_get(k, &z)) continue;
        char when[32] = "";
        const time_t tt = (time_t)z.mtime;
        if (const struct tm* lt = localtime(&tt)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", lt);
        const std::string path = z.path;
        const int replace = z.exists;
        mrow_action(R, sfmt("zip:%s", z.file).c_str(), z.file, z.exists ? "Replace" : "Install",
                    [path, replace] { ps3_game_mods_zip_install(path.c_str(), replace); })
            .d(z.ok ? first_line(z.what) : std::string(z.what[0] ? z.what : "Cannot be installed."))
            .st(sfmt("%s, %s, %.1f MB%s", z.where, when, (double)z.size / (1024.0 * 1024.0), z.exists ? " (the mod there stays until the new one is in)" : ""))
            .off(!z.ok || busy);
    }
    if (!n) mrow_head(R, "#none", "No mod zip files in Downloads or the mods folder");
    if (const int other = ps3_game_mods_zip_not_mods()) mrow_head(R, "#other", sfmt("%d other zip file(s) there are not mods", other));
#ifdef _WIN32
    mrow_action(R, "choose", "Choose another file", "Browse", [] { ps3_game_mods_zip_choose(); })
        .d("Opens the Windows file window (Alt+Tab if it stays behind).").off(busy);
#endif
    mrow_action(R, "again", "Look again", "Refresh", [] { ps3_game_mods_zips_refresh(); }).d("Lists the zip files again.");
}

static void page_update(Rows& R)
{
    if (!s_upd_dir[0] && !s_census) {
        const char* by = getenv("PS3_UPDATE_BY");
        MRow& r = mrow_action(R, "check", "Updates", "Check now", [] {});
        r.off(true).d(by && !strcmp(by, "recomplauncher") ? "RecompLauncher installs each new release by itself."
                      : by && !strcmp(by, "git") ? "A git clone: \"git pull\", then Setup.exe builds it."
                      : "Start the game with \"Play inFAMOUS 2\" from a Setup folder to get updates here.");
        return;
    }
    static UpdateStatus v;
    static unsigned vseq = ~0u;
    static bool vhave;
    { std::lock_guard<std::mutex> g(s_upd_lock);
      if (s_upd_seq != vseq) { v = s_upd; vseq = s_upd_seq; vhave = s_upd_have; } }
    if (s_upd_auto < 0) { const char* e = getenv("PS3_UPDATE_CHECK"); s_upd_auto = (e && e[0] == '0' && !e[1]) ? 0 : 1; }
    const bool avail = vhave && !strcmp(v.state, "available");
    const double t = now_s();
    const bool asking = s_upd_checking && t - s_upd_check_t < 30.0;
    { MRow& r = mrow_action(R, "check", "Updates", asking ? "Asking..." : "Check now",
                            [] { s_upd_check_t = now_s(); s_upd_checking = 1; s_upd_check_req = 1; fprintf(stderr, "[update] menu: check now\n"); });
      r.vtext = !vhave ? "Asking GitHub..." : avail ? sfmt("%s out", v.latest[0] ? v.latest : "New") : !strcmp(v.state, "current") ? "Newest" : v.state;
      r.d("Downloads only what changed.").off(asking);
      std::string s = sfmt("This copy %s", vhave && v.installed[0] ? v.installed : "...");
      if (avail) s += sfmt(", new %s%s%s", v.latest[0] ? v.latest : "version", v.published[0] ? " since " : "", v.published);
      if (vhave && v.checked[0]) s += sfmt(", asked %s", v.checked);
      r.st(s + ".", avail).nt(v.message[0] ? first_line(v.message) : ""); }
    if (avail && ps3_host_can_update()) {
        mrow_toggle(R, "updquit", "Update when I quit", ps3_host_update_pending() != 0, [](bool on) {
            ps3_host_set_update(on ? 1 : 0);
            notice_for(on ? "The update starts when you quit the game" : "No update: the game quits as usual", 5.0); })
            .d("Setup updates the game after you quit. Saves, settings and mods stay.");
        mrow_action(R, "updnow", "Quit and update now", "Update", [] { s_upd_ask = 1; })
            .d("Quits now; Setup updates in its own window (a few minutes).");
    } else if (avail) mrow_head(R, "#setup", "Run Setup.exe and choose \"Update to the latest version\"", C_AMBER);
    if (avail && !v.notes.empty())
        mrow_action(R, "notes", sfmt("What's new in %s", v.latest[0] ? v.latest : "it"), "Read", [] { s_sub_want = SUB_NOTES; })
            .d(v.title[0] ? std::string(v.title) : std::string("The release notes."));
    mrow_toggle(R, "auto", "Check automatically", s_upd_auto != 0, [](bool on) { s_upd_auto = on ? 1 : 0; extra_set("PS3_UPDATE_CHECK", on ? "1" : "0"); })
        .d("Looks for a new version at start (at most every 6 hours).").ex("PS3_UPDATE_CHECK").wh(WH_INSTANT);
}

static std::string cheat_brief(const char* help, std::string* status)
{
    if (!help || !*help) return "";
    const char* nl = strchr(help, '\n');
    if (nl && status) { const char* last = strrchr(help, '\n'); *status = first_line(last + 1); }
    return first_line(help, 200);
}
static std::string cheat_val(const ps3_cheat* c, double v)
{
    char b[64];
    if (c->kind == PS3_CHEAT_FLOAT) snprintf(b, sizeof b, c->fmt ? c->fmt : "%.2f", v);
    else snprintf(b, sizeof b, c->fmt ? c->fmt : "%d", (int)v);
    return b;
}

static void page_cheats(Rows& R)
{
    vk_overlay_cheats_refresh();
    const bool rem = vk_overlay_cheats_remember() != 0;
    mrow_toggle(R, "remember", "Remember cheats", rem, [](bool on) { vk_overlay_cheats_set_remember(on ? 1 : 0); })
        .d("Keep cheats on after a restart.").st(rem ? "One-time actions (Add ...) are never repeated." : "All cheats are off at every start.");
    const int n = ps3_cheats_count();
    const char* groups[PS3_CHEATS_MAX];
    int ng = 0;
    for (int i = 0; i < n; i++) {
        const char* g = g_ps3_cheats.e[i]->group ? g_ps3_cheats.e[i]->group : "Cheats";
        bool seen = false;
        for (int k = 0; k < ng; k++) if (!strcmp(groups[k], g)) seen = true;
        if (!seen) groups[ng++] = g;
    }
    const std::string save_note = "The game writes it into your save the next time it saves.";
    for (int k = 0; k < ng; k++) {
        mrow_head(R, sfmt("#g%d", k).c_str(), groups[k]);
        for (int i = 0; i < n; i++) {
            ps3_cheat* c = g_ps3_cheats.e[i];
            if (strcmp(c->group ? c->group : "Cheats", groups[k])) continue;
            const std::string id = std::string("cheat:") + c->id;
            std::string status;
            const std::string brief = cheat_brief(c->help, &status);
            MRow* r = NULL;
            switch (c->kind) {
            case PS3_CHEAT_TOGGLE:
                r = &mrow_toggle(R, id.c_str(), c->name, c->value != 0.0, [c](bool on) { vk_overlay_cheat_change(c, on ? 1.0 : 0.0); });
                break;
            case PS3_CHEAT_CHOICE: {
                std::vector<std::string> o;
                for (int j = 0; j < c->n_choices && c->choices; j++) o.push_back(c->choices[j]);
                r = &mrow_choice(R, id.c_str(), c->name, o, o.empty() ? -1 : (int)c->value, [c](int j) { vk_overlay_cheat_change(c, j); });
                if (o.empty()) { r->vtext = "None yet"; r->off(true); }
                break; }
            case PS3_CHEAT_INT: case PS3_CHEAT_FLOAT: {
                const float span = (float)(c->max - c->min);
                const float step = c->kind == PS3_CHEAT_INT ? (span <= 40.0f ? 1.0f : floorf(span / 40.0f)) : span / 50.0f;
                r = &mrow_slider(R, id.c_str(), c->name, (float)c->value, (float)c->min, (float)c->max, step, cheat_val(c, c->value),
                                 [c](float v) { vk_overlay_cheat_change(c, v); });
                break; }
            case PS3_CHEAT_ACTION: {
                const char* lab = c->button ? c->button : c->name;
                std::string verb = lab;
                const size_t sp = verb.find(' ');
                if (sp != std::string::npos) verb.resize(sp);
                r = &mrow_action(R, id.c_str(), lab, verb, [c] { vk_overlay_cheat_change(c, c->value); });
                if (c->max > c->min) {
                    r->has_amount = true;
                    r->mn = (float)c->min; r->mx = (float)c->max; r->v = (float)c->value;
                    r->step = c->min > 0 ? (float)c->min : std::max(1.0f, (float)(c->max - c->min) / 20.0f);
                    r->vtext = cheat_val(c, c->value);
                    r->setf = [c](float v) { c->value = v; };
                }
                break; }
            default: continue;
            }
            r->d(brief).wh(WH_INSTANT);
            if (!status.empty()) r->st(status);
            if (c->flags & PS3_CHEAT_SAVES) { r->save_tag = true; r->nt(save_note); }
            if (c->unavailable) r->off(true).st(sfmt("Unavailable: %s.", c->unavailable), true);
            else if (vk_overlay_cheat_failed(c)) r->st("Not applied: the game refused it just now.", true);
        }
    }
}

/* [about] the port, its page, the supporters (supporters.json: vk_overlay_about.inc). The names are
 * text from the network: a row's label is drawn as it is (AddText), never as a format. No list
 * (not fetched yet, none shipped, an empty one): "No list yet", nothing else. The support row is
 * there only while the file carries a support_url; the card shows where it leads. */
static void page_about(Rows& R)
{
    static AboutFile f;
    static unsigned fseq = ~0u;
    { std::lock_guard<std::mutex> g(s_about_lock);
      if (s_about_seq != fseq) { f = s_about; fseq = s_about_seq; } }
    char ver[96] = "";
    { std::lock_guard<std::mutex> g(s_upd_lock);
      if (s_upd_have) snprintf(ver, sizeof ver, "%s", s_upd.installed); }
    mrow_info(R, "port", "inFAMOUS 2 PC port", ver);
    mrow_info(R, "by", "by Sami Enazi");
    mrow_action(R, "page", "Project page", "Open", [] { about_open_url(ABOUT_PROJECT_URL); })
        .d("Opens in your browser.").st(std::string(ABOUT_PROJECT_URL).substr(8));
    if (!f.support_url.empty()) {
        const std::string url = f.support_url;
        mrow_action(R, "support", "Support the developer", "Open", [url] { about_open_url(url); })
            .d("Opens in your browser.").st(url.substr(8));
    }
    if (f.supporters.empty()) {
        mrow_head(R, "#sup", "Supporters");
        mrow_head(R, "#nolist", "No list yet");
    } else {
        mrow_head(R, "#sup", "Supporters: thank you");
        for (size_t i = 0; i < f.supporters.size(); i++)
            mrow_info(R, sfmt("sup:%d", (int)i).c_str(), f.supporters[i]).d("Thank you.").compact = true;
    }
    if (!f.contributors.empty()) {
        mrow_head(R, "#con", "Contributors");
        for (size_t i = 0; i < f.contributors.size(); i++)
            mrow_info(R, sfmt("con:%d", (int)i).c_str(), f.contributors[i]).d("Code in the port.").compact = true;
    }
}

typedef void (*PageFn)(Rows&);
static const PageFn kPageFn[TAB_ABOUT + 1] = { page_display, page_graphics, page_audio, page_controls, page_hud,
                                               page_lighting, page_effects, page_mods, page_update, page_cheats, page_about };

/* The settings outside kVar that a page row must show (census). */
static const char* const kMenuExtras[] = {
    "PS3RECOMP_SOFT_SHADOWS", "PS3RECOMP_SSAO", "PS3RECOMP_SSAO_STRENGTH", "PS3RECOMP_LIGHT_DITHER",
    "PS3RECOMP_TONE_SHADOWS", "PS3RECOMP_TONE_MIDTONES", "PS3RECOMP_TONE_HIGHLIGHTS", "PS3RECOMP_CLARITY",
    "PS3RECOMP_FX_RES", "PS3RECOMP_FX_GLOW", "PS3_MOUSE_MODE", "PS3_MOUSE_SENS", "PS3_MOUSE_AIM_SENS",
    "PS3_MODS", "PS3_MODS_DUMP", "PS3_UPDATE_CHECK", "PS3RECOMP_SHADOW_RES" };

/* Every page built once: a loud warning for any setting without a row; the startup line. */
static void menu_census(void)
{
    s_census = true;
    s_census_vars = 0;
    std::vector<std::string> extras;
    char counts[256] = "";
    int total = 0;
    for (int t = 0; t < TAB_CHEATS; t++) {
        Rows R;
        kPageFn[t](R);
        int k = 0;
        for (const MRow& r : R) {
            s_census_vars |= r.vars;
            if (r.extra) extras.push_back(r.extra);
            if (r.kind != RK_HEAD) k++;
        }
        total += k;
        const size_t l = strlen(counts);
        snprintf(counts + l, sizeof counts - l, "%s%s %d", t ? ", " : "", kTabName[t], k);
    }
    s_census = false;
    int missing = 0;
    for (int v = 0; v < V_N; v++)
        if (!(s_census_vars >> v & 1ull)) {
            fprintf(stderr, "[overlay] !!!!!!!! WARNING: the setting %s has NO ROW in the F2 menu (vk_overlay_menu.inc) -- "
                            "the player cannot change it !!!!!!!!\n", kVar[v]);
            missing++;
        }
    for (const char* x : kMenuExtras) {
        bool found = false;
        for (const std::string& e : extras) found = found || e == x;
        if (!found) { fprintf(stderr, "[overlay] !!!!!!!! WARNING: the setting %s has NO ROW in the F2 menu !!!!!!!!\n", x); missing++; }
    }
    fprintf(stderr, "[overlay] menu E: %d pages + Cheats (%d), %d setting rows (%s); %s\n", TAB_CHEATS, vk_overlay_cheats_count(),
            total, counts, missing ? "SOME SETTINGS HAVE NO ROW (warnings above)" : "every setting has a row");
    if (missing) notice_for("Menu: some settings have no row (see the log)", 10.0);
}

/* ---- input ---------------------------------------------------------------------- */
static bool menu_modal(void)
{
    return s_quit_ask || s_restart_ask || s_cf_ask || s_upd_ask || s_rm.state != RM_IDLE ||
           ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}
static bool kp(ImGuiKey k, bool rep = true) { return ImGui::IsKeyPressed(k, rep); }
static bool nav_scripted(const char* what)
{
    if (!s_nav_script_n || strcmp(s_nav_script, what)) return false;
    s_nav_script_n = 0;
    return true;
}
static int nav_dy(void)
{
    if (kp(ImGuiKey_UpArrow) || kp(ImGuiKey_GamepadDpadUp) || kp(ImGuiKey_GamepadLStickUp) || nav_scripted("up")) return -1;
    if (kp(ImGuiKey_DownArrow) || kp(ImGuiKey_GamepadDpadDown) || kp(ImGuiKey_GamepadLStickDown) || nav_scripted("down")) return 1;
    return 0;
}
static int nav_dx(void)
{
    if (kp(ImGuiKey_LeftArrow) || kp(ImGuiKey_GamepadDpadLeft) || kp(ImGuiKey_GamepadLStickLeft) || nav_scripted("left")) return -1;
    if (kp(ImGuiKey_RightArrow) || kp(ImGuiKey_GamepadDpadRight) || kp(ImGuiKey_GamepadLStickRight) || nav_scripted("right")) return 1;
    return 0;
}
static bool nav_ok(void)
{
    return kp(ImGuiKey_Enter, false) || kp(ImGuiKey_KeypadEnter, false) || kp(ImGuiKey_Space, false) ||
           kp(ImGuiKey_GamepadFaceDown, false) || nav_scripted("ok");
}

static void op(std::function<void()> f) { s_ops.push_back(std::move(f)); }
static void log_change(const MRow& r, const char* what)
{
    fprintf(stderr, "[overlay] menu: %s/%s (%s) %s\n", kTabName[tab_page(s_tab)], r.id.c_str(), r.label.c_str(), what);
}
/* a row's value moved by -1 / +1 (left / right), or Cross / a click on it (dir 0) */
static void row_step(const MRow& r, int dir)
{
    if (r.disabled) return;
    switch (r.kind) {
    case RK_CHOICE: {
        const int n = (int)r.opts.size();
        if (n <= 0) return;
        const int i = r.cur < 0 ? 0 : ((r.cur + (dir < 0 ? -1 : 1)) % n + n) % n;
        log_change(r, sfmt("-> %s", r.opts[(size_t)i].c_str()).c_str());
        auto f = r.set; op([f, i] { f(i); });
        break; }
    case RK_TOGGLE: {
        const bool on = dir == 0 ? !r.on : dir > 0;
        if (on == r.on) return;
        log_change(r, on ? "-> on" : "-> off");
        auto f = r.set; op([f, on] { f(on ? 1 : 0); });
        break; }
    case RK_SLIDER: {
        if (!dir) return;
        float v = r.v + r.step * (float)dir;
        v = v < r.mn ? r.mn : v > r.mx ? r.mx : v;
        if (v == r.v) return;
        log_change(r, sfmt("-> %g", v).c_str());
        auto f = r.setf; op([f, v] { f(v); });
        break; }
    case RK_ACTION:
        if (dir == 0) {
            log_change(r, sfmt("pressed (%s)", r.button.c_str()).c_str());
            auto f = r.press; if (f) op(f);
        } else if (r.has_amount) {
            float v = r.v + r.step * (float)dir;
            v = v < r.mn ? r.mn : v > r.mx ? r.mx : v;
            auto f = r.setf; op([f, v] { f(v); });
        }
        break;
    default: break;
    }
}
static void slider_to(const MRow& r, float v)
{
    v = r.mn + floorf((v - r.mn) / r.step + 0.5f) * r.step;
    v = v < r.mn ? r.mn : v > r.mx ? r.mx : v;
    if (v == r.v || r.disabled) return;
    auto f = r.setf; op([f, v] { f(v); });
}
static bool focusable(const MRow& r) { return r.kind != RK_HEAD; }

/* ---- layout (720-line units) ------------------------------------------------------ */
enum { L_PANEL = 780, L_RAILX = 24, L_RAILY = 84, L_RAILB = 44, L_RAILGAP = 6, L_COLX = 100, L_COLW = 560,
       L_TOP = 26, L_BOTTOM = 22, L_ROWH = 50, L_ROWGAP = 2, L_HEADH = 30, L_LISTH = 34, L_CARDW = 360, L_CARDM = 26 };
static float row_h(const MRow& r) { return S(r.kind == RK_HEAD ? L_HEADH : r.compact ? L_LISTH : L_ROWH); }

struct Card {
    std::string title, desc, status, note;
    bool warn = false;
    int when = WH_NONE, cost = -1, cmp = CMP_NONE;
    bool env = false;
    uint64_t vars = 0;
    ImTextureData* pic = NULL;
};

static void draw_chip(ImDrawList* dl, ImVec2 rmax, int when, bool env, float* out_w)
{
    if (when == WH_NONE) { *out_w = 0; return; }
    const char* t = when == WH_INSTANT ? (env ? "Instant \xC2\xB7 env" : "Instant")
                  : when == WH_PENDING ? (env ? "Restart needed \xC2\xB7 env" : "Restart needed")
                                       : (env ? "After restart \xC2\xB7 env" : "After restart");
    const ImU32 bg = when == WH_INSTANT ? acc_soft() : when == WH_PENDING ? rgba(242, 163, 58, 0.34f) : rgba(242, 163, 58, 0.18f);
    const ImU32 fg = when == WH_INSTANT ? acc() : when == WH_PENDING ? rgba(0xff, 0xd0, 0x8a) : C_AMBER;
    const float w = tw(f_semi(), 12, t) + S(18), h = S(20);
    rect(dl, ImVec2(rmax.x - w, rmax.y), ImVec2(rmax.x, rmax.y + h), bg, 10);
    txt(dl, f_semi(), 12, ImVec2(rmax.x - w + S(9), rmax.y + S(3)), fg, t);
    *out_w = w;
}

/* The card bottom right over the game. Returns its top. */
static float draw_card(ImDrawList* dl, const Card& cd, float W, float H, bool show_split)
{
    const float cw = S(L_CARDW), pad_x = S(20), pad_y = S(18), inner = cw - 2 * pad_x;
    float x = W - S(L_CARDM) - cw;
    const float minx = S(L_COLX + L_COLW + 20);
    if (x < minx && W - S(L_CARDM) - minx >= S(240)) x = minx;
    const float w = W - S(L_CARDM) - x, iw = w - 2 * pad_x;
    (void)inner;
    /* height */
    ImVec2 pic_sz(0, 0);
    if (cd.pic) {
        const float s1 = iw / (float)cd.pic->Width, s2 = S(170) / (float)cd.pic->Height;
        const float s = s1 < s2 ? s1 : s2;
        pic_sz = ImVec2((float)cd.pic->Width * s, (float)cd.pic->Height * s);
    }
    const float desc_h = cd.desc.empty() ? 0.0f : f_reg()->CalcTextSizeA(S(14), FLT_MAX, iw, cd.desc.c_str()).y;
    const float st_h = cd.status.empty() ? 0.0f : f_reg()->CalcTextSizeA(S(12.5f), FLT_MAX, iw, cd.status.c_str()).y;
    const float nt_h = cd.note.empty() ? 0.0f : f_reg()->CalcTextSizeA(S(12.5f), FLT_MAX, iw, cd.note.c_str()).y;
    float h = pad_y * 2 + S(24);
    if (cd.pic) h += pic_sz.y + S(10);
    if (desc_h > 0) h += S(10) + desc_h;
    if (st_h > 0) h += S(6) + st_h;
    if (nt_h > 0) h += S(4) + nt_h;
    if (cd.cost >= 0) h += S(10) + S(14);
    if (cd.cmp == CMP_SPLIT && show_split) h += S(10) + S(36);
    else if (cd.cmp == CMP_F3) h += S(10) + S(16);
    const float y1 = H - S(L_CARDM), y0 = y1 - h;
    rect(dl, ImVec2(x, y0), ImVec2(x + w, y1), rgba(5, 8, 13, 0.82f), 12);
    dl->AddRect(ImVec2(x, y0), ImVec2(x + w, y1), rgba(255, 255, 255, 0.10f), S(12), 0, S(1));
    float y = y0 + pad_y;
    const float cx = x + pad_x;
    if (cd.pic) {
        dl->AddImage(cd.pic->GetTexRef(), ImVec2(cx, y), ImVec2(cx + pic_sz.x, y + pic_sz.y));
        y += pic_sz.y + S(10);
    }
    float chip_w = 0;
    draw_chip(dl, ImVec2(x + w - pad_x, y + S(2)), cd.when, cd.env, &chip_w);
    { const float room = iw - chip_w - S(10);
      const float tw0 = tw_cond(24, cd.title.c_str());
      if (tw0 <= room) txt_cond(dl, 24, ImVec2(cx, y), C_TEXT, cd.title.c_str());
      else txt_cond(dl, 24, ImVec2(cx, y), C_TEXT, cd.title.c_str(), 0.84f * room / tw0 > 0.6f ? 0.84f * room / tw0 : 0.6f); }
    y += S(24);
    if (desc_h > 0) { y += S(10); dl->AddText(f_reg(), S(14), ImVec2(cx, y), C_DESC, cd.desc.c_str(), NULL, iw); y += desc_h; }
    if (st_h > 0) { y += S(6); dl->AddText(f_reg(), S(12.5f), ImVec2(cx, y), cd.warn ? C_AMBER : C_MUTED2, cd.status.c_str(), NULL, iw); y += st_h; }
    if (nt_h > 0) { y += S(4); dl->AddText(f_reg(), S(12.5f), ImVec2(cx, y), C_MUTED2, cd.note.c_str(), NULL, iw); y += nt_h; }
    if (cd.cost >= 0) {
        y += S(10);
        txt(dl, f_reg(), 12, ImVec2(cx, y), C_MUTED2, "GPU cost");
        const float bx = cx + tw(f_reg(), 12, "GPU cost") + S(8);
        for (int k = 1; k <= 3; k++)
            rect(dl, ImVec2(bx + (k - 1) * S(17), y + S(4)), ImVec2(bx + (k - 1) * S(17) + S(14), y + S(11)),
                 k <= cd.cost ? (cd.cost >= 3 ? rgba(0xf2, 0xa3, 0x3a) : acc()) : C_BAROFF, 2);
        y += S(14);
    }
    if (cd.cmp == CMP_SPLIT && show_split) {
        y += S(10);
        txt(dl, f_reg(), 12, ImVec2(cx, y), C_MUTED2, "Drag to compare with the PS3 look (L2 / R2)");
        const float ty = y + S(26);
        const ImVec2 a(cx, ty - S(2)), b(cx + iw, ty + S(2));
        rect(dl, a, b, C_TRACK, 2);
        const float kx = cx + iw * s_split;
        rect(dl, a, ImVec2(kx, b.y), acc(), 2);
        dl->AddCircleFilled(ImVec2(kx, ty), S(7), IM_COL32_WHITE);
        if (!menu_modal() && ImGui::IsMouseClicked(0) && hov(ImVec2(cx - S(8), ty - S(10)), ImVec2(cx + iw + S(8), ty + S(10)))) s_split_drag = true;
        if (s_split_drag) {
            if (!ImGui::IsMouseDown(0)) s_split_drag = false;
            else { float f = (ImGui::GetIO().MousePos.x - cx) / iw; s_split = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f; }
        }
        y += S(36);
    } else if (cd.cmp == CMP_F3) {
        y += S(10);
        txt(dl, f_semi(), 12.5f, ImVec2(cx, y), acc(), "Hold F3 to compare with the PS3 look");
    }
    return y0;
}

/* the compare line over the game: "PS3 | YOURS" */
static void draw_split(ImDrawList* dl, float W, float H)
{
    const float x0 = S(700) < W * 0.9f ? S(700) : W * 0.5f;
    const float x = x0 + (W - x0) * s_split;
    dl->AddRectFilled(ImVec2(x - S(1), 0), ImVec2(x + S(1), H), rgba(255, 255, 255, 0.9f));
    const float lw = txt_spaced(NULL, f_semi(), 12, ImVec2(0, 0), 0, "PS3", 1) + S(18);
    rect(dl, ImVec2(x - S(10) - lw, S(22)), ImVec2(x - S(10), S(22) + S(21)), rgba(0, 0, 0, 0.65f), 5);
    txt_spaced(dl, f_semi(), 12, ImVec2(x - S(10) - lw + S(9), S(25)), C_TEXT, "PS3", 1);
    const float yw = txt_spaced(NULL, f_bold(), 12, ImVec2(0, 0), 0, "YOURS", 1) + S(18);
    rect(dl, ImVec2(x + S(10), S(22)), ImVec2(x + S(10) + yw, S(22) + S(21)), acc(), 5);
    txt_spaced(dl, f_bold(), 12, ImVec2(x + S(19), S(25)), C_DARK, "YOURS", 1);
    /* the mouse drags the line itself (not through the card: last frame's top) */
    const ImVec2 mp = ImGui::GetIO().MousePos;
    if (!menu_modal() && ImGui::IsMouseClicked(0) && hov(ImVec2(x - S(8), 0), ImVec2(x + S(8), H)) &&
        (s_card_top <= 0.0f || mp.y < s_card_top - S(6))) s_line_drag = true;
    if (s_line_drag) {
        if (!ImGui::IsMouseDown(0)) s_line_drag = false;
        else { const float f = (mp.x - x0) / (W - x0); s_split = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f; }
    }
    vk_postfx_set_compare(x / W);
}

/* one row's controls on the right; mouse clicks act through row_step / slider_to */
static void draw_row(ImDrawList* dl, const MRow& r, ImVec2 a, ImVec2 b, bool focus, bool mouse)
{
    const bool d = r.disabled;
    if (r.kind == RK_HEAD) {
        const float y = b.y - S(20);
        txt_spaced(dl, f_semi(), 12, ImVec2(a.x + S(16), y), r.head_col ? r.head_col : C_MUTED, r.label.c_str(), 1.5f);
        return;
    }
    if (focus) rect(dl, a, b, rgba(255, 255, 255, 0.07f), 8);
    else if (mouse && hov(a, b)) rect(dl, a, b, rgba(255, 255, 255, 0.035f), 8);
    const float cy = (a.y + b.y) * 0.5f;
    const float lx = a.x + S(16) + (r.indent ? S(20) : 0.0f);
    const float lpx = r.indent ? 15.0f : 16.0f;
    float right = b.x - S(6);
    /* what the controls take on the right, so the label can be fitted */
    float ctl_w = S(208);
    if (r.kind == RK_TOGGLE) ctl_w = S(48 + 76);
    else if (r.kind == RK_INFO) ctl_w = r.vtext.empty() ? 0.0f : tw(f_semi(), 14, r.vtext.c_str()) + S(54);
    else if (r.kind == RK_ACTION) ctl_w = tw(f_semi(), 14, r.button.c_str()) + S(32) + (r.has_amount ? S(8 + 208) : S(54)) +
                                          (!r.has_amount && !r.vtext.empty() ? tw(f_semi(), 14, r.vtext.c_str()) + S(12) : 0.0f);
    const float lab_w = right - ctl_w - S(12) - lx;
    float lab_end = lx;
    { const ImU32 lc = dim(focus ? IM_COL32_WHITE : C_LABEL, d);
      const float w = tw(f_reg(), lpx, r.label.c_str());
      txt_fit(dl, f_reg(), lpx, ImVec2(lx, cy - S(lpx) * 0.62f), lc, r.label.c_str(), lab_w - (r.save_tag ? S(46) : 0.0f));
      lab_end = lx + (w < lab_w ? w : lab_w); }
    if (r.save_tag) {   /* cheats the game writes into the save */
        const float x = lab_end + S(8), w = tw(f_semi(), 10.5f, "SAVE") + S(10);
        rect(dl, ImVec2(x, cy - S(8)), ImVec2(x + w, cy + S(8)), rgba(242, 163, 58, d ? 0.08f : 0.18f), 4);
        txt(dl, f_semi(), 10.5f, ImVec2(x + S(5), cy - S(7)), dim(C_AMBER, d), "SAVE");
    }
    const bool click = mouse && !d && ImGui::IsMouseClicked(0);
    auto choice_block = [&](float bx, const std::string& val, int cur, int n, bool arrows) {
        /* "<  value  >" with the place in the list under it */
        const ImU32 ac = dim(C_MUTED2, d);
        const ImVec2 pa(bx, cy - S(15)), pb(bx + S(30), cy + S(15));
        const ImVec2 na(bx + S(178), cy - S(15)), nb(bx + S(208), cy + S(15));
        if (arrows) {
            const bool hp = mouse && hov(pa, pb), hn = mouse && hov(na, nb);
            txt(dl, f_reg(), 20, ImVec2(bx + S(10), cy - S(14)), hp && !d ? C_TEXT : ac, "\xE2\x80\xB9");
            txt(dl, f_reg(), 20, ImVec2(bx + S(188), cy - S(14)), hn && !d ? C_TEXT : ac, "\xE2\x80\xBA");
            if (click && hp) row_step(r, -1);
            else if (click && hn) row_step(r, 1);
        }
        const float vx = bx + S(34), vw = S(140);
        const bool pips = n > 1;
        txt_fit(dl, f_semi(), 15, ImVec2(vx, cy - S(pips ? 13 : 9)), dim(focus ? acc() : C_TEXT, d), val.c_str(), vw, 0.5f);
        if (pips) {
            const float py = cy + S(8);
            if (n <= 8) {
                const float total = n * S(14) + (n - 1) * S(3);
                float x = vx + (vw - total) * 0.5f;
                for (int i = 0; i < n; i++, x += S(17))
                    rect(dl, ImVec2(x, py), ImVec2(x + S(14), py + S(3)), i == cur ? dim(acc(), d) : rgba(255, 255, 255, 0.18f), 2);
            } else {
                rect(dl, ImVec2(vx + S(2), py), ImVec2(vx + vw - S(2), py + S(3)), rgba(255, 255, 255, 0.18f), 2);
                const float f = cur < 0 ? 0.0f : (float)cur / (float)(n - 1);
                const float mx = vx + S(2) + (vw - S(18)) * f;
                rect(dl, ImVec2(mx, py), ImVec2(mx + S(14), py + S(3)), dim(acc(), d), 2);
            }
        }
        if (click && arrows && hov(ImVec2(vx, cy - S(18)), ImVec2(vx + vw, cy + S(18)))) row_step(r, 1);
    };
    switch (r.kind) {
    case RK_CHOICE: {
        const std::string val = r.cur >= 0 && r.cur < (int)r.opts.size() ? r.opts[(size_t)r.cur] : r.vtext;
        choice_block(right - S(208), val, r.cur, (int)r.opts.size(), true);
        break; }
    case RK_TOGGLE: {
        const float x1 = right - S(76), x0 = x1 - S(48);
        const ImVec2 ta(x0, cy - S(13)), tb(x1, cy + S(13));
        rect(dl, ta, tb, dim(r.on ? acc() : C_TRACK, d), 13);
        const float kx = r.on ? x0 + S(25) : x0 + S(3);
        dl->AddCircleFilled(ImVec2(kx + S(10), cy), S(10), dim(IM_COL32_WHITE, d));
        if (click && hov(a, b)) row_step(r, 0);
        break; }
    case RK_SLIDER: {
        const float tx1 = right - S(54), tx0 = tx1 - S(150);
        const ImVec2 ta(tx0, cy - S(2)), tb(tx1, cy + S(2));
        rect(dl, ta, tb, dim(C_TRACK, d), 2);
        const float f = r.mx > r.mn ? (r.v - r.mn) / (r.mx - r.mn) : 0.0f;
        const float kx = tx0 + (tx1 - tx0) * (f < 0 ? 0 : f > 1 ? 1 : f);
        const float zx = r.bipolar ? tx0 + (tx1 - tx0) * (-r.mn / (r.mx - r.mn)) : tx0;
        rect(dl, ImVec2(kx < zx ? kx : zx, cy - S(2)), ImVec2(kx < zx ? zx : kx, cy + S(2)), dim(acc(), d), 2);
        if (r.bipolar) dl->AddLine(ImVec2(zx, cy - S(6)), ImVec2(zx, cy + S(6)), dim(C_MUTED, d), S(1));
        dl->AddCircleFilled(ImVec2(kx, cy), S(focus ? 8 : 7), dim(IM_COL32_WHITE, d));
        if (focus && !d) dl->AddCircle(ImVec2(kx, cy), S(10), acc(0.8f), 20, S(1.5f));
        txt_fit(dl, f_semi(), 15, ImVec2(right - S(50), cy - S(9)), dim(focus ? acc() : C_TEXT, d), r.vtext.c_str(), S(50), 1.0f);
        if (click && hov(ImVec2(tx0 - S(8), cy - S(14)), ImVec2(tx1 + S(8), cy + S(14)))) s_drag_id = r.id;
        if (s_drag_id == r.id && !d) {
            if (!ImGui::IsMouseDown(0)) s_drag_id.clear();
            else slider_to(r, r.mn + (r.mx - r.mn) * ((ImGui::GetIO().MousePos.x - tx0) / (tx1 - tx0)));
        }
        break; }
    case RK_ACTION: {
        const float bw = tw(f_semi(), 14, r.button.c_str()) + S(32);
        const float bx1 = right - (r.has_amount ? 0.0f : S(54)), bx0 = bx1 - bw;
        const ImVec2 ba(bx0, cy - S(16)), bb(bx1, cy + S(16));
        const bool hb = mouse && hov(ba, bb);
        if (focus && !d) rect(dl, ba, bb, acc_soft(), 6);
        dl->AddRect(ba, bb, dim(focus && !d ? acc() : hb && !d ? C_LABEL : C_LINE, d), S(6), 0, S(1));
        txt(dl, f_semi(), 14, ImVec2(bx0 + S(16), cy - S(9)), dim(C_TEXT, d), r.button.c_str());
        if (click && hb) row_step(r, 0);
        if (r.has_amount) choice_block(bx0 - S(8) - S(208), r.vtext, -1, 0, true);
        else if (!r.vtext.empty()) {
            const float vw = tw(f_semi(), 14, r.vtext.c_str());
            txt(dl, f_semi(), 14, ImVec2(bx0 - S(12) - vw, cy - S(9)), dim(focus ? acc() : C_MUTED2, d), r.vtext.c_str());
        }
        break; }
    case RK_INFO:
        if (!r.vtext.empty()) {
            const float vw = tw(f_semi(), 14, r.vtext.c_str());
            txt(dl, f_semi(), 14, ImVec2(right - S(54) - vw, cy - S(9)), focus ? acc() : C_MUTED2, r.vtext.c_str());
        }
        break;
    default: break;
    }
}

/* ---- the remap sub-page: the 24 controls x (key, other key, mouse, controller) ---- */
static int s_rm_row;   /* the focused control (index into prm_rows); PIM_N_CONTROLS = the Reset row */
static void remap_page(ImDrawList* dl, ImVec2 a, float w, float h, double now, bool nav, bool mouse, Card& cd)
{
    s_rm.drawn_t = now;
    if (!s_rm.order_init) remap_order_reset();
    PadInputMap m;
    cellPad_host_input_map(&m);
    const int N = PIM_N_CONTROLS;
    if (nav) {
        const int dy = nav_dy(), dx = nav_dx();
        if (dy) { s_rm_row = ((s_rm_row + dy) % (N + 1) + N + 1) % (N + 1); s_scroll_focus = true; }
        if (dx && s_rm_row < N) s_rm_col = ((s_rm_col + dx) % 4 + 4) % 4;
        if (nav_ok() && s_rm.state == RM_IDLE) {
            if (s_rm_row < N) remap_begin(prm_rows[s_rm_row].ctl, s_rm_col, now);
            else { s_rm.state = RM_RESET; s_rm.modal_cancel = false; s_rm.open_reset = true; }
        }
    }
    const bool clear_key = nav && (kp(ImGuiKey_GamepadFaceLeft, false) || kp(ImGuiKey_Delete, false) || kp(ImGuiKey_Backspace, false));
    const float rh = S(36), hh = S(26);
    const float cellw = S(82), gap = S(6);
    const float cells_x = a.x + w - S(6) - 4 * cellw - 3 * gap;
    /* column titles, outside the scrolling list */
    static const char* const kCol[4] = { "Key", "Other key", "Mouse", "Controller" };
    for (int c = 0; c < 4; c++)
        txt_fit(dl, f_semi(), 12, ImVec2(cells_x + c * (cellw + gap), a.y + S(4)), C_MUTED, kCol[c], cellw, 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + hh));
    ImGui::BeginChild("##remap", ImVec2(w, h - hh), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImDrawList* cl = ImGui::GetWindowDrawList();
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float sy = ImGui::GetScrollY(), vh = h - hh;
    int cap_slots[2] = { -1, -1 };
    for (int i = 0; i <= N; i++) {
        const float y = base.y + i * (rh + S(2));
        const ImVec2 ra(base.x, y), rb(base.x + w, y + rh);
        const bool focus = i == s_rm_row;
        if (s_scroll_focus && focus) {
            const float top = i * (rh + S(2)), bot = top + rh;
            if (top < sy) ImGui::SetScrollY(top); else if (bot > sy + vh) ImGui::SetScrollY(bot - vh);
            s_scroll_focus = false;
        }
        const bool mrow_h = mouse && ImGui::IsWindowHovered() && hov(ra, rb);
        if (mrow_h && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) s_rm_row = i;
        if (focus) rect(cl, ra, rb, rgba(255, 255, 255, 0.07f), 8);
        const float cy = y + rh * 0.5f;
        if (i == N) {   /* Reset to defaults */
            txt(cl, f_reg(), 15, ImVec2(ra.x + S(16), cy - S(10)), focus ? IM_COL32_WHITE : C_LABEL, "Reset to defaults");
            const float bw = tw(f_semi(), 13, "Reset") + S(26), bx1 = rb.x - S(6), bx0 = bx1 - bw;
            const bool hb = mrow_h && hov(ImVec2(bx0, cy - S(14)), ImVec2(bx1, cy + S(14)));
            cl->AddRect(ImVec2(bx0, cy - S(14)), ImVec2(bx1, cy + S(14)), focus ? acc() : hb ? C_LABEL : C_LINE, S(6), 0, S(1));
            txt(cl, f_semi(), 13, ImVec2(bx0 + S(13), cy - S(8)), C_TEXT, "Reset");
            if (hb && ImGui::IsMouseClicked(0) && s_rm.state == RM_IDLE) { s_rm.state = RM_RESET; s_rm.modal_cancel = false; s_rm.open_reset = true; }
            if (focus) { cd.title = "Reset to defaults"; cd.desc = "Every key, mouse button and controller button back to the defaults.";
                         cd.status = "Esc is Pause (START) and Enter is free. Esc cancels a key change, so only this Reset brings it back. "
                                     "F1-F3, F7, F9, F11 and the Windows key stay the window's. Alt binds; Alt+Enter, Alt+Tab and Alt+F4 still work."; }
            continue;
        }
        const PrmRow& r = prm_rows[i];
        int slots[2];
        const int extra = prm_key_slots(&m, r.ctl, s_rm.order[r.ctl], slots);
        s_rm.order[r.ctl][0] = slots[0]; s_rm.order[r.ctl][1] = slots[1];
        if (s_rm.state != RM_IDLE && s_rm.ctl == r.ctl) { cap_slots[0] = slots[0]; cap_slots[1] = slots[1]; }
        { const float nw = tw(f_semi(), 14, r.name);
          txt(cl, f_semi(), 14, ImVec2(ra.x + S(12), cy - S(9)), focus ? IM_COL32_WHITE : C_LABEL, r.name);
          txt_fit(cl, f_reg(), 12.5f, ImVec2(ra.x + S(12) + nw + S(6), cy - S(7)), C_MUTED2, r.does, cells_x - (ra.x + S(12) + nw + S(6)) - S(6)); }
        for (int col = 0; col < 4; col++) {
            char name[48], tmp[24];
            if (col < 2) {
                if (slots[col] < 0) snprintf(name, sizeof name, "-");
                else if (col == 1 && extra) snprintf(name, sizeof name, "%s +%d", prm_key_name((uint32_t)slots[col], tmp, sizeof tmp), extra);
                else snprintf(name, sizeof name, "%s", prm_key_name((uint32_t)slots[col], tmp, sizeof tmp));
            } else if (col == 2) remap_bits_label(m.mouse[r.ctl], PRM_MOUSE, name, sizeof name);
            else remap_bits_label(m.pad[r.ctl], PRM_PAD, name, sizeof name);
            const bool waiting = s_rm.state == RM_CAPTURE && s_rm.ctl == r.ctl && s_rm.slot == col;
            const float cx0 = cells_x + col * (cellw + gap);
            const ImVec2 ca(cx0, cy - S(13)), cb(cx0 + cellw, cy + S(13));
            const bool cf = focus && s_rm_col == col;
            const bool ch = mrow_h && hov(ca, cb);
            if (ch && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) s_rm_col = col;
            rect(cl, ca, cb, cf ? acc_soft() : rgba(255, 255, 255, 0.04f), 6);
            if (cf || ch) cl->AddRect(ca, cb, cf ? acc() : C_LINE, S(6), 0, S(1));
            txt_fit(cl, f_semi(), 13, ImVec2(cx0 + S(4), cy - S(8)), waiting ? acc() : !strcmp(name, "-") ? C_MUTED : C_TEXT,
                    waiting ? "..." : name, cellw - S(8), 0.5f);
            bool clear = false;
            if (ch && s_rm.state == RM_IDLE) {
                if (ImGui::IsMouseClicked(0)) remap_begin(r.ctl, col, now);
                if (ImGui::IsMouseClicked(1)) clear = true;
            }
            if (cf && clear_key && s_rm.state == RM_IDLE) clear = true;
            if (clear) {
                char what[120];
                if (col < 2 && slots[col] >= 0) {
                    snprintf(what, sizeof what, "%s: %s cleared", r.name, prm_key_name((uint32_t)slots[col], tmp, sizeof tmp));
                    prm_clear(&m, r.ctl, PRM_KEY, slots[col]);
                    s_rm.order[r.ctl][col] = -1;
                    remap_commit(m, what);
                } else if (col == 2 && m.mouse[r.ctl]) {
                    prm_clear(&m, r.ctl, PRM_MOUSE, -1);
                    snprintf(what, sizeof what, "%s: no mouse input", r.name);
                    remap_commit(m, what);
                } else if (col == 3 && m.pad[r.ctl]) {
                    prm_clear(&m, r.ctl, PRM_PAD, -1);
                    snprintf(what, sizeof what, "%s: no controller input", r.name);
                    remap_commit(m, what);
                }
            }
            if (cf) {
                cd.title = sfmt("%s \xC2\xB7 %s", r.name, kCol[col]);
                cd.desc = sfmt("%s. Cross or click, then press the new input. Square or right-click clears.", r.does);
                cd.status = name;
            }
        }
    }
    ImGui::Dummy(ImVec2(w, (N + 1) * (rh + S(2))));
    ImGui::EndChild();
    if (s_rm.status[0]) { cd.note = s_rm.status; cd.warn = s_rm.status_bad; }
    cd.when = WH_INSTANT;
    remap_capture_popup(m, cap_slots, now);
    remap_swap_popup(m);
    remap_reset_popup(m);
}

/* the release notes (Update > What's new) */
static void notes_page(ImVec2 a, float w, float h, bool nav)
{
    static UpdateStatus v;
    { std::lock_guard<std::mutex> g(s_upd_lock); v = s_upd; }
    ImGui::SetCursorScreenPos(a);
    ImGui::BeginChild("##notes", ImVec2(w, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    if (nav) { const int dy = nav_dy(); if (dy) ImGui::SetScrollY(ImGui::GetScrollY() + dy * S(60)); }
    ImGui::PushTextWrapPos(w - S(16));
    if (v.title[0]) { ImGui::PushFont(f_bold(), 17.0f); ImGui::TextUnformatted(v.title); ImGui::PopFont(); }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.77f, 0.81f, 1.0f));
    ImGui::PushFont(NULL, 15.0f);
    ImGui::TextUnformatted(v.notes.c_str(), v.notes.c_str() + v.notes.size());
    if (v.url[0]) ImGui::TextUnformatted(v.url);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

/* ---- the frame ------------------------------------------------------------------- */
static void draw_menu(double now)
{
    if (vk_overlay_saves_active()) { s_card_top = 0; vk_postfx_set_compare(0.0f); vk_overlay_saves_draw(S(1.0f)); return; }   /* [saves] */
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    s_ntabs = tab_count();
    if (s_tab >= s_ntabs) s_tab = 0;
    if (s_tab_want >= 0) { menu_set_tab(s_tab_want); s_tab_want = -1; }
    if (s_sub_want >= 0) { menu_sub(s_sub_want); s_sub_want = -1; }
    if (s_sub == SUB_REMAP && s_tab != 3) s_sub = SUB_NONE;
    if (s_sub == SUB_ZIP && s_tab != TAB_MODS) s_sub = SUB_NONE;
    if (s_sub == SUB_NOTES && s_tab != TAB_UPDATE) s_sub = SUB_NONE;

    const bool modal = menu_modal();
    const bool nav = !modal;
    const bool mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;

    /* the rows of the page (or sub-page) */
    static Rows R;
    R.clear();
    const bool list_page = s_sub == SUB_NONE || s_sub == SUB_ZIP;
    if (s_sub == SUB_ZIP) page_zip(R);
    else if (s_sub == SUB_NONE) kPageFn[tab_page(s_tab)](R);
    const int n = (int)R.size();
    const bool pills = s_sub == SUB_NONE && s_tab == 1;      /* Graphics: the presets */
    const bool back_pill = s_sub != SUB_NONE;
    const int fmin = (pills || back_pill) ? -1 : 0;
    /* the focus: kept on the same row by its id while the list changes */
    if (!s_focus_req.empty()) { s_fkey = s_focus_req; s_focus_req.clear(); s_scroll_focus = true; }
    if (list_page) {
        int found = -2;
        if (!s_fkey.empty()) {
            if (s_fkey == "#pills" && fmin < 0) found = -1;
            else if (s_fkey == "#footer") found = n;
            else for (int i = 0; i < n; i++) if (R[(size_t)i].id == s_fkey) { found = i; break; }
        }
        if (found != -2) s_fi = found;
        if (s_fi < fmin) s_fi = fmin;
        if (s_fi > n) s_fi = n;
        while (s_fi >= 0 && s_fi < n && !focusable(R[(size_t)s_fi])) s_fi++;
        if (s_fi == n && n > 0 && s_fkey != "#footer") { s_fi = n - 1; while (s_fi > 0 && !focusable(R[(size_t)s_fi])) s_fi--; }
    }

    /* keys and the pad (not while a question is up) */
    if (nav) {
        if (kp(ImGuiKey_Q, false) || (kp(ImGuiKey_Tab, false) && io.KeyShift) || nav_scripted("l1")) s_tab_want = (s_tab + s_ntabs - 1) % s_ntabs;
        else if (kp(ImGuiKey_E, false) || (kp(ImGuiKey_Tab, false) && !io.KeyShift) || nav_scripted("r1")) s_tab_want = (s_tab + 1) % s_ntabs;
        if (nav_scripted("back")) menu_escape();
        if (kp(ImGuiKey_GamepadFaceUp, false)) s_quit_ask = 1;
        const float dt = io.DeltaTime > 0 ? io.DeltaTime : 1.0f / 60.0f;
        if (ImGui::IsKeyDown(ImGuiKey_GamepadL2) || ImGui::IsKeyDown(ImGuiKey_LeftBracket)) s_split = std::max(0.0f, s_split - dt * 0.6f);
        if (ImGui::IsKeyDown(ImGuiKey_GamepadR2) || ImGui::IsKeyDown(ImGuiKey_RightBracket)) s_split = std::min(1.0f, s_split + dt * 0.6f);
    }
    if (nav && list_page) {
        const int dy = nav_dy();
        if (dy) {
            int i = s_fi;
            for (int guard = 0; guard < n + 3; guard++) {
                i += dy;
                if (i > n) i = fmin;
                if (i < fmin) i = n;
                if (i == -1 || i == n || focusable(R[(size_t)i])) break;
            }
            s_fi = i;
            s_scroll_focus = true;
        }
        const int dx = nav_dx();
        const bool ok = nav_ok();
        if (s_fi == -1) {
            if (pills) { if (dx) s_pill = s_pill ? 0 : 1; if (ok) { const bool low = s_pill == 0; op([low] { lowend_preset(low); }); fprintf(stderr, "[overlay] menu: preset %s\n", low ? "Low-end PC" : "Standard"); } }
            else if (ok) s_sub_want = SUB_NONE;
        } else if (s_fi == n) {
            if (dx) s_foot = s_foot ? 0 : 1;
            if (ok) { if (s_foot) op([] { set_open(0); }); else s_quit_ask = 1; }
        } else if (s_fi >= 0 && s_fi < n) {
            const MRow& r = R[(size_t)s_fi];
            if (dx) row_step(r, dx);
            if (ok) row_step(r, 0);
        }
        s_fkey = s_fi == -1 ? "#pills" : s_fi == n ? "#footer" : (s_fi >= 0 && s_fi < n) ? R[(size_t)s_fi].id : "";
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##menuE", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool mouse = !modal && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

    /* the card's content: the focused row, the pills or the footer */
    Card cd;
    if (list_page && s_fi >= 0 && s_fi < n) {
        const MRow& r = R[(size_t)s_fi];
        cd.title = r.label; cd.desc = r.desc; cd.status = r.status; cd.warn = r.status_warn; cd.note = r.note;
        cd.when = r.kind == RK_ACTION && r.when == WH_INSTANT && !r.vars && !r.extra ? WH_NONE : r.when;
        cd.cost = r.cost; cd.cmp = r.cmp; cd.env = r.env; cd.vars = r.vars; cd.pic = r.pic;
        if (r.env) {
            for (int k = 0; k < V_N; k++)
                if ((r.vars >> k & 1ull) && s_env_set[k]) { cd.note = sfmt("%s was set at launch: it wins at the next start.", kVar[k]); break; }
        }
    } else if (s_fi == -1 && pills) {
        cd.title = "Presets";
        cd.desc = "Low-end PC: 720p, PS3 effects and shadows, less video memory. Standard: the port's usual look.";
        cd.status = lowend_status();
        if (s_le_auto == 1 && s_le_changed > 0) cd.note = "This PC started with the Low-end PC values for the settings you had not set.";
        cd.cost = s_pill == 0 ? 1 : 3;
        cd.when = WH_INSTANT;
    } else if (s_fi == -1 && back_pill) {
        cd.title = "Back"; cd.desc = sfmt("Back to %s (Circle or Esc).", kTabName[tab_page(s_tab)]);
    } else if (s_fi == n) {
        cd.title = s_foot ? "Resume" : "Quit game";
        cd.desc = s_foot ? "Back to the game (Circle or Esc). Changes are saved." : "Ends the game. Progress since your last save is lost.";
        cd.status = s_ini_path[0] ? "Settings are saved as you change them." : "Not saved: PS3_OVERLAY_INI is empty.";
    }

    /* the compare line (under the panel's fade) */
    const bool show_split = cd.cmp == CMP_SPLIT && !s_lfx.comparing;
    if (show_split) draw_split(dl, W, H);
    else { vk_postfx_set_compare(0.0f); s_split_drag = s_line_drag = false; }

    /* the panel: dark at the left, fading into the game */
    { const float pw = S(L_PANEL) < W ? S(L_PANEL) : W, mid = pw * 0.72f;
      const ImU32 c0 = rgba(5, 8, 13, 0.97f), c1 = rgba(5, 8, 13, 0.93f), c2 = rgba(5, 8, 13, 0.0f);
      dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(mid, H), c0, c1, c1, c0);
      dl->AddRectFilledMultiColor(ImVec2(mid, 0), ImVec2(pw, H), c1, c2, c2, c1); }

    /* the rail */
    for (int t = 0; t < s_ntabs; t++) {
        const ImVec2 a(S(L_RAILX), S(L_RAILY) + t * S(L_RAILB + L_RAILGAP)), b(a.x + S(L_RAILB), a.y + S(L_RAILB));
        const bool on = t == s_tab, h = mouse && hov(a, b);
        rect(dl, a, b, on ? acc_soft() : h ? rgba(255, 255, 255, 0.06f) : 0, 10);
        icon(dl, tab_page(t), ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), S(20), on ? acc() : h ? C_LABEL : C_MUTED);
        if (h) { ImGui::SetTooltip("%s", kTabName[tab_page(t)]); if (ImGui::IsMouseClicked(0)) s_tab_want = t; }
    }

    /* the header */
    const float cx = S(L_COLX), cw = S(L_COLW);
    float y = S(L_TOP);
    { std::string small = "SETTINGS", big = kTabName[tab_page(s_tab)];
      if (s_sub) { small = kTabName[tab_page(s_tab)]; big = s_sub == SUB_REMAP ? "Buttons and keys" : s_sub == SUB_ZIP ? "Install from a zip" : "What's new"; }
      for (char& ch : small) ch = (char)toupper((unsigned char)ch);
      for (char& ch : big) ch = (char)toupper((unsigned char)ch);
      txt_spaced(dl, f_semi(), 12, ImVec2(cx, y), C_MUTED, small.c_str(), 3);
      txt_cond(dl, 38, ImVec2(cx, y + S(15)), C_TEXT, big.c_str()); }
    y += S(15 + 2 + 40 + 10);

    /* the presets (Graphics) or Back (a sub-page) */
    if (pills || back_pill) {
        const int st = pills ? lowend_state() : 0;
        static const char* const kPill[3] = { "Low-end PC", "Standard", "Your mix" };
        float x = cx;
        const int np = pills ? 3 : 1;
        for (int i = 0; i < np; i++) {
            const char* t = pills ? kPill[i] : "\xE2\x80\xB9  Back";
            const float w = tw(f_semi(), 14, t) + S(32);
            const ImVec2 a(x, y), b(x + w, y + S(34));
            const bool active = pills && ((i == 0 && st == 1) || (i == 1 && st == 2) || (i == 2 && st == 0));
            const bool focus = s_fi == -1 && (back_pill || s_pill == i);
            const bool h = mouse && hov(a, b);
            rect(dl, a, b, active ? acc() : h ? rgba(255, 255, 255, 0.06f) : 0, 17);
            dl->AddRect(a, b, active ? acc() : h ? C_LABEL : C_LINE, S(17), 0, S(1));
            if (focus) dl->AddRect(ImVec2(a.x - S(3), a.y - S(3)), ImVec2(b.x + S(3), b.y + S(3)), vk_ov_u32(vk_ov_theme().accent_hi, 0.9f), S(20), 0, S(2));
            txt(dl, f_semi(), 14, ImVec2(a.x + S(16), a.y + S(8)), active ? C_DARK : C_LABEL, t);
            if (h && mouse_moved && (back_pill || i < 2)) { s_fi = -1; s_fkey = "#pills"; if (pills) s_pill = i; }
            if (h && ImGui::IsMouseClicked(0)) {
                if (back_pill) s_sub_want = SUB_NONE;
                else if (i < 2) { const bool low = i == 0; op([low] { lowend_preset(low); }); fprintf(stderr, "[overlay] menu: preset %s (mouse)\n", low ? "Low-end PC" : "Standard"); }
            }
            x += w + S(8);
        }
        y += S(34 + 10);
    }

    /* the rows */
    const float foot_top = H - S(L_BOTTOM) - S(36) - S(10);
    const float rows_y = y + S(4), rows_h = foot_top - S(10) - rows_y;
    if (s_sub == SUB_REMAP) {
        remap_page(dl, ImVec2(cx, rows_y), cw, rows_h, now, nav, mouse, cd);
        if (nav && s_fi == -1) { /* the Back pill keeps the focus only by the mouse */ }
    } else if (s_sub == SUB_NOTES) {
        notes_page(ImVec2(cx, rows_y), cw, rows_h, nav);
        cd.title = "What's new"; cd.desc = "The release notes of the new version.";
    } else {
        ImGui::SetCursorScreenPos(ImVec2(cx, rows_y));
        ImGui::BeginChild("##rows", ImVec2(cw, rows_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        ImDrawList* rl = ImGui::GetWindowDrawList();
        const ImVec2 base = ImGui::GetCursorScreenPos();
        const float sy = ImGui::GetScrollY();
        const bool rmouse = mouse && ImGui::IsWindowHovered();
        float off = 0;
        for (int i = 0; i < n; i++) {
            const MRow& r = R[(size_t)i];
            const float rh = row_h(r);
            const ImVec2 a(base.x, base.y + off), b(base.x + cw, base.y + off + rh);
            if (i == s_fi && s_scroll_focus) {
                const float top = off - (i > 0 && R[(size_t)i - 1].kind == RK_HEAD ? S(L_HEADH) : 0.0f), bot = off + rh;
                if (top < sy) ImGui::SetScrollY(top);
                else if (bot > sy + rows_h) ImGui::SetScrollY(bot - rows_h);
                s_scroll_focus = false;
            }
            if (b.y >= rows_y - S(60) && a.y <= rows_y + rows_h + S(60)) {
                const bool h = rmouse && hov(a, b);
                if (h && mouse_moved && focusable(r)) { s_fi = i; s_fkey = r.id; }
                draw_row(rl, r, a, b, i == s_fi, h);
            }
            off += rh + S(L_ROWGAP);
        }
        if (s_scroll_focus && (s_fi < 0 || s_fi >= n)) { ImGui::SetScrollY(s_fi < 0 ? 0.0f : off); s_scroll_focus = false; }
        ImGui::Dummy(ImVec2(cw, off > 0 ? off : 1.0f));
        const float maxs = ImGui::GetScrollMaxY();
        ImGui::EndChild();
        if (maxs > 0.5f) {   /* a slim scroll indicator */
            const float th = rows_h * rows_h / (rows_h + maxs), ty = rows_y + (rows_h - th) * (sy / maxs);
            rect(dl, ImVec2(cx + cw + S(6), rows_y), ImVec2(cx + cw + S(9), rows_y + rows_h), rgba(255, 255, 255, 0.06f), 2);
            rect(dl, ImVec2(cx + cw + S(6), ty), ImVec2(cx + cw + S(9), ty + th), rgba(255, 255, 255, 0.28f), 2);
        }
    }

    /* the footer */
    dl->AddLine(ImVec2(cx, foot_top), ImVec2(cx + cw, foot_top), rgba(255, 255, 255, 0.08f), S(1));
    { const float fy = foot_top + S(10), cyf = fy + S(18);
      float x = cx;
      auto keycap = [&](const char* k) {
          const float w = tw(f_reg(), 13, k) + S(12);
          dl->AddRect(ImVec2(x, cyf - S(10)), ImVec2(x + w, cyf + S(10)), C_LINE, S(4), 0, S(1));
          txt(dl, f_reg(), 13, ImVec2(x + S(6), cyf - S(9)), C_LABEL, k);
          x += w + S(5);
      };
      auto label = [&](const char* t) { txt(dl, f_reg(), 13, ImVec2(x, cyf - S(9)), C_MUTED2, t); x += tw(f_reg(), 13, t) + S(14); };
      keycap("L1"); keycap("R1"); label("Tabs");
      keycap("F3"); label("PS3 look");
      if (s_sub) { keycap("Esc"); label("Back"); }
      /* Resume, Quit game */
      const float rw = tw(f_bold(), 14, "Resume") + S(40), qw = tw(f_semi(), 14, "Quit game") + S(32);
      const ImVec2 ra(cx + cw - rw, fy), rb(cx + cw, fy + S(36));
      const ImVec2 qa(ra.x - S(8) - qw, fy), qb(ra.x - S(8), fy + S(36));
      const bool hr = mouse && hov(ra, rb), hq = mouse && hov(qa, qb);
      rect(dl, ra, rb, hr ? vk_ov_u32(vk_ov_theme().accent_hi) : acc(), 8);
      txt(dl, f_bold(), 14, ImVec2(ra.x + S(20), fy + S(9)), C_DARK, "Resume");
      if (hq) rect(dl, qa, qb, rgba(255, 255, 255, 0.06f), 8);
      dl->AddRect(qa, qb, hq ? C_LABEL : C_LINE, S(8), 0, S(1));
      txt(dl, f_semi(), 14, ImVec2(qa.x + S(16), fy + S(9)), C_LABEL, "Quit game");
      const ImU32 ring = vk_ov_u32(vk_ov_theme().accent_hi, 0.9f);
      if (list_page && s_fi == n) {
          if (s_foot) dl->AddRect(ImVec2(ra.x - S(3), ra.y - S(3)), ImVec2(rb.x + S(3), rb.y + S(3)), ring, S(10), 0, S(2));
          else dl->AddRect(ImVec2(qa.x - S(3), qa.y - S(3)), ImVec2(qb.x + S(3), qb.y + S(3)), ring, S(10), 0, S(2));
      }
      if ((hr || hq) && mouse_moved && list_page) { s_fi = n; s_fkey = "#footer"; s_foot = hr ? 1 : 0; }
      if (hr && ImGui::IsMouseClicked(0)) op([] { set_open(0); });
      if (hq && ImGui::IsMouseClicked(0)) s_quit_ask = 1; }

    /* the card */
    s_card_top = draw_card(dl, cd, W, H, show_split);

    quit_confirm();
    restart_confirm();   /* [modsui] */
    conflict_dialog();   /* [modsui] a mod turned on conflicts with another */
    update_confirm();    /* [update] */
    ImGui::End();

    /* the changes asked for this frame */
    std::vector<std::function<void()>> ops;
    ops.swap(s_ops);
    for (auto& f : ops) f();
}
