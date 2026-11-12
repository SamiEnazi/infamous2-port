/* clang -std=gnu17 -I include libs/system/tests/test_savedata_noicon.c
 * -Wl,-dead_strip -o /tmp/test_savedata_noicon && /tmp/test_savedata_noicon
 * (Windows: gcc -std=gnu17 -I include libs/system/tests/test_savedata_noicon.c -o x.exe)
 *
 * inFAMOUS 2's autosave, as the game does it: funcStat sets the PARAM.SFO,
 * then funcFile asks for WRITE ICON0 from the icon it read at boot, WRITE
 * STATE (a secure file), and ends with OK_LAST_NOCONFIRM. When the icon file
 * was not found the ICON0 write has fileBuf NULL and fileBufSize/fileSize 0:
 * an empty file, not an error (the console's own checks only refuse a NULL
 * buffer that has bytes to move). The save must go through, STATE included. */
#include "../cellSaveData.c"
#include <assert.h>
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
/* Referenced by cellSaveData_register_ctx_handlers, which this test never
 * calls (-dead_strip drops it on macOS; GNU ld keeps it). */
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }

ps3_guest_caller_fn g_ps3_guest_caller;

#define DIR_EA     0x300u   /* the save directory name */
#define NAME_EA    0x340u   /* "STATE" */
#define STATE_EA   0x400u   /* the STATE bytes */
#define SETBUF_EA  0x200u
#define STAT_OPD   0x900u
#define FILE_OPD   0xA00u

static const char state[] = "inFAMOUS 2 STATE payload";
static unsigned step;
static u32 icon_buf_size;   /* the ICON0 write's fileBufSize: 0 = the icon was not found */

static void callback(u32 opd, u64 cb, u64 get, u64 set,
                     u64 a, u64 b, u64 c, u64 d, u64 e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (opd == STAT_OPD) {               /* funcStat: the PARAM.SFO to write */
        strcpy((char*)vm_base + get + 64, "inFAMOUS 2");
        strcpy((char*)vm_base + get + 64 + 128, "(Autosave) 3. Forward Momentum");
        vm_write32((u32)set, (u32)get + 64);
        vm_write32((u32)set + 4, 2);
        vm_write32((u32)cb, CELL_SAVEDATA_CBRESULT_OK_NEXT);
        return;
    }
    assert(opd == FILE_OPD);             /* funcFile: ICON0, STATE, then the end */
    for (u32 i = 0; i < 48; i += 4) vm_write32((u32)set + i, 0);
    if (step == 0) {
        vm_write32((u32)set + 0, CELL_SAVEDATA_FILEOP_WRITE);
        vm_write32((u32)set + 8, CELL_SAVEDATA_FILETYPE_CONTENT_ICON0);
        vm_write32((u32)set + 36, 0);              /* fileSize: the icon's size */
        vm_write32((u32)set + 40, icon_buf_size);  /* fileBufSize */
        vm_write32((u32)set + 44, 0);              /* fileBuf: none */
    } else if (step == 1) {
        vm_write32((u32)set + 0, CELL_SAVEDATA_FILEOP_WRITE);
        vm_write32((u32)set + 8, CELL_SAVEDATA_FILETYPE_SECUREFILE);
        vm_write32((u32)set + 28, NAME_EA);
        vm_write32((u32)set + 36, sizeof state);
        vm_write32((u32)set + 40, sizeof state);
        vm_write32((u32)set + 44, STATE_EA);
    }
    vm_write32((u32)cb, step >= 2 ? CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM : CELL_SAVEDATA_CBRESULT_OK_NEXT);
    step++;
}

static long file_size(const char* root, const char* dir, const char* name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s/%s", root, dir, name);
    FILE* f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void rm(const char* root, const char* dir, const char* name)
{
    char p[512];
    if (name) { snprintf(p, sizeof p, "%s/%s/%s", root, dir, name); remove(p); }
    else { snprintf(p, sizeof p, "%s/%s", root, dir); rmdir(p); }
}

int main(void)
{
    char root[512];
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(root, sizeof root, "%sps3recomp-savedata-noicon-%lu", tmp, (unsigned long)GetCurrentProcessId());
    assert(HOST_MKDIR(root) == 0);
#else
    strcpy(root, "/tmp/ps3recomp-savedata-noicon-XXXXXX");
    assert(mkdtemp(root));
#endif
    strcpy(s_save_root, root);
    vm_base = calloc(1, 0x200000); assert(vm_base);
    assert(cellSaveData_set_scratch_region(0x100000, 0x20000) == CELL_OK);
    strcpy((char*)vm_base + NAME_EA, "STATE");
    memcpy(vm_base + STATE_EA, state, sizeof state);
    vm_write32(SETBUF_EA + 0, 10);   /* dirListMax, as the game sets it */
    vm_write32(SETBUF_EA + 4, 1);    /* fileListMax */
    g_ps3_guest_caller = callback;

    /* 1. No icon (fileBuf NULL, size 0): the save goes through. */
    strcpy((char*)vm_base + DIR_EA, "BCUS98125_INFAMOUS2_0");
    step = 0; icon_buf_size = 0;
    assert(cellSaveDataAutoSave2(0, (const char*)(uintptr_t)DIR_EA, 2, (void*)(uintptr_t)SETBUF_EA,
                                 (void*)(uintptr_t)STAT_OPD, (void*)(uintptr_t)FILE_OPD, 0xFFFFFFFFu,
                                 (void*)7) == CELL_OK);
    assert(step == 3);
    assert(file_size(root, "BCUS98125_INFAMOUS2_0", "STATE") == (long)sizeof state);
    assert(file_size(root, "BCUS98125_INFAMOUS2_0", "ICON0.PNG") == 0);
    assert(file_size(root, "BCUS98125_INFAMOUS2_0", "PARAM.SFO") > 0);

    /* 2. A NULL buffer that claims bytes (fileBufSize 64) is still refused,
     *    before anything else is written (the console's parameter error 73). */
    strcpy((char*)vm_base + DIR_EA, "BCUS98125_INFAMOUS2_1");
    step = 0; icon_buf_size = 64;
    assert(cellSaveDataAutoSave2(0, (const char*)(uintptr_t)DIR_EA, 2, (void*)(uintptr_t)SETBUF_EA,
                                 (void*)(uintptr_t)STAT_OPD, (void*)(uintptr_t)FILE_OPD, 0xFFFFFFFFu,
                                 (void*)7) == CELL_SAVEDATA_ERROR_PARAM);
    assert(step == 1);
    assert(file_size(root, "BCUS98125_INFAMOUS2_1", "STATE") < 0);
    assert(file_size(root, "BCUS98125_INFAMOUS2_1", "PARAM.SFO") < 0);

    rm(root, "BCUS98125_INFAMOUS2_0", "STATE");
    rm(root, "BCUS98125_INFAMOUS2_0", "ICON0.PNG");
    rm(root, "BCUS98125_INFAMOUS2_0", "PARAM.SFO");
    rm(root, "BCUS98125_INFAMOUS2_0", "PARAM.PFD");
    rm(root, "BCUS98125_INFAMOUS2_0", NULL);
    rm(root, "BCUS98125_INFAMOUS2_1", NULL);
    assert(rmdir(root) == 0);
    free(vm_base);
    puts("An autosave whose icon is missing (ICON0 with no buffer and size 0) writes its STATE and PARAM.SFO; "
         "a NULL buffer with bytes is still refused");
    return 0;
}
