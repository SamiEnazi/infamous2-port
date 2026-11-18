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
static volatile bool s_upd_cancel;       /* [menu-e] Esc / Circle on "Quit and update?" */

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

static void update_confirm(void)
{
    if (s_upd_ask == 1) { ImGui::OpenPopup("###updask"); s_upd_ask = 2; }
    if (s_upd_ask != 2) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Quit and update?###updask", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                                                ImGuiWindowFlags_NoSavedSettings)) {
        s_upd_ask = 0; s_upd_cancel = false; return;
    }
    ImGui::TextUnformatted("The game closes and Setup updates it, in its own window.");
    ImGui::TextUnformatted("Progress since your last save is lost. Your saves, settings and mods are kept.");
    ImGui::Spacing();
    const bool go = ImGui::Button("Quit and update");
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    ImGui::SetItemDefaultFocus();   /* Cancel: a stray Cross does not quit */
    if (cancel || s_upd_cancel) { s_upd_ask = 0; s_upd_cancel = false; ImGui::CloseCurrentPopup(); }   /* [menu-e] Esc / Circle too */
    else if (go) {
        s_upd_ask = 0;
        ImGui::CloseCurrentPopup();
        set_open(0);                /* saves the settings */
        notice_for("Quitting the game for the update...", 10.0);
        ps3_host_update_now("F2 > Update: Quit and update now");
    }
    ImGui::EndPopup();
}
