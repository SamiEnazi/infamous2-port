/*
 * Host thread placement on hybrid CPUs -- see host_place.h.
 */
#include "host_place.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PL_OFF = 0, PL_P, PL_PE, PL_DED, PL_DEDX, PL_OWN };
enum { PL_MAXCPU = 256 };

static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static int   s_mode = PL_OFF;
/* CPU set ids per role; n == 0 = leave the thread to the OS */
static ULONG s_ids[5][PL_MAXCPU];
static ULONG s_n[5];
static __declspec(thread) int t_placed;

static const char* role_name(int r)
{
    static const char* const n[] = { "main", "rsx", "spu", "ppu", "other" };
    return (r >= 0 && r < 5) ? n[r] : "?";
}

static void add(int role, ULONG id) { if (s_n[role] < PL_MAXCPU) s_ids[role][s_n[role]++] = id; }

static void fmt_ids(char* out, size_t cap, int role)
{
    size_t o = 0; out[0] = 0;
    for (ULONG i = 0; i < s_n[role] && o + 8 < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "%s%lu", i ? "," : "", (unsigned long)s_ids[role][i]);
}

/* ---- core budget (perf60) -------------------------------------------------
 * How many CPUs this process may really use: the process affinity mask (set by
 * the user, a launcher, or PS3_HOST_AFFINITY=<hex mask> for tests that emulate
 * a smaller CPU on a big one), counted as logical CPUs and as physical cores.
 * GetSystemInfo's dwNumberOfProcessors ignores the mask, so every "how many
 * workers / may I spin" decision asks here instead. */
static INIT_ONCE s_bud_once = INIT_ONCE_STATIC_INIT;
static int s_bud_logical = 1, s_bud_physical = 1, s_bud_scarce = 0;

static BOOL CALLBACK budget_init(PINIT_ONCE once, PVOID p, PVOID* ctx)
{
    (void)once; (void)p; (void)ctx;
    const char* af = getenv("PS3_HOST_AFFINITY");
    if (af && *af && strcmp(af, "0")) {
        const DWORD_PTR want = (DWORD_PTR)_strtoui64(af, NULL, 16);
        if (want && SetProcessAffinityMask(GetCurrentProcess(), want))
            fprintf(stderr, "[place] PS3_HOST_AFFINITY: process affinity mask 0x%llx\n", (unsigned long long)want);
        else
            fprintf(stderr, "[place] PS3_HOST_AFFINITY=%s FAILED (%lu)\n", af, GetLastError());
    }
    DWORD_PTR pm = 0, sm = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) || !pm) pm = ~(DWORD_PTR)0;
    int logical = 0, physical = 0;
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    unsigned char* buf = (unsigned char*)malloc(len ? len : 1);
    if (buf && len && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        unsigned char seen[256]; memset(seen, 0, sizeof seen);
        for (ULONG off = 0; off < len;) {
            PSYSTEM_CPU_SET_INFORMATION s = (PSYSTEM_CPU_SET_INFORMATION)(buf + off);
            if (s->Type == CpuSetInformation && s->CpuSet.Group == 0 && s->CpuSet.LogicalProcessorIndex < 64 &&
                ((pm >> s->CpuSet.LogicalProcessorIndex) & 1)) {
                logical++;
                if (!seen[s->CpuSet.CoreIndex]) { seen[s->CpuSet.CoreIndex] = 1; physical++; }
            }
            off += s->Size ? s->Size : len;
        }
    }
    free(buf);
    if (logical <= 0) { SYSTEM_INFO si; GetSystemInfo(&si); logical = (int)si.dwNumberOfProcessors; physical = logical > 1 ? logical / 2 : 1; }
    s_bud_logical = logical; s_bud_physical = physical > 0 ? physical : 1;
    /* "scarce": fewer real cores than the game's ~7 busy threads (main, RSX, 5 SPURS
     * workers) plus the helpers. PS3_CORE_SCARCE=0/1 forces it; PS3_CORE_SCARCE_BELOW=<n>
     * moves the line (default 8 physical cores). */
    int below = 8;
    { const char* e = getenv("PS3_CORE_SCARCE_BELOW"); if (e && atoi(e) > 0) below = atoi(e); }
    s_bud_scarce = s_bud_physical < below;
    { const char* e = getenv("PS3_CORE_SCARCE"); if (e && *e) s_bud_scarce = atoi(e) != 0; }
    fprintf(stderr, "[place] core budget: %d logical / %d physical CPUs for this process -> %s\n",
            s_bud_logical, s_bud_physical, s_bud_scarce ? "scarce (waits block)" : "plenty");
    return TRUE;
}

int ps3_host_cpus(void)       { InitOnceExecuteOnce(&s_bud_once, budget_init, NULL, NULL); return s_bud_logical; }
int ps3_host_cores(void)      { InitOnceExecuteOnce(&s_bud_once, budget_init, NULL, NULL); return s_bud_physical; }
int ps3_host_core_scarce(void) { InitOnceExecuteOnce(&s_bud_once, budget_init, NULL, NULL); return s_bud_scarce; }

static BOOL CALLBACK place_init(PINIT_ONCE once, PVOID p, PVOID* ctx)
{
    (void)once; (void)p; (void)ctx;
    (void)ps3_host_cpus();   /* PS3_HOST_AFFINITY before any placement */
    const char* hq = getenv("PS3_HIGHQOS");
    if (hq && atoi(hq) > 0) {
        PROCESS_POWER_THROTTLING_STATE st;
        memset(&st, 0, sizeof st);
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = 0;   /* controlled, not throttled = high QoS */
        BOOL ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
        fprintf(stderr, "[place] PS3_HIGHQOS: process power throttling off %s\n", ok ? "ok" : "FAILED");
    }

    const char* e = getenv("PS3_CPU_PLACE");
    if (!e || !*e || !strcmp(e, "0")) return TRUE;
    if (!strcmp(e, "p")) s_mode = PL_P;
    else if (!strcmp(e, "pe")) s_mode = PL_PE;
    else if (!strcmp(e, "ded")) s_mode = PL_DED;
    else if (!strcmp(e, "dedx")) s_mode = PL_DEDX;
    else if (!strcmp(e, "own")) s_mode = PL_OWN;
    else { fprintf(stderr, "[place] PS3_CPU_PLACE=%s: unknown mode (p|pe|ded|dedx|own)\n", e); return TRUE; }

    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    unsigned char* buf = (unsigned char*)malloc(len ? len : 1);
    if (!buf || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        fprintf(stderr, "[place] GetSystemCpuSetInformation failed (%lu): placement off\n", GetLastError());
        free(buf); s_mode = PL_OFF; return TRUE;
    }
    struct { ULONG id; BYTE core, eff; } cpu[PL_MAXCPU];
    int n = 0;
    BYTE emax = 0, emin = 255;
    for (ULONG off = 0; off < len && n < PL_MAXCPU;) {
        PSYSTEM_CPU_SET_INFORMATION s = (PSYSTEM_CPU_SET_INFORMATION)(buf + off);
        if (s->Type == CpuSetInformation && s->CpuSet.Group == 0) {
            cpu[n].id = s->CpuSet.Id; cpu[n].core = s->CpuSet.CoreIndex; cpu[n].eff = s->CpuSet.EfficiencyClass;
            if (cpu[n].eff > emax) emax = cpu[n].eff;
            if (cpu[n].eff < emin) emin = cpu[n].eff;
            n++;
        }
        off += s->Size ? s->Size : len;
    }
    free(buf);
    const int hybrid = emax != emin;
    if (!hybrid && (s_mode == PL_P || s_mode == PL_PE)) {
        fprintf(stderr, "[place] PS3_CPU_PLACE=%s: one efficiency class only, nothing to do\n", e);
        s_mode = PL_OFF; return TRUE;
    }
    /* the two highest-numbered performance cores for main and RSX (core 0
     * takes most of the device interrupts and DPCs) */
    int pcores[64], npc = 0;
    for (int i = 0; i < n; i++) {
        if (cpu[i].eff != emax) continue;
        int seen = 0;
        for (int k = 0; k < npc; k++) seen |= pcores[k] == cpu[i].core;
        if (!seen && npc < 64) pcores[npc++] = cpu[i].core;
    }
    const int core_main = npc >= 3 ? pcores[npc - 1] : -1;
    const int core_rsx  = npc >= 3 ? pcores[npc - 2] : -1;
    for (int i = 0; i < n; i++) {
        const int isp = cpu[i].eff == emax;
        const ULONG id = cpu[i].id;
        switch (s_mode) {
        case PL_P:
            if (isp) { add(PS3_PLACE_MAIN, id); add(PS3_PLACE_RSX, id); add(PS3_PLACE_SPU, id); }
            break;
        case PL_PE:
            if (isp) { add(PS3_PLACE_MAIN, id); add(PS3_PLACE_RSX, id); add(PS3_PLACE_SPU, id); }
            else     { add(PS3_PLACE_PPU, id); add(PS3_PLACE_OTHER, id); }
            break;
        case PL_OWN:   /* main and RSX own a P-core each; nothing else is confined */
            if (core_main < 0) break;
            if (isp && cpu[i].core == core_main) add(PS3_PLACE_MAIN, id);
            else if (isp && cpu[i].core == core_rsx) add(PS3_PLACE_RSX, id);
            else { add(PS3_PLACE_SPU, id); add(PS3_PLACE_PPU, id); add(PS3_PLACE_OTHER, id); }
            break;
        case PL_DED: case PL_DEDX:
            if (core_main < 0) break;
            if (isp && cpu[i].core == core_main) add(PS3_PLACE_MAIN, id);
            else if (isp && cpu[i].core == core_rsx) add(PS3_PLACE_RSX, id);
            else if (isp) {   /* uniform CPU: the rest share the other cores */
                add(PS3_PLACE_SPU, id);
                if (!hybrid) { add(PS3_PLACE_PPU, id); add(PS3_PLACE_OTHER, id); }
            } else {
                if (s_mode == PL_DEDX) add(PS3_PLACE_SPU, id);
                add(PS3_PLACE_PPU, id); add(PS3_PLACE_OTHER, id);
            }
            break;
        }
    }
    if ((s_mode == PL_DED || s_mode == PL_DEDX || s_mode == PL_OWN) && core_main < 0) {
        fprintf(stderr, "[place] PS3_CPU_PLACE=%s: fewer than 3 performance cores, placement off\n", e);
        s_mode = PL_OFF; return TRUE;
    }

    /* Threads nobody places (driver, audio, Win32 pool) follow the process
     * default, which is the "other" set. */
    if (s_n[PS3_PLACE_OTHER]) {
        BOOL ok = SetProcessDefaultCpuSets(GetCurrentProcess(), s_ids[PS3_PLACE_OTHER], s_n[PS3_PLACE_OTHER]);
        if (!ok) fprintf(stderr, "[place] SetProcessDefaultCpuSets failed (%lu)\n", GetLastError());
    }
    char a[512];
    for (int r = 0; r < 5; r++) {
        fmt_ids(a, sizeof a, r);
        fprintf(stderr, "[place] PS3_CPU_PLACE=%s %-5s -> %s\n", e, role_name(r), s_n[r] ? a : "(OS)");
    }
    return TRUE;
}

/* Per-thread knobs for the frame-critical roles, independent of PS3_CPU_PLACE:
 * PS3_HIGHQOS=1 also turns the thread's own power throttling off
 * (ThreadPowerThrottling, EcoQoS), PS3_PLACE_PRIO=1 raises main and RSX to
 * THREAD_PRIORITY_ABOVE_NORMAL (=2: HIGHEST) so a poller or a driver thread
 * cannot preempt them on their core. */
static void place_thread_knobs(int role)
{
    static int qos = -1, prio = -1;
    if (qos < 0) { const char* e = getenv("PS3_HIGHQOS"); qos = e ? atoi(e) : 0; }
    if (prio < 0) { const char* e = getenv("PS3_PLACE_PRIO"); prio = e ? atoi(e) : 0; }
    const int hot = role == PS3_PLACE_MAIN || role == PS3_PLACE_RSX || role == PS3_PLACE_SPU;
    if (qos > 0 && hot) {
        THREAD_POWER_THROTTLING_STATE st;
        memset(&st, 0, sizeof st);
        st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = 0;
        if (!SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof st))
            fprintf(stderr, "[place] ThreadPowerThrottling(%s) failed (%lu)\n", role_name(role), GetLastError());
    }
    if (prio > 0 && (role == PS3_PLACE_MAIN || role == PS3_PLACE_RSX)) {
        const int p = prio >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL;
        if (!SetThreadPriority(GetCurrentThread(), p))
            fprintf(stderr, "[place] SetThreadPriority(%s) failed (%lu)\n", role_name(role), GetLastError());
        else
            fprintf(stderr, "[place] thread %lu (%s) priority %d\n", GetCurrentThreadId(), role_name(role), p);
    }
}

void ps3_host_place_self(int role)
{
    InitOnceExecuteOnce(&s_once, place_init, NULL, NULL);
    t_placed = 1;
    if (role >= 0 && role <= 4) place_thread_knobs(role);
    if (s_mode == PL_OFF || role < 0 || role > 4) return;
    if (!s_n[role]) {
        /* a role with no set of its own: drop any earlier selection so the
         * thread follows the process default */
        SetThreadSelectedCpuSets(GetCurrentThread(), NULL, 0);
        return;
    }
    if (!SetThreadSelectedCpuSets(GetCurrentThread(), s_ids[role], s_n[role]))
        fprintf(stderr, "[place] SetThreadSelectedCpuSets(%s) failed (%lu)\n", role_name(role), GetLastError());
    else
        fprintf(stderr, "[place] thread %lu -> %s\n", GetCurrentThreadId(), role_name(role));
}

void ps3_host_place_self_once(int role)
{
    if (!t_placed) ps3_host_place_self(role);
}

#else  /* !_WIN32 */
#include <stdlib.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif
static int bud_logical(void)
{
#ifdef __linux__
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) { const int n = CPU_COUNT(&set); if (n > 0) return n; }
#endif
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
int ps3_host_cpus(void)  { return bud_logical(); }
int ps3_host_cores(void) { const int n = bud_logical(); return n > 1 ? n / 2 : 1; }   /* SMT assumed */
int ps3_host_core_scarce(void)
{
    const char* e = getenv("PS3_CORE_SCARCE");
    if (e && *e) return atoi(e) != 0;
    int below = 8;
    e = getenv("PS3_CORE_SCARCE_BELOW"); if (e && atoi(e) > 0) below = atoi(e);
    return ps3_host_cores() < below;
}
void ps3_host_place_self(int role) { (void)role; }
void ps3_host_place_self_once(int role) { (void)role; }
#endif
