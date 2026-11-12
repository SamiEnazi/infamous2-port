/*
 * ps3recomp - sceNpTrophy HLE implementation
 *
 * Trophy management with persistent storage.  Unlocked trophies are saved
 * to a JSON file at gamedata/trophies/{commId}.json.
 *
 * JSON format (hand-written, no external dependency):
 * { "trophies": [ { "id": N, "timestamp": T }, ... ] }
 *
 * That file is the source of truth. From it (and the game's TROPHY.TRP) an
 * RPCS3-layout trophy folder is kept for trophy trackers, RecompLauncher hears
 * of each unlock, and the platinum comes with the last trophy linked to it:
 * see "The game's trophy pack" below.
 */

#include "sceNpTrophy.h"
#include "../system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include "ps3emu/endian.h"                   /* ps3_bswap64 -- guest is big-endian */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
/* HLE args arrive as guest effective addresses; translate before deref. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_trophy_initialized = 0;
static char s_storage_path[512] = "gamedata/trophies";
static int  s_storage_path_set = 0;

/* $PS3_GAMEDATA_ROOT (see cellSaveData.c) moves the default next to the saves;
 * an explicit sceNpTrophySetStoragePath still wins. */
static void trophy_adopt_gamedata_root(void)
{
    static int s_done = 0;
    if (s_done || s_storage_path_set) return;
    s_done = 1;
    const char* env = getenv("PS3_GAMEDATA_ROOT");
    if (env && *env)
        snprintf(s_storage_path, sizeof(s_storage_path), "%s/trophies", env);
}

/* Per-context data */
typedef struct {
    int                      in_use;
    int                      registered;
    SceNpCommunicationId     commId;
    u8                       unlocked[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u64                      unlock_time[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u32                      total_trophies;
} TrophyContext;

typedef struct {
    int in_use;
} TrophyHandle;

/* ID zero is the firmware invalid sentinel; retain all four usable slots. */
static TrophyContext s_contexts[SCE_NP_TROPHY_MAX_CONTEXTS + 1];
static TrophyHandle  s_handles[SCE_NP_TROPHY_MAX_HANDLES + 1];

/* ---------------------------------------------------------------------------
 * Persistent storage helpers
 * -----------------------------------------------------------------------*/

static void trophy_ensure_dir(void)
{
    char tmp[512];
    char* p;

    trophy_adopt_gamedata_root();
    strncpy(tmp, s_storage_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Create directories recursively */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir_p(tmp);
            *p = PATH_SEP;
        }
    }
    mkdir_p(tmp);
}

static void trophy_get_filepath(const SceNpCommunicationId* commId,
                                 char* buf, size_t bufsize)
{
    trophy_adopt_gamedata_root();
    snprintf(buf, bufsize, "%s/%s.json", s_storage_path, commId->data);
}

static void trophy_save(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    int first = 1;

    trophy_ensure_dir();
    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "w");
    if (!fp) {
        printf("[sceNpTrophy] WARNING: Could not save trophies to %s\n",
               filepath);
        return;
    }

    fprintf(fp, "{\n  \"trophies\": [");
    for (u32 i = 0; i < ctx->total_trophies; i++) {
        if (ctx->unlocked[i]) {
            if (!first) fprintf(fp, ",");
            fprintf(fp, "\n    { \"id\": %u, \"timestamp\": %llu }",
                    i, (unsigned long long)ctx->unlock_time[i]);
            first = 0;
        }
    }
    fprintf(fp, "\n  ]\n}\n");
    fclose(fp);
}

static void trophy_load(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    char line[256];

    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "r");
    if (!fp)
        return; /* no saved data, all locked */

    /*
     * Simple JSON parser -- look for "id": N and "timestamp": T patterns.
     * This avoids any external JSON library dependency.
     */
    while (fgets(line, sizeof(line), fp)) {
        const char* id_pos = strstr(line, "\"id\":");
        if (id_pos) {
            u32 tid = 0;
            u64 ts = 0;
            const char* ts_pos;

            if (sscanf(id_pos, "\"id\": %u", &tid) == 1 &&
                tid < SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
                ts_pos = strstr(line, "\"timestamp\":");
                if (ts_pos)
                    sscanf(ts_pos, "\"timestamp\": %llu",
                           (unsigned long long*)&ts);
                ctx->unlocked[tid] = 1;
                ctx->unlock_time[tid] = ts;
            }
        }
    }
    fclose(fp);

    printf("[sceNpTrophy] Loaded trophy data from %s\n", filepath);
}

static u32 trophy_count_unlocked(TrophyContext* ctx)
{
    u32 count = 0;
    for (u32 i = 0; i < ctx->total_trophies; i++)
        if (ctx->unlocked[i]) count++;
    return count;
}

/* ---------------------------------------------------------------------------
 * The game's trophy pack, the RPCS3-layout folder, RecompLauncher, the notice
 *
 * The pack (TROPHY.TRP in PS3_GAME/TROPDIR/<commId>_<nn>/ under the disc root
 * $PS3_VFS_ROOT, or in a dev_hdd0/game folder) is read once, at RegisterContext:
 * each trophy's grade, platinum link and name (trophy_files.c). Without it the
 * trophies keep their numbers and nothing below happens.
 *
 * The folder: <home>/trophy/<commId>_<nn>/ in RPCS3's layout -- TROPCONF.SFM,
 * TROP*.SFM, ICON0.PNG, TROP<nnn>.PNG and TROPUSR.DAT -- so the trophy trackers
 * that read RPCS3's folder (Playnite's SuccessStory and Playnite Achievements)
 * read the port's too. <home> = $PS3_GAMEDATA_ROOT/dev_hdd0/home/00000001, beside
 * the saves (PS3_TROPHY_HOME=<dir> puts it elsewhere). TROPUSR.DAT is rewritten
 * from our own file (the JSON above stays the source of truth) at RegisterContext
 * and after every unlock, through a temporary file and a rename.
 * PS3_TROPHY_FILES=0: no folder.
 *
 * PS3_TROPHY_RPCS3_DIR=<RPCS3's folder, its dev_hdd0 or a home/<user>> (off by
 * default): the unlocks are also merged into RPCS3's own trophy folder for the
 * game, for a tracker that reads only that one. A trophy RPCS3 has stays as it
 * is; one only the port has is added with its time; one both have keeps the
 * earlier time. Only TROPUSR.DAT changes there (a copy of RPCS3's is kept once as
 * TROPUSR.DAT.port-backup), and a file that does not read as one is left alone.
 *
 * RecompLauncher (ps3_achievements.h): each unlock is posted as a native one,
 * the trophies the player already had as earlier ones, the names once.
 *
 * The notice: "Trophy unlocked: <name>" in the in-game menu's corner, in the
 * language PS3_LANG asks for (TROP_<id>.SFM, else TROP.SFM); hidden while
 * RecompLauncher shows its own; PS3_TROPHY_NOTICE=0: none.
 * ------------------------------------------------------------------------*/
#include "trophy_files.h"
#include "ps3emu/ps3_achievements.h"

extern const char* ppu_vfs_root;
#if defined(PS3RECOMP_RSX_VULKAN)
void vk_overlay_notice_for(const char* text, double secs);           /* libs/video/vk_overlay.cpp */
#  define TROPHY_SHOW(text) vk_overlay_notice_for(text, 5.0)
#elif !defined(_WIN32)
void ps3_overlay_notice(const char* text) __attribute__((weak));     /* libs/video/ps3_overlay.mm */
#  define TROPHY_SHOW(text) do { if (ps3_overlay_notice) ps3_overlay_notice(text); } while (0)
#else
#  define TROPHY_SHOW(text) ((void)(text))
#endif

static trp_index   s_trp;
static trophy_set  s_set;          /* text in PS3_LANG's language */
static trophy_set  s_set_en;       /* English text: RecompLauncher's names */
static int         s_set_state;    /* 0 = not looked for, 1 = loaded, -1 = no pack */
static char        s_folder[32];   /* <commId>_<nn> */
static ps3_ach_def s_ach_defs[TROPHY_FILES_MAX];
static int         s_ach_started;
static char        s_files_lock;

static int trophy_env_off(const char* name)
{
    const char* v = getenv(name);
    return v && !strcmp(v, "0");
}

/* PS3 system language ids of the TROP_<id>.SFM files, by PS3_LANG prefix. */
static const char* trp_lang_file(void)
{
    static const struct { const char* lang; const char* file; } k[] = {
        { "ja", "TROP_00.SFM" }, { "fr", "TROP_02.SFM" }, { "es", "TROP_03.SFM" },
        { "de", "TROP_04.SFM" }, { "it", "TROP_05.SFM" }, { "nl", "TROP_06.SFM" },
        { "pt", "TROP_07.SFM" }, { "ru", "TROP_08.SFM" }, { "ko", "TROP_09.SFM" },
        { "zh", "TROP_11.SFM" }, { "fi", "TROP_12.SFM" }, { "sv", "TROP_13.SFM" },
        { "da", "TROP_14.SFM" }, { "no", "TROP_15.SFM" }, { "pl", "TROP_16.SFM" },
    };
    const char* l = getenv("PS3_LANG");
    if (!l) return "TROP.SFM";
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strncmp(l, k[i].lang, 2)) return k[i].file;
    return "TROP.SFM";
}

static void trophy_set_ensure(const TrophyContext* ctx)
{
    if (s_set_state) return;           /* one pack per run: the first context's */
    s_set_state = -1;
    snprintf(s_folder, sizeof s_folder, "%.9s_%02u", ctx->commId.data, (unsigned)ctx->commId.num);
    const char* vfs = ppu_vfs_root && *ppu_vfs_root ? ppu_vfs_root : ".";
    const char* hdd0 = getenv("PS3_HDD0_ROOT");
    char hdd0_buf[1024];
    if (!hdd0 || !*hdd0) { snprintf(hdd0_buf, sizeof hdd0_buf, "%s/dev_hdd0", vfs); hdd0 = hdd0_buf; }
    char trp[1200];
    if (!trophy_find_trp(vfs, hdd0, s_folder, trp, sizeof trp) || !trp_open(trp, &s_trp)) {
        printf("[sceNpTrophy] no TROPHY.TRP for %s: trophies keep their numbers only\n", s_folder);
        return;
    }
    if (trophy_set_load(&s_set, &s_trp, trp_lang_file()) <= 0) {
        printf("[sceNpTrophy] %s holds no trophy list\n", trp);
        return;
    }
    trophy_set_load(&s_set_en, &s_trp, "TROP_01.SFM");   /* English; else TROP.SFM */
    s_set_state = 1;
    printf("[sceNpTrophy] %s: %d trophies (%s) from %s\n", s_folder, s_set.n, s_set.title, trp);
}

static int trophy_is_platinum(int id)
{
    const trophy_def* d = s_set_state == 1 ? trophy_set_find(&s_set, id) : NULL;
    return d && d->grade == 'P';
}

/* The platinum unlocks with the last trophy linked to it (pid), as on a PS3.
 * `when` = its time (0: the latest of the linked ones). Returns its id when it
 * unlocked now, -1 otherwise. */
static int trophy_platinum_check(TrophyContext* ctx, u64 when)
{
    if (s_set_state != 1) return -1;
    int plat = -1;
    for (int i = 0; i < s_set.n; i++)
        if (s_set.t[i].grade == 'P') { plat = s_set.t[i].id; break; }
    if (plat < 0 || plat >= SCE_NP_TROPHY_MAX_NUM_TROPHIES || ctx->unlocked[plat]) return -1;
    int linked = 0;
    u64 latest = 0;
    for (int i = 0; i < s_set.n; i++) {
        const trophy_def* d = &s_set.t[i];
        if (d->id == plat || d->pid != plat) continue;
        if (d->id < 0 || d->id >= SCE_NP_TROPHY_MAX_NUM_TROPHIES || !ctx->unlocked[d->id]) return -1;
        if (ctx->unlock_time[d->id] > latest) latest = ctx->unlock_time[d->id];
        linked++;
    }
    if (!linked) return -1;
    ctx->unlocked[plat] = 1;
    ctx->unlock_time[plat] = when ? when : latest;
    printf("[sceNpTrophy] platinum #%d unlocked: every trophy linked to it is\n", plat);
    return plat;
}

static void trophy_home(char* out, size_t n)
{
    const char* h = getenv("PS3_TROPHY_HOME");
    if (h && *h) { snprintf(out, n, "%s", h); return; }
    const char* g = getenv("PS3_GAMEDATA_ROOT");
    snprintf(out, n, "%s/dev_hdd0/home/00000001", g && *g ? g : "./gamedata");
}

/* The folder(s) from our state. at_start: the pack's files too. */
static void trophy_files_update(TrophyContext* ctx, int at_start)
{
    if (s_set_state != 1 || trophy_env_off("PS3_TROPHY_FILES")) return;
    while (__atomic_test_and_set(&s_files_lock, __ATOMIC_ACQUIRE)) { }
    char home[1024], dir[1200];
    trophy_home(home, sizeof home);
    snprintf(dir, sizeof dir, "%s/trophy/%s", home, s_folder);
    if (at_start) {
        const int w = trophy_folder_install(dir, &s_trp, trp_lang_file());
        if (w < 0) printf("[sceNpTrophy] WARNING: could not write all of the trophy folder %s\n", dir);
        else if (w > 0) printf("[sceNpTrophy] trophy folder (RPCS3's layout, for trophy trackers): %s, %d file(s) written\n", dir, w);
    }
    if (trophy_folder_write_usr(dir, &s_set, ctx->unlocked, ctx->unlock_time, 0) < 0)
        printf("[sceNpTrophy] WARNING: could not write %s/TROPUSR.DAT\n", dir);
    const char* r = getenv("PS3_TROPHY_RPCS3_DIR");
    if (r && *r) {
        char rhome[1024];
        if (!trophy_rpcs3_home(r, rhome, sizeof rhome)) {
            if (at_start) printf("[sceNpTrophy] PS3_TROPHY_RPCS3_DIR=%s: no RPCS3 user folder (dev_hdd0/home/<user>) there\n", r);
        } else {
            snprintf(dir, sizeof dir, "%s/trophy/%s/TROPCONF.SFM", rhome, s_folder);
            FILE* f = fopen(dir, "rb");
            snprintf(dir, sizeof dir, "%s/trophy/%s", rhome, s_folder);
            if (f) fclose(f);
            else if (at_start && trophy_folder_install(dir, &s_trp, trp_lang_file()) < 0)
                printf("[sceNpTrophy] WARNING: could not write RPCS3's trophy folder %s\n", dir);
            const int m = trophy_folder_write_usr(dir, &s_set, ctx->unlocked, ctx->unlock_time, 1);
            if (m < 0) printf("[sceNpTrophy] RPCS3's %s/TROPUSR.DAT is not one this port reads: left as it is\n", dir);
            else if (m > 0) printf("[sceNpTrophy] RPCS3's %s/TROPUSR.DAT updated (a merge: nothing there is locked)\n", dir);
        }
    }
    __atomic_clear(&s_files_lock, __ATOMIC_RELEASE);
}

/* RecompLauncher: the names once, and the trophies the player already had. */
static void trophy_report_start(const TrophyContext* ctx)
{
    if (s_ach_started) return;
    s_ach_started = 1;
    if (s_set_state == 1) {
        int n = 0;
        for (int i = 0; i < s_set_en.n && n < TROPHY_FILES_MAX; i++) {
            const trophy_def* d = &s_set_en.t[i];
            if (d->grade == 'P' || d->id < 0) continue;      /* RecompLauncher leaves the platinum out */
            /* gamerscore: the Xbox-style weight its points come from (10 -> 5, 30 -> 10, 75 -> 25) */
            s_ach_defs[n].id = (uint32_t)d->id;
            s_ach_defs[n].title = d->name;
            s_ach_defs[n].description = d->detail;
            s_ach_defs[n].gamerscore = d->grade == 'G' ? 75u : d->grade == 'S' ? 30u : 10u;
            n++;
        }
        if (n) ps3_ach_publish_defs(s_ach_defs, n);
    }
    for (int id = 0; id < SCE_NP_TROPHY_MAX_NUM_TROPHIES; id++)
        if (ctx->unlocked[id] && !trophy_is_platinum(id)) ps3_ach_post(PS3_ACH_EV_EARLIER, (uint32_t)id);
}

static void trophy_notice(int id)
{
    if (trophy_env_off("PS3_TROPHY_NOTICE")) return;
    const trophy_def* d = s_set_state == 1 ? trophy_set_find(&s_set, id) : NULL;
    const char* l = getenv("PS3_LANG");
    const int pt = l && !strncmp(l, "pt", 2);
    char text[256];
    if (d && d->name[0]) snprintf(text, sizeof text, pt ? "Troféu desbloqueado: %s" : "Trophy unlocked: %s", d->name);
    else snprintf(text, sizeof text, pt ? "Troféu desbloqueado (#%d)" : "Trophy unlocked (#%d)", id);
    printf("[sceNpTrophy] %s\n", text);
    if (!ps3_ach_quiet()) TROPHY_SHOW(text);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

void sceNpTrophySetStoragePath(const char* path)
{
    path = GUEST_PTR(path, const char*);
    if (path) {
        strncpy(s_storage_path, path, sizeof(s_storage_path) - 1);
        s_storage_path[sizeof(s_storage_path) - 1] = '\0';
        s_storage_path_set = 1;
    }
}

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options)
{
    (void)poolPtr; (void)poolSize; (void)containerId; (void)options;

    printf("[sceNpTrophy] Init()\n");

    if (s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 1;
    return CELL_OK;
}

s32 sceNpTrophyTerm(void)
{
    printf("[sceNpTrophy] Term()\n");

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* Save all registered contexts */
    for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (s_contexts[i].in_use && s_contexts[i].registered)
            trophy_save(&s_contexts[i]);
    }

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 0;
    return CELL_OK;
}

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options)
{
    (void)commSign; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!context || !commId)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    const SceNpCommunicationId* commId_h = GUEST_PTR(commId, const SceNpCommunicationId*);

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (!s_contexts[i].in_use) {
            memset(&s_contexts[i], 0, sizeof(TrophyContext));
            s_contexts[i].in_use = 1;
            s_contexts[i].commId = *commId_h;
            s_contexts[i].total_trophies = SCE_NP_TROPHY_MAX_NUM_TROPHIES;
            vm_write32(GUEST_EA(context), (u32)i);
            printf("[sceNpTrophy] CreateContext(commId=\"%s\") -> ctx=%d\n",
                   commId_h->data, i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyContext(SceNpTrophyContext context)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (s_contexts[context].registered)
        trophy_save(&s_contexts[context]);

    s_contexts[context].in_use = 0;
    printf("[sceNpTrophy] DestroyContext(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!handle)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            s_handles[i].in_use = 1;
            vm_write32(GUEST_EA(handle), (u32)i);
            printf("[sceNpTrophy] CreateHandle() -> handle=%d\n", i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    s_handles[handle].in_use = 0;
    printf("[sceNpTrophy] DestroyHandle(handle=%d)\n", handle);
    return CELL_OK;
}

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options)
{
    (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_ALREADY_REG;

    if (!statusCb) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!g_ps3_guest_caller) return SCE_NP_TROPHY_ERROR_UNKNOWN;

    /* Load any previously saved trophy data */
    trophy_load(&s_contexts[context]);
    /* The game's pack; a platinum earned by unlocks of an older build; the
     * RPCS3-layout folder; RecompLauncher's earlier unlocks. */
    trophy_set_ensure(&s_contexts[context]);
    if (trophy_platinum_check(&s_contexts[context], 0) >= 0) trophy_save(&s_contexts[context]);
    trophy_files_update(&s_contexts[context], 1);
    trophy_report_start(&s_contexts[context]);
    s_contexts[context].registered = 1;

    printf("[sceNpTrophy] RegisterContext(ctx=%d, handle=%d, statusCb=0x%08X, "
           "arg=0x%08X) -> loaded saved data\n", context, handle, statusCb, arg);

    /* Use the runner's OPD bridge, not the optional code/TOC scaffold entry
     * point. Registration status is synchronous; its terminal notification is
     * delivered through sysutil polling, after the HLE storage load completes.
     * Intermediate firmware installation progress is not simulated here. */
    g_ps3_guest_caller(statusCb, (u32)context, SCE_NP_TROPHY_STATUS_INSTALLED,
                       0, 0, arg, 0, 0, 0);
    if (!s_trophy_initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!s_contexts[context].in_use) return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
    if (!s_handles[handle].in_use) return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;
    const u64 completion[8] = {(u32)context, SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE,
                               0, 0, arg, 0, 0, 0};
    if (cellSysutilQueueGuestCallbackArgs(statusCb, completion) != CELL_OK) {
        s_contexts[context].registered = 0;
        return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
    }

    return CELL_OK;
}

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options)
{
    (void)handle; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!reqSpace)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    reqSpace = GUEST_PTR(reqSpace, u64*);

    /* Typical trophy pack size. The guest is big-endian and reads this u64
     * back in BE order, so the value MUST be byte-swapped on write -- otherwise
     * a host-LE 0x100000 is read as 0x0010000000000000 (2^44). LBP feeds this
     * "required disk space" straight into its save-data free-space accounting
     * (sub_379EC0: errB = budget - usedKB - reqSpaceKB); a garbage 2^44 makes
     * errB underflow negative -> cellGame GameData check returns ERROR-B ->
     * boot bails. (Same LE-write class as the cellUserInfo fix.) */
    *reqSpace = ps3_bswap64((u64)(1024 * 1024)); /* 1 MB, big-endian */
    printf("[sceNpTrophy] GetRequiredDiskSpace(ctx=%d) -> 1MB\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    details = GUEST_PTR(details, SceNpTrophyGameDetails*);
    data = GUEST_PTR(data, SceNpTrophyGameData*);

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyGameDetails));
        details->numTrophies = 32; /* default trophy set */
        details->numPlatinum = 1;
        details->numGold     = 2;
        details->numSilver   = 8;
        details->numBronze   = 21;
        strncpy(details->title, "PS3 Game",
                SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE - 1);
        strncpy(details->description, "Trophy set",
                SCE_NP_TROPHY_GAME_DESC_MAX_SIZE - 1);
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyGameData));
        data->unlockedTrophies = trophy_count_unlocked(&s_contexts[context]);
        /* Count by grade would require per-trophy grade data;
         * return conservative estimates */
        data->unlockedPlatinum = 0;
        data->unlockedGold     = 0;
        data->unlockedSilver   = 0;
        data->unlockedBronze   = data->unlockedTrophies;
    }

    printf("[sceNpTrophy] GetGameInfo(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    details = GUEST_PTR(details, SceNpTrophyDetails*);
    data = GUEST_PTR(data, SceNpTrophyData*);

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyDetails));
        details->trophyId = (u32)trophyId;
        details->trophyGrade = SCE_NP_TROPHY_GRADE_BRONZE;
        snprintf(details->name, SCE_NP_TROPHY_NAME_MAX_SIZE,
                 "Trophy %d", trophyId);
        snprintf(details->description, SCE_NP_TROPHY_DESC_MAX_SIZE,
                 "Trophy #%d description", trophyId);
        details->hidden = 0;
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyData));
        data->trophyId = (u32)trophyId;
        data->unlocked = s_contexts[context].unlocked[trophyId];
        data->timestamp = s_contexts[context].unlock_time[trophyId];
    }

    return CELL_OK;
}


s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (s_contexts[context].unlocked[trophyId])
        return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;

    /* Unlock the trophy */
    s_contexts[context].unlocked[trophyId] = 1;
    s_contexts[context].unlock_time[trophyId] = (u64)time(NULL);

    printf("************************************************************\n");
    printf("*  TROPHY UNLOCKED!  Trophy #%d                            \n",
           trophyId);
    printf("*  Context: %d  CommId: %s                                 \n",
           context, s_contexts[context].commId.data);
    printf("************************************************************\n");

    /* The platinum comes with the last trophy linked to it (needs the pack's grades) */
    const int plat = trophy_platinum_check(&s_contexts[context], s_contexts[context].unlock_time[trophyId]);

    /* Save immediately; then the RPCS3-layout folder, RecompLauncher, the notice */
    trophy_save(&s_contexts[context]);
    trophy_files_update(&s_contexts[context], 0);
    if (!trophy_is_platinum(trophyId)) ps3_ach_post(PS3_ACH_EV_NATIVE, (uint32_t)trophyId);
    trophy_notice(trophyId);
    if (plat >= 0) trophy_notice(plat);

    if (platinumId)
        vm_write32((u32)(uintptr_t)platinumId, plat >= 0 ? (u32)plat : (u32)SCE_NP_TROPHY_INVALID_TROPHY_ID);

    return CELL_OK;
}

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!flags || !count)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 flags_ea = (u32)(uintptr_t)flags;
    for (u32 o = 0; o < sizeof(SceNpTrophyFlagArray); o += 4)
        vm_write32(flags_ea + o, 0);

    for (u32 i = 0; i < s_contexts[context].total_trophies; i++) {
        if (s_contexts[context].unlocked[i])
            {
            u32 word_ea = flags_ea + (i / 32) * 4;
            vm_write32(word_ea, vm_read32(word_ea) | (1u << (i % 32)));
        }
    }

    vm_write32((u32)(uintptr_t)count, (u32)s_contexts[context].total_trophies);
    return CELL_OK;
}

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!percentage)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 total = s_contexts[context].total_trophies;
    u32 unlocked = trophy_count_unlocked(&s_contexts[context]);

    s32 pct = total > 0 ? (s32)((unlocked * 100) / total) : 0;
    vm_write32(GUEST_EA(percentage), (u32)pct);

    printf("[sceNpTrophy] GetGameProgress(ctx=%d) -> %d%%\n",
           context, pct);
    return CELL_OK;
}
