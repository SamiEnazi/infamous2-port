/* ps3_liveab.c -- PS3_LIVEAB: an A/B inside one game run (bench only, default off); see include/ps3emu/ps3_liveab.h.
 *
 * (ft1, 2026-11-19) Why: between two game runs the frame rate differs by 1-2 % (the route's path, the power state,
 * the boot), while the changes measured are 0.5-1 %; an A/B over separate runs needs 4-8 runs a side (1-1.5 h). Inside
 * one run the A and B blocks share the boot, the save, the shader cache and the power state, so a pair of blocks
 * differs only by the switch, the scene's drift between the two blocks and the block's own noise.
 *
 * Off (PS3_LIVEAB unset): the first present reads the variable and clears g_ps3_liveab_on; from then on the present
 * path tests one global, and every switch's accessor reads one -1 more than before. Nothing else runs.
 *
 * On: the presenting thread puts each frame's time and draw count into a ring (one store pair); a thread of its own
 * ("ps3-liveab", wakes every 20 ms) reads the ring, runs the schedule, flips the switches, and at the start and end of
 * each block's measured part reads every thread's CPU time (/proc/self/task/<tid>/schedstat, by thread name) and the
 * GPU busy total; it samples the SMU's metrics (gpu_metrics, 10 Hz) while a block is measured. Its own CPU time is in
 * the `other` group: ~0.1 % of one CPU. */
#include "ps3emu/ps3_liveab.h"

volatile int g_ps3_lab[PS3_LAB_N] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
volatile int g_ps3_liveab_on = 1;
volatile unsigned long long g_ps3_liveab_gpu_ns;
volatile unsigned long long g_ps3_liveab_ev;

#if !defined(__linux__)
void ps3_liveab_present(unsigned draws) { (void)draws; g_ps3_liveab_on = 0; }
#else
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#include "../platform/host_place.h"

#define LAB_MAXSW 4
#define LAB_RING 16384u
#define LAB_MAXB 64
#define LAB_DT 8192

static const struct { const char* name; int id; } k_sw[] = {
    { "PS3RECOMP_VK_LIGHT_CULL", PS3_LAB_LIGHT_CULL }, { "PS3_PAD_THREAD_US", PS3_LAB_PAD_US },
    { "PS3_CPU_PLACE_MAIN", PS3_LAB_PLACE_MAIN },      { "PS3_DBG_FLIP_DELAY_US", PS3_LAB_FLIP_DELAY },
    { "PS3_DBG_PRESENT_DELAY_US", PS3_LAB_PRESENT_DELAY }, { "PS3_JOBDELAY_US", PS3_LAB_JOBDELAY_US },
    { "null", PS3_LAB_NULL },              { "PS3_OVERLAY_HUD", PS3_LAB_HUD },
    { "PS3_DBG_DRAW_DELAY_NS", PS3_LAB_DRAW_DELAY }, { "PS3_DBG_DMA_DELAY_NS", PS3_LAB_DMA_DELAY },
    { "PS3_KICKDELAY_US", PS3_LAB_KICKDELAY_US }, { "PS3_LAB_GPUCLK", PS3_LAB_GPUCLK },
    { "PS3_NATIVE_JOBS_STICKY", PS3_LAB_STICKY }, { "PS3_JOBS_PRIO", PS3_LAB_JOBPRIO },
};
/* (sz1) PS3_NATIVE_JOBS_STICKY: the runner's own static, through its exported pointer (absent: the switch does nothing) */
extern int* const g_wws_sticky_ptr __attribute__((weak));
static int s_sticky0 = -1;
static void lab_sticky(int v)
{
    if (!&g_wws_sticky_ptr || !g_wws_sticky_ptr) return;
    if (s_sticky0 < 0) s_sticky0 = *g_wws_sticky_ptr;
    __atomic_store_n(g_wws_sticky_ptr, v < 0 ? s_sticky0 : v, __ATOMIC_RELEASE);
}
/* (sz1) PS3_LAB_GPUCLK: the wanted GPU clock goes to a file a watcher outside the game applies (steamosctl) */
static void lab_gpuclk(int mhz)
{
    const char* p = getenv("PS3_LAB_GPUCLK_FILE");
    if (!p || !*p || mhz <= 0) return;
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%d\n", mhz); fclose(f);
    rename(tmp, p);
}
static struct { int id; int v[2]; const char* name; char txt[2][24]; } s_sw[LAB_MAXSW];
static int s_nsw;
/* a count: seconds, or frames with an f suffix */
typedef struct { double v; int frames; } lab_n;
static lab_n s_block = { 25.0, 0 }, s_settle = { 3.0, 0 }, s_start = { 0.0, 0 };
static int s_nblocks = 12, s_abba = 1, s_first = 0, s_state, s_place0, s_trace;

static struct { uint64_t t; uint32_t d; } s_ring[LAB_RING];
static volatile uint64_t s_w;

static uint64_t lab_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int lab_place_val(const char* s)
{
    if (!strcmp(s, "core") || !strcmp(s, "1")) return 1;
    if (!strcmp(s, "cpu") || !strcmp(s, "2")) return 2;
    return 0;   /* off, x0, 0 */
}

static lab_n lab_count(const char* s)
{
    lab_n n = { atof(s), 0 };
    const size_t l = strlen(s);
    if (l && (s[l - 1] == 'f' || s[l - 1] == 'F')) n.frames = 1;
    return n;
}

/* "<sw>=<a>|<b>[,...];key=val;..." -> 0 ok */
static int lab_parse(const char* spec)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    char* save = NULL;
    for (char* part = strtok_r(buf, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
        while (*part == ' ') part++;
        char* eq = strchr(part, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = part; char* val = eq + 1;
        if (!strcmp(key, "block")) s_block = lab_count(val);
        else if (!strcmp(key, "settle")) s_settle = lab_count(val);
        else if (!strcmp(key, "start")) s_start = lab_count(val);
        else if (!strcmp(key, "blocks")) s_nblocks = atoi(val);
        else if (!strcmp(key, "order")) s_abba = strcmp(val, "ab") != 0;
        else if (!strcmp(key, "first")) s_first = !strcmp(val, "b");
        else if (!strcmp(key, "trace")) s_trace = atoi(val);   /* 1: a [liveab] trace line every 0.5 s (the settle's course) */
        else {
            /* the first switch is <key>=<a>|<b>; more follow after commas: k1=a|b,k2=a|b */
            *eq = '=';
            char* save2 = NULL;
            for (char* sw = strtok_r(part, ",", &save2); sw; sw = strtok_r(NULL, ",", &save2)) {
                char* e2 = strchr(sw, '='); char* bar = e2 ? strchr(e2 + 1, '|') : NULL;
                if (!e2 || !bar || s_nsw >= LAB_MAXSW) { fprintf(stderr, "[liveab] cannot read \"%s\"\n", sw); return -1; }
                *e2 = 0; *bar = 0;
                int id = -1; const char* nm = NULL;
                for (size_t i = 0; i < sizeof k_sw / sizeof k_sw[0]; i++)
                    if (!strcmp(sw, k_sw[i].name)) { id = k_sw[i].id; nm = k_sw[i].name; }
                if (id < 0) { fprintf(stderr, "[liveab] unknown switch %s\n", sw); return -1; }
                s_sw[s_nsw].id = id; s_sw[s_nsw].name = nm;
                const char* v[2] = { e2 + 1, bar + 1 };
                for (int k = 0; k < 2; k++) {
                    snprintf(s_sw[s_nsw].txt[k], sizeof s_sw[s_nsw].txt[k], "%s", v[k]);
                    s_sw[s_nsw].v[k] = id == PS3_LAB_PLACE_MAIN ? lab_place_val(v[k]) : atoi(v[k]);
                }
                s_nsw++;
            }
        }
    }
    if (!s_nsw) { fprintf(stderr, "[liveab] no switch given\n"); return -1; }
    if (s_nblocks < 2) s_nblocks = 2;
    if (s_nblocks > LAB_MAXB) s_nblocks = LAB_MAXB;
    return 0;
}

/* the label of block k: abba = a b b a a b b a ... (pairs (0,1), (2,3), ... alternate their order), ab = a b a b */
static int lab_label(int k) { return ((s_abba ? (k + 1) / 2 : k) + s_first) & 1; }

static void lab_set(int label)
{
    for (int i = 0; i < s_nsw; i++) {
        const int v = s_sw[i].v[label];
        if (s_sw[i].id == PS3_LAB_PLACE_MAIN) ps3_host_place_main_live(v);
        if (s_sw[i].id == PS3_LAB_GPUCLK) lab_gpuclk(v);
        if (s_sw[i].id == PS3_LAB_STICKY) lab_sticky(v);
        __atomic_store_n(&g_ps3_lab[s_sw[i].id], v, __ATOMIC_RELEASE);
    }
}
static void lab_restore(void)
{
    for (int i = 0; i < s_nsw; i++) {
        if (s_sw[i].id == PS3_LAB_PLACE_MAIN) ps3_host_place_main_live(s_place0);
        if (s_sw[i].id == PS3_LAB_STICKY) lab_sticky(-1);
        __atomic_store_n(&g_ps3_lab[s_sw[i].id], -1, __ATOMIC_RELEASE);
    }
}

/* CPU ns per thread group: 0 main (the process thread), 1 rsx, 2 pool (ps3-spuj*), 3 spu (ps3-spu*), 4 other */
#define LAB_G 5
static void lab_cpu(uint64_t out[LAB_G])
{
    memset(out, 0, sizeof(uint64_t) * LAB_G);
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    const pid_t pid = getpid();
    struct dirent* de;
    char path[64], buf[96];
    while ((de = readdir(d)) != NULL) {
        const pid_t tid = (pid_t)atoi(de->d_name);
        if (tid <= 0) continue;
        int g = 4;
        if (tid == pid) g = 0;
        else {
            snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)tid);
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            const ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = 0;
            if (!strncmp(buf, "ps3-rsx", 7)) g = 1;
            else if (!strncmp(buf, "ps3-spuj", 8)) g = 2;
            else if (!strncmp(buf, "ps3-spu", 7)) g = 3;
        }
        snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", (int)tid);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        const ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        out[g] += strtoull(buf, NULL, 10);
    }
    closedir(d);
}

/* the SMU's metrics (amdgpu gpu_metrics v2.x, world-readable; layout as work/bench/mt2-power.py) */
typedef struct { double sock, cpu, soc, gfx, mhz, gfxmhz; int n; } lab_pw;
static void lab_power(lab_pw* p)
{
    unsigned char b[256];
    int fd = open("/sys/class/drm/card0/device/gpu_metrics", O_RDONLY);
    if (fd < 0) return;
    const ssize_t n = read(fd, b, sizeof b);
    close(fd);
    if (n < 0x60) return;
#define U16(o) ((double)(b[(o)] | (b[(o) + 1] << 8)))
    p->sock += U16(0x28); p->cpu += U16(0x2A); p->soc += U16(0x2C); p->gfx += U16(0x2E);
    p->gfxmhz += U16(0x4C);
    p->mhz += (U16(0x58) + U16(0x5A) + U16(0x5C) + U16(0x5E)) / 4.0;
#undef U16
    p->n++;
}

static int lab_fcmp(const void* a, const void* b)
{
    const float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

typedef struct {
    int label; double fps, p99, ms_frame, draws, grp[LAB_G], gpu, W, cpuW, gfxW, socW, mhz, gfxmhz;
    int frames, n33;
} lab_blk;
static lab_blk s_res[LAB_MAXB];
static float s_dt[LAB_DT];

static void lab_pair_summary(int nb)
{
    /* pairs (0,1), (2,3), ...: B against A in each; the mean over the pairs (abba: their orders alternate, so a
     * linear drift of the scene cancels) and its standard error from the pairs' spread */
    double d[LAB_MAXB / 2][6]; int np = 0;
    for (int k = 0; k + 1 < nb; k += 2) {
        const lab_blk* x = &s_res[k]; const lab_blk* y = &s_res[k + 1];
        const lab_blk* A = x->label == 0 ? x : y; const lab_blk* B = x->label == 0 ? y : x;
        if (A->label == B->label || A->fps <= 0 || B->fps <= 0) continue;
        d[np][0] = 100.0 * (B->fps / A->fps - 1.0);
        d[np][1] = B->grp[0] - A->grp[0];
        d[np][2] = B->grp[1] - A->grp[1];
        d[np][3] = B->gpu - A->gpu;
        d[np][4] = B->W - A->W;
        d[np][5] = B->mhz - A->mhz;
        fprintf(stderr, "[liveab] pair %d (blocks %d,%d %s): b vs a fps %+.3f %%  main %+.3f ms  rsx %+.3f ms  gpu %+.3f ms  W %+.3f  MHz %+.1f\n",
                np, k, k + 1, x->label == 0 ? "ab" : "ba", d[np][0], d[np][1], d[np][2], d[np][3], d[np][4], d[np][5]);
        np++;
    }
    if (!np) return;
    static const char* const nm[6] = { "fps %", "main ms", "rsx ms", "gpu ms", "W", "MHz" };
    char line[512]; int o = snprintf(line, sizeof line, "[liveab] done: %d pairs, b vs a:", np);
    for (int c = 0; c < 6; c++) {
        double m = 0, s2 = 0;
        for (int i = 0; i < np; i++) m += d[i][c];
        m /= np;
        for (int i = 0; i < np; i++) s2 += (d[i][c] - m) * (d[i][c] - m);
        const double se = np > 1 ? sqrt(s2 / (np - 1) / np) : 0.0;
        o += snprintf(line + o, sizeof line - (size_t)o, "  %s %+.3f (se %.3f)", nm[c], m, se);
        if (o >= (int)sizeof line) break;
    }
    fprintf(stderr, "%s\n", line);
}

static void* lab_thread(void* arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "ps3-liveab", 0, 0, 0);
    const char* mark = getenv("PS3RECOMP_ENG_WORLD_MARK");
    enum { WAIT_ANCHOR, WAIT_START, SETTLE, MEASURE, DONE } ph = WAIT_ANCHOR;
    uint64_t r = 0, t_anchor = 0, i_anchor = 0, prev_t = 0;
    uint64_t blk_i0 = 0, blk_t0 = 0, m_i0 = 0, m_t0 = 0, m_p0 = 0, gpu0 = 0, ev0 = 0;
    uint64_t cpu0[LAB_G], cpu1[LAB_G];
    double draws = 0; int ndt = 0, k = 0, tick = 0, tr_tick = 0;
    uint64_t tr_r = 0, tr_t = lab_now();
    lab_pw pw;
    memset(&pw, 0, sizeof pw);
    if (!mark || !*mark) { t_anchor = lab_now(); ph = WAIT_START; i_anchor = 0; }
    for (;;) {
        struct timespec ts = { 0, 20 * 1000000L };
        nanosleep(&ts, NULL);
        if (ph == WAIT_ANCHOR) {
            if (access(mark, F_OK) != 0) continue;
            t_anchor = lab_now(); i_anchor = UINT64_MAX; ph = WAIT_START;
            fprintf(stderr, "[liveab] world mark seen: the schedule starts\n");
        }
        if (ph == MEASURE && (tick++ % 5) == 0) lab_power(&pw);
        const uint64_t w = __atomic_load_n(&s_w, __ATOMIC_ACQUIRE);
        if (s_trace && (ph == SETTLE || ph == MEASURE) && ++tr_tick % 25 == 0) {   /* trace=1: every 0.5 s */
            const uint64_t now = lab_now();
            lab_pw one; memset(&one, 0, sizeof one); lab_power(&one);
            if (one.n && now > tr_t)
                fprintf(stderr, "[liveab] trace block %d %c +%.0f ms fps %.2f W %.3f cpuW %.3f gfxW %.3f mhz %.0f gfxmhz %.0f\n",
                        k, "ab"[lab_label(k)], (double)(now - blk_t0) / 1e6, (double)(w - tr_r) * 1e9 / (double)(now - tr_t),
                        one.sock / 1e3, one.cpu / 1e3, one.gfx / 1e3, one.mhz, one.gfxmhz);
            tr_r = w; tr_t = now;
        }
        if (w - r > LAB_RING) r = w - LAB_RING;          /* (never at 20 ms polls; a stalled thread loses frames) */
        for (; r < w && ph != DONE; r++) {
            const uint64_t t = s_ring[r % LAB_RING].t; const uint32_t dr = s_ring[r % LAB_RING].d;
            if (ph == WAIT_START) {
                if (i_anchor == UINT64_MAX) { if (t < t_anchor) { prev_t = t; continue; } i_anchor = r; }
                const int go = s_start.frames ? (double)(r - i_anchor) >= s_start.v : (double)(t - t_anchor) >= s_start.v * 1e9;
                if (!go) { prev_t = t; continue; }
                lab_set(lab_label(0));
                fprintf(stderr, "[liveab] block 0 %c starts\n", "ab"[lab_label(0)]);
                blk_i0 = r; blk_t0 = t; ph = SETTLE;
            }
            if (ph == SETTLE) {
                const int go = s_settle.frames ? (double)(r - blk_i0) >= s_settle.v : (double)(t - blk_t0) >= s_settle.v * 1e9;
                if (go) {
                    m_i0 = r; m_t0 = t; m_p0 = lab_now(); lab_cpu(cpu0); gpu0 = g_ps3_liveab_gpu_ns; ev0 = g_ps3_liveab_ev;
                    memset(&pw, 0, sizeof pw); draws = 0; ndt = 0; tick = 0;
                    ph = MEASURE; prev_t = t;
                    continue;
                }
            }
            if (ph == MEASURE) {
                if (ndt < LAB_DT) s_dt[ndt++] = (float)((double)(t - prev_t) / 1e6);
                draws += dr;
                const int end = s_block.frames ? (double)(r - blk_i0) >= s_block.v : (double)(t - blk_t0) >= s_block.v * 1e9;
                if (end) {
                    const uint64_t p1 = lab_now(); lab_cpu(cpu1); const uint64_t gpu1 = g_ps3_liveab_gpu_ns, ev1 = g_ps3_liveab_ev;
                    lab_blk* b = &s_res[k];
                    memset(b, 0, sizeof *b);
                    b->label = lab_label(k);
                    b->frames = (int)(r - m_i0);
                    const double span = (double)(t - m_t0) / 1e6, pspan = (double)(p1 - m_p0) / 1e6;
                    b->fps = span > 0 ? b->frames * 1000.0 / span : 0;
                    b->ms_frame = b->frames ? span / b->frames : 0;
                    for (int g = 0; g < LAB_G; g++)   /* CPU share of the poll-to-poll span x the mean frame time */
                        b->grp[g] = pspan > 0 ? (double)(cpu1[g] - cpu0[g]) / 1e6 / pspan * b->ms_frame : 0;
                    b->gpu = pspan > 0 ? (double)(gpu1 - gpu0) / 1e6 / pspan * b->ms_frame : 0;
                    b->draws = b->frames ? draws / b->frames : 0;
                    if (ndt) {
                        qsort(s_dt, (size_t)ndt, sizeof s_dt[0], lab_fcmp);
                        b->p99 = s_dt[(int)(0.99 * (ndt - 1))];
                        for (int i = 0; i < ndt; i++) b->n33 += s_dt[i] > 33.4f;
                    }
                    if (pw.n) {
                        b->W = pw.sock / pw.n / 1e3; b->cpuW = pw.cpu / pw.n / 1e3; b->gfxW = pw.gfx / pw.n / 1e3;
                        b->socW = pw.soc / pw.n / 1e3; b->mhz = pw.mhz / pw.n; b->gfxmhz = pw.gfxmhz / pw.n;
                    }
                    char sv[160]; int o = 0; sv[0] = 0;
                    for (int i = 0; i < s_nsw && o < (int)sizeof sv; i++)
                        o += snprintf(sv + o, sizeof sv - (size_t)o, "%s%s=%s", i ? "," : "", s_sw[i].name, s_sw[i].txt[b->label]);
                    fprintf(stderr, "[liveab] block %d %c frames %d ms %.1f fps %.3f p99 %.2f n33 %d draws %.0f main %.3f rsx %.3f "
                                    "pool %.3f spu %.3f other %.3f gpu %.3f W %.3f cpuW %.3f gfxW %.3f socW %.3f mhz %.0f gfxmhz %.0f "
                                    "npw %d ev %.3f sw %s\n",
                            k, "ab"[b->label], b->frames, span, b->fps, b->p99, b->n33, b->draws, b->grp[0], b->grp[1], b->grp[2],
                            b->grp[3], b->grp[4], b->gpu, b->W, b->cpuW, b->gfxW, b->socW, b->mhz, b->gfxmhz, pw.n,
                            pspan > 0 ? (double)(ev1 - ev0) / pspan * b->ms_frame : 0.0, sv);
                    k++;
                    if (k >= s_nblocks) {
                        lab_restore();
                        lab_pair_summary(k);
                        fprintf(stderr, "[liveab] finished: %d blocks, switches back to their own settings\n", k);
                        ph = DONE;
                        break;
                    }
                    lab_set(lab_label(k));
                    fprintf(stderr, "[liveab] block %d %c starts\n", k, "ab"[lab_label(k)]);
                    blk_i0 = r; blk_t0 = t; ph = SETTLE;
                }
                prev_t = t;
            }
        }
        if (ph == DONE) break;
    }
    { struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL); }   /* the restored switches reach the present path first */
    __atomic_store_n(&g_ps3_liveab_on, 0, __ATOMIC_RELEASE);   /* the present path stops calling in */
    return NULL;
}

static void lab_init(void)
{
    s_state = -1;
    const char* e = getenv("PS3_LIVEAB");
    if (!e || !*e || lab_parse(e)) return;
    const char* pm = getenv("PS3_CPU_PLACE_MAIN");
    s_place0 = pm ? lab_place_val(pm) : 0;
    char sv[256]; int o = 0; sv[0] = 0;
    for (int i = 0; i < s_nsw && o < (int)sizeof sv; i++)
        o += snprintf(sv + o, sizeof sv - (size_t)o, "%s%s=%s|%s", i ? ", " : "", s_sw[i].name, s_sw[i].txt[0], s_sw[i].txt[1]);
    fprintf(stderr, "[liveab] on: %s; %d blocks of %g %s (settle %g %s), order %s, first %c, from the world mark + %g %s\n",
            sv, s_nblocks, s_block.v, s_block.frames ? "frames" : "s", s_settle.v, s_settle.frames ? "frames" : "s",
            s_abba ? "abba" : "ab", "ab"[s_first], s_start.v, s_start.frames ? "frames" : "s");
    pthread_t th;
    if (pthread_create(&th, NULL, lab_thread, NULL) != 0) { fprintf(stderr, "[liveab] no thread: off\n"); return; }
    pthread_detach(th);
    s_state = 1;
}

void ps3_liveab_present(unsigned draws)
{
    if (__builtin_expect(s_state == 0, 0)) {
        lab_init();
        if (s_state < 0) { g_ps3_liveab_on = 0; return; }
    }
    const uint64_t w = s_w;
    s_ring[w % LAB_RING].t = lab_now();
    s_ring[w % LAB_RING].d = draws;
    __atomic_store_n(&s_w, w + 1, __ATOMIC_RELEASE);
}
#endif
