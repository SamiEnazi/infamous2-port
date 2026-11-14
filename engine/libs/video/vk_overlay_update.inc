/* ---- [update] F2 > Update: a newer version of the port ---------------------
 * Included by vk_overlay.cpp. tools/play.ps1 (tools/update_check.ps1) asks
 * GitHub in the background while the game starts and writes the answer into
 * PS3_UPDATE_DIR/status.txt (vk_overlay_update_status.h). A thread here waits
 * for that file and reads it again whenever it changes (one stat every 2 s,
 * never on the render thread); the first "available" puts a notice up. The
 * page shows the installed and the new version and the release notes; its
 * buttons end the game with PS3_EXIT_UPDATE (cellSysutil.c
 * ps3_host_set_update / ps3_host_update_now), after which play.ps1 runs
 * Setup.exe --update: the saves, settings, mods and controls are kept, and
 * Setup offers Play when it is done. "Check now" writes
 * PS3_UPDATE_DIR/check-now (play.ps1 asks GitHub again). The checkbox is
 * PS3_UPDATE_CHECK in the settings file (play.ps1 reads it: 0 = it never asks
 * by itself). Without PS3_UPDATE_DIR (a git clone, RecompLauncher, a test:
 * PS3_UPDATE_BY says which) the page says how this copy is updated. */
#include "vk_overlay_update_status.h"
#include <chrono>

extern "C" {
int  ps3_host_can_update(void);                  /* libs/system/cellSysutil.c */
void ps3_host_set_update(int on);
int  ps3_host_update_pending(void);
void ps3_host_update_now(const char* why);
}

static std::mutex   s_upd_lock;
static UpdateStatus s_upd;               /* the file as last read (under s_upd_lock) */
static unsigned     s_upd_seq;           /* +1 at each new read */
static bool         s_upd_have;
static char         s_upd_dir[1024];     /* PS3_UPDATE_DIR ("" = no launcher that updates) */
static volatile int s_upd_check_req;     /* "Check now": the watcher writes the request file */
static volatile int s_upd_checking;      /* a request is out, until the status file changes */
static double       s_upd_check_t;
static volatile int s_upd_ask;           /* 1 open "Quit and update now?", 2 showing */
static int          s_upd_auto = -1;     /* PS3_UPDATE_CHECK: 1 on, 0 off, -1 not read yet */

/* the file's write time and size as one number; false = no file */
static bool upd_sig(const char* path, unsigned long long* sig)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return false;
    *sig = (((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime) ^
           ((unsigned long long)a.nFileSizeLow << 7);
#else
    struct stat st;
    if (stat(path, &st)) return false;
    *sig = ((unsigned long long)st.st_mtime * 1000003ull) ^ (unsigned long long)st.st_size;
#endif
    return true;
}

static void upd_watch(void)
{
    char status[1100], req[1100];
    snprintf(status, sizeof status, "%s/status.txt", s_upd_dir);
    snprintf(req, sizeof req, "%s/check-now", s_upd_dir);
    unsigned long long seen = 0;
    bool have = false, noticed = false;
    for (;;) {
        if (__atomic_exchange_n(&s_upd_check_req, 0, __ATOMIC_ACQ_REL)) {
            if (FILE* f = fopen(req, "w")) { fputs("check\n", f); fclose(f); }
        }
        unsigned long long sig = 0;
        if (upd_sig(status, &sig) && (!have || sig != seen)) {
            std::string text;
            if (FILE* f = fopen(status, "rb")) {
                char buf[8192];
                size_t n;
                while (text.size() < (64u << 10) && (n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
                fclose(f);
            }
            UpdateStatus st;
            update_status_parse(text.data(), text.size(), &st);
            if (st.state[0]) {   /* a whole file: play.ps1 puts it in place in one move */
                have = true;
                seen = sig;
                {
                    std::lock_guard<std::mutex> g(s_upd_lock);
                    s_upd = st;
                    s_upd_seq++;
                    s_upd_have = true;
                }
                s_upd_checking = 0;
                fprintf(stderr, "[update] %s: this copy %s, the newest %s\n", st.state,
                        st.installed[0] ? st.installed : "?", st.latest[0] ? st.latest : "?");
                if (!noticed && !strcmp(st.state, "available")) {
                    noticed = true;
                    char b[160];
                    snprintf(b, sizeof b, "Update available: %s  -  F2 > Update", st.latest[0] ? st.latest : "a new version");
                    notice_for(b, 12.0);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

static void update_attach(void)
{
    static bool started;
    const char* d = getenv("PS3_UPDATE_DIR");
    if (started || !d || !*d) return;
    started = true;
    snprintf(s_upd_dir, sizeof s_upd_dir, "%s", d);
    std::thread(upd_watch).detach();
}

static void tab_update(double now)
{
    section("Update");
    if (!s_upd_dir[0]) {
        const char* by = getenv("PS3_UPDATE_BY");
        if (by && !strcmp(by, "recomplauncher"))
            hint("RecompLauncher keeps this copy up to date: it installs each new release of the port by itself.");
        else if (by && !strcmp(by, "git"))
            hint("This folder is a git clone: \"git pull\" gets the newest code, then Setup.exe "
                 "(or Setup.cmd) builds it.");
        else
            hint("No update information: the game was not started with \"Play inFAMOUS 2\" (or the desktop shortcut) "
                 "from a folder Setup.exe made.");
        return;
    }
    static UpdateStatus v;
    static unsigned vseq = ~0u;
    static bool vhave;
    {
        std::lock_guard<std::mutex> g(s_upd_lock);
        if (s_upd_seq != vseq) { v = s_upd; vseq = s_upd_seq; vhave = s_upd_have; }
    }
    if (s_upd_auto < 0) { const char* e = getenv("PS3_UPDATE_CHECK"); s_upd_auto = (e && e[0] == '0' && !e[1]) ? 0 : 1; }
    const bool avail = vhave && !strcmp(v.state, "available");
    const ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);

    ImGui::Text("This copy:  %s", vhave && v.installed[0] ? v.installed : "...");
    if (!vhave) ImGui::TextColored(kGrey, "Asking GitHub for the newest version...");
    else if (avail) {
        ImGui::PushFont(vk_ov_theme_bold(), 0.0f);
        ImGui::TextColored(kOrange, "New:  %s", v.latest[0] ? v.latest : "a newer version");
        ImGui::PopFont();
        if (v.published[0]) { ImGui::SameLine(); ImGui::TextColored(kGrey, "(out since %s)", v.published); }
    } else if (!strcmp(v.state, "current")) ImGui::TextColored(kGreen, "You have the newest version.");
    if (v.message[0]) hint(v.message);
    if (vhave && v.checked[0]) { char b[96]; snprintf(b, sizeof b, "Last asked GitHub: %s", v.checked); hint(b); }

    if (avail) {
        ImGui::Spacing();
        if (ps3_host_can_update()) {
            const bool pending = ps3_host_update_pending() != 0;
            if (ImGui::Button(pending ? "Don't update when I quit" : "Update when I quit")) {
                ps3_host_set_update(pending ? 0 : 1);
                notice_for(pending ? "No update: the game quits as usual" : "The update starts when you quit the game", 5.0);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
            if (ImGui::Button("Quit and update now")) s_upd_ask = 1;
            ImGui::PopStyleColor();
            if (pending) ImGui::TextColored(kOrange, "The update starts when you quit the game.");
            hint("Setup downloads the new version and builds it from your copy of the game, in its own window: a few "
                 "minutes when only a little changed (the first update after an install takes about as long as the "
                 "install). Your saves, settings, mods and controls are kept. Press Play there when it is done.");
        } else hint("Run Setup.exe in the game's folder and choose \"Update to the latest version\".");
    }

    ImGui::Spacing();
    const bool asking = s_upd_checking && now - s_upd_check_t < 30.0;
    if (asking) ImGui::BeginDisabled();
    if (ImGui::Button(asking ? "Asking GitHub..." : "Check now")) { s_upd_check_t = now; s_upd_checking = 1; s_upd_check_req = 1; }
    if (asking) ImGui::EndDisabled();
    ImGui::SameLine();
    bool aut = s_upd_auto != 0;
    if (ImGui::Checkbox("Check for updates automatically", &aut)) {
        s_upd_auto = aut ? 1 : 0;
        extra_set("PS3_UPDATE_CHECK", aut ? "1" : "0");   /* play.ps1 reads it at the next start */
    }
    hint("When the game starts, at most every 6 hours: a short question to GitHub, in the background (the game "
         "never waits for it). Off: only Check now asks.");

    if (avail && !v.notes.empty()) {
        char h[120];
        snprintf(h, sizeof h, "What's new in %s", v.latest[0] ? v.latest : "the new version");
        section(h);
        if (v.title[0]) { ImGui::PushFont(vk_ov_theme_bold(), 0.0f); ImGui::TextWrapped("%s", v.title); ImGui::PopFont(); }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(v.notes.c_str(), v.notes.c_str() + v.notes.size());
        ImGui::PopTextWrapPos();
        if (v.url[0]) hint(v.url);
    }
}

static void update_confirm(void)
{
    if (s_upd_ask == 1) { ImGui::OpenPopup("###updask"); s_upd_ask = 2; }
    if (s_upd_ask != 2) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Quit and update?###updask", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                                                ImGuiWindowFlags_NoSavedSettings)) {
        s_upd_ask = 0; return;
    }
    ImGui::TextUnformatted("The game closes and Setup updates it, in its own window.");
    ImGui::TextUnformatted("Progress since your last save is lost. Your saves, settings and mods are kept.");
    ImGui::Spacing();
    const bool go = ImGui::Button("Quit and update");
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    ImGui::SetItemDefaultFocus();   /* Cancel: a stray Cross does not quit */
    if (cancel) { s_upd_ask = 0; ImGui::CloseCurrentPopup(); }
    else if (go) {
        s_upd_ask = 0;
        ImGui::CloseCurrentPopup();
        set_open(0);                /* saves the settings */
        notice_for("Quitting the game for the update...", 10.0);
        ps3_host_update_now("F2 > Update: Quit and update now");
    }
    ImGui::EndPopup();
}
