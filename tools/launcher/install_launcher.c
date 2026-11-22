/* "Install inFAMOUS 2": the double-click program at the top of the Linux release's folder.
 *
 * It starts tools/install_gui.sh (the installer: zenity, kdialog, or the text way in a terminal) from the folder it is
 * in. GNOME's file manager runs a program on a double-click but not a .desktop file kept in a folder; KDE's Dolphin,
 * Xfce's Thunar and the others run programs too (Dolphin uses "Install inFAMOUS 2.desktop", which stays beside it).
 *   - started from a terminal, or with no desktop (no DISPLAY / WAYLAND_DISPLAY): bash tools/install_gui.sh, here;
 *   - zenity or kdialog there: the same (the installer shows its own windows);
 *   - neither: a terminal window running it (konsole, GNOME's ptyxis / kgx / gnome-terminal, xfce4-terminal, ...).
 * Without bash or the script it says so on stderr and exits 1.
 *
 * No C library and no dynamic loader: Linux system calls only, built static (-nostdlib -static), so the same file runs
 * on every x86-64 Linux. tools/make_release_zip_linux.py compiles it into the zip (it is not committed as a binary):
 *   clang --target=x86_64-linux-gnu -Os -static -nostdlib -ffreestanding -fno-pic -no-pie -fuse-ld=lld \
 *         -o "Install inFAMOUS 2" tools/launcher/install_launcher.c
 */

typedef unsigned long usize;

#define SYS_write 1
#define SYS_ioctl 16
#define SYS_access 21
#define SYS_execve 59
#define SYS_exit 60
#define SYS_chdir 80
#define SYS_readlink 89
#define X_OK 1
#define R_OK 4
#define TCGETS 0x5401
#define PATH_MAX 4096

static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

/* the compiler may turn loops into these calls even when freestanding */
void* memcpy(void* d, const void* s, usize n)
{
    unsigned char* o = d;
    const unsigned char* i = s;
    while (n--) *o++ = *i++;
    return d;
}
void* memset(void* d, int v, usize n)
{
    unsigned char* o = d;
    while (n--) *o++ = (unsigned char)v;
    return d;
}

static usize len(const char* s)
{
    usize n = 0;
    while (s[n]) n++;
    return n;
}

static void say(const char* s) { sys3(SYS_write, 2, (long)s, (long)len(s)); }

/* out = a + b (+ c), within PATH_MAX; 0 = too long */
static int join(char* out, const char* a, const char* b, const char* c)
{
    usize n = 0;
    const char* parts[3] = {a, b, c};
    for (int p = 0; p < 3; p++)
        for (const char* s = parts[p]; s && *s; s++) {
            if (n + 1 >= PATH_MAX) return 0;
            out[n++] = *s;
        }
    out[n] = 0;
    return 1;
}

static const char* getenv_(char** envp, const char* name)
{
    usize n = len(name);
    for (; *envp; envp++) {
        usize i = 0;
        while (i < n && (*envp)[i] == name[i]) i++;
        if (i == n && (*envp)[n] == '=') return *envp + n + 1;
    }
    return 0;
}

/* the program <name> on PATH (or in /usr/bin, /bin when PATH is empty) -> out; 0 = not found */
static int which(char* out, char** envp, const char* name)
{
    const char* path = getenv_(envp, "PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char dir[PATH_MAX];
    while (*path) {
        usize n = 0;
        while (path[n] && path[n] != ':') n++;
        if (n > 0 && n < PATH_MAX - 1) {
            memcpy(dir, path, n);
            dir[n] = 0;
            if (join(out, dir, "/", name) && sys3(SYS_access, (long)out, X_OK, 0) == 0) return 1;
        }
        path += n;
        if (*path == ':') path++;
    }
    return 0;
}

static int isatty0(void)
{
    char t[64];   /* struct termios */
    return sys3(SYS_ioctl, 0, TCGETS, (long)t) == 0;
}

__attribute__((noreturn)) static void quit(int code)
{
    for (;;) sys3(SYS_exit, code, 0, 0);
}

__attribute__((used, noreturn)) void start_c(long* sp)
{
    long argc = sp[0];
    char** envp = (char**)(sp + 1 + argc + 1);
    static char self[PATH_MAX], dir[PATH_MAX], script[PATH_MAX], bash[PATH_MAX], term[PATH_MAX];

    long n = sys3(SYS_readlink, (long)"/proc/self/exe", (long)self, PATH_MAX - 1);
    if (n <= 0) { say("Install inFAMOUS 2: cannot tell which folder it is in (/proc/self/exe)\n"); quit(1); }
    self[n] = 0;
    while (n > 0 && self[n - 1] != '/') n--;
    if (n <= 1) { say("Install inFAMOUS 2: odd program path\n"); quit(1); }
    memcpy(dir, self, (usize)(n - 1));
    dir[n - 1] = 0;
    if (!join(script, dir, "/tools/install_gui.sh", 0) || sys3(SYS_access, (long)script, R_OK, 0) != 0) {
        say("Install inFAMOUS 2: tools/install_gui.sh is not next to this program: unpack the whole zip, then start it again\n");
        quit(1);
    }
    if (!which(bash, envp, "bash")) {
        if (sys3(SYS_access, (long)"/bin/bash", X_OK, 0) == 0) join(bash, "/bin/bash", 0, 0);
        else { say("Install inFAMOUS 2: no bash on this system\n"); quit(1); }
    }
    sys3(SYS_chdir, (long)dir, 0, 0);

    const char* disp = getenv_(envp, "DISPLAY");
    const char* wl = getenv_(envp, "WAYLAND_DISPLAY");
    int desktop = (disp && *disp) || (wl && *wl);
    char* run_here[] = {bash, script, 0};
    if (isatty0() || !desktop || which(term, envp, "zenity") || which(term, envp, "kdialog")) {
        sys3(SYS_execve, (long)bash, (long)run_here, (long)envp);
        say("Install inFAMOUS 2: starting bash failed\n");
        quit(1);
    }
    /* no dialog program: a terminal window with the installer's text way. How each one takes a command: */
    static const struct { const char* name; const char* flag; } terms[] = {
        {"konsole", "-e"}, {"ptyxis", "--"}, {"kgx", "--"}, {"gnome-terminal", "--"}, {"xfce4-terminal", "-x"},
        {"mate-terminal", "-x"}, {"tilix", "--"}, {"x-terminal-emulator", "-e"}, {"alacritty", "-e"},
        {"kitty", 0}, {"foot", 0}, {"xterm", "-e"},
    };
    for (usize i = 0; i < sizeof(terms) / sizeof(terms[0]); i++) {
        if (!which(term, envp, terms[i].name)) continue;
        char* a[] = {term, (char*)terms[i].flag, bash, script, 0};
        if (!terms[i].flag) { a[1] = bash; a[2] = script; a[3] = 0; }
        sys3(SYS_execve, (long)term, (long)a, (long)envp);
    }
    /* no terminal either: the installer itself says what it can (its last resort is the same search) */
    sys3(SYS_execve, (long)bash, (long)run_here, (long)envp);
    say("Install inFAMOUS 2: starting bash failed\n");
    quit(1);
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("xor %ebp, %ebp\n\t"
                     "mov %rsp, %rdi\n\t"
                     "and $-16, %rsp\n\t"
                     "call start_c\n\t"
                     "ud2");
}
