/*
 * vk_overlay -- the Vulkan backend's in-game overlay (see vk_overlay.h).
 *
 * Dear ImGui (third_party/imgui v1.92.9b, MIT) with its own Vulkan renderer
 * (imgui_impl_vulkan, entry points loaded through the backend's
 * vkGetInstanceProcAddr: the backend loads libvulkan at run time) and SDL2
 * platform backend (imgui_impl_sdl2, started the first time the menu opens:
 * the HUD alone needs no input and leaves SDL's cursor and capture alone).
 *
 * It draws into the swapchain image after the game's frame was blitted into
 * it: a barrier to COLOR_ATTACHMENT_OPTIMAL, then its own render pass (load,
 * store, final layout PRESENT_SRC_KHR), so the image leaves ready to present.
 *
 * The settings, the HUD's numbers and the menu's pages follow the Metal
 * backend's ps3_overlay.mm (same variable-file format, same "env" marks,
 * same pad navigation from cellPad's g_pad_overlay_nav).
 */
#include "vk_overlay.h"

#include <SDL2/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"   /* [modsui] ImGui::RegisterUserTexture: the mods' preview pictures (as vk_overlay_saves.cpp) */
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_sdl2.h"

#include "../../include/ps3emu/ps3_settings.h"
#include "../../include/ps3emu/ps3_title_view.h"   /* (dofov) the Depth of field page: does the title tell gameplay from cutscenes */
#include "../../include/ps3emu/ps3_lightfx.h"   /* [light] the Lighting page (vk_overlay_light.inc) */
#include "../../include/ps3emu/ps3_game_mods.h" /* [mods] the Mods page: runtime/ppu/ppu_fs_mods.h */
#include "mod_preview.h"                        /* [modsui] a mod's preview picture, decoded and shrunk */
#include "vk_postfx.h"
#include "vk_overlay_theme.h"   /* the inFAMOUS look: Hero (blue) / Infamous (red) */
#include "../../include/ps3emu/ps3_pad_icons.h"
#include "../../runtime/spu/spu_patch.h"   /* [trim] the SPU instruction alternatives (Mesh trimming) */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"   /* the parser half is cellPad's */
#endif
#include "../input/pad_input_map.h"                     /* PadInputMap, labels */
#include "../input/pad_remap.h"                         /* [remap] the rebinding rules (vk_overlay_remap.inc) */
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sys/resource.h>
#include <sys/stat.h>
#endif

/* ---- what the menu changes: the toolkit's live setters ------------------- */
extern "C" {
int   cellAudio_host_volume(void);
void  cellAudio_host_set_volume(int pct);
int   cellPad_host_rumble_strength(void);
int   cellPad_host_controller_names(char (*out)[128], int max);
void  cellPad_host_set_rumble_strength(int pct);
int   cellPad_host_input_mode(void);
void  cellPad_host_set_input_mode(int mode);
void  cellPad_host_input_map(PadInputMap* out);
void  ps3_button_icons_set_mode(int mode);      /* rsx_button_icons.h */
int   ps3_button_icons_mode_setting(void);
int   ps3_button_icons_effective(void);
void  ps3_host_quit(const char* why);           /* libs/system/cellSysutil.c */
void  ps3_host_restart(const char* why);        /* [modsui] "Restart to apply" (cellSysutil.c) */
int   ps3_host_can_restart(void);
void  ps3_host_set_relauncher(int (*fn)(void));
void  ps3_sysutil_host_language(char* out, size_t n);   /* cellSysutil.c: "Automatic"'s PS3_LANG code */
int   ps3_sysutil_language_in_use(void);                /* CELL_SYSUTIL_LANG_* the game was told, -1 */
void  ps3_lang_in_use(int kind, char* asked, char* given, size_t n);   /* runtime/ppu/ppu_fs_lang.h */
int   ps3_lang_available(int kind, char (*out)[24], int max);
void  ps3_lang_missing(int kind, char* out, size_t n);   /* the dub wanted but not on the disc */
int   vk_overlay_cheats_count(void);            /* vk_overlay_cheats.cpp: the title's cheats (ps3_cheats.h) */
int   ps3_overlay_cheat_set(const char* id, double v);   /* below: "cheat=id=value" in the timed script */
int   vk_overlay_cheats_owns_key(const char* k);         /* its PS3_CHEAT_* lines of the settings file */
void  vk_overlay_cheats_write(FILE* f);
void  vk_overlay_cheats_attach(void);                    /* the remembered cheats, at window open */
int   vk_overlay_cheats_remember(void);
void  vk_overlay_cheats_hotkey(void);                    /* [spot] F7: the PS3_CHEAT_HOTKEY action */
/* [saves] the system's save-data list (vk_overlay_saves.cpp, include/ps3emu/ps3_savedlg.h): it
 * takes the menu's window while up -- the same input hold, cursor and RSX-thread drawing */
int   vk_overlay_saves_active(void);
int   vk_overlay_saves_poll(void);                 /* 1: open the window for it, 2: close it */
void  vk_overlay_saves_draw(float scale);
void  vk_overlay_saves_pad(uint32_t buttons, int lx, int ly);
void  vk_overlay_saves_back(void);                 /* Esc */
void  vk_overlay_saves_attach(void);
void  vk_overlay_saves_shutdown(void);
extern volatile uint32_t g_pad_overlay_inject;   /* cellPad: scripted buttons while a system dialog holds the pad */
extern volatile int      g_pad_overlay_open;   /* libs/input/cellPad.c */
extern volatile int      g_pad_overlay_present; /* cellPad: a menu exists */
extern volatile int      g_pad_layout_note;     /* cellPad: 1 = the player's old input.toml just got Esc as START */
extern volatile uint32_t g_pad_overlay_nav, g_pad_overlay_nav2, g_pad_overlay_raw;
}

volatile int g_vk_overlay_active;
volatile int g_vk_overlay_on_worker;   /* PS3RECOMP_VK_PRESENT_ASYNC: the recording thread is drawing */
/* ImGui's context: the HUD may be built on the recording thread while the
 * RSX thread's event pump opens or closes the menu (set_open touches io). */
static std::recursive_mutex s_imgui_lock;

/* CELL_PAD_CTRL_* (libs/input/cellPad.h) */
enum {
    PAD_SELECT = 1 << 0, PAD_L3 = 1 << 1, PAD_R3 = 1 << 2, PAD_START = 1 << 3,
    PAD_UP = 1 << 4, PAD_RIGHT = 1 << 5, PAD_DOWN = 1 << 6, PAD_LEFT = 1 << 7,
    PAD_L2 = 1 << 8, PAD_R2 = 1 << 9, PAD_L1 = 1 << 10, PAD_R1 = 1 << 11,
    PAD_TRIANGLE = 1 << 12, PAD_CIRCLE = 1 << 13, PAD_CROSS = 1 << 14, PAD_SQUARE = 1 << 15,
};

static double now_s(void)
{
    static double inv;
    if (!inv) inv = 1.0 / (double)SDL_GetPerformanceFrequency();
    return (double)SDL_GetPerformanceCounter() * inv;
}

static bool env_on(const char* v)
{
    const char* e = getenv(v);
    return e && *e && strcmp(e, "0") != 0;
}

/* ---- settings and their file ---------------------------------------------
 * The file is a list of VARIABLE=value lines with the same names and values
 * as the environment variables. Before main, each line whose variable is not
 * set is exported, so every module reads it the way it always read its
 * variable; one that is set wins. Lines for variables the menu does not
 * manage are applied the same way and kept when the file is rewritten. */
enum {
    V_FPS_CAP, V_PRESENT, V_FULLSCREEN, V_CAPTURE,
    V_AA, V_SHARPEN, V_RENDER_SCALE, V_ANISO, V_UPSCALE,
    V_DOF, V_EDGE_BLUR, V_MESH_TRIM, V_EDGE_VP,
    V_VOLUME,
    V_INPUT_MODE, V_BUTTON_ICONS, V_RUMBLE, V_RUMBLE_STRENGTH,
    V_HUD, V_HUD_CORNER, V_HUD_OPACITY, V_HUD_GRAPH, V_HUD_CPU,
    V_THEME,
    V_LANG, V_VOICE_LANG, V_TEXT_LANG,
    V_ASPECT,                 /* [wide] */
    V_GAME_MODS,              /* [mods] */
    V_FOV,                    /* (dofov) the title's field of view (cheats.cpp), live */
    V_MSAA, V_ALPHA_AA,       /* [aa] read at start (rsx_vulkan_backend.c vk_msaa.inc, rsx_fp_apply_alpha_aa) */
    V_N
};
static const char* const kVar[V_N] = {
    "PS3RECOMP_FPS_CAP", "PS3RECOMP_VK_PRESENT", "PS3RECOMP_VK_FULLSCREEN", "PS3RECOMP_VK_CAPTURE_FRIENDLY",
    "PS3RECOMP_AA", "PS3RECOMP_SHARPEN", "PS3RECOMP_RENDER_SCALE", "PS3RECOMP_ANISO", "PS3RECOMP_UPSCALE",
    "PS3RECOMP_DOF", "PS3RECOMP_EDGE_BLUR", "PS3RECOMP_MESH_TRIM", "PS3RECOMP_EDGE_VP",
    "PS3_AUDIO_VOLUME",
    "PS3_INPUT_MODE", "PS3_BUTTON_ICONS", "PS3_RUMBLE", "PS3_RUMBLE_STRENGTH",
    "PS3_OVERLAY_HUD", "PS3_OVERLAY_HUD_CORNER", "PS3_OVERLAY_HUD_OPACITY",
    "PS3_OVERLAY_HUD_GRAPH", "PS3_OVERLAY_HUD_CPU",
    "PS3_OVERLAY_THEME",
    "PS3_LANG", "PS3_VOICE_LANG", "PS3_TEXT_LANG",   /* the game reads them once, at start */
    "PS3RECOMP_ASPECT",                               /* [wide] read once, at start */
    "PS3_GAME_MODS",                                  /* [mods] read once, at start */
    "PS3_FOV",                                        /* (dofov) percent of the game's field of view */
    "PS3RECOMP_MSAA", "PS3RECOMP_ALPHA_AA",           /* [aa] read once, at start */
};
/* Plain arrays: filled during this file's dynamic initialisation, before
 * main (zero-initialised storage exists before any of it runs). */
static bool s_env_set[V_N];          /* in the environment when the game started */
static char s_saved[V_N][48];        /* the file's value ("" = not in it) */
static char s_value[V_N][48];        /* what the menu set this session ("" = untouched) */
static char s_start[V_N][48];        /* the value in effect at start (env, file or default) */
enum { EXTRA_N = 32 };
static char s_extra[EXTRA_N][2][160];   /* other VAR=value lines of the file, kept */
static int  s_n_extra;
static char s_ini_path[1024];
static bool s_dirty;
static double s_dirty_t;
static std::mutex s_set_lock;

static bool overlay_off(void)
{
    const char* e = getenv("PS3_OVERLAY");
    return e && e[0] == '0' && e[1] == 0;
}

/* ---- (lowend) the settings the "Low-end PC" preset and the automatic choice
 * for small graphics cards set (vk_overlay_lowend.inc), and where each one's
 * value came from at start: the player's (a variable set when the game
 * started, a line in the file) or not (the launcher's PS3_OVERLAY_DEFAULTS,
 * nothing). Only those that are not the player's change automatically. The
 * standard values are the launcher's (tools/play.ps1 PS3_OVERLAY_DEFAULTS)
 * when it names them, else the ones here (the same, 2026-11-13); `unset` is
 * what the modules use when the variable is not set (the PS3 look).
 * Measured on the New Marais walk (RTX 2070 Super, GPU timestamps): standard
 * 6.2 ms of GPU a frame and ~0.9 GB of video memory; these 5.0 ms and ~0.65 GB.
 * At 720p the decals' own-target reads are copies unless FBFETCH is on (49
 * passes, 2.3 ms of copies a frame): 720p without it costs as much as 1080p. */
static const struct { const char* var; const char* low; const char* std_; const char* unset; } kLowEnd[] = {
    { "PS3RECOMP_RENDER_SCALE",    "1",   "1.5",  "1" },   /* 720p: the screen targets 2.25x smaller (~240 MB) */
    { "PS3RECOMP_ENG_FBFETCH",     "1",   "",     ""  },   /* decals read their targets in place at 720p too */
    { "PS3RECOMP_SSAO",            "0",   "2",    "0" },   /* the game's half-resolution AO */
    { "PS3RECOMP_SOFT_SHADOWS",    "0",   "2",    "0" },   /* the PS3's shadow filter */
    { "PS3RECOMP_SHADOW_RES",      "1",   "2",    "2" },   /* [shadow] the PS3's 1024x1024 sun-shadow maps (2x: ~50 MB more) */
    { "PS3RECOMP_FX_RES",          "1",   "full", "1" },   /* fire and smoke at the game's 512x288 */
    { "PS3RECOMP_ENG_TEXCACHE_MB", "512", "0",    "0" },   /* past 512 MB, textures not used for 5 s go */
    { "PS3RECOMP_VRAM_PRESSURE",   "1",   "0",    "0" },   /* near the budget the texture cache stops growing */
};
enum { LOWEND_N = (int)(sizeof kLowEnd / sizeof kLowEnd[0]) };
enum { LE_NONE = 0, LE_ENV, LE_FILE, LE_DEFAULTS };
static unsigned char s_le_src[LOWEND_N];   /* where the value at start came from */
static char s_le_def[LOWEND_N][24];        /* PS3_OVERLAY_DEFAULTS' value ("" = it names none) */
static int le_index(const char* k)
{
    for (int i = 0; i < LOWEND_N; i++) if (!strcmp(k, kLowEnd[i].var)) return i;
    return -1;
}

static void trim(char*& s)
{
    while (*s == ' ' || *s == '\t' || *s == '"') s++;
    for (char* e = s + strlen(s) - 1; e >= s && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t' || *e == '"'); e--) *e = 0;
}

static int var_index(const char* k)
{
    for (int i = 0; i < V_N; i++) if (!strcmp(k, kVar[i])) return i;
    return -1;
}

static void default_ini_path(char* out, size_t n)
{
    out[0] = 0;
#ifdef _WIN32
    const char* base = getenv("APPDATA");
    if (base && *base) snprintf(out, n, "%s\\inFAMOUS2-port\\settings.ini", base);
#else
    const char* x = getenv("XDG_CONFIG_HOME");
    const char* h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/infamous2-port/settings.ini", x);
    else if (h && *h) snprintf(out, n, "%s/.config/infamous2-port/settings.ini", h);
#endif
}

/* ---- [modsui] the start environment, for "Restart to apply" ----------------
 * Taken before this file's other start-up code changes the environment
 * (preload below exports the settings file's lines into it, the menu sets
 * variables as it applies choices): the game started again gets what this
 * one started with, or every setting changed in the menu would come back as
 * a variable "set when the game started" and win over the file. Same exe,
 * command line, environment and working directory, the same stdout/stderr
 * (a session log goes on); PS3_RESTARTED=<n> counts the restarts (20 in a
 * row at most) and PS3_RESTART_WAIT_PID makes the new game wait here, before
 * anything else, until the old one has ended. libs/system/cellSysutil.c calls
 * relaunch_self when the old one ends (ps3_host_exit_status), unless a
 * launcher answers the restart (PS3_LAUNCHER_RESTART=1, tools/play.ps1). */
#ifdef _WIN32
static std::vector<wchar_t> s_env0;   /* the environment block at start */
static std::wstring s_cwd0;
static int relaunch_self(void)
{
    static wchar_t exe[32768];
    const DWORD n = GetModuleFileNameW(NULL, exe, 32768);
    if (!n || n >= 32768 || s_env0.empty()) return 0;
    int count = 1;
    if (const char* r = getenv("PS3_RESTARTED")) count = atoi(r) + 1;
    if (count > 20) { fprintf(stderr, "[restart] 20 restarts in a row: not again\n"); return 0; }
    std::vector<std::wstring> vars;
    for (const wchar_t* p = s_env0.data(); *p; p += wcslen(p) + 1) {
        if (!_wcsnicmp(p, L"PS3_RESTART_WAIT_PID=", 21) || !_wcsnicmp(p, L"PS3_RESTARTED=", 14)) continue;
        vars.push_back(p);
    }
    vars.push_back(L"PS3_RESTART_WAIT_PID=" + std::to_wstring(GetCurrentProcessId()));
    vars.push_back(L"PS3_RESTARTED=" + std::to_wstring(count));
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> env;
    for (const std::wstring& v : vars) { env.insert(env.end(), v.begin(), v.end()); env.push_back(0); }
    env.push_back(0);
    std::vector<wchar_t> cmd(GetCommandLineW(), GetCommandLineW() + wcslen(GetCommandLineW()) + 1);
    /* the same stdout/stderr when they are files or pipes (a log); a console is shared anyway */
    HANDLE hs[3] = { GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
    bool redirected = false;
    HANDLE list[3];
    int nl = 0;
    for (HANDLE h : hs) {
        if (!h || h == INVALID_HANDLE_VALUE) continue;
        const DWORD t = GetFileType(h);
        if (t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE) redirected = true;
    }
    if (redirected)
        for (HANDLE& h : hs) {
            if (!h || h == INVALID_HANDLE_VALUE || !SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) { h = NULL; continue; }
            bool dup = false;
            for (int k = 0; k < nl; k++) if (list[k] == h) dup = true;
            if (!dup) list[nl++] = h;
        }
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    std::vector<unsigned char> attr;
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (nl) {
        SIZE_T sz = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
        attr.resize(sz);
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr.data();
        if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &sz) &&
            UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, sizeof(HANDLE) * (SIZE_T)nl, NULL, NULL)) {
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = hs[0]; si.StartupInfo.hStdOutput = hs[1]; si.StartupInfo.hStdError = hs[2];
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        } else { si.lpAttributeList = NULL; nl = 0; }
    }
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    const BOOL ok = CreateProcessW(exe, cmd.data(), NULL, NULL, nl ? TRUE : FALSE, flags, env.data(),
                                   s_cwd0.empty() ? NULL : s_cwd0.c_str(), &si.StartupInfo, &pi);
    if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) { fprintf(stderr, "[restart] could not start %ls (error %lu)\n", exe, GetLastError()); return 0; }
    fprintf(stderr, "[restart] started the game again as pid %lu (restart #%d)\n", (unsigned long)pi.dwProcessId, count);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}
static struct StartEnv {
    StartEnv()
    {
        if (const char* w = getenv("PS3_RESTART_WAIT_PID")) {   /* started by a restart: the old game ends first */
            const DWORD pid = (DWORD)strtoul(w, NULL, 10);
            HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
            const DWORD r = h ? WaitForSingleObject(h, 20000) : WAIT_OBJECT_0;
            if (h) CloseHandle(h);
            _putenv_s("PS3_RESTART_WAIT_PID", "");
            wchar_t cwd[1024];
            if (!GetCurrentDirectoryW(1024, cwd)) cwd[0] = 0;
            fprintf(stderr, "[restart] restart #%s: pid %lu, the old game (pid %lu) %s; cwd %ls\n", getenv("PS3_RESTARTED") ? getenv("PS3_RESTARTED") : "?",
                    (unsigned long)GetCurrentProcessId(), (unsigned long)pid, r == WAIT_OBJECT_0 ? "has ended" : "still runs after 20 s", cwd);
        }
        if (LPWCH e = GetEnvironmentStringsW()) {
            const wchar_t* b = e;
            const wchar_t* p = b;
            while (*p) p += wcslen(p) + 1;
            s_env0.assign(b, p + 1);
            FreeEnvironmentStringsW(e);
        }
        static wchar_t cwd[32768];
        const DWORD n = GetCurrentDirectoryW(32768, cwd);
        if (n && n < 32768) s_cwd0.assign(cwd, n);
        ps3_host_set_relauncher(relaunch_self);
    }
} s_start_env;
#endif

static void preload(void)
{
    if (overlay_off()) return;
    for (int i = 0; i < V_N; i++) s_env_set[i] = getenv(kVar[i]) != NULL;
    for (int i = 0; i < LOWEND_N; i++) s_le_src[i] = getenv(kLowEnd[i].var) ? LE_ENV : LE_NONE;   /* (lowend) */
    /* Headless runs never read the player's file. */
    if (!env_on("PS3RECOMP_VK_WINDOW")) return;
    const char* p = getenv("PS3_OVERLAY_INI");
    if (p) snprintf(s_ini_path, sizeof s_ini_path, "%s", p);   /* "" = none */
    else default_ini_path(s_ini_path, sizeof s_ini_path);
    int applied = 0, kept = 0;
    bool in_file[V_N] = {};
    if (s_ini_path[0]) {
        FILE* f = fopen(s_ini_path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                char* s = line;
                while (*s == ' ' || *s == '\t') s++;
                if (*s == '#' || *s == ';' || *s == '[' || !*s || *s == '\n' || *s == '\r') continue;
                char* eq = strchr(s, '=');
                if (!eq) continue;
                *eq = 0;
                char* k = s; char* v = eq + 1;
                for (char* e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
                trim(v);
                if (!*k) continue;
                { const int le = le_index(k); if (le >= 0 && s_le_src[le] == LE_NONE) s_le_src[le] = LE_FILE; }   /* (lowend) */
                const int i = var_index(k);
                if (i >= 0) {
                    snprintf(s_saved[i], sizeof s_saved[i], "%s", v);
                    in_file[i] = true;
                } else if (vk_overlay_cheats_owns_key(k)) {
                    /* the Cheats page writes its own lines (vk_overlay_cheats_write) */
                } else if (s_n_extra < EXTRA_N) {
                    snprintf(s_extra[s_n_extra][0], sizeof s_extra[0][0], "%s", k);
                    snprintf(s_extra[s_n_extra][1], sizeof s_extra[0][1], "%s", v);
                    s_n_extra++;
                }
                if (getenv(k)) { kept++; continue; }
                ps3_setting_setenv(k, v);
                applied++;
            }
            fclose(f);
        }
    }
    /* PS3_OVERLAY_DEFAULTS="K=V;K=V": a launcher's defaults, below the file. */
    if (const char* d = getenv("PS3_OVERLAY_DEFAULTS")) {
        char buf[1024];
        int n = 0;
        snprintf(buf, sizeof buf, "%s", d);
        for (char* tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
            char* eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            char* k = tok; char* v = eq + 1;
            trim(k); trim(v);
            const int i = var_index(k);
            { const int le = le_index(k);                                          /* (lowend) */
              if (le >= 0) {
                  snprintf(s_le_def[le], sizeof s_le_def[le], "%s", v);
                  if (s_le_src[le] == LE_NONE) s_le_src[le] = LE_DEFAULTS;
              } }
            if (!*k || getenv(k) || (i >= 0 && in_file[i])) continue;
            ps3_setting_setenv(k, v);
            n++;
        }
        fprintf(stderr, "[overlay] PS3_OVERLAY_DEFAULTS: %d default(s) applied\n", n);
    }
    for (int i = 0; i < V_N; i++) {
        const char* e = getenv(kVar[i]);
        snprintf(s_start[i], sizeof s_start[i], "%s", e ? e : "");
    }
    if (s_ini_path[0])
        fprintf(stderr, "[overlay] %s: %d setting(s) applied, %d left to the environment\n",
                s_ini_path, applied, kept);
}
static struct Preload { Preload() { preload(); } } s_preload_run;

static void set_value(int v, const char* fmt, ...)
{
    char buf[48];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> g(s_set_lock);
    if (strcmp(s_value[v], buf)) {
        snprintf(s_value[v], sizeof s_value[v], "%s", buf);
        s_dirty = true;
        s_dirty_t = now_s();
    }
}

/* A setting that is not in kVar (a page with its own list, e.g. the Lighting
 * page, vk_overlay_light.inc): kept with the file's other lines and written
 * at the next save. */
static void extra_set(const char* name, const char* value)
{
    std::lock_guard<std::mutex> g(s_set_lock);
    int i = 0;
    while (i < s_n_extra && strcmp(s_extra[i][0], name)) i++;
    if (i == s_n_extra) {
        if (s_n_extra >= EXTRA_N) return;
        snprintf(s_extra[i][0], sizeof s_extra[i][0], "%s", name);
        s_extra[i][1][0] = 0;
        s_n_extra++;
    }
    if (strcmp(s_extra[i][1], value)) {
        snprintf(s_extra[i][1], sizeof s_extra[i][1], "%s", value);
        s_dirty = true;
        s_dirty_t = now_s();
    }
}

/* A setting a module may take live (ps3_settings.h): environment, applier,
 * and the file. 1 = live now. */
/* [theme] the menu's look (vk_overlay_theme.h: Hero blue / Infamous red). A new
 * one is put in at the start of the next frame, never inside one: a colour
 * pushed before the switch would be popped back to the old look. -1 = none. */
static volatile int s_theme_want = -1;
static int apply_setting(int v, const char* value)
{
    set_value(v, "%s", value);
    if (v == V_THEME) s_theme_want = vk_ov_theme_parse(value);   /* [theme] the look is the overlay's own */
    return ps3_setting_apply(kVar[v], value);
}

static void mkdirs_for(const char* path)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    for (char* q = dir + 1; *q; q++) {
        if (*q != '/' && *q != '\\') continue;
        const char c = *q; *q = 0;
        if (!(q == dir + 2 && dir[1] == ':')) {   /* not "C:" itself */
#ifdef _WIN32
            CreateDirectoryA(dir, NULL);
#else
            mkdir(dir, 0755);
#endif
        }
        *q = c;
    }
}

/* Write the file: what it had plus what the menu changed. 1 = written. */
static int settings_save(void)
{
    if (!s_ini_path[0]) return 0;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_ini_path);
    mkdirs_for(s_ini_path);
    FILE* f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "# inFAMOUS 2 port -- settings of the in-game menu (F2, or hold L3+R3).\n"
               "# Same names and values as the environment variables; a variable that is\n"
               "# set when the game starts wins over its line here.\n");
    {
        std::lock_guard<std::mutex> g(s_set_lock);
        for (int i = 0; i < V_N; i++) {
            const char* v = s_value[i][0] ? s_value[i] : s_saved[i];
            if (*v) fprintf(f, "%s=%s\n", kVar[i], v);
            if (s_value[i][0]) snprintf(s_saved[i], sizeof s_saved[i], "%s", s_value[i]);
        }
        for (int i = 0; i < s_n_extra; i++) fprintf(f, "%s=%s\n", s_extra[i][0], s_extra[i][1]);
        if (vk_overlay_cheats_count() > 0) vk_overlay_cheats_write(f);
        s_dirty = false;
    }
    bool ok = fclose(f) == 0;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, s_ini_path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileA(tmp);
#else
    ok = ok && rename(tmp, s_ini_path) == 0;
    if (!ok) remove(tmp);
#endif
    return ok ? 1 : 0;
}

/* ---- state ---------------------------------------------------------------- */
static vk_overlay_attach_info s_vk;
static bool s_attached, s_enabled, s_imgui_ready, s_sdl_ready, s_vk_ready, s_failed;
static volatile int s_open;
static volatile int s_hud = 1;
static int   s_hud_corner = 0;            /* 0 top left, 1 top right, 2 bottom left, 3 bottom right */
static float s_hud_opacity = 0.55f;
static bool  s_hud_graph = true;
static bool  s_hud_cpu = true;
static int   s_tab, s_tab_want = -1;     /* the menu's page; one to switch to */
static int   s_ntabs = 5;                /* pages in draw_menu's list (L1/R1 wrap round it) */
static volatile int  s_quit_ask;         /* 1 open the confirmation, 2 showing */
static volatile bool s_quit_cancel;
static double s_t_attach;
static bool  s_ui_stale;                 /* a setting changed outside the menu: re-read its view */
static bool  s_pad_chord = true;
static bool  s_rel_mouse_saved;
static char  s_gpu_name[256];
static int   s_refresh_hz;

static VkFormat s_sc_format = VK_FORMAT_UNDEFINED;
static bool s_color_ok;

/* Vulkan entry points the overlay itself uses (ImGui loads its own). */
static PFN_vkCreateRenderPass    p_vkCreateRenderPass;
static PFN_vkDestroyRenderPass   p_vkDestroyRenderPass;
static PFN_vkCreateFramebuffer   p_vkCreateFramebuffer;
static PFN_vkDestroyFramebuffer  p_vkDestroyFramebuffer;
static PFN_vkCreateImageView     p_vkCreateImageView;
static PFN_vkDestroyImageView    p_vkDestroyImageView;
static PFN_vkCmdPipelineBarrier  p_vkCmdPipelineBarrier;
static PFN_vkCmdBeginRenderPass  p_vkCmdBeginRenderPass;
static PFN_vkCmdEndRenderPass    p_vkCmdEndRenderPass;
static PFN_vkDeviceWaitIdle      p_vkDeviceWaitIdle;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;

static VkRenderPass s_rp;
static VkFormat     s_rp_format = VK_FORMAT_UNDEFINED;
enum { SC_MAX = 8 };
static struct { VkImage img; VkImageView view; VkFramebuffer fb; VkExtent2D ext; } s_fb[SC_MAX];

/* Notices (any thread). */
struct Notice { char text[160]; double until; };
static Notice s_notice[3];
static std::mutex s_notice_lock;
static volatile double s_notice_until;

static void update_active(void)
{
    g_vk_overlay_active = (s_enabled && s_color_ok && !s_failed &&
                           (s_open || s_hud || s_notice_until > now_s())) ? 1 : 0;
}

/* [saves] a system dialog can be drawn (vk_overlay_saves.cpp waits for the player) */
extern "C" int vk_overlay_can_show(void) { return s_enabled && s_color_ok && !s_failed; }

static void notice_for(const char* text, double secs)
{
    if (!s_enabled || !text) return;
    {
        std::lock_guard<std::mutex> g(s_notice_lock);
        memmove(&s_notice[1], &s_notice[0], sizeof s_notice[0] * 2);
        snprintf(s_notice[0].text, sizeof s_notice[0].text, "%s", text);
        s_notice[0].until = now_s() + secs;
        if (s_notice[0].until > s_notice_until) s_notice_until = s_notice[0].until;
    }
    update_active();
}

extern "C" void vk_overlay_notice(const char* text) { notice_for(text, 3.0); }
extern "C" void vk_overlay_notice_for(const char* text, double secs) { notice_for(text, secs); }
/* Something outside this file's settings changed what the file holds (the Cheats page). */
extern "C" void vk_overlay_settings_changed(void)
{
    std::lock_guard<std::mutex> g(s_set_lock);
    s_dirty = true;
    s_dirty_t = now_s();
}
static void set_open(int o);
static void menu_escape(void);          /* [menu-e] Esc / Circle: a question's Cancel, Back, or close (vk_overlay_menu.inc) */
static void menu_closed(void);          /* [menu-e] set_open(0): no compare line, no sub-page */
/* [remap] F2 > Controls rebinding (vk_overlay_remap.inc) */
static bool remap_capturing(void);
static bool remap_escape(void);
static void remap_cancel(void);
static void remap_script(const char* spec);
/* Open the menu from outside (Esc did, before it became the game's START; F2 and
 * L3+R3 are handled here). 1 = opened (an overlay exists). */
extern "C" int vk_overlay_open_menu(void) { if (!s_enabled) return 0; set_open(1); return 1; }

/* ---- the menu opening and closing ------------------------------------------ */
extern "C" int vk_overlay_menu_open(void) { return s_enabled && s_open; }
static void set_open(int o)
{
    if (!s_enabled || (o != 0) == (s_open != 0)) return;
    if (!o && vk_overlay_saves_active()) return;   /* [saves] the save-data list closes itself */
    std::lock_guard<std::recursive_mutex> il(s_imgui_lock);
    s_open = o;
    g_pad_overlay_open = o;          /* cellPad: every port reads neutral while open */
    fprintf(stderr, "[overlay] menu %s\n", o ? "open" : "closed");   /* [menu-e] (test drivers wait for it) */
    if (o) {
        /* The cursor back for the menu, whatever the game had done with it. */
        s_rel_mouse_saved = SDL_GetRelativeMouseMode() == SDL_TRUE;
        if (s_rel_mouse_saved) SDL_SetRelativeMouseMode(SDL_FALSE);
        if (s_imgui_ready) {
            ImGuiIO& io = ImGui::GetIO();
            io.ClearInputKeys();
            io.ClearInputMouse();
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        }
    } else {
        s_quit_ask = 0;
        remap_cancel();                  /* [remap] a binding being changed: dropped */
        menu_closed();                   /* [menu-e] no compare line, no sub-page */
        if (s_imgui_ready) {
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            io.ClearInputKeys();
            io.ClearInputMouse();
        }
        if (s_sdl_ready) SDL_CaptureMouse(SDL_FALSE);
        if (s_rel_mouse_saved) SDL_SetRelativeMouseMode(SDL_TRUE);
        s_rel_mouse_saved = false;
        if (s_dirty) {
            if (settings_save()) vk_overlay_notice("Settings saved");
            else if (!s_ini_path[0]) vk_overlay_notice("Settings kept for this session (PS3_OVERLAY_INI is empty)");
            else vk_overlay_notice("Could not save the settings");
        }
    }
    update_active();
}

static void set_hud(bool on)
{
    s_hud = on ? 1 : 0;
    set_value(V_HUD, "%d", on ? 1 : 0);
    update_active();
}

/* [theme] the applier (so the menu marks the setting "live" and ps3_setting_apply takes it) */
static int theme_apply(const char* value) { s_theme_want = vk_ov_theme_parse(value); return 1; }

static void toggle_fullscreen(void)
{
    SDL_Window* w = (SDL_Window*)s_vk.sdl_window;
    const bool fs = w && vk_backend_window_full() != 0;
    if (!apply_setting(V_FULLSCREEN, fs ? "0" : "1"))
        vk_overlay_notice("Fullscreen: applies when the game restarts");
    s_ui_stale = true;
}

/* The menu's pages: the settings pages, then Cheats (always the last one) when the title
 * has any. A new settings page goes before it: TAB_CHEATS is the number of settings pages
 * ([light] Lighting = 5, [fx] Effects = 6). */
enum { TAB_MODS = 7, TAB_UPDATE = 8, TAB_CHEATS = 9, TAB_ABOUT = 10, MENU_TABS = TAB_CHEATS };   /* [mods] Mods, [update] Update before Cheats */
/* [about] About is the rail's last icon, after Cheats: s_tab counts along the rail, tab_page
 * names the page there (without cheats, About sits where Cheats would). */
static int tab_count(void) { return TAB_CHEATS + (vk_overlay_cheats_count() > 0 ? 1 : 0) + 1; }
static int tab_page(int t) { return t >= tab_count() - 1 ? TAB_ABOUT : t; }

static int tab_from_name(const char* e)
{
    static const char* const names[TAB_CHEATS + 1] = { "display", "graphics", "audio", "controls", "hud", "lighting", "effects", "mods", "update", "cheats" };
    if (!e || !*e) return -1;
    if (e[0] >= '0' && e[0] <= '9') { const int t = atoi(e); return t >= 0 && t < tab_count() ? t : 0; }
    if (!SDL_strcasecmp(e, "about")) return tab_count() - 1;   /* [about] */
    for (int i = 0; i < tab_count() - 1; i++) if (!SDL_strcasecmp(e, names[i])) return i;
    if (!SDL_strcasecmp(e, "performance") || !SDL_strcasecmp(e, "perf")) return 4;
    if (!SDL_strcasecmp(e, "language") || !SDL_strcasecmp(e, "lang")) return 2;   /* Audio holds Language */
    return -1;
}

static void lowend_preset(bool low);   /* (lowend) vk_overlay_lowend.inc */
static void menu_script(const char* w);   /* [menu-e] the timed script's menu verbs (vk_overlay_menu.inc) */
/* PS3RECOMP_OVERLAY_MENU_AT="40:display,50:audio,60:close" (tests). */
enum { SCRIPT_N = 16 };
static struct { double t; char what[64]; bool done; } s_script[SCRIPT_N];
static int s_n_script;
static void script_load(void)
{
    const char* e = getenv("PS3RECOMP_OVERLAY_MENU_AT");
    if (!e || !*e) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char* tok = strtok(buf, ",; "); tok && s_n_script < SCRIPT_N; tok = strtok(NULL, ",; ")) {
        char* c = strchr(tok, ':');
        s_script[s_n_script].t = atof(tok);
        snprintf(s_script[s_n_script].what, sizeof s_script[0].what, "%s", c ? c + 1 : "open");
        s_n_script++;
    }
}
static void script_poll(double now)
{
    for (int i = 0; i < s_n_script; i++) {
        if (s_script[i].done || now - s_t_attach < s_script[i].t) continue;
        s_script[i].done = true;
        const char* w = s_script[i].what;
        fprintf(stderr, "[overlay] script t=%.0f s: %s\n", s_script[i].t, w);
        if (!SDL_strcasecmp(w, "close")) set_open(0);
        else if (!SDL_strcasecmp(w, "hud0")) set_hud(false);
        else if (!SDL_strcasecmp(w, "hud1")) set_hud(true);
        else if (!SDL_strncasecmp(w, "set=", 4)) {   /* "set=VAR=value": as the menu would set it */
            char k[64]; snprintf(k, sizeof k, "%s", w + 4);
            char* eq = strchr(k, '=');
            const int v = eq ? (*eq = 0, var_index(k)) : -1;
            if (v == V_VOLUME) { cellAudio_host_set_volume(atoi(eq + 1)); set_value(v, "%d", atoi(eq + 1)); }
            else if (v >= 0) fprintf(stderr, "[overlay] %s=%s: %s\n", k, eq + 1, apply_setting(v, eq + 1) ? "live" : "at the next start");
            s_ui_stale = true;
        }
        else if (!SDL_strncasecmp(w, "cheat=", 6)) {   /* "cheat=id=value": as the Cheats page would set it */
            char k[64]; snprintf(k, sizeof k, "%s", w + 6);
            char* eq = strchr(k, '=');
            if (eq) { *eq = 0; ps3_overlay_cheat_set(k, atof(eq + 1)); }
        }
        else if (!SDL_strncasecmp(w, "bind=", 5)) remap_script(w + 5);   /* [remap] "bind=cross=key:F17", "bind=reset" */
        else if (!SDL_strncasecmp(w, "sub=", 4) || !SDL_strncasecmp(w, "focus=", 6) || !SDL_strncasecmp(w, "nav=", 4) ||
                 !SDL_strncasecmp(w, "split=", 6)) menu_script(w);            /* [menu-e] sub=remap|zip|notes|none, focus=<row id>, nav=<key>, split=<0..1> */
        else if (!SDL_strcasecmp(w, "preset=low")) lowend_preset(true);       /* (lowend) the Graphics page's buttons */
        else if (!SDL_strcasecmp(w, "preset=standard")) lowend_preset(false);
        else { const int t = tab_from_name(w); if (t >= 0) s_tab_want = t; set_open(1); }
    }
}

/* ---- events from the pump -------------------------------------------------- */
static std::mutex s_ev_lock;
static SDL_Event s_ev[128];
static int s_ev_n;
static void ev_push(const SDL_Event* e)
{
    std::lock_guard<std::mutex> g(s_ev_lock);
    if (s_ev_n >= 128) return;
    s_ev[s_ev_n] = *e;
    /* [menu-e] a key the window got without SDL's keyboard focus (posted to it, e.g. by a test driver
     * or a launcher) has windowID 0, which imgui_impl_sdl2 drops: it is this window's */
    if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) && !e->key.windowID && s_vk.sdl_window)
        s_ev[s_ev_n].key.windowID = SDL_GetWindowID((SDL_Window*)s_vk.sdl_window);
    s_ev_n++;
}

static bool lfx_compare_toggle(void);   /* [light] F3, vk_overlay_light.inc */

extern "C" int vk_overlay_event(const void* ev)
{
    if (!s_enabled || !ev) return 0;
    const SDL_Event* e = (const SDL_Event*)ev;
    switch (e->type) {
    case SDL_KEYDOWN: case SDL_KEYUP: {
        const SDL_Keycode k = e->key.keysym.sym;
        const bool press = e->type == SDL_KEYDOWN && !e->key.repeat;
        /* [remap] a binding waits for a key: every key goes to it (F1, F2, Esc too: it says
         * whose they are, Esc cancels) */
        if (s_open && remap_capturing()) { ev_push(e); return 1; }
        if (k == SDLK_F1) { if (press) { set_hud(!s_hud); if (!s_hud) vk_overlay_notice("Performance HUD off (F1 shows it)"); } return 1; }
        if (k == SDLK_F2) { if (press && !vk_overlay_saves_active()) set_open(!s_open); return 1; }
        if (k == SDLK_F3) {   /* [light] lighting + effects A/B; [menu-e] held over 0.35 s = a peek (back on release) */
            static double t_down;
            static bool flipped;
            if (press) { flipped = lfx_compare_toggle(); t_down = now_s(); }
            else if (e->type == SDL_KEYUP && flipped) { flipped = false; if (now_s() - t_down > 0.35) lfx_compare_toggle(); }
            return 1;
        }
        if (k == SDLK_F7) { if (press) vk_overlay_cheats_hotkey(); return 1; }   /* [spot] the Cheats page's quick key */
        if (k == SDLK_F11 || (k == SDLK_RETURN && (e->key.keysym.mod & KMOD_ALT))) {
            if (press) toggle_fullscreen();
            return 1;
        }
        if (!s_open) return 0;
        if (k == SDLK_ESCAPE) {
            if (press) { if (vk_overlay_saves_active()) vk_overlay_saves_back(); else menu_escape(); }   /* [menu-e] a question's Cancel, Back, or close */
            return 1;
        }
        ev_push(e);
        return 1;
    }
    case SDL_TEXTINPUT: case SDL_TEXTEDITING:
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
        if (!s_open) return 0;
        ev_push(e);
        return 1;
    case SDL_WINDOWEVENT:
        if (s_open) ev_push(e);      /* focus and leave events: the backend acts on them too */
        return 0;
    default:
        return 0;
    }
}

extern "C" void vk_overlay_poll(void)
{
    if (!s_enabled) return;
    const double now = now_s();
    if (g_pad_layout_note == 1) {   /* once, for a player who knew Esc as this menu's key */
        g_pad_layout_note = 2;
        notice_for("Esc is Pause now (it was Enter). F2 opens this port's menu.", 12.0);
    }
    if (s_n_script) script_poll(now);
    switch (vk_overlay_saves_poll()) {   /* [saves] the save-data list opens / closes the window */
    case 1: set_open(1); if (s_open) g_pad_overlay_open = 2; break;   /* 2: scripted input goes to it (cellPad) */
    case 2: set_open(0); break;
    default: break;
    }
    /* Hold L3+R3 for a second: port 0 as cellPad saw it before the menu's
     * neutral copy, or the two stick clicks on any controller (SDL buttons 7
     * and 8 in g_pad_overlay_raw, whatever they are bound to). cellPad keeps
     * L3 and R3 from the game while they are held together. */
    if (s_pad_chord) {
        static double t0;
        static bool fired;
        const uint32_t sticks = (1u << 7) | (1u << 8);
        const bool both = (g_pad_overlay_nav & (PAD_L3 | PAD_R3)) == (PAD_L3 | PAD_R3) ||
                          (g_pad_overlay_raw & sticks) == sticks;
        if (!both) { t0 = 0; fired = false; }
        else if (!t0) t0 = now;
        else if (!fired && now - t0 >= 1.0) { fired = true; set_open(!s_open); }
    }
}

/* ---- performance numbers ---------------------------------------------------- */
enum { FT_N = 240 };
static float  s_ft[FT_N];               /* ms between presents */
static int    s_ft_i, s_ft_count;
static double s_last_present;
struct Stats { double fps, avg_ms, max_ms, low1_fps; bool valid; };
static Stats  s_stats;

struct CpuSample { float proc_pct, cores, rsx_pct, top_pct; int busy, ncpu; bool valid; };
static std::mutex s_cpu_lock;
static CpuSample s_cpu;

#ifdef _WIN32
/* Per-thread CPU on a helper thread once a second (a thread snapshot of the
 * whole system can take a millisecond or two: not on the present thread). */
static volatile LONG s_cpu_want;
static HANDLE s_cpu_thread;
static DWORD  s_rsx_tid;
static unsigned long long ft64(const FILETIME& f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static DWORD WINAPI cpu_thread_main(LPVOID)
{
    enum { TN = 512 };
    struct Th { DWORD tid; HANDLE h; unsigned long long last; bool seen; };
    static Th th[TN];
    int n = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    const int ncpu = (int)si.dwNumberOfProcessors;
    const DWORD pid = GetCurrentProcessId();
    unsigned long long p_last = 0, wall_last = 0;
    int ticks = 0;
    for (;;) {
        Sleep(1000);
        if (!s_cpu_want) { p_last = 0; continue; }
        FILETIME c, x, k, u, nowf;
        GetSystemTimeAsFileTime(&nowf);
        const unsigned long long wall = ft64(nowf);
        if ((ticks++ % 5) == 0) {        /* new threads every 5 s */
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te; te.dwSize = sizeof te;
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != pid) continue;
                    bool have = false;
                    for (int i = 0; i < n; i++) if (th[i].tid == te.th32ThreadID) { have = true; break; }
                    if (have || n >= TN) continue;
                    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (!h) continue;
                    th[n].tid = te.th32ThreadID; th[n].h = h; th[n].seen = false;
                    th[n].last = (GetThreadTimes(h, &c, &x, &k, &u)) ? ft64(k) + ft64(u) : 0;
                    n++;
                }
                CloseHandle(snap);
            }
        }
        CpuSample cs = {};
        cs.ncpu = ncpu;
        if (GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u)) {
            const unsigned long long t = ft64(k) + ft64(u);
            if (p_last && wall > wall_last) {
                cs.cores = (float)((double)(t - p_last) / (double)(wall - wall_last));
                cs.proc_pct = cs.cores * 100.0f / (float)(ncpu > 0 ? ncpu : 1);
                cs.valid = true;
            }
            p_last = t;
        }
        const double span = wall > wall_last && wall_last ? (double)(wall - wall_last) : 0.0;
        for (int i = 0; i < n; ) {
            DWORD code = 0;
            if (!GetThreadTimes(th[i].h, &c, &x, &k, &u) || (GetExitCodeThread(th[i].h, &code) && code != STILL_ACTIVE)) {
                CloseHandle(th[i].h);
                th[i] = th[--n];
                continue;
            }
            const unsigned long long t = ft64(k) + ft64(u);
            if (span > 0 && th[i].seen) {
                const float pct = (float)((double)(t - th[i].last) * 100.0 / span);
                if (th[i].tid == s_rsx_tid) cs.rsx_pct = pct;
                else if (pct > cs.top_pct) cs.top_pct = pct;
                if (pct >= 80.0f) cs.busy++;
            }
            th[i].last = t; th[i].seen = true;
            i++;
        }
        wall_last = wall;
        std::lock_guard<std::mutex> g(s_cpu_lock);
        s_cpu = cs;
    }
}
#endif

static void cpu_want(bool on)
{
#ifdef _WIN32
    s_cpu_want = on ? 1 : 0;
    if (on && !s_cpu_thread) {
        s_cpu_thread = CreateThread(NULL, 64 * 1024, cpu_thread_main, NULL, 0, NULL);
        if (s_cpu_thread) SetThreadPriority(s_cpu_thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
#else
    (void)on;
#endif
}

static int cmp_float(const void* a, const void* b)
{
    const float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

static double s_fps_target = 60.0;      /* the cap, or 60 uncapped: the HUD's colours */
static double fps_target(void) { return s_fps_target; }
static void fps_target_read(void)
{
    const char* e = getenv("PS3RECOMP_FPS_CAP");
    const double f = e ? atof(e) : 0.0;
    s_fps_target = f > 0.0 ? f : 60.0;
}

static double s_stats_t0;               /* 0: start a new measuring window */
static void stats_tick(double now)
{
    double& t0 = s_stats_t0;
    static int frames;
    frames++;
    if (!t0) { t0 = now; frames = 0; return; }
    const double dt = now - t0;
    if (dt < 0.5) return;
    Stats st = {};
    st.fps = frames / dt;
    const int n = s_ft_count < FT_N ? s_ft_count : FT_N;
    double sum = 0, mx = 0; int k = 0;
    for (int i = 0; i < n && i < frames; i++) {
        const float v = s_ft[(s_ft_i - 1 - i + FT_N) % FT_N];
        sum += v; if (v > mx) mx = v; k++;
    }
    st.avg_ms = k ? sum / k : 0; st.max_ms = mx;
    /* 1% low over the whole ring (the last ~2-4 s): the 99th-percentile frame. */
    if (n >= 20) {
        float tmp[FT_N];
        for (int i = 0; i < n; i++) tmp[i] = s_ft[(s_ft_i - 1 - i + FT_N) % FT_N];
        qsort(tmp, (size_t)n, sizeof tmp[0], cmp_float);
        const float p99 = tmp[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1];
        st.low1_fps = p99 > 0.0f ? 1000.0 / p99 : 0.0;
    }
    st.valid = true;
    s_stats = st;
    t0 = now; frames = 0;
    fps_target_read();
#ifndef _WIN32
    { static double c0, w0;
      struct rusage ru; getrusage(RUSAGE_SELF, &ru);
      const double c = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
      if (w0 > 0 && now > w0) { std::lock_guard<std::mutex> g(s_cpu_lock); s_cpu.cores = (float)((c - c0) / (now - w0)); s_cpu.valid = true; }
      c0 = c; w0 = now; }
#endif
}


/* ---- ImGui ---------------------------------------------------------------------- */
static ImGuiStyle s_base_style;
static ImFont* s_font_semi_load;   /* [menu-e] imgui_setup's semibold face (vk_overlay_menu.inc s_font_semi) */
static float s_scale_applied = -1.0f;

static void check_vk(VkResult r)
{
    if (r < 0) fprintf(stderr, "[overlay] Vulkan error %d in the ImGui renderer\n", (int)r);
}

static PFN_vkVoidFunction load_vk(const char* name, void*)
{
    PFN_vkVoidFunction f = s_vk.get_instance_proc_addr(s_vk.instance, name);
    return f;
}

static bool load_own_vk(void)
{
#define OV_LOAD(n) if (!(p_##n = (PFN_##n)s_vk.get_instance_proc_addr(s_vk.instance, #n))) return false;
    OV_LOAD(vkCreateRenderPass) OV_LOAD(vkDestroyRenderPass)
    OV_LOAD(vkCreateFramebuffer) OV_LOAD(vkDestroyFramebuffer)
    OV_LOAD(vkCreateImageView) OV_LOAD(vkDestroyImageView)
    OV_LOAD(vkCmdPipelineBarrier) OV_LOAD(vkCmdBeginRenderPass) OV_LOAD(vkCmdEndRenderPass)
    OV_LOAD(vkDeviceWaitIdle) OV_LOAD(vkGetPhysicalDeviceProperties)
#undef OV_LOAD
    return true;
}

static void fb_destroy_all(void)
{
    for (int i = 0; i < SC_MAX; i++) {
        if (s_fb[i].fb) p_vkDestroyFramebuffer(s_vk.device, s_fb[i].fb, NULL);
        if (s_fb[i].view) p_vkDestroyImageView(s_vk.device, s_fb[i].view, NULL);
        s_fb[i].fb = VK_NULL_HANDLE; s_fb[i].view = VK_NULL_HANDLE; s_fb[i].img = VK_NULL_HANDLE;
    }
}

static bool render_pass_create(VkFormat fmt)
{
    VkAttachmentDescription att = {};
    att.format = fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;          /* the game's frame is in it */
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = 0;
    VkRenderPassCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1; ci.pAttachments = &att;
    ci.subpassCount = 1; ci.pSubpasses = &sub;
    ci.dependencyCount = 2; ci.pDependencies = dep;
    if (p_vkCreateRenderPass(s_vk.device, &ci, NULL, &s_rp) != VK_SUCCESS) { s_rp = VK_NULL_HANDLE; return false; }
    s_rp_format = fmt;
    return true;
}

static bool vk_backend_init(void)
{
    static bool loaded;
    if (!loaded && !(loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, load_vk, NULL))) {
        fprintf(stderr, "[overlay] Vulkan entry points for ImGui missing\n");
        return false;
    }
    if (!render_pass_create(s_sc_format)) { fprintf(stderr, "[overlay] render pass creation failed\n"); return false; }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_0;
    ii.Instance = s_vk.instance;
    ii.PhysicalDevice = s_vk.phys;
    ii.Device = s_vk.device;
    ii.QueueFamily = s_vk.queue_family;
    ii.Queue = s_vk.queue;
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE * 8;   /* [saves] + up to 32 save icons */
    ii.MinImageCount = 2;
    ii.ImageCount = 4;                 /* vertex buffers rotated: > frames in flight */
    ii.PipelineInfoMain.RenderPass = s_rp;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&ii)) { fprintf(stderr, "[overlay] ImGui_ImplVulkan_Init failed\n"); return false; }
    s_vk_ready = true;
    return true;
}

static void vk_backend_shutdown(void)
{
    if (s_vk_ready) { ImGui_ImplVulkan_Shutdown(); s_vk_ready = false; }
    if (s_rp) { p_vkDestroyRenderPass(s_vk.device, s_rp, NULL); s_rp = VK_NULL_HANDLE; }
    s_rp_format = VK_FORMAT_UNDEFINED;
}

static bool file_exists(const char* p)
{
    FILE* f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool imgui_setup(void)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;     /* the menu keeps its own file */
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (!s_open) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   /* the cursor is the game's */
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImFont* font = NULL;
#ifdef _WIN32
    { char win[MAX_PATH] = "C:\\Windows";
      GetWindowsDirectoryA(win, sizeof win);
      static const char* const fonts[] = { "segoeui.ttf", "arial.ttf", "verdana.ttf" };
      for (const char* f : fonts) {
          char p[MAX_PATH + 32];
          snprintf(p, sizeof p, "%s\\Fonts\\%s", win, f);
          if (file_exists(p) && (font = io.Fonts->AddFontFromFileTTF(p, 17.0f)) != NULL) break;
      } }
#else
    { static const char* const fonts[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                           "/usr/share/fonts/TTF/DejaVuSans.ttf" };
      for (const char* f : fonts) if (file_exists(f) && (font = io.Fonts->AddFontFromFileTTF(f, 17.0f)) != NULL) break; }
#endif
    if (!font) io.Fonts->AddFontDefaultVector();
    /* the bold face for headings, titles and tabs (vk_overlay_theme): the PC's own, never a game asset */
    { ImFont* bold = NULL;
#ifdef _WIN32
      char win[MAX_PATH] = "C:\\Windows";
      GetWindowsDirectoryA(win, sizeof win);
      static const char* const bolds[] = { "segoeuib.ttf", "arialbd.ttf" };
      for (const char* f : bolds) {
          char p[MAX_PATH + 32];
          snprintf(p, sizeof p, "%s\\Fonts\\%s", win, f);
          if (file_exists(p) && (bold = io.Fonts->AddFontFromFileTTF(p, 17.0f)) != NULL) break;
      }
#else
      static const char* const bolds[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                           "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf" };
      for (const char* f : bolds) if (file_exists(f) && (bold = io.Fonts->AddFontFromFileTTF(f, 17.0f)) != NULL) break;
#endif
      vk_ov_theme_set_bold(bold); }
    /* [menu-e] the semibold face for the menu's values and chips (Segoe UI Semibold; else the bold one) */
    { ImFont* semi = NULL;
#ifdef _WIN32
      char win[MAX_PATH] = "C:\\Windows";
      GetWindowsDirectoryA(win, sizeof win);
      char p[MAX_PATH + 32];
      snprintf(p, sizeof p, "%s\\Fonts\\seguisb.ttf", win);
      if (file_exists(p)) semi = io.Fonts->AddFontFromFileTTF(p, 17.0f);
#endif
      s_font_semi_load = semi; }

    ImGuiStyle& st = ImGui::GetStyle();
    vk_ov_theme_style(st);
    s_base_style = st;
    s_imgui_ready = true;
    return true;
}


/* The UI is laid out for 720 lines: everything scales with the window. */
static float s_scale = 1.0f;
static float S(float v) { return v * s_scale; }
static void apply_scale(float h)
{
    float k = h / 720.0f;
    k = k < 0.5f ? 0.5f : k > 4.0f ? 4.0f : k;
    if (fabsf(k - s_scale_applied) < 0.01f) return;
    ImGuiStyle& st = ImGui::GetStyle();
    st = s_base_style;
    st.ScaleAllSizes(k);
    st.FontScaleDpi = k;
    s_scale_applied = k;
    s_scale = k;
}

static bool sdl_setup(void)
{
    if (s_sdl_ready) return true;
    if (!ImGui_ImplSDL2_InitForVulkan((SDL_Window*)s_vk.sdl_window)) return false;
    /* Controllers come from cellPad (g_pad_overlay_nav), with its mapping and
     * its port order; the backend's own gamepad reading stays off. */
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, NULL, 0);
    s_sdl_ready = true;
    return true;
}

/* ---- the menu's view of the settings, read when it opens -------------------- */
static const int kCap[] = { 30, 60, 72, 120, 144, 0 };
static const char* const kPresent[] = { "fifo", "mailbox", "immediate" };
/* [res] PS3RECOMP_RENDER_SCALE (rsx_vulkan_backend.c's [res] block) */
static const char* const kScale[] = { "1", "1.5", "2", "3" };
static const char* const kAniso[] = { "1", "2", "4", "8", "16" };
/* [wide] PS3RECOMP_ASPECT (vk_postfx.c ps3_aspect_wanted, docs/WIDESCREEN.md) */
static const char* const kAspect[] = { "16:9", "auto", "21:9", "32:9", "16:10" };
static const char* const kUp[] = { "fsr", "cas", "bilinear" };   /* VK_POSTFX_UP_* order */
/* (dofov) PS3RECOMP_DOF (rsx_draw_engine.c): the game's depth of field */
static const char* const kDof[] = { "1", "play", "0" };
/* (dofov) PS3_FOV: the field of view, percent of the game's own (the title's cheats.cpp; 100 = the game's) */
enum { kFovMin = 70, kFovMax = 130 };
static const double kFovGameV = 0.9;   /* the free camera's vertical field of view in play, rad (NOTES.md) */
/* (The rows' short names, as the menu shows them, are in vk_overlay_menu.inc [menu-e].) */

struct Ui {
    int cap, cap_custom;      /* index in kCap; -1 = cap_custom fps */
    int present;
    bool fullscreen;
    bool capture;               // PS3RECOMP_VK_CAPTURE_FRIENDLY (GitHub #51)
    int aa, scale, aniso, upscale; float sharpen;
    int dof;                  /* (dofov) the game's depth of field: index in kDof (rsx_draw_engine.c ENG_DOF_BLEND_FP) */
    bool edge_blur;           /* the game's own edge blur (the same pass) */
    int fov;                  /* (dofov) PS3_FOV: percent of the game's field of view (kFovMin..kFovMax) */
    int mesh_trim;            /* [trim]/[edgevp] 0 = the game's culling at the render resolution,
                               * 1 = off (whole meshes; the default: tools/spu_patches.txt), 2 = the PS3's 720p culling */
    int aspect;               /* [wide] index in kAspect; -1 = another value (env / file) */
    int volume;
    int input_mode, button_icons;
    bool rumble; int rumble_strength;
};
static Ui s_ui;

static int pick(const char* v, const char* const* list, int n, int def)
{
    if (!v || !*v) return def;
    for (int i = 0; i < n; i++) if (!SDL_strcasecmp(v, list[i])) return i;
    return def;
}

static void lang_ui_load(void);   /* the Language rows' lists (below; Audio page, vk_overlay_menu.inc) */
static void ui_load(void)
{
    lang_ui_load();
    { const char* e = getenv(kVar[V_FPS_CAP]);
      const int f = e ? atoi(e) : 0;
      s_ui.cap = -1; s_ui.cap_custom = f;
      for (int i = 0; i < 6; i++) if (kCap[i] == f) s_ui.cap = i; }
    s_ui.present = pick(getenv(kVar[V_PRESENT]), kPresent, 3, 1);
    { SDL_Window* w = (SDL_Window*)s_vk.sdl_window;
      s_ui.fullscreen = w && vk_backend_window_full() != 0;
      s_ui.capture = env_on(kVar[V_CAPTURE]); }
    /* image quality: the values in effect (vk_postfx.c owns the defaults) */
    { const int a = vk_postfx_aa();
      s_ui.aa = a == VK_POSTFX_AA_SMAA ? 2 : a == VK_POSTFX_AA_FXAA ? 1 : 0; }
    s_ui.sharpen = vk_postfx_sharpen();
    s_ui.upscale = vk_postfx_upscale();
    { const char* e = getenv(kVar[V_DOF]);   /* (dofov) 1/unset on, play = off in gameplay, 0 off */
      s_ui.dof = !e || !*e ? 0 : !strcmp(e, "0") ? 2 : (!strcmp(e, "play") || !strcmp(e, "gameplay") || !strcmp(e, "2")) ? 1 : 0; }
    { const char* e = getenv(kVar[V_FOV]);   /* (dofov) */
      int f = e && *e ? atoi(e) : 100;
      s_ui.fov = f < kFovMin ? kFovMin : f > kFovMax ? kFovMax : f; }
    { const char* e = getenv(kVar[V_EDGE_BLUR]); s_ui.edge_blur = !(e && !strcmp(e, "0")); }
    { const int t = spu_patch_find("mesh_trim");   /* [trim] what the SPU code runs now */
      const bool culls = t < 0 || spu_patch_alt(t) == 0;
      const char* e = getenv(kVar[V_EDGE_VP]);       /* [edgevp] culling at the render resolution */
      s_ui.mesh_trim = !culls ? 1 : (e && e[0] == '0') ? 2 : 0; }
    { const char* e = getenv(kVar[V_RENDER_SCALE]);   /* [res] numbers or 720p/1080p/1440p/4k */
      double v = e ? atof(e) : 1.0;
      if (e && !SDL_strcasecmp(e, "1080p")) v = 1.5;
      else if (e && !SDL_strcasecmp(e, "1440p")) v = 2.0;
      else if (e && (!SDL_strcasecmp(e, "4k") || !SDL_strcasecmp(e, "2160p"))) v = 3.0;
      s_ui.scale = v >= 2.5 ? 3 : v >= 1.75 ? 2 : v >= 1.25 ? 1 : 0; }
    { const int v = vk_postfx_aniso();
      s_ui.aniso = v >= 16 ? 4 : v >= 8 ? 3 : v >= 4 ? 2 : v >= 2 ? 1 : 0; }
    { const char* e = getenv(kVar[V_ASPECT]);   /* [wide] what the next start reads */
      s_ui.aspect = (!e || !*e) ? 0 : pick(e, kAspect, 5, -1); }
    s_ui.volume = cellAudio_host_volume();
    s_ui.input_mode = cellPad_host_input_mode();
    s_ui.button_icons = ps3_button_icons_mode_setting();
    const int r = cellPad_host_rumble_strength();
    s_ui.rumble = r > 0;
    if (r > 0) s_ui.rumble_strength = r; else if (!s_ui.rumble_strength) s_ui.rumble_strength = 100;
}

static const ImVec4 kOrange(1.0f, 0.60f, 0.20f, 1.0f);
static const ImVec4 kGrey(0.62f, 0.66f, 0.72f, 1.0f);

/* "env" after a setting whose variable was set at start: it wins next time.
 * live = false: a setting the game only reads at start (nothing applies now). */
static void env_mark(int v, bool live = true)
{
    if (!s_env_set[v]) return;
    ImGui::SameLine();
    ImGui::TextColored(kOrange, "env");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(live ? "%s was set when the game started (the launcher or the shell).\n"
                                 "The change applies now, but that variable wins again next time."
                               : "%s was set when the game started (the launcher or the shell).\n"
                                 "The choice is saved, but that variable wins over it at the next start.", kVar[v]);
}

static void hint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void section(const char* text) { vk_ov_heading(text); }   /* [theme] bold, accent, a fading rule */

/* "on restart", or "restart needed" once changed, after a setting read at start. */
static void restart_mark(int v, bool changed)
{
    ImGui::SameLine();
    ImGui::TextColored(changed ? kOrange : kGrey, changed ? "restart needed" : "on restart");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s is read when the game starts: a change applies the next time.", kVar[v]);
}

/* "live" or "restart" after a setting a module may or may not take live. */
static void live_mark(int v)
{
    const bool live = ps3_setting_applier(kVar[v]) != NULL;
    const char* now = getenv(kVar[v]);
    const bool changed = strcmp(now ? now : "", s_start[v]) != 0;
    if (live) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "live");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s changes while the game runs.", kVar[v]);
    } else {
        restart_mark(v, changed);
    }
    env_mark(v);
}

static float combo_w(void) { return S(330.0f); }

#include "vk_overlay_aa.inc"         /* [aa] aa_choice_*, thin_lines_* */
/* ---- Language (on the Audio page) -------------------------------------------
 * PS3_LANG is the console language the game is told (libs/system/cellSysutil.c;
 * unset or "auto" = the Windows display language, English when the game has
 * none of it). PS3_VOICE_LANG / PS3_TEXT_LANG split the dub from what is read
 * on screen (runtime/ppu/ppu_fs_lang.h serves that language's files; "same" =
 * follow the game). The game reads all three once, at start, so the menu only
 * saves them (set_value): the environment is left alone and nothing changes
 * halfway through this session. The lists are what this disc has
 * (cache/psarc: <name>_text.psarc, and <name>.psarc beside it for a dub). */
#ifdef _WIN32
static const char* const kHostOs = "Windows";
#else
static const char* const kHostOs = "system";
#endif
/* The game's languages: the files' spelling (ppu_fs_lang.h), the PS3_LANG code
 * the menu writes for one (cellSysutil.c), the label, and a short name for
 * sentences ("no Italian dub"). Endonyms only in scripts the menu's font has
 * (Segoe UI: Latin and Cyrillic). */
static const struct { const char* name; const char* code; const char* label; const char* brief; } kLang[] = {
    { "english",    "en",     "English (US)",                              "English" },
    { "british",    "en-gb",  "English (UK)",                              "UK English" },
    { "french",     "fr",     "French (Fran\u00e7ais)",                    "French" },
    { "german",     "de",     "German (Deutsch)",                          "German" },
    { "italian",    "it",     "Italian (Italiano)",                        "Italian" },
    { "spanish",    "es-es",  "Spanish, Spain (Espa\u00f1ol)",             "Spanish (Spain)" },
    { "latino",     "es-419", "Spanish, Latin America (Espa\u00f1ol)",     "Latin-American Spanish" },
    { "portuguese", "pt-pt",  "Portuguese, Portugal (Portugu\u00eas)",     "Portuguese (Portugal)" },
    { "brazilian",  "pt-br",  "Portuguese, Brazil (Portugu\u00eas)",       "Brazilian Portuguese" },
    { "dutch",      "nl",     "Dutch (Nederlands)",                        "Dutch" },
    { "polish",     "pl",     "Polish (Polski)",                           "Polish" },
    { "russian",    "ru",     "Russian (\u0420\u0443\u0441\u0441\u043a\u0438\u0439)", "Russian" },
    { "turkish",    "tr",     "Turkish (T\u00fcrk\u00e7e)",                "Turkish" },
    { "finnish",    "fi",     "Finnish (Suomi)",                           "Finnish" },
    { "swedish",    "sv",     "Swedish (Svenska)",                         "Swedish" },
    { "danish",     "da",     "Danish (Dansk)",                            "Danish" },
    { "norwegian",  "no",     "Norwegian (Norsk)",                         "Norwegian" },
    { "japanese",   "ja",     "Japanese",                                  "Japanese" },
    { "korean",     "ko",     "Korean",                                    "Korean" },
    { "chinese",    "zh-t",   "Chinese, traditional",                      "Chinese (traditional)" },
    { "chinese_s",  "zh-s",   "Chinese, simplified",                       "Chinese (simplified)" },
};

struct LangUi {
    char voice[32][24], text[32][24];   /* this disc's dubs and text languages */
    int  nvoice, ntext;
    char host[8];                       /* the PS3_LANG code "Automatic" gives ("" = none: English) */
    char win[64];                       /* the display language's name */
};
static LangUi s_lang;

static const char* lang_label(const char* name)
{
    for (const auto& k : kLang) if (!SDL_strcasecmp(k.name, name)) return k.label;
    return name;
}
static const char* lang_brief(const char* name)
{
    for (const auto& k : kLang) if (!SDL_strcasecmp(k.name, name)) return k.brief;
    return name;
}

/* A CELL_SYSUTIL_LANG_* number (es and pt: the disc's own Spanish / Portuguese). */
static const char* lang_id_name(int id)
{
    static const char* const ids[20] = { "japanese", "english", "french", NULL, "german", "italian", "dutch", NULL,
                                         "russian", "korean", "chinese", "chinese_s", "finnish", "swedish", "danish",
                                         "norwegian", "polish", "brazilian", "british", "turkish" };
    return id >= 0 && id < 20 ? ids[id] : NULL;
}
static const char* lang_id_label(int id)
{
    if (id == 3) return "Spanish";
    if (id == 7) return "Portuguese";
    const char* n = lang_id_name(id);
    return n ? lang_label(n) : "?";
}

/* The one file-name language a PS3_LANG code names in the table, else NULL. */
static const char* lang_code_name(const char* code)
{
    for (const auto& k : kLang) if (!SDL_strcasecmp(code, k.code)) return k.name;
    return NULL;
}

/* A PS3_LANG value (a code from the list, another code cellSysutil takes, or a number). */
static const char* lang_code_label(const char* code)
{
    for (const auto& k : kLang) if (!SDL_strcasecmp(code, k.code)) return k.label;
    if (!SDL_strcasecmp(code, "en-us")) return lang_label("english");
    if (!SDL_strcasecmp(code, "es")) return lang_id_label(3);
    if (!SDL_strcasecmp(code, "pt")) return lang_id_label(7);
    char* end = NULL;
    const long n = strtol(code, &end, 10);
    if (end && end != code && !*end) return lang_id_label((int)n);
    return code;
}

/* "" and the words that mean "not chosen" ("auto" for PS3_LANG, "same" for the other two). */
static bool lang_unset(int v, const char* s)
{
    if (!*s) return true;
    if (v == V_LANG) return !SDL_strcasecmp(s, "auto") || !SDL_strcasecmp(s, "system");
    return !SDL_strcasecmp(s, "same") || !SDL_strcasecmp(s, "auto") || !SDL_strcasecmp(s, "default");
}
static bool lang_same(int v, const char* a, const char* b)
{
    const bool ua = lang_unset(v, a), ub = lang_unset(v, b);
    return (ua || ub) ? ua == ub : !SDL_strcasecmp(a, b);
}

/* The menu's choice: this session's, else the one in effect at start. */
static void lang_choice(int v, char* out, size_t n)
{
    std::lock_guard<std::mutex> g(s_set_lock);
    snprintf(out, n, "%s", s_value[v][0] ? s_value[v] : s_start[v]);
}
static bool lang_changed(int v)
{
    char cur[48];
    lang_choice(v, cur, sizeof cur);
    return !lang_same(v, cur, s_start[v]);
}
static void lang_set(int v, const char* value)
{
    char cur[48];
    lang_choice(v, cur, sizeof cur);
    if (!lang_same(v, cur, value)) set_value(v, "%s", value);
}

static bool lang_has(char (*list)[24], int n, const char* name)
{
    for (int i = 0; i < n; i++) if (!SDL_strcasecmp(list[i], name)) return true;
    return false;
}

/* When the menu opens (ui_load). The disc's lists are read until found (a disc
 * not laid out as <root>/PS3_GAME/USRDIR is found once the game opened its
 * voice archive) and then kept: a listing on a slow drive is not repeated. */
static void lang_ui_load(void)
{
    if (!s_lang.ntext) {
        s_lang.nvoice = ps3_lang_available(0, s_lang.voice, 32);
        s_lang.ntext = ps3_lang_available(1, s_lang.text, 32);
    }
    if (s_lang.win[0]) return;
    ps3_sysutil_host_language(s_lang.host, sizeof s_lang.host);
#ifdef _WIN32
    if (!GetLocaleInfoA(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), LOCALE_SENGLISHLANGUAGENAME,
                        s_lang.win, (int)sizeof s_lang.win))
        s_lang.win[0] = 0;
#endif
    if (!s_lang.win[0])
        snprintf(s_lang.win, sizeof s_lang.win, "%s", s_lang.host[0] ? lang_code_label(s_lang.host) : "unknown");
}

/* The dub this disc plays for a language it has no dub of (ppu_fs_lang.h's
 * fallback: english, else its first), NULL when it has that one or no list. */
static const char* lang_dub_fallback(const char* name)
{
    if (!name || !s_lang.nvoice || lang_has(s_lang.voice, s_lang.nvoice, name)) return NULL;
    return lang_has(s_lang.voice, s_lang.nvoice, "english") ? "english" : s_lang.voice[0];
}

static char   s_pads[4][128];
static int    s_n_pads;
static double s_pads_t;
static void pads_refresh(double now)
{
    if (now - s_pads_t < 1.0 && s_pads_t > 0) return;
    s_pads_t = now;
    /* cellPad's list of the controllers on its ports: asking SDL here takes
     * its joystick lock, which a controller being opened holds for as long
     * as the open takes (this is the thread that presents). */
    s_n_pads = cellPad_host_controller_names(s_pads, 4);
}

#include "vk_overlay_remap.inc"   /* [remap] the bindings' rules and dialogs, rebinding live (issue #6); the grid: vk_overlay_menu.inc */
#include "vk_overlay_mouse.inc"   /* [mouse] cellPad's mouse camera setters (the rows: vk_overlay_menu.inc) */

static void stats_lines(bool compact)
{
    const Stats& st = s_stats;
    if (!st.valid) { ImGui::TextUnformatted("measuring..."); return; }
    const double target = fps_target();
    const ImVec4 col = st.fps >= target * 0.95 ? ImVec4(0.45f, 0.90f, 0.45f, 1.0f)
                     : st.fps >= target * 0.75 ? ImVec4(0.95f, 0.85f, 0.30f, 1.0f)
                                               : ImVec4(1.0f, 0.40f, 0.35f, 1.0f);
    ImGui::PushFont(NULL, compact ? 22.0f : 24.0f);
    ImGui::TextColored(col, "%.0f", st.fps);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("FPS");
    ImGui::Text("%.1f ms", st.avg_ms);
    ImGui::EndGroup();
    if (s_hud_graph || !compact) {
        float buf[FT_N]; const int n = s_ft_count < FT_N ? s_ft_count : FT_N;
        for (int i = 0; i < n; i++) buf[i] = s_ft[(s_ft_i - n + i + FT_N) % FT_N];
        const float top = st.max_ms > 50.0 ? (float)st.max_ms : 50.0f;
        const ImVec2 size(compact ? S(210.0f) : S(420.0f), compact ? S(38.0f) : S(70.0f));
        ImGui::PlotLines("##ft", buf, n, 0, NULL, 0.0f, top, size);
        /* the target frame time as a faint line */
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float y = b.y - (float)(1000.0 / target) / top * (b.y - a.y);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 60), 1.0f);
    }
    if (st.low1_fps > 0) ImGui::Text("1%% low %.0f   max %.1f ms", st.low1_fps, st.max_ms);
    else ImGui::Text("max %.1f ms", st.max_ms);
    if (s_hud_cpu || !compact) {
        CpuSample cs;
        { std::lock_guard<std::mutex> g(s_cpu_lock); cs = s_cpu; }
        if (cs.valid) {
#ifdef _WIN32
            ImGui::Text("CPU %.0f%%  RSX %.0f%%  top %.0f%%", cs.proc_pct, cs.rsx_pct, cs.top_pct);
            if (!compact) ImGui::Text("%.1f of %d logical cores in use, %d thread(s) at 80%% or more", cs.cores, cs.ncpu, cs.busy);
#else
            ImGui::Text("CPU %.1f cores", cs.cores);
#endif
        }
    }
}

/* ---- Effects page (fx): effects resolution (texture packs: the Mods page) --
 * A page of its own. Its settings are not in the V_ table: fx_set applies
 * one live (ps3_setting_apply: rsx_draw_engine.c registers the appliers) and
 * keeps it as one of the file's other lines, which preload() exports at the
 * next start like any other. */
extern "C" const char* rsx_texpack_dir(void);
extern "C" void rsx_texpack_status(unsigned* packs, unsigned* files, unsigned* applied, int* dumping);

static void fx_set(const char* name, const char* value)
{
    ps3_setting_apply(name, value);
    std::lock_guard<std::mutex> g(s_set_lock);
    int i = 0;
    while (i < s_n_extra && strcmp(s_extra[i][0], name)) i++;
    if (i == s_n_extra) {
        if (s_n_extra >= EXTRA_N) return;
        snprintf(s_extra[i][0], sizeof s_extra[i][0], "%s", name);
        s_n_extra++;
    }
    snprintf(s_extra[i][1], sizeof s_extra[i][1], "%s", value ? value : "");
    s_dirty = true;
    s_dirty_t = now_s();
}

/* The pack folders inside the packs folder ("_dump" and other "_" names are not packs). */
static int fx_pack_list(char names[][64], int max)
{
    const char* dir = rsx_texpack_dir();
    int n = 0;
    if (!dir || !*dir) return 0;
#ifdef _WIN32
    char pat[700];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || fd.cFileName[0] == '_') continue;
        if (n < max) snprintf(names[n++], 64, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    return n;
}

/* ---- Mods page [mods]: game mods and texture packs ------------------------
 * Game mods are the community's inFAMOUS Mod Manager mods (mod.json, files/,
 * texture_patches/, xpps_patches/, modular.json), served by the file layer
 * (runtime/ppu/ppu_fs_mods.h) from the mods folder (PS3_GAME_MODS_DIR, the
 * launcher's "mods" folder). Which mods are on, their options and the ones
 * put first are saved at once in mods.ini (beside settings.ini) and apply at
 * the next start; the page shows that start's result at once (the clash rule
 * over the saved choices). "Use mods" is PS3_GAME_MODS in settings.ini.
 * [modsui] preview pictures, "Use this one instead", "Add a mod from a zip",
 * "Restart to apply". */

/* ---- [modsui] preview pictures: decoded on their own thread (mod_preview.h),
 * made ImGui user textures on the render thread (as the save list's icons,
 * vk_overlay_saves.cpp), freed when the page is left or the list is read again ---- */
enum { PIC_SIDE = 512 };
struct ModPic { std::string path; ImTextureData* tex; int state; };   /* state 0 loading, 1 ready, -1 none */
struct PicDone { std::string path; unsigned gen; std::vector<unsigned char> rgba; int w, h; bool ok; };
static std::vector<ModPic> s_pics;                  /* render thread */
static std::vector<ImTextureData*> s_pics_dying;    /* render thread: waiting for the renderer to drop them */
static std::vector<std::pair<std::string, std::string>> s_pic_paths;   /* render thread: folder -> picture ("" = none) */
static std::mutex s_pic_lock;                       /* below */
static std::vector<std::string> s_pic_queue;
static std::vector<PicDone> s_pic_done;
static bool s_pic_worker;
static unsigned s_pic_gen;                          /* results of an older generation are dropped */

static void pics_worker(void)
{
    for (;;) {
        std::string path;
        unsigned gen;
        {
            std::lock_guard<std::mutex> g(s_pic_lock);
            if (s_pic_queue.empty()) { s_pic_worker = false; return; }
            path = s_pic_queue.front();
            s_pic_queue.erase(s_pic_queue.begin());
            gen = s_pic_gen;
        }
        PicDone d;
        d.path = path; d.gen = gen; d.w = d.h = 0;
        std::string why;
        d.ok = mod_preview_load(path.c_str(), PIC_SIDE, d.rgba, d.w, d.h, &why) != 0;
        if (!d.ok) fprintf(stderr, "[overlay] mod picture %s: %s\n", path.c_str(), why.c_str());
        std::lock_guard<std::mutex> g(s_pic_lock);
        s_pic_done.push_back(std::move(d));
    }
}

static void pics_release_all(void)
{
    for (ModPic& p : s_pics)
        if (p.tex) { p.tex->WantDestroyNextFrame = true; s_pics_dying.push_back(p.tex); }
    s_pics.clear();
    s_pic_paths.clear();
    std::lock_guard<std::mutex> g(s_pic_lock);
    s_pic_queue.clear();
    s_pic_done.clear();
    s_pic_gen++;
}

/* Once a frame, after ImGui::NewFrame (render thread, ImGui lock held). */
static void pics_tick(void)
{
    if (!s_open || s_tab != TAB_MODS) { if (!s_pics.empty() || !s_pic_paths.empty()) pics_release_all(); }
    for (size_t i = 0; i < s_pics_dying.size(); ) {
        ImTextureData* t = s_pics_dying[i];
        if (t->Status == ImTextureStatus_Destroyed) {
            ImGui::UnregisterUserTexture(t);
            delete t;
            s_pics_dying.erase(s_pics_dying.begin() + (ptrdiff_t)i);
        } else i++;
    }
    std::vector<PicDone> done;
    {
        std::lock_guard<std::mutex> g(s_pic_lock);
        /* at most two new textures a frame: each upload waits for the queue */
        while (!s_pic_done.empty() && done.size() < 2) { done.push_back(std::move(s_pic_done.front())); s_pic_done.erase(s_pic_done.begin()); }
    }
    for (PicDone& d : done) {
        if (d.gen != s_pic_gen) continue;
        for (ModPic& p : s_pics) {
            if (p.path != d.path || p.state != 0) continue;
            if (!d.ok) { p.state = -1; break; }
            ImTextureData* t = new ImTextureData();
            t->Create(ImTextureFormat_RGBA32, d.w, d.h);
            memcpy(t->Pixels, d.rgba.data(), (size_t)d.w * d.h * 4);
            t->UseColors = true;
            ImGui::RegisterUserTexture(t);
            p.tex = t; p.state = 1;
            break;
        }
    }
}

/* The overlay is going away: the renderer has destroyed every texture. */
static void pics_shutdown(void)
{
    for (ModPic& p : s_pics) delete p.tex;
    for (ImTextureData* t : s_pics_dying) delete t;
    s_pics.clear(); s_pics_dying.clear(); s_pic_paths.clear();
    std::lock_guard<std::mutex> g(s_pic_lock);
    s_pic_queue.clear(); s_pic_done.clear(); s_pic_gen++;
}

/* Mod i's picture: NULL while it loads or when it has none. */
static ImTextureData* pic_for(int i, const ps3_game_mod& m)
{
    if (!m.has_image) return NULL;
    const char* path = NULL;
    for (const auto& fp : s_pic_paths) if (fp.first == m.folder) { path = fp.second.c_str(); break; }
    if (!path) {
        char p[1024];
        s_pic_paths.push_back({ m.folder, ps3_game_mods_image(i, p, sizeof p) ? std::string(p) : std::string() });
        path = s_pic_paths.back().second.c_str();
    }
    if (!*path) return NULL;
    for (ModPic& p : s_pics) if (p.path == path) return p.state == 1 ? p.tex : NULL;
    s_pics.push_back({ path, NULL, 0 });
    std::lock_guard<std::mutex> g(s_pic_lock);
    s_pic_queue.push_back(path);
    if (!s_pic_worker) { s_pic_worker = true; std::thread(pics_worker).detach(); }
    return NULL;
}

/* ---- [modsui] "Restart to apply" ------------------------------------------- */
static volatile int s_restart_ask;   /* 1 open the question, 2 showing */
static volatile bool s_restart_cancel;   /* [menu-e] Esc / Circle on it */

/* Saves, then ends this game and starts it again (cellSysutil.c ps3_host_restart:
 * the launcher or this exe again). The test hook (PS3_GAME_MODS_TEST "restart")
 * goes straight to ps3_host_restart. */
extern "C" void vk_overlay_restart_game(const char* why)
{
    if (s_dirty) settings_save();
    if (s_enabled) {
        set_open(0);
        notice_for("Restarting the game: it comes back at the title screen", 10.0);
    }
    ps3_host_restart(why ? why : "in-game menu");
}

static void restart_confirm(void)
{
    if (s_restart_ask == 1) { ImGui::OpenPopup("###restartask"); s_restart_ask = 2; }
    if (s_restart_ask != 2) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Restart the game?###restartask", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                                                  ImGuiWindowFlags_NoSavedSettings)) {
        s_restart_ask = 0; s_restart_cancel = false; return;
    }
    ImGui::TextUnformatted("The game closes and starts again with the mods as you set them.");
    ImGui::TextUnformatted("It comes back at the title screen: choose Continue there to go on from your last save.");
    ImGui::TextUnformatted("Progress since that save is lost.");
    ImGui::Spacing();
    const bool go = ImGui::Button("Restart");
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    ImGui::SetItemDefaultFocus();   /* Cancel: a stray Cross does not restart */
    if (cancel || s_restart_cancel) { s_restart_ask = 0; s_restart_cancel = false; ImGui::CloseCurrentPopup(); }
    else if (go) {
        s_restart_ask = 0;
        ImGui::CloseCurrentPopup();
        vk_overlay_restart_game("Mods: Restart to apply");
    }
    ImGui::EndPopup();
}

/* ---- [modsui] conflicts: a mod turned on changes what another one that is on does ----
 * The question lists every part of a game file both change (ps3_game_mods_conflicts):
 * the ones they change in different bytes are merged on their own; for each other
 * part the player picks whose version is used, and the rest of both mods is used
 * together (ps3_game_mods_resolve). Live mods take it at once; any other mod needs a
 * restart, which the question offers next. */
static volatile int s_cf_ask;        /* 1 open the question, 2 showing */
static volatile bool s_cf_cancel;    /* Esc / Circle on it */
static int s_cf_mod = -1, s_cf_stage;   /* the mod; 0 pick the versions, 1 merged: restart? */
static std::vector<ps3_game_mod_conflict> s_cf;
static std::vector<int> s_cf_pick;
static std::string s_cf_name, s_cf_others, s_cf_done;

static void cf_others(void)
{
    std::vector<std::string> o;
    for (const ps3_game_mod_conflict& c : s_cf)
        if (std::find(o.begin(), o.end(), std::string(c.other_name)) == o.end()) o.push_back(c.other_name);
    s_cf_others.clear();
    for (size_t k = 0; k < o.size(); k++) s_cf_others += (k == 0 ? "" : k + 1 == o.size() ? " and " : ", ") + o[k];
}

/* After mod i was turned on (toggled), or from its "Resolve" row: ask if a part
 * needs a choice; else, when it merged on its own and a restart is needed, say so. */
static void mods_conflict_open(int i, bool toggled)
{
    ps3_game_mod m;
    if (!ps3_game_mods_get(i, &m)) return;
    const int n = ps3_game_mods_conflicts(i, NULL, 0);
    if (n <= 0) return;
    s_cf.assign((size_t)n, ps3_game_mod_conflict());   /* every part: one left out of the answer leaves the mod out */
    const int got = ps3_game_mods_conflicts(i, s_cf.data(), (int)s_cf.size());
    s_cf.resize((size_t)(got < (int)s_cf.size() ? got : (int)s_cf.size()));
    s_cf_pick.assign(s_cf.size(), 0);
    bool open = false, choose = false, live_only = true;
    for (size_t k = 0; k < s_cf.size(); k++) {
        const ps3_game_mod_conflict& c = s_cf[k];
        if (!c.merges) { choose = true; if (c.winner < 0) open = true; }
        s_cf_pick[k] = c.winner >= 0 ? c.winner : 0;   /* the mod just turned on, unless chosen before */
        if (!c.live) live_only = false;
    }
    s_cf_mod = i;
    s_cf_name = m.name;
    cf_others();
    if (open || (!toggled && choose)) { s_cf_stage = 0; s_cf_ask = 1; return; }
    if (!toggled) return;
    if (!live_only && ps3_game_mods_restart_needed()) {   /* merged on its own: the restart */
        s_cf_done = s_cf_name + " is combined with " + s_cf_others + " (they change different places in the same files).";
        if (ps3_host_can_restart()) { s_cf_stage = 1; s_cf_ask = 1; }
        else notice_for((s_cf_done + " Restart the game to use it.").c_str(), 8.0);
    }
}

static void conflict_dialog(void)
{
    if (s_cf_ask == 1) { ImGui::OpenPopup("###modconflict"); s_cf_ask = 2; }
    if (s_cf_ask != 2) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(s_cf_stage ? "Restart to use the combined mods?###modconflict" : "Combine these mods?###modconflict", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        s_cf_ask = 0; s_cf_cancel = false; return;
    }
    auto close = [] { s_cf_ask = 0; s_cf_cancel = false; ImGui::CloseCurrentPopup(); };
    if (s_cf_stage == 1) {
        ImGui::TextUnformatted(s_cf_done.c_str());
        ImGui::TextUnformatted("The game has to restart to load the combined files. It comes back at the title screen:");
        ImGui::TextUnformatted("choose Continue there to go on from your last save. Progress since that save is lost.");
        ImGui::Spacing();
        const bool go = ImGui::Button("Restart now");
        ImGui::SameLine();
        const bool later = ImGui::Button("Later");
        ImGui::SetItemDefaultFocus();   /* Later: a stray Cross does not restart */
        if (later || s_cf_cancel) {
            close();
            notice_for("The combined files are used after a restart (F2 > Mods > Restart to apply).", 6.0);
        } else if (go) {
            close();
            vk_overlay_restart_game("Mods: merged");
        }
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("%s changes the same game files as %s.", s_cf_name.c_str(), s_cf_others.c_str());
    ImGui::TextUnformatted("Combine keeps the changes of both. Turning one of them off later takes its changes back out");
    ImGui::TextUnformatted("(the files are built again from the game's own at every start; see merge_log.txt in the mods folder).");
    ImGui::Spacing();
    static bool other_first;   /* where both change the very same bytes: the other mod's */
    if (ImGui::IsWindowAppearing()) {   /* as chosen before, else the mod just turned on */
        other_first = false;
        for (size_t k = 0; k < s_cf.size(); k++) if (!s_cf[k].merges && s_cf[k].winner == 1) other_first = true;
    }
    const float row = ImGui::GetTextLineHeightWithSpacing();
    int automatic = 0, shown = 0;
    for (const ps3_game_mod_conflict& c : s_cf) { if (c.merges) automatic++; else shown++; }
    const float h = (float)(shown + (automatic ? 1 : 0)) * row + row * 0.5f;
    const float hmax = io.DisplaySize.y * 0.45f;
    ImGui::BeginChild("##cflist", ImVec2(0.0f, h < hmax ? h : hmax), ImGuiChildFlags_AutoResizeX);
    for (size_t k = 0; k < s_cf.size(); k++) {   /* the parts that need the answer; the rest as one line */
        const ps3_game_mod_conflict& c = s_cf[k];
        if (c.merges) continue;
        const char* win = other_first ? c.other_name : s_cf_name.c_str();
        const char* lose = other_first ? s_cf_name.c_str() : c.other_name;
        if (c.part[0] == 'p') ImGui::Text("%s: combined; where both change the same bytes, %s's are used", c.what, win);
        else ImGui::Text("%s: can't be combined: %s's copy is used, ALL of %s's changes to it are left out", c.what, win, lose);
    }
    if (automatic) ImGui::TextDisabled("%d more part(s): combined on their own (they change different places)", automatic);
    ImGui::EndChild();
    int overlaps = 0;
    for (const ps3_game_mod_conflict& c : s_cf) if (!c.merges) overlaps++;
    if (overlaps) ImGui::Checkbox(("Where both change the same bytes, use " + s_cf_others + "'s").c_str(), &other_first);
    for (size_t k = 0; k < s_cf.size(); k++) s_cf_pick[k] = other_first ? 1 : 0;
    ImGui::Spacing();
    const bool merge = ImGui::Button("Combine");
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    const bool off = ImGui::Button(("Turn " + s_cf_name + " off").c_str());
    ImGui::SameLine();
    const bool later = ImGui::Button("Decide later");
    if (later || s_cf_cancel) {
        close();
        notice_for((s_cf_name + " is left out until it is combined (F2 > Mods > Combine).").c_str(), 6.0);
    } else if (off) {
        ps3_game_mods_set_on(s_cf_mod, 0);
        close();
    } else if (merge) {
        std::vector<int> w(s_cf.size(), -1);
        for (size_t k = 0; k < s_cf.size(); k++) if (!s_cf[k].merges) w[k] = s_cf_pick[k];
        const int r = ps3_game_mods_resolve(s_cf_mod, w.data(), (int)w.size());
        s_cf_done = "Combined: " + s_cf_name + " and " + s_cf_others + " are used together.";
        if (r == 2 && ps3_host_can_restart()) { s_cf_stage = 1; s_cf_cancel = false; }   /* the same question, next page */
        else {
            close();
            notice_for(r == 2 ? (s_cf_done + " Restart the game to use it.").c_str() : r == 1 ? (s_cf_done + " In use now.").c_str()
                                                                                         : "Nothing to merge.", 6.0);
        }
    }
    ImGui::EndPopup();
}

static void quit_confirm(void)
{
    if (s_quit_ask == 1) { ImGui::OpenPopup("###quitask"); s_quit_ask = 2; }
    if (s_quit_ask != 2) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Quit the game?###quitask", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                                               ImGuiWindowFlags_NoSavedSettings)) {
        s_quit_ask = 0; s_quit_cancel = false; return;
    }
    ImGui::TextUnformatted("Progress since the last autosave will be lost.");
    ImGui::Spacing();
    const bool quit = ImGui::Button("Quit");
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    ImGui::SetItemDefaultFocus();   /* Cancel: a stray Cross does not quit */
    if (cancel || s_quit_cancel) {
        s_quit_ask = 0; s_quit_cancel = false;
        ImGui::CloseCurrentPopup();
    } else if (quit) {
        s_quit_ask = 0;
        ImGui::CloseCurrentPopup();
        set_open(0);                  /* saves the settings */
        vk_overlay_notice("Quitting the game...");
        ps3_host_quit("in-game menu");
    }
    ImGui::EndPopup();
}

#include "vk_overlay_shadow.inc"  /* [shadow] shadow_quality_* (the Lighting page's Shadow quality row) */
#include "vk_overlay_light.inc"   /* [light] lfx_choose, lfx_compare_toggle (F3) */
#include "vk_overlay_update.inc"  /* [update] the status watcher, update_confirm, update_attach */
#include "vk_overlay_about.inc"   /* [about] the supporters list's reader, about_open_url, about_attach */
#include "vk_overlay_lowend.inc"  /* (lowend) the automatic choice for small cards, the Graphics presets */

#include "vk_overlay_menu.inc"  /* [menu-e] the settings menu, design E: rows, rail, info card */

static void draw_hud(void)
{
    const ImGuiIO& io = ImGui::GetIO();
    const float pad = S(10.0f);
    const ImVec2 pos((s_hud_corner & 1) ? io.DisplaySize.x - pad : pad, (s_hud_corner & 2) ? io.DisplaySize.y - pad : pad);
    const ImVec2 pivot((s_hud_corner & 1) ? 1.0f : 0.0f, (s_hud_corner & 2) ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(s_hud_opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(9), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(2)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##hud", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::PushFont(NULL, 14.0f);
        stats_lines(true);
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
}

static void draw_notices(double now)
{
    Notice n[3];
    { std::lock_guard<std::mutex> g(s_notice_lock); memcpy(n, s_notice, sizeof n); }
    const ImGuiIO& io = ImGui::GetIO();
    /* bottom right (bottom left when the HUD is there) */
    const bool left = (s_hud_corner == 3) && !s_open;
    float y = io.DisplaySize.y - S(14.0f);
    if (s_open && s_card_top > 0.0f) y = s_card_top - S(10.0f);   /* [menu-e] above the menu's card */
    for (int i = 0; i < 3; i++) {
        if (n[i].until <= now) continue;
        const float a = (float)((n[i].until - now) < 0.5 ? (n[i].until - now) / 0.5 : 1.0);
        ImGui::SetNextWindowPos(ImVec2(left ? S(14.0f) : io.DisplaySize.x - S(14.0f), y), ImGuiCond_Always,
                                ImVec2(left ? 0.0f : 1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.85f * a);
        char id[16]; snprintf(id, sizeof id, "##note%d", i);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        if (ImGui::Begin(id, NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::TextUnformatted(n[i].text);
            y -= ImGui::GetWindowHeight() + S(6.0f);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

/* The controller, as cellPad saw it before holding the game's copy neutral. */
static void feed_pad(ImGuiIO& io)
{
    static uint16_t prev;
    const uint32_t nav = g_pad_overlay_nav, nav2 = g_pad_overlay_nav2;
    /* plus the buttons of every other controller, by position (raw SDL buttons) */
    static const struct { uint8_t sdl; uint16_t pad; } kRaw[] = {
        { 0, PAD_CROSS }, { 1, PAD_CIRCLE }, { 2, PAD_SQUARE }, { 3, PAD_TRIANGLE },
        { 9, PAD_L1 }, { 10, PAD_R1 }, { 11, PAD_UP }, { 12, PAD_DOWN }, { 13, PAD_LEFT }, { 14, PAD_RIGHT },
    };
    uint16_t b = (uint16_t)nav;
    const uint32_t raw = g_pad_overlay_raw;
    for (const auto& r : kRaw) if (raw >> r.sdl & 1u) b |= r.pad;
    b |= (uint16_t)g_pad_overlay_inject;   /* [saves] PAD_SCRIPT / PAD_FILE while a system dialog holds the pad */
    int lx = (nav >> 16) & 255, ly = (nav >> 24) & 255, ry = (nav2 >> 8) & 255;
    if (!vk_overlay_saves_active() && remap_pad_hold(b)) { b = 0; lx = ly = ry = 128; }   /* [remap] a press is awaited: not the menu's */
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (b & PAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (b & PAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (b & PAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (b & PAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (b & PAD_CROSS) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (b & PAD_CIRCLE) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  (b & PAD_SQUARE) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    (b & PAD_TRIANGLE) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadL2,        (b & PAD_L2) != 0);   /* [menu-e] the compare line */
    io.AddKeyEvent(ImGuiKey_GamepadR2,        (b & PAD_R2) != 0);
    auto axis = [](int v, int dir) {   /* dir -1: towards 0, +1: towards 255 */
        float f = dir < 0 ? (128 - v) / 127.0f : (v - 128) / 127.0f;
        f = (f - 0.25f) / 0.75f;
        return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    };
    const struct { ImGuiKey k; float v; } sticks[] = {
        { ImGuiKey_GamepadLStickLeft, axis(lx, -1) }, { ImGuiKey_GamepadLStickRight, axis(lx, 1) },
        { ImGuiKey_GamepadLStickUp, axis(ly, -1) },   { ImGuiKey_GamepadLStickDown, axis(ly, 1) },
        { ImGuiKey_GamepadRStickUp, axis(ry, -1) },   { ImGuiKey_GamepadRStickDown, axis(ry, 1) },
    };
    for (const auto& s : sticks) io.AddKeyAnalogEvent(s.k, s.v > 0.1f, s.v);
    const uint16_t pressed = b & (uint16_t)~prev;
    prev = b;
    if (vk_overlay_saves_active()) { vk_overlay_saves_pad(b, lx, ly); return; }   /* [saves] */
    if (pressed) fprintf(stderr, "[overlay] menu pad: 0x%04X\n", (unsigned)pressed);   /* [menu-e] the test runs' presses, in the log */
    if (menu_modal()) { if (pressed & PAD_CIRCLE) menu_escape(); return; }   /* a question: Circle = its Cancel */
    if (pressed & PAD_L1) s_tab_want = (s_tab + tab_count() - 1) % tab_count();   /* wrapping */
    if (pressed & PAD_R1) s_tab_want = (s_tab + 1) % tab_count();
    if (pressed & PAD_CIRCLE) menu_escape();                                       /* Back, or close */
}

/* ---- the entry points ------------------------------------------------------ */
extern "C" void vk_overlay_attach(const vk_overlay_attach_info* info)
{
    s_enabled = false;
    g_vk_overlay_active = 0;
    if (!info || overlay_off() || !info->get_instance_proc_addr || !info->device || !info->sdl_window) return;
    s_vk = *info;
    if (!load_own_vk()) { fprintf(stderr, "[overlay] Vulkan entry points missing: overlay off\n"); return; }
    s_attached = true;
    s_t_attach = now_s();
    { const char* e = getenv("PS3_OVERLAY_HUD"); s_hud = (e && *e) ? (strcmp(e, "0") != 0) : 1; }
    { const char* e = getenv("PS3_OVERLAY_HUD_CORNER"); if (e && *e) s_hud_corner = atoi(e) & 3; }
    { const char* e = getenv("PS3_OVERLAY_HUD_OPACITY");
      if (e && *e) { float v = (float)atof(e); s_hud_opacity = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; } }
    { const char* e = getenv("PS3_OVERLAY_HUD_GRAPH"); if (e && *e) s_hud_graph = strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3_OVERLAY_HUD_CPU"); if (e && *e) s_hud_cpu = strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3_OVERLAY_PAD"); s_pad_chord = !(e && e[0] == '0'); }
    vk_ov_theme_select(vk_ov_theme_parse(getenv(kVar[V_THEME])));   /* [theme] default Hero */
    ps3_setting_register("PS3_OVERLAY_THEME", theme_apply);
    { const int t = tab_from_name(getenv("PS3RECOMP_OVERLAY_TAB")); if (t >= 0) s_tab_want = t; }
    if (p_vkGetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties pp;
        p_vkGetPhysicalDeviceProperties(s_vk.phys, &pp);
        snprintf(s_gpu_name, sizeof s_gpu_name, "%s", pp.deviceName);
    }
    { SDL_DisplayMode dm;
      const int di = SDL_GetWindowDisplayIndex((SDL_Window*)s_vk.sdl_window);
      if (di >= 0 && SDL_GetCurrentDisplayMode(di, &dm) == 0) s_refresh_hz = dm.refresh_rate; }
#ifdef _WIN32
    s_rsx_tid = GetCurrentThreadId();   /* the thread that presents */
#endif
    script_load();
    s_enabled = true;
    g_pad_overlay_present = 1;
    if (env_on("PS3RECOMP_OVERLAY_MENU_OPEN") || env_on("PS3_OVERLAY_FORCE_OPEN")) set_open(1);
    notice_for("F2: settings    F1: performance HUD", 6.0);
    lowend_attach();                 /* (lowend) the automatic lighter settings, said once */
    vk_overlay_cheats_attach();      /* remembered cheats on again, and a notice naming them */
    vk_overlay_saves_attach();       /* [saves] the save-data list dialog for cellSaveData */
    update_attach();                 /* [update] the launcher's answer about a newer version (F2 > Update) */
    about_attach();                  /* [about] the supporters list (F2 > About) */
    menu_census();                   /* [menu-e] every setting has a row (else a loud warning), the startup line */
    update_active();
    fprintf(stderr, "[overlay] in-game menu: F2 (or hold L3+R3); HUD %s (F1); fullscreen Alt+Enter / F11; settings %s\n",
            s_hud ? "on" : "off", s_ini_path[0] ? s_ini_path : "not saved (PS3_OVERLAY_INI empty)");
}

extern "C" void vk_overlay_swapchain_changed(VkFormat format, int color_ok)
{
    if (s_attached) fb_destroy_all();
    s_sc_format = format;
    s_color_ok = color_ok != 0;
    if (!s_color_ok) fprintf(stderr, "[overlay] swapchain images cannot be colour attachments: overlay off\n");
    update_active();
}

static bool fb_for(uint32_t idx, VkImage image, VkExtent2D ext)
{
    if (idx >= SC_MAX) return false;
    if (s_fb[idx].fb && s_fb[idx].img == image && s_fb[idx].ext.width == ext.width && s_fb[idx].ext.height == ext.height)
        return true;
    if (s_fb[idx].fb || s_fb[idx].view) {          /* changed without notice: make sure it is idle */
        p_vkDeviceWaitIdle(s_vk.device);
        if (s_fb[idx].fb) p_vkDestroyFramebuffer(s_vk.device, s_fb[idx].fb, NULL);
        if (s_fb[idx].view) p_vkDestroyImageView(s_vk.device, s_fb[idx].view, NULL);
        s_fb[idx].fb = VK_NULL_HANDLE; s_fb[idx].view = VK_NULL_HANDLE;
    }
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = s_rp_format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (p_vkCreateImageView(s_vk.device, &vi, NULL, &s_fb[idx].view) != VK_SUCCESS) { s_fb[idx].view = VK_NULL_HANDLE; return false; }
    VkFramebufferCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = s_rp;
    fi.attachmentCount = 1;
    fi.pAttachments = &s_fb[idx].view;
    fi.width = ext.width; fi.height = ext.height; fi.layers = 1;
    if (p_vkCreateFramebuffer(s_vk.device, &fi, NULL, &s_fb[idx].fb) != VK_SUCCESS) { s_fb[idx].fb = VK_NULL_HANDLE; return false; }
    s_fb[idx].img = image;
    s_fb[idx].ext = ext;
    return true;
}

extern "C" int vk_overlay_draw(VkCommandBuffer cmd, uint32_t index, VkImage image, VkExtent2D ext,
                               VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access)
{
    if (!s_enabled || !s_color_ok || s_failed || !cmd || !ext.width || !ext.height) return 0;
    if (s_open && g_vk_overlay_on_worker) return 0;   /* the menu: on the RSX thread only (next frame) */
    std::lock_guard<std::recursive_mutex> il(s_imgui_lock);
    const double now = now_s();
    if (s_last_present > 0) {
        s_ft[s_ft_i] = (float)((now - s_last_present) * 1000.0);
        s_ft_i = (s_ft_i + 1) % FT_N; s_ft_count++;
    }
    const double dt = s_last_present > 0 ? now - s_last_present : 1.0 / 60.0;
    s_last_present = now;
    if (s_hud || s_open) stats_tick(now);
    cpu_want((s_hud && s_hud_cpu) || (s_open && s_tab == 4));
    const bool notices = s_notice_until > now;
    update_active();
    if (!s_open && !s_hud && !notices) { s_last_present = 0; s_stats_t0 = 0; cpu_want(false); return 0; }

    if (!s_imgui_ready && !imgui_setup()) { s_failed = true; update_active(); return 0; }
    if (s_vk_ready && s_rp_format != s_sc_format) {   /* a new swapchain format: rebuild the renderer */
        p_vkDeviceWaitIdle(s_vk.device);
        fb_destroy_all();
        vk_backend_shutdown();
    }
    if (!s_vk_ready && !vk_backend_init()) {
        fprintf(stderr, "[overlay] Vulkan renderer setup failed: overlay off\n");
        s_failed = true; update_active(); return 0;
    }
    if (s_open && !s_sdl_ready && !sdl_setup()) fprintf(stderr, "[overlay] SDL platform backend setup failed\n");
    if (!fb_for(index, image, ext)) return 0;

    static int was_open;
    if (s_open && (!was_open || s_ui_stale)) { ui_load(); s_ui_stale = false; }
    was_open = s_open;

    ImGuiIO& io = ImGui::GetIO();
    if (s_ev_n) {
        static SDL_Event ev[128];
        int n;
        { std::lock_guard<std::mutex> g(s_ev_lock); n = s_ev_n; memcpy(ev, s_ev, sizeof ev[0] * (size_t)n); s_ev_n = 0; }
        if (s_open && s_sdl_ready)
            for (int i = 0; i < n; i++) if (!remap_event(&ev[i])) ImGui_ImplSDL2_ProcessEvent(&ev[i]);   /* [remap] a capture's press */
    }

    ImGui_ImplVulkan_NewFrame();
    if (s_open && s_sdl_ready) {
        ImGui_ImplSDL2_NewFrame();
    } else {
        int ww = 0, wh = 0;
        SDL_GetWindowSize((SDL_Window*)s_vk.sdl_window, &ww, &wh);
        io.DisplaySize = ww > 0 && wh > 0 ? ImVec2((float)ww, (float)wh) : ImVec2((float)ext.width, (float)ext.height);
        io.DeltaTime = (float)(dt > 0.0001 && dt < 1.0 ? dt : 1.0 / 60.0);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) io.DisplaySize = ImVec2((float)ext.width, (float)ext.height);
    /* What is drawn into is the swapchain image, whatever the window says. */
    io.DisplayFramebufferScale = ImVec2((float)ext.width / io.DisplaySize.x, (float)ext.height / io.DisplaySize.y);
    if (s_theme_want >= 0) {   /* [theme] a new look: the whole style again, then scaled below */
        vk_ov_theme_select(s_theme_want);
        s_theme_want = -1;
        ImGuiStyle st;
        vk_ov_theme_style(st);
        s_base_style = st;
        s_scale_applied = -1.0f;
    }
    apply_scale(io.DisplaySize.y);
    if (s_open) { io.BackendFlags |= ImGuiBackendFlags_HasGamepad; feed_pad(io); }

    ImGui::NewFrame();
    pics_tick();                     /* [modsui] the mods' pictures: made, or freed off the Mods page */
    if (s_open) draw_menu(now);
    else if (s_hud) draw_hud();
    if (notices) draw_notices(now);
    ImGui::Render();

    /* Save a change made in the menu soon after it, not only when it closes. */
    if (s_dirty && now - s_dirty_t > 1.0) settings_save();

    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = access;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = layout;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdPipelineBarrier(cmd, stage ? stage : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkRenderPassBeginInfo rb = {};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = s_rp;
    rb.framebuffer = s_fb[index].fb;
    rb.renderArea.extent = ext;
    p_vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    p_vkCmdEndRenderPass(cmd);
    return 1;
}

extern "C" void vk_overlay_shutdown(void)
{
    if (!s_attached) return;
    if (s_dirty) settings_save();
    if (s_vk.device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(s_vk.device);
    if (s_imgui_ready) {
        vk_backend_shutdown();
        if (s_sdl_ready) { ImGui_ImplSDL2_Shutdown(); s_sdl_ready = false; }
        ImGui::DestroyContext();
        s_imgui_ready = false;
        pics_shutdown();             /* [modsui] the renderer has destroyed their images */
    }
    fb_destroy_all();
    cpu_want(false);
    s_enabled = false;
    s_attached = false;
    g_vk_overlay_active = 0;
    g_pad_overlay_open = 0;
    vk_overlay_saves_shutdown();     /* [saves] a waiting save/load backs out; its icons freed */
}
