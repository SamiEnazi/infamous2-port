/* The RPCS3-layout trophy folder (trophy_files.c) and the trophy HLE around it.
 *
 * A synthetic TROPHY.TRP (4 trophies: platinum 0, gold 1, silver 2, hidden
 * bronze 3; English and French text; icons) is installed into a folder, the
 * unlock state written as TROPUSR.DAT, and the file read back by two readers
 * written here from the trackers' own logic, independently of trophy_files.c:
 *   pa_read  Playnite Achievements (Rpcs3TrophyParser.TryReadTropusrStates): walk
 *            the table headers to type 6; +16 id, +20 state, +40 time 2
 *   ss_read  SuccessStory (Rpcs3Achievements.GetAchievements): the file as hex,
 *            split at "0000000400000050000000" / "0000000600000060000000", the
 *            last <trophy count> pieces; id = first byte, state = chars 18..25,
 *            time = chars 44..57 (the low 7 bytes of time 1), microseconds
 * Then the merge into an "RPCS3" file, the RPCS3 folder lookup, and the HLE:
 * register, unlock, the platinum, the folder, RecompLauncher's events.
 *
 * Windows (the play build's compiler and flags):
 *   clang-cl /std:c17 /FIruntime\platform\win32_posix_min.h -Iinclude -Iruntime\platform\win32_include
 *     -Ilibs\system libs\network\tests\test_trophy_files.c /Fe:test_trophy_files.exe
 * POSIX:
 *   cc -std=gnu17 -Iinclude -Ilibs/system libs/network/tests/test_trophy_files.c -o /tmp/test_trophy_files
 */
#include "../sceNpTrophy.c"
#include "../trophy_files.c"
#include "ps3emu/ps3_cheats.h"
#include <assert.h>

#ifdef _WIN32
#  define TEST_GETPID() ((int)GetCurrentProcessId())
static void test_setenv(const char* k, const char* v) { _putenv_s(k, v ? v : ""); }
#else
#  include <unistd.h>
#  define TEST_GETPID() ((int)getpid())
static void test_setenv(const char* k, const char* v) { if (v) setenv(k, v, 1); else unsetenv(k); }
#endif

/* what the HLE links against in the game */
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
const char* ppu_vfs_root = ".";
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]) { (void)opd; (void)args; return CELL_OK; }
static void status_cb(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7)
{ (void)opd; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; }

/* ---- a synthetic TROPHY.TRP ---- */

static const char kConf[] =
    "<!--Sce-Np-Trophy-Signature: 00-->\n<trophyconf version=\"1.1\">\n"
    " <npcommid>NPWR99999_00</npcommid>\n <trophyset-version>01.00</trophyset-version>\n"
    " <parental-level license-area=\"default\">0</parental-level>\n"
    " <trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"/>\n"
    " <trophy id=\"001\" hidden=\"no\" ttype=\"G\" pid=\"000\"/>\n"
    " <trophy id=\"002\" hidden=\"no\" ttype=\"S\" pid=\"000\"/>\n"
    " <trophy id=\"003\" hidden=\"yes\" ttype=\"B\" pid=\"000\"/>\n"
    "</trophyconf>\n";
#define SFM_TEXT(title, n0, n1, n2, n3) \
    "<!--Sce-Np-Trophy-Signature: 00-->\n<trophyconf version=\"1.1\">\n" \
    " <npcommid>NPWR99999_00</npcommid>\n <trophyset-version>01.00</trophyset-version>\n" \
    " <parental-level license-area=\"default\">0</parental-level>\n" \
    " <title-name>" title "</title-name>\n <title-detail>Trophy set</title-detail>\n" \
    " <trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\">\n  <name>" n0 "</name>\n  <detail>All</detail>\n </trophy>\n" \
    " <trophy id=\"001\" hidden=\"no\" ttype=\"G\" pid=\"000\">\n  <name>" n1 "</name>\n  <detail>Win</detail>\n </trophy>\n" \
    " <trophy id=\"002\" hidden=\"no\" ttype=\"S\" pid=\"000\">\n  <name>" n2 "</name>\n  <detail>Half</detail>\n </trophy>\n" \
    " <trophy id=\"003\" hidden=\"yes\" ttype=\"B\" pid=\"000\">\n  <name>" n3 "</name>\n  <detail>Secret &amp; more</detail>\n </trophy>\n" \
    "</trophyconf>\n"
static const char kEn[] = SFM_TEXT("Test Game", "Platinum", "A &quot;Gold&quot; One", "Silver", "Bronze &lt;3");
static const char kFr[] = SFM_TEXT("Jeu de test", "Platine", "Or", "Argent", "Bronze &#233;t&#xE9;");

typedef struct { const char* name; const void* data; size_t size; } file_spec;

static void write_trp(const char* path, const file_spec* f, int n)
{
    size_t off = 0x40 + (size_t)n * 0x40, total = off;
    for (int i = 0; i < n; i++) total += (f[i].size + 15) & ~(size_t)15;
    uint8_t* b = (uint8_t*)calloc(1, total);
    assert(b);
    wr32(b, 0xDCA24D00u); wr32(b + 4, 1); wr64(b + 8, total); wr32(b + 16, (uint32_t)n); wr32(b + 20, 0x40);
    for (int i = 0; i < n; i++) {
        uint8_t* e = b + 0x40 + (size_t)i * 0x40;
        memcpy(e, f[i].name, strlen(f[i].name));
        wr64(e + 32, off); wr64(e + 40, f[i].size);
        memcpy(b + off, f[i].data, f[i].size);
        off += (f[i].size + 15) & ~(size_t)15;
    }
    FILE* fp = fopen(path, "wb");
    assert(fp && fwrite(b, 1, total, fp) == total);
    fclose(fp);
    free(b);
}

static int file_has(const char* dir, const char* name, const void* data, size_t n)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    size_t got = 0;
    uint8_t* d = trophy_read_file(p, &got);
    const int ok = d && got == n && !memcmp(d, data, n);
    free(d);
    return ok;
}

/* ---- the trackers' readers, written from their code ---- */

typedef struct { int id, unlocked; uint64_t time; } rec;

static int pa_read(const uint8_t* b, size_t len, rec* out)            /* -1 = rejected */
{
    if (len < 0x30 || rd32(b) != 0x818F54ADu) return -1;
    const uint32_t tables = rd32(b + 8);
    if (!tables || tables > (len - 0x30) / 0x20) return -1;
    int seen = 0, n = 0;
    for (uint32_t t = 0; t < tables; t++) {
        const uint8_t* h = b + 0x30 + t * 0x20;
        const uint32_t type = rd32(h), size = rd32(h + 4), count = rd32(h + 12);
        const uint64_t off = rd64(h + 16), esz = (uint64_t)size + 0x10;
        if (off > len || (count && (esz > len || count > (len - off) / esz))) return -1;
        if (type != 6) continue;
        if (seen || size != 0x60) return -1;
        seen = 1;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* e = b + off + i * esz;
            if (rd32(e) != 6 || rd32(e + 4) != 0x60) return -1;
            for (int k = 0; k < n; k++) if (out[k].id == (int)rd32(e + 16)) return -1;
            out[n].id = (int)rd32(e + 16);
            out[n].unlocked = rd32(e + 20) != 0;
            out[n].time = out[n].unlocked ? rd64(e + 40) : 0;
            n++;
        }
    }
    return seen ? n : -1;
}

static int ss_read(const uint8_t* b, size_t len, int trophy_count, rec* out)
{
    char* hex = (char*)malloc(len * 2 + 1);
    assert(hex);
    for (size_t i = 0; i < len; i++) sprintf(hex + i * 2, "%02X", b[i]);   /* Tools.ToHex: upper case */
    hex[len * 2] = 0;
    static const char* const sep[2] = { "0000000400000050000000", "0000000600000060000000" };
    const char* piece[1024];
    size_t plen[1024];
    int np = 0;
    const char* p = hex;
    for (;;) {                               /* String.Split: the earliest separator each time */
        const char* best = NULL;
        size_t bl = 0;
        for (int s = 0; s < 2; s++) {
            const char* f = strstr(p, sep[s]);
            if (f && (!best || f < best)) { best = f; bl = strlen(sep[s]); }
        }
        assert(np < 1024);
        piece[np] = p;
        plen[np] = best ? (size_t)(best - p) : strlen(p);
        np++;
        if (!best) break;
        p = best + bl;
    }
    int n = 0;
    if (np >= trophy_count)
        for (int i = np - trophy_count; i < np; i++) {
            if (plen[i] < 58) continue;
            char t[17];
            memcpy(t, piece[i], 2); t[2] = 0;
            const int id = (int)strtol(t, NULL, 16);
            memcpy(t, piece[i] + 18, 8); t[8] = 0;
            const int on = !strcmp(t, "00000001");
            memcpy(t, piece[i] + 44, 14); t[14] = 0;
            out[n].id = id;
            out[n].unlocked = on;
            out[n].time = on ? strtoull(t, NULL, 16) : 0;
            n++;
        }
    free(hex);
    return n;
}

static const rec* find_rec(const rec* r, int n, int id)
{
    for (int i = 0; i < n; i++) if (r[i].id == id) return &r[i];
    return NULL;
}

static uint8_t* read_usr(const char* dir, size_t* n)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/TROPUSR.DAT", dir);
    return trophy_read_file(p, n);
}

/* CellRtc ticks -> Unix seconds, as .NET's new DateTime(us * 10) then to Unix */
static uint64_t unix_of(uint64_t us) { return us / 1000000ull - 62135596800ull; }

int main(void)
{
    char root[512], disc[600], trp_dir[800], trp[900], dir[900], rpcs3[700], rdir[900];
    snprintf(root, sizeof root, "%s/ps3recomp-trophy-%d",
#ifdef _WIN32
             getenv("TEMP") ? getenv("TEMP") : ".",
#else
             "/tmp",
#endif
             TEST_GETPID());
    snprintf(disc, sizeof disc, "%s/disc", root);
    snprintf(trp_dir, sizeof trp_dir, "%s/PS3_GAME/TROPDIR/NPWR99999_00", disc);
    assert(trophy_mkdirs(trp_dir) == 0);
    snprintf(trp, sizeof trp, "%s/TROPHY.TRP", trp_dir);
    static const uint8_t png[] = { 0x89, 'P', 'N', 'G', 1, 2, 3 };
    static const uint8_t png2[] = { 0x89, 'P', 'N', 'G', 9, 9, 9, 9, 9 };
    const file_spec files[] = {
        { "TROPCONF.SFM", kConf, sizeof kConf - 1 }, { "TROP.SFM", kEn, sizeof kEn - 1 },
        { "TROP_01.SFM", kEn, sizeof kEn - 1 }, { "TROP_02.SFM", kFr, sizeof kFr - 1 },
        { "ICON0.PNG", png2, sizeof png2 }, { "TROP000.PNG", png, sizeof png }, { "TROP001.PNG", png, sizeof png },
        { "TROP002.PNG", png, sizeof png }, { "TROP003.PNG", png2, sizeof png2 },
    };
    write_trp(trp, files, (int)(sizeof files / sizeof files[0]));

    /* 1. the pack */
    static trp_index t;
    static trophy_set fr, en;
    assert(trp_open(trp, &t) && t.n == 9);
    assert(trophy_set_load(&fr, &t, "TROP_02.SFM") == 4 && trophy_set_load(&en, &t, "TROP_99.SFM") == 4);
    assert(!strcmp(fr.comm_id, "NPWR99999_00") && !strcmp(fr.title, "Jeu de test") && !strcmp(en.title, "Test Game"));
    assert(fr.t[0].grade == 'P' && fr.t[0].pid == -1 && fr.t[1].grade == 'G' && fr.t[1].pid == 0);
    assert(fr.t[3].hidden && !fr.t[2].hidden && fr.t[3].grade == 'B');
    assert(!strcmp(en.t[1].name, "A \"Gold\" One") && !strcmp(en.t[3].name, "Bronze <3") && !strcmp(en.t[3].detail, "Secret & more"));
    assert(!strcmp(fr.t[3].name, "Bronze \xC3\xA9t\xC3\xA9"));

    /* 2. the folder */
    snprintf(dir, sizeof dir, "%s/home/00000001/trophy/NPWR99999_00", root);
    assert(trophy_folder_install(dir, &t, "TROP_02.SFM") == 9);   /* TROPCONF.SFM (French) + 8 others */
    assert(file_has(dir, "TROPCONF.SFM", kFr, sizeof kFr - 1) && file_has(dir, "TROP.SFM", kEn, sizeof kEn - 1));
    assert(file_has(dir, "TROP_02.SFM", kFr, sizeof kFr - 1) && file_has(dir, "TROP003.PNG", png2, sizeof png2));
    assert(file_has(dir, "ICON0.PNG", png2, sizeof png2));
    assert(trophy_folder_install(dir, &t, "TROP_02.SFM") == 0);   /* nothing changed: nothing written */
    assert(trophy_folder_install(dir, &t, "TROP_77.SFM") == 1);   /* no such language: TROP.SFM's text */
    assert(file_has(dir, "TROPCONF.SFM", kEn, sizeof kEn - 1));

    /* 3. TROPUSR.DAT from our state, read back as each tracker reads it */
    uint8_t unlocked[TROPHY_FILES_MAX] = { 0 };
    uint64_t secs[TROPHY_FILES_MAX] = { 0 };
    unlocked[1] = 1; secs[1] = 1794438822;          /* 2026-11-11 */
    unlocked[3] = 1; secs[3] = 1794194840;
    assert(trophy_folder_write_usr(dir, &en, unlocked, secs, 0) == 1);
    assert(trophy_folder_write_usr(dir, &en, unlocked, secs, 0) == 0);   /* same bytes: not rewritten */
    size_t n = 0;
    uint8_t* usr = read_usr(dir, &n);
    assert(usr && n == tropusr_size(4) && n == 0x70 + 4 * 0x60 + 4 * 0x70);
    rec pa[8], ss[8];
    assert(pa_read(usr, n, pa) == 4 && ss_read(usr, n, 4, ss) == 4);
    for (int id = 0; id < 4; id++) {
        const rec* a = find_rec(pa, 4, id);
        const rec* s = find_rec(ss, 4, id);
        assert(a && s && a->unlocked == unlocked[id] && s->unlocked == unlocked[id]);
        if (unlocked[id]) assert(unix_of(a->time) == secs[id] && unix_of(s->time) == secs[id]);
    }
    /* the grade table: 1 platinum .. 4 bronze, pid */
    assert(rd32(usr + 0x70 + 0 * 0x60 + 20) == 1 && rd32(usr + 0x70 + 1 * 0x60 + 20) == 2);
    assert(rd32(usr + 0x70 + 3 * 0x60 + 20) == 4 && rd32(usr + 0x70 + 0 * 0x60 + 24) == 0xFFFFFFFFu);
    free(usr);

    /* 4. the merge into someone else's file: nothing locked, the earlier time wins */
    snprintf(rpcs3, sizeof rpcs3, "%s/RPCS3", root);
    snprintf(rdir, sizeof rdir, "%s/dev_hdd0/home/00000002/trophy/NPWR99999_00", rpcs3);
    assert(trophy_mkdirs(rdir) == 0);
    uint8_t theirs[TROPHY_FILES_MAX] = { 0 };
    uint64_t tsecs[TROPHY_FILES_MAX] = { 0 };
    theirs[1] = 1; tsecs[1] = 1794438822 + 3600;    /* later than ours */
    theirs[2] = 1; tsecs[2] = 1700000000;           /* only theirs */
    assert(trophy_folder_write_usr(rdir, &en, theirs, tsecs, 0) == 1);
    usr = read_usr(rdir, &n);
    uint8_t* before = (uint8_t*)malloc(n);
    memcpy(before, usr, n);
    free(usr);
    assert(trophy_folder_write_usr(rdir, &en, unlocked, secs, 1) == 1);
    usr = read_usr(rdir, &n);
    assert(pa_read(usr, n, pa) == 4);
    assert(find_rec(pa, 4, 1)->unlocked && unix_of(find_rec(pa, 4, 1)->time) == secs[1]);    /* earlier */
    assert(find_rec(pa, 4, 2)->unlocked && unix_of(find_rec(pa, 4, 2)->time) == tsecs[2]);   /* kept */
    assert(find_rec(pa, 4, 3)->unlocked && unix_of(find_rec(pa, 4, 3)->time) == secs[3]);    /* added */
    assert(!find_rec(pa, 4, 0)->unlocked);
    free(usr);
    char bak[1000];
    snprintf(bak, sizeof bak, "%s/TROPUSR.DAT.port-backup", rdir);
    usr = trophy_read_file(bak, &n);
    assert(usr && !memcmp(usr, before, n));                                                /* their original */
    free(usr); free(before);
    assert(trophy_folder_write_usr(rdir, &en, unlocked, secs, 1) == 0);                    /* again: no change */
    /* a later unlock of theirs is never moved later, an invalid file is left alone */
    char rusr[1000];
    snprintf(rusr, sizeof rusr, "%s/TROPUSR.DAT", rdir);
    assert(trophy_write_atomic(rusr, "junk", 4) == 0);
    assert(trophy_folder_write_usr(rdir, &en, unlocked, secs, 1) == -1);
    assert(file_has(rdir, "TROPUSR.DAT", "junk", 4));

    /* 5. RPCS3's folder from what a player gives */
    char home[1000], want[1000], p[1000];
    snprintf(p, sizeof p, "%s/GuiConfigs", rpcs3);
    assert(trophy_mkdirs(p) == 0);
    snprintf(p, sizeof p, "%s/GuiConfigs/persistent_settings.dat", rpcs3);
    FILE* fp = fopen(p, "w");
    assert(fp);
    fputs("[Meta]\ncurrentStylesheet=x\n[Users]\nactive_user=00000002\n", fp);
    fclose(fp);
    snprintf(want, sizeof want, "%s/dev_hdd0/home/00000002", rpcs3);
    assert(trophy_rpcs3_home(rpcs3, home, sizeof home) && !strcmp(home, want));          /* the RPCS3 folder */
    assert(trophy_rpcs3_home(want, home, sizeof home) && !strcmp(home, want));           /* home/<user> */
    snprintf(p, sizeof p, "%s/trophy/", want);
    assert(trophy_rpcs3_home(p, home, sizeof home) && !strcmp(home, want));              /* its trophy folder */
    snprintf(p, sizeof p, "%s/nothing", root);
    assert(!trophy_rpcs3_home(p, home, sizeof home));

    /* 6. the HLE: register (an older unlock), unlock the rest, the platinum, the folder, the events */
    char store[700], thome[800], json[900];
    snprintf(store, sizeof store, "%s/store", root);
    assert(trophy_mkdirs(store) == 0);
    snprintf(json, sizeof json, "%s/NPWR99999.json", store);
    fp = fopen(json, "w");
    assert(fp);
    fputs("{\n  \"trophies\": [\n    { \"id\": 2, \"timestamp\": 1790000000 }\n  ]\n}\n", fp);
    fclose(fp);
    snprintf(thome, sizeof thome, "%s/gamedata/dev_hdd0/home/00000001", root);
    test_setenv("PS3_TROPHY_HOME", thome);
    test_setenv("PS3_TROPHY_RPCS3_DIR", rpcs3);
    test_setenv("PS3_LANG", "fr");
    ppu_vfs_root = disc;
    vm_base = (uint8_t*)calloc(1, 65536);
    assert(vm_base);
    strcpy((char*)vm_base + 0x400, store);
    sceNpTrophySetStoragePath((void*)0x400);
    memcpy(vm_base + 0x100, "NPWR99999", 10);                 /* data[9], term, num = 0 */
    g_ps3_guest_caller = status_cb;
    assert(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 0) == CELL_OK);
    assert(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK);
    assert(sceNpTrophyRegisterContext(1, 1, 0x300, 0, 0) == CELL_OK);
    assert(s_set_state == 1 && !strcmp(s_folder, "NPWR99999_00") && !strcmp(s_set.title, "Jeu de test"));
    snprintf(dir, sizeof dir, "%s/trophy/NPWR99999_00", thome);
    assert(file_has(dir, "TROPCONF.SFM", kFr, sizeof kFr - 1));      /* PS3_LANG=fr */
    usr = read_usr(dir, &n);
    assert(pa_read(usr, n, pa) == 4 && find_rec(pa, 4, 2)->unlocked && unix_of(find_rec(pa, 4, 2)->time) == 1790000000);
    free(usr);
    /* RecompLauncher: the names (not the platinum), the earlier unlock */
    assert(g_ps3_ach.n_defs == 3 && g_ps3_ach.defs[0].id == 1 && !strcmp(g_ps3_ach.defs[0].title, "A \"Gold\" One"));
    assert(g_ps3_ach.defs[0].gamerscore == 75 && g_ps3_ach.defs[1].gamerscore == 30 && g_ps3_ach.defs[2].gamerscore == 10);
    int type;
    uint32_t id;
    assert(ps3_ach_take(&type, &id) && type == PS3_ACH_EV_EARLIER && id == 2 && !ps3_ach_take(&type, &id));
    /* RPCS3's folder got 2 merged into the junk-free copy? It holds junk: left alone */
    assert(file_has(rdir, "TROPUSR.DAT", "junk", 4));
    remove(rusr);                                                    /* now missing: written from ours */
    vm_write32(0x208, 0x12345678);
    assert(sceNpTrophyUnlockTrophy(1, 1, 1, (void*)0x208) == CELL_OK);
    assert((s32)vm_read32(0x208) == SCE_NP_TROPHY_INVALID_TROPHY_ID);
    assert(sceNpTrophyUnlockTrophy(1, 1, 1, (void*)0x208) == (s32)SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED);
    assert(sceNpTrophyUnlockTrophy(1, 1, 3, (void*)0x208) == CELL_OK);
    assert(vm_read32(0x208) == 0);                                   /* the platinum came with the last one */
    assert(s_contexts[1].unlocked[0] && s_contexts[1].unlock_time[0] == s_contexts[1].unlock_time[3]);
    assert(ps3_ach_take(&type, &id) && type == PS3_ACH_EV_NATIVE && id == 1);
    assert(ps3_ach_take(&type, &id) && type == PS3_ACH_EV_NATIVE && id == 3);
    assert(!ps3_ach_take(&type, &id));                               /* no platinum for RecompLauncher */
    usr = read_usr(dir, &n);
    assert(pa_read(usr, n, pa) == 4 && ss_read(usr, n, 4, ss) == 4);
    for (int k = 0; k < 4; k++) assert(find_rec(pa, 4, k)->unlocked && find_rec(ss, 4, k)->unlocked);
    free(usr);
    usr = read_usr(rdir, &n);                                        /* RPCS3's: ours, as it had none */
    assert(usr && pa_read(usr, n, pa) == 4);
    for (int k = 0; k < 4; k++) assert(find_rec(pa, 4, k)->unlocked);
    free(usr);
    assert(sceNpTrophyTerm() == CELL_OK);
    /* our own file has all four: the source of truth */
    fp = fopen(json, "r");
    char buf[512];
    const size_t got = fread(buf, 1, sizeof buf - 1, fp);
    buf[got] = 0;
    fclose(fp);
    assert(strstr(buf, "\"id\": 0") && strstr(buf, "\"id\": 1") && strstr(buf, "\"id\": 2") && strstr(buf, "\"id\": 3"));

    /* 7. a cheat used: one CASUAL event; one marked NO_CASUAL or switched off: none */
    {
        static ps3_cheat god = { "god", "God", NULL, "Cole", PS3_CHEAT_TOGGLE, NULL, 0, 0, 0, NULL, NULL, 0, NULL, 0, NULL };
        static ps3_cheat slot = { "slot", "Slot", NULL, "x", PS3_CHEAT_CHOICE, NULL, 0, 0, 0, NULL, NULL, 3, NULL, PS3_CHEAT_NO_CASUAL, NULL };
        ps3_cheat_note_use(&slot, 1);
        slot.value = 2;
        ps3_cheat_note_use(&slot, 1);
        ps3_cheat_note_use(&god, 1);                                 /* off */
        assert(!ps3_ach_take(&type, &id));
        god.value = 1;
        ps3_cheat_note_use(&god, 0);                                 /* not applied */
        assert(!ps3_ach_take(&type, &id));
        ps3_cheat_note_use(&god, 1);
        assert(ps3_ach_take(&type, &id) && type == PS3_ACH_EV_CASUAL && id == PS3_ACH_CASUAL_CHEATS);
    }
    free(vm_base);
    printf("trophy files: pack, folder, TROPUSR.DAT (both trackers' readers), merge, RPCS3 folder, HLE, events: ok\n");
    printf("(left for a look: %s)\n", root);
    return 0;
}
