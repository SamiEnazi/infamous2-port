/* ---- [about] F2 > About: the port, its page, the supporters --------------------
 * Included by vk_overlay.cpp after vk_overlay_update.inc. The supporters list is
 * supporters.json (vk_overlay_about_file.h: every limit on that untrusted text).
 * tools/update_check.ps1 fetches it from GitHub together with the update check
 * (same switch, same 6 hours) into PS3_UPDATE_DIR/supporters.json; the copy that
 * came with the install is PS3_SUPPORTERS_FILE (tools/play.ps1, tools/play.sh).
 * A thread here reads the fetched copy, else the shipped one, and again when
 * either changes (one stat every 3 s, never on the render thread). A file that
 * is refused changes nothing: the list read before stays. No file: "No list yet".
 * The page's rows are page_about (vk_overlay_menu.inc). */
#include "vk_overlay_about_file.h"
#ifdef _WIN32
#include <shellapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#define ABOUT_PROJECT_URL "https://github.com/SamiEnazi/infamous2-port"

static std::mutex s_about_lock;
static AboutFile  s_about;               /* the list as last read (under s_about_lock) */
static unsigned   s_about_seq;           /* +1 at each new read */
static char       s_about_cache[1100];   /* the fetched copy ("" = no launcher that fetches) */
static char       s_about_ship[1100];    /* the copy that came with the install */

static bool about_read(const char* path, AboutFile* f)
{
    if (!path[0]) return false;
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    std::string text;
    char buf[8192];
    size_t n;
    while (text.size() <= (size_t)ABOUT_FILE_MAX && (n = fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
    fclose(fp);
    return about_file_parse(text.data(), text.size(), f);   /* over 64 KB: refused */
}

static void about_watch(void)
{
    unsigned long long seen[2] = { 0, 0 };
    bool first = true;
    for (;;) {
        unsigned long long sig[2] = { 0, 0 };
        if (s_about_cache[0]) upd_sig(s_about_cache, &sig[0]);
        if (s_about_ship[0]) upd_sig(s_about_ship, &sig[1]);
        if (first || sig[0] != seen[0] || sig[1] != seen[1]) {
            first = false;
            seen[0] = sig[0]; seen[1] = sig[1];
            AboutFile f;
            const bool fetched = about_read(s_about_cache, &f);
            if (fetched || about_read(s_about_ship, &f)) {
                fprintf(stderr, "[about] supporters list (%s): %d supporter(s), %d contributor(s)%s\n", fetched ? "fetched" : "shipped",
                        (int)f.supporters.size(), (int)f.contributors.size(), f.support_url.empty() ? "" : ", a support link");
                std::lock_guard<std::mutex> g(s_about_lock);
                s_about = std::move(f);
                s_about_seq++;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

static void about_attach(void)
{
    static bool started;
    if (started) return;
    const char* c = getenv("PS3_SUPPORTERS_CACHE");
    const char* d = getenv("PS3_UPDATE_DIR");
    const char* s = getenv("PS3_SUPPORTERS_FILE");
    if (c && *c) snprintf(s_about_cache, sizeof s_about_cache, "%s", c);
    else if (d && *d) snprintf(s_about_cache, sizeof s_about_cache, "%s/supporters.json", d);
    if (s && *s) snprintf(s_about_ship, sizeof s_about_ship, "%s", s);
    if (!s_about_cache[0] && !s_about_ship[0]) return;
    started = true;
    std::thread(about_watch).detach();
}

/* Opens an https:// address in the player's browser (checked once more here: only the
 * project page and a support_url that passed about_clean_url ever get this far). */
static void about_open_url(const std::string& url)
{
    if (about_clean_url(url).empty()) return;
    fprintf(stderr, "[about] opening %s\n", url.c_str());
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
    const pid_t pid = fork();            /* twice: the browser is nobody's child, no shell reads the text */
    if (pid == 0) {
        if (fork() == 0) {
#ifdef __APPLE__
            execlp("open", "open", url.c_str(), (char*)NULL);
#else
            execlp("xdg-open", "xdg-open", url.c_str(), (char*)NULL);
#endif
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
#endif
    notice_for("Opened in your browser", 5.0);
}
