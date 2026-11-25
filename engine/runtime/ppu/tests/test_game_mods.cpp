/* The Mods page's logic (runtime/ppu/ppu_fs_mods.h + ppu_fs_mods_zip.h,
 * libs/video/mod_preview.h), without the game:
 *  - the zip reader and every refusal, on zips written here: zip-slip,
 *    absolute paths, drive letters, streams, device names, backslash tricks,
 *    a program inside, ZIP64, multi-disk, encryption, unknown compression,
 *    oversized files and totals, too many entries, no / two-deep / two
 *    mod.json, not for inFAMOUS 2, broken JSON, duplicate names, damaged
 *    data (CRC), a local header that disagrees; install, Replace, leftovers;
 *  - the clash plan with priorities, "Use this one instead" and its text,
 *    restart-needed, the build-failure memory;
 *  - mods.ini, old and new format, read and written back;
 *  - the preview picture's path rules and decoding.
 * With a downloads folder (argv[1]) the real community zips are checked and
 * installed too, and with the game (argv[2] = the port's game folder) the
 * texture version check of Sasha's Black Tar runs on the real archives
 * (read only).
 *
 * Windows (clang-cl, as the build): scratchpad build_test_mods.ps1, or
 *   clang-cl /EHsc /std:c++20 /O1 /FIruntime/platform/win32_posix_min.h -Iinclude -Iruntime/ppu
 *     runtime/ppu/tests/test_game_mods.cpp shell32.lib /Fe:test_game_mods.exe
 *   test_game_mods.exe <work folder> [<downloads> [<game folder>]]
 */
#include "../../platform/win32_dirent.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#include "../../../libs/codec/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../../libs/codec/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

extern "C" const char* ppu_vfs_root = ".";
static int g_restarts;
extern "C" void ps3_host_restart(const char*) { g_restarts++; }

#include "../ppu_fs_lang.h"
#include "../ppu_fs_mods.h"
/* the live mods' guest-memory hooks, which the game provides (unused here) */
extern "C" { uint8_t* vm_base = nullptr; void (*g_vm_host_io_hook)(uint32_t, uint32_t, int) = nullptr; }
#include "../../../libs/video/mod_preview.h"

using namespace gmods;

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
#define CHECK_HAS(s, sub) do { const std::string s_ = (s); if (has(s_, sub)) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: \"%s\" lacks \"%s\"\n", __FILE__, __LINE__, s_.c_str(), sub); } } while (0)

static std::string W;   /* the work folder ('/' separated) */

static void put_file(const std::string& p, const std::string& data)
{
    mkdirs(dir_of(p));
    FILE* f = fopen(p.c_str(), "wb");
    if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
}
static std::string get_file(const std::string& p)
{
    Bytes b;
    if (!read_all(p, b)) return std::string("<missing>");
    return std::string(b.begin(), b.end());
}
static void wipe(const std::string& p)   /* the test's own folders only */
{
    if (rel_to(W, p).empty()) return;
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) { RemoveDirectoryA(p.c_str()); DeleteFileA(p.c_str()); return; }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { DeleteFileA(p.c_str()); return; }
    for (const std::string& n : list_dir(p)) wipe(join(p, n));
    RemoveDirectoryA(p.c_str());
#endif
}

/* ---- a zip writer for the crafted cases --------------------------------------- */
struct ZE {
    std::string name, data;
    int method = 8;          /* 0 stored, 8 deflate, else as given (data stored) */
    uint16_t flags = 0;
    bool descriptor = false; /* sizes in a data descriptor (bit 3), as Windows zips */
    bool zip64extra = false;
    uint32_t usize_override = 0, crc_xor = 0;
    std::string local_name;  /* "" = name */
};
struct ZOpts { bool multidisk = false, zip64eocd = false, zip64loc = false; uint16_t count_override = 0; };
static void p16(std::string& s, uint32_t v) { s += (char)(v & 0xFF); s += (char)((v >> 8) & 0xFF); }
static void p32(std::string& s, uint32_t v) { p16(s, v & 0xFFFF); p16(s, v >> 16); }
static std::string deflate_raw(const std::string& d)
{
    int n = 0;
    unsigned char* z = stbi_zlib_compress((unsigned char*)d.data(), (int)d.size(), &n, 8);
    std::string r = n > 6 ? std::string((const char*)z + 2, (size_t)n - 6) : std::string();
    free(z);
    return r;
}
static void write_zip(const std::string& path, const std::vector<ZE>& es, const ZOpts& o = ZOpts())
{
    std::string out, cd;
    for (const ZE& e : es) {
        const uint32_t crc = crc32_of((const uint8_t*)e.data.data(), e.data.size()) ^ e.crc_xor;
        const std::string comp = e.method == 8 ? deflate_raw(e.data) : e.data;
        const uint32_t usize = e.usize_override ? e.usize_override : (uint32_t)e.data.size();
        const uint16_t flags = (uint16_t)(e.flags | (e.descriptor ? 8 : 0));
        const uint32_t lho = (uint32_t)out.size();
        const std::string ln = e.local_name.empty() ? e.name : e.local_name;
        p32(out, 0x04034b50); p16(out, 20); p16(out, flags); p16(out, (uint32_t)e.method); p16(out, 0); p16(out, 0);
        p32(out, e.descriptor ? 0 : crc); p32(out, e.descriptor ? 0 : (uint32_t)comp.size()); p32(out, e.descriptor ? 0 : usize);
        p16(out, (uint32_t)ln.size()); p16(out, 0);
        out += ln;
        out += comp;
        if (e.descriptor) { p32(out, 0x08074b50); p32(out, crc); p32(out, (uint32_t)comp.size()); p32(out, usize); }
        std::string extra;
        if (e.zip64extra) { p16(extra, 0x0001); p16(extra, 8); p32(extra, usize); p32(extra, 0); }
        p32(cd, 0x02014b50); p16(cd, 20); p16(cd, 20); p16(cd, flags); p16(cd, (uint32_t)e.method); p16(cd, 0); p16(cd, 0);
        p32(cd, crc); p32(cd, (uint32_t)comp.size()); p32(cd, usize);
        p16(cd, (uint32_t)e.name.size()); p16(cd, (uint32_t)extra.size()); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, lho);
        cd += e.name;
        cd += extra;
    }
    const uint32_t cd_off = (uint32_t)out.size();
    out += cd;
    if (o.zip64loc) { p32(out, 0x07064b50); p32(out, 0); p32(out, 0); p32(out, 0); p32(out, 1); }
    p32(out, 0x06054b50); p16(out, o.multidisk ? 1 : 0); p16(out, o.multidisk ? 1 : 0);
    const uint16_t n = o.count_override ? o.count_override : (uint16_t)es.size();
    p16(out, o.zip64eocd ? 0xFFFF : n); p16(out, o.zip64eocd ? 0xFFFF : n);
    p32(out, (uint32_t)cd.size()); p32(out, cd_off); p16(out, 0);
    put_file(path, out);
}
static const char* kMJ = "{\"game\": \"inf2\", \"name\": \"Red Jacket\", \"author\": \"t\", \"version\": \"1.0\", \"image\": \"pic.png\"}";

static std::string check_err(const std::vector<ZE>& es, const ZOpts& o = ZOpts(), const char* name = "c.zip")
{
    const std::string p = join(W, std::string("zips/") + name);
    write_zip(p, es, o);
    ZipPlan z;
    zip_check(p, z);
    return z.err;
}

/* a valid tiny DDS: DXT1 4x4, 1 mip (128-byte header + 8 bytes) */
static std::string tiny_dds()
{
    std::string h(128, '\0');
    memcpy(&h[0], "DDS ", 4);
    h[12] = 4; h[16] = 4; h[28] = 1;
    memcpy(&h[84], "DXT1", 4);
    return h + std::string(8, 'x');
}

static void test_zips()
{
    const std::string mods = join(W, "mods");
    wipe(mods); mkdirs(mods);
    g.dir = mods; g.state_path = join(W, "state.ini"); g.cache = join(mods, ".port_cache");
    remove(g.state_path.c_str());

    /* a good one: mod files extracted, the rest left out */
    {
        std::vector<ZE> es;
        ZE d; d.name = "Red Jacket/"; d.method = 0; es.push_back(d);
        ZE a; a.name = "Red Jacket/mod.json"; a.data = kMJ; es.push_back(a);
        ZE b; b.name = "Red Jacket/texture_patches/cole_default.xpps/data00000000_x_4x4_DXT1_m1.dds"; b.data = tiny_dds(); b.method = 0; es.push_back(b);
        ZE c; c.name = "Red Jacket\\readme.txt"; c.data = std::string(5000, 'r'); c.descriptor = true; es.push_back(c);
        ZE e; e.name = "Red Jacket/source.psd"; e.data = "psd"; es.push_back(e);
        ZE f; f.name = "other.txt"; f.data = "o"; es.push_back(f);
        ZE pic; pic.name = "Red Jacket/pic.png"; pic.data = "not really a png"; es.push_back(pic);
        const std::string p = join(W, "zips/Red Jacket v1.zip");
        write_zip(p, es);
        ZipPlan z;
        CHECK(zip_check(p, z));
        CHECK(z.err.empty());
        CHECK(z.folder == "Red Jacket");
        CHECK(z.name == "Red Jacket" && z.version == "1.0" && z.author == "t");
        CHECK(z.files.size() == 4);   /* mod.json, the dds, readme.txt (backslashes), pic.png */
        CHECK(z.left_out == 2);
        CHECK_HAS(z.left_kinds, ".psd");
        CHECK_HAS(z.left_kinds, "outside the mod's folder");
        CHECK(!z.exists);
        std::string err;
        CHECK(zip_install(z, false, err));
        CHECK(get_file(join(mods, "Red Jacket/readme.txt")) == std::string(5000, 'r'));
        CHECK(get_file(join(mods, "Red Jacket/mod.json")) == kMJ);
        CHECK(!finfo(join(mods, "Red Jacket/source.psd")).ok);
        CHECK(!finfo(join(mods, "other.txt")).ok);
        /* again: it is there now -- only Replace */
        ZipPlan z2;
        CHECK(zip_check(p, z2) && z2.exists);
        CHECK(!zip_install(z2, false, err));
        CHECK_HAS(err, "there already");
        /* Replace with v2 */
        es[3].data = "v2 readme";
        const std::string p2 = join(W, "zips/Red Jacket v2.zip");
        write_zip(p2, es);
        ZipPlan z3;
        CHECK(zip_check(p2, z3) && z3.exists);
        CHECK(zip_install(z3, true, err));
        CHECK(get_file(join(mods, "Red Jacket/readme.txt")) == "v2 readme");
        bool left = false;
        for (const std::string& n : list_dir(mods)) if (n[0] == '.') left = true;
        CHECK(!left);   /* no .installing- / .replaced- folder stays */
    }
    /* mod.json at the top: the folder is the zip's name */
    {
        ZE a; a.name = "mod.json"; a.data = kMJ;
        ZE b; b.name = "files/zeke.xpps"; b.data = "PACK";
        const std::string p = join(W, "zips/Top Level.zip");
        write_zip(p, { a, b });
        ZipPlan z;
        CHECK(zip_check(p, z));
        CHECK(z.folder == "Top Level");
        CHECK(z.files.size() == 2);
    }
    ZE mj; mj.name = "X/mod.json"; mj.data = kMJ;
    auto one = [&](const char* name) { ZE e; e.name = name; e.data = "d"; return std::vector<ZE>{ mj, e }; };
    /* names */
    CHECK_HAS(check_err(one("X/../../evil.txt")), "\"..\"");
    CHECK_HAS(check_err(one("../evil.txt")), "\"..\"");
    CHECK_HAS(check_err(one("X/./a.txt")), "\"..\" or \".\"");
    CHECK_HAS(check_err(one("/etc/passwd.txt")), "absolute");
    CHECK_HAS(check_err(one("\\\\server\\share\\a.txt")), "absolute");
    CHECK_HAS(check_err(one("C:/Windows/a.txt")), "drive letter");
    CHECK_HAS(check_err(one("C:a.txt")), "drive letter");
    CHECK_HAS(check_err(one("X\\..\\..\\evil.txt")), "\"..\"");
    CHECK_HAS(check_err(one("X/mod.json:evil")), "':'");
    CHECK_HAS(check_err(one("X/CON.txt")), "device");
    CHECK_HAS(check_err(one("X/nul")), "device");
    CHECK_HAS(check_err(one("X/com1.json")), "device");
    CHECK_HAS(check_err(one("X/a.txt.")), "dot or a space");
    CHECK_HAS(check_err(one("X/a.txt ")), "dot or a space");
    CHECK_HAS(check_err(one("X//a.txt")), "empty");
    CHECK_HAS(check_err(one("X/a?.txt")), "does not allow");
    CHECK_HAS(check_err(one("X/caf\xC3\xA9.txt")), "ASCII");
    {
        std::vector<ZE> v = one("X/a.dds");
        ZE d; d.name = "X/A.DDS"; d.data = "e"; v.push_back(d);
        CHECK_HAS(check_err(v), "two files named");
    }
    {
        std::vector<ZE> v = one("X/a");
        ZE d; d.name = "X/a/b.txt"; d.data = "e"; v.push_back(d);
        CHECK_HAS(check_err(v), "a file and a folder");
    }
    /* programs */
    CHECK_HAS(check_err(one("X/tool.exe")), "contains a program, not a mod");
    CHECK_HAS(check_err(one("X/sub/run.BAT")), "contains a program, not a mod");
    CHECK_HAS(check_err(one("elsewhere/x.dll")), "contains a program, not a mod");
    CHECK_HAS(check_err(one("X/a.ps1")), "contains a program");
    CHECK_HAS(check_err(one("X/a.lnk")), "contains a program");
    CHECK_HAS(check_err(one("X/a.py")), "contains a program");
    {   /* the list shows a program zip only when it has a mod.json (else: "not a mod", counted) */
        ZE ex; ex.name = "Tool/tool.exe"; ex.data = "MZ";
        const std::string p = join(W, "zips/tool.zip");
        write_zip(p, { ex });
        ZipPlan z;
        CHECK(!zip_check(p, z) && z.not_mod);
        CHECK_HAS(z.err, "contains a program, not a mod");
        write_zip(p, one("X/tool.exe"));
        CHECK(!zip_check(p, z) && !z.not_mod);
    }
    /* the container */
    {
        ZOpts o; o.zip64eocd = true;
        std::string e = check_err(one("X/a.txt"), o);
        CHECK_HAS(e, "ZIP64");
        ZOpts o2; o2.zip64loc = true;
        CHECK_HAS(check_err(one("X/a.txt"), o2), "ZIP64");
        std::vector<ZE> v = one("X/a.txt"); v[1].zip64extra = true;
        CHECK_HAS(check_err(v), "ZIP64");
        ZOpts o3; o3.multidisk = true;
        CHECK_HAS(check_err(one("X/a.txt"), o3), "several parts");
    }
    {
        std::vector<ZE> v = one("X/a.txt"); v[1].flags = 1;
        CHECK_HAS(check_err(v), "password");
        v = one("X/a.txt"); v[1].method = 12;
        CHECK_HAS(check_err(v), "compression method");
        v = one("X/a.dds"); v[1].usize_override = 600u << 20;
        CHECK_HAS(check_err(v), "bigger than 512 MB");
        v = { mj };
        for (int k = 0; k < 5; k++) { ZE e; e.name = "X/big" + std::to_string(k) + ".bin"; e.data = "z"; e.usize_override = 450u << 20; v.push_back(e); }
        CHECK_HAS(check_err(v), "more than 2 GB");
        v = { mj };
        for (int k = 0; k < 20001; k++) { ZE e; e.name = "X/" + std::to_string(k) + ".txt"; e.method = 0; v.push_back(e); }
        CHECK_HAS(check_err(v), "more than 20,000");
    }
    /* mod.json */
    {
        ZE e; e.name = "X/a.txt"; e.data = "d";
        const std::string p = join(W, "zips/nomod.zip");
        write_zip(p, { e });
        ZipPlan z;
        CHECK(!zip_check(p, z) && z.not_mod);
        CHECK_HAS(z.err, "no mod.json");
        ZE deep; deep.name = "a/b/mod.json"; deep.data = kMJ;
        write_zip(p, { deep });
        CHECK(!zip_check(p, z) && !z.not_mod);
        CHECK_HAS(z.err, "no mod.json at the top or one folder down");
        ZE m2; m2.name = "Y/mod.json"; m2.data = kMJ;
        CHECK_HAS(check_err({ mj, m2 }), "more than one mod");
        ZE i1; i1.name = "X/mod.json"; i1.data = "{\"game\": \"inf1\"}";
        CHECK_HAS(check_err({ i1 }), "made for inFAMOUS, not inFAMOUS 2");
        ZE ng; ng.name = "X/mod.json"; ng.data = "{\"name\": \"x\"}";
        CHECK_HAS(check_err({ ng }), "does not name the game");
        ZE bj; bj.name = "X/mod.json"; bj.data = "{\"game\": \"inf2\",";
        CHECK_HAS(check_err({ bj }), "not valid JSON");
        ZE big; big.name = "X/mod.json"; big.data = std::string(1100000, ' ') + "{}";
        CHECK_HAS(check_err({ big }), "bigger than 1 MB");
        ZE dot; dot.name = ".hidden/mod.json"; dot.data = kMJ;
        CHECK_HAS(check_err({ dot }), "starts with a dot");
    }
    /* damaged */
    {
        std::vector<ZE> v = one("X/a.txt"); v[1].local_name = "X/b.txt";
        CHECK_HAS(check_err(v), "does not match");
        v = one("X/a.txt"); v[1].data = std::string(3000, 'q'); v[1].crc_xor = 1;
        const std::string p = join(W, "zips/crc.zip");
        write_zip(p, v);
        ZipPlan z;
        CHECK(zip_check(p, z));   /* the CRC is only known by unpacking */
        std::string err;
        CHECK(!zip_install(z, false, err));
        CHECK_HAS(err, "CRC");
        CHECK(!finfo(join(mods, "X")).ok);
        bool left = false;
        for (const std::string& n : list_dir(mods)) if (n[0] == '.') left = true;
        CHECK(!left);
    }
    /* remove_ours takes nothing but its own folders */
    CHECK(!remove_ours(join(mods, "Red Jacket")));
    CHECK(!remove_ours(join(W, ".installing-1")));
    CHECK(finfo(join(mods, "Red Jacket/mod.json")).ok);
}

/* ---- the clash plan ---------------------------------------------------------- */
static Mod mk(const char* folder, std::vector<std::pair<std::string, uint32_t>> tex, const char* whole = nullptr)
{
    Mod m;
    m.folder = folder; m.name = folder; m.path = join(W, std::string("plan/") + folder); m.for_game = true;
    for (auto& t : tex) { Tex x; x.target = t.first; x.off = t.second; x.path = m.path + "/t" + std::to_string(t.second) + ".dds"; m.tex.push_back(x); m.targets.insert(t.first); }
    if (whole) { Part p; p.target = whole; p.path = m.path + "/" + whole; m.files.push_back(p); m.targets.insert(whole); }
    return m;
}
static int idx(const char* folder) { for (size_t i = 0; i < g.list.size(); i++) if (g.list[i].folder == folder) return (int)i; return -1; }
static std::string status_of(const char* folder) { ps3_game_mod m; ps3_game_mods_get(idx(folder), &m); return m.status; }

static void test_plan()
{
    g.dir = join(W, "plan"); g.state_path = join(W, "plan_state.ini");
    remove(g.state_path.c_str());
    g.start.clear(); g.start_sig.clear(); g.use = true;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list = { mk("A", { { "cole_default.xpps", 0x100 } }),
                   mk("B", { { "cole_default.xpps", 0x100 }, { "cole_default.xpps", 0x200 } }),
                   mk("C", { { "cole_default.xpps", 0x200 } }),
                   mk("D", {}, "zeke.xpps"),
                   mk("E", { { "zeke.xpps", 0x10 } }) };   /* E patches what D replaces: clash */
        g.gen++;
    }
    CHECK(mods_clash(g.list[0], g.list[1]) && mods_clash(g.list[1], g.list[0]));
    CHECK(!mods_clash(g.list[0], g.list[2]));
    CHECK(mods_clash(g.list[3], g.list[4]) && mods_clash(g.list[4], g.list[3]));
    {   /* folder order: A, C, D used; B lost to A; E lost to D */
        std::lock_guard<std::mutex> lk(g.mx);
        plan_locked();
        CHECK(g.plan.next[0] && !g.plan.next[1] && g.plan.next[2] && g.plan.next[3] && !g.plan.next[4]);
        CHECK_HAS(g.plan.why[1], "same files as A");
        CHECK(g.plan.prefer[1] == 1);
        CHECK(g.plan.prefer_text[1] == "A and C will be left out");
        CHECK(g.plan.prefer_text[4] == "D will be left out");
    }
    CHECK_HAS(status_of("B"), "Left out from the next start: it changes the same files as A");
    CHECK_HAS(status_of("A"), "Used from the next start");
    CHECK(ps3_game_mods_restart_needed() == 1);   /* nothing used now, A C D next */
    ps3_game_mod b;
    ps3_game_mods_get(1, &b);
    CHECK(b.clash == 1 && !strcmp(b.prefer, "A and C will be left out"));
    /* "Use this one instead" on B */
    CHECK(ps3_game_mods_prefer(1) == 1);
    CHECK(g.list[1].have_prio && g.list[1].prio == 1);
    {
        std::lock_guard<std::mutex> lk(g.mx);
        plan_locked();
        CHECK(!g.plan.next[0] && g.plan.next[1] && !g.plan.next[2] && g.plan.next[3]);
        CHECK(g.plan.prefer[0] == 2);                                   /* above B's 1 */
        CHECK(g.plan.prefer_text[0] == "B will be left out; C will be used");
    }
    CHECK_HAS(get_file(g.state_path), "[B]\npriority = 1\n");
    {   /* C now loses to B: its button would put it above B (2), and A comes back with it */
        std::lock_guard<std::mutex> lk(g.mx);
        plan_locked();
        CHECK(g.plan.prefer[2] == 2);
        CHECK(g.plan.prefer_text[2] == "B will be left out; A will be used");
    }
    CHECK(ps3_game_mods_prefer(3) == 0);   /* D is used: no button, nothing changes */
    /* the order is the saved priorities, then folder order -- deterministic */
    {
        std::vector<Prio> p = { { false, 0 }, { true, 1 }, { false, 0 }, { true, 5 }, { true, 1 } };
        std::vector<size_t> o = mod_order(p);
        CHECK((o == std::vector<size_t>{ 3, 1, 4, 0, 2 }));
    }
    /* restart needed: compare with what "this session" used */
    {
        std::lock_guard<std::mutex> lk(g.mx);
        plan_locked();
        g.start_sig = g.plan.sig;
    }
    CHECK(ps3_game_mods_restart_needed() == 0);
    ps3_game_mods_set_on(3, 0);
    CHECK(ps3_game_mods_restart_needed() == 1);
    ps3_game_mods_set_on(3, 1);
    CHECK(ps3_game_mods_restart_needed() == 0);
#ifdef _WIN32
    _putenv_s("PS3_GAME_MODS", "0");   /* "Use mods" off in the menu */
    CHECK(ps3_game_mods_restart_needed() == 1);
    CHECK_HAS(status_of("A"), "Not used: mods are off");
    _putenv_s("PS3_GAME_MODS", "");
#endif
    /* a build that failed at start stays out while nothing changed */
    {
        std::lock_guard<std::mutex> lk(g.mx);
        StartInfo si; si.fail = true; si.alone = true; si.key = mod_key(g.list[3]); si.why = "could not build zeke.xpps: x";
        g.start["d"] = si;
        g.gen++;
        plan_locked();
        CHECK(!g.plan.next[3]);
        CHECK(g.plan.why[3] == si.why);
        CHECK(g.plan.next[4]);   /* E no longer loses to D */
    }
    CHECK_HAS(status_of("D"), "Not used: could not build zeke.xpps");
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list[3].stamp ^= 1;    /* its files changed: tried again */
        g.gen++;
        plan_locked();
        CHECK(g.plan.next[3]);
    }
}

/* ---- mods.ini ------------------------------------------------------------------- */
static void test_state()
{
    g.state_path = join(W, "state_rt.ini");
    put_file(g.state_path, "# old\n[Sasha's Black Tar]\npick = default,jbintro\n\n[Young Kessler]\non = 0\n[Gone]\non = 0\n");
    std::vector<std::string> zd;
    std::map<std::string, Saved> st = state_read(&zd);
    CHECK(st.size() == 3 && zd.empty());
    CHECK(st["sasha's black tar"].have_pick && st["sasha's black tar"].pick == (std::vector<std::string>{ "default", "jbintro" }));
    CHECK(st["sasha's black tar"].on && !st["sasha's black tar"].have_prio);
    CHECK(st["young kessler"].have_on && !st["young kessler"].on);
    {
        std::lock_guard<std::mutex> lk(g.mx);
        Mod s; s.folder = "Sasha's Black Tar"; s.have_pick = true; s.pick = st["sasha's black tar"].pick;
        Mod y; y.folder = "Young Kessler"; y.on = false;
        Mod d; d.folder = "Doom Kessler";
        g.list = { d, s, y };
        g.zip_dirs.clear();
        state_write_locked();
    }
    const std::string old_out = get_file(g.state_path);
    CHECK_HAS(old_out, "[Sasha's Black Tar]\npick = default,jbintro\n[Young Kessler]\non = 0\n");
    CHECK(!has(old_out, "priority") && !has(old_out, "[Doom Kessler]") && !has(old_out, ".zips"));
    /* new keys: priority and the zip folders, round trip */
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list[0].have_prio = true; g.list[0].prio = 3;
        g.zip_dirs = { "D:/Downloads", "C:/Users/x/Mods zips" };
        state_write_locked();
    }
    const std::string new_out = get_file(g.state_path);
    CHECK_HAS(new_out, "[Doom Kessler]\npriority = 3\n");
    CHECK_HAS(new_out, "[.zips]\nfolder = D:/Downloads\nfolder = C:/Users/x/Mods zips\n");
    zd.clear();
    st = state_read(&zd);
    CHECK(st["doom kessler"].have_prio && st["doom kessler"].prio == 3 && st["doom kessler"].on);
    CHECK(!st.count(".zips"));
    CHECK((zd == std::vector<std::string>{ "D:/Downloads", "C:/Users/x/Mods zips" }));
    CHECK(st["young kessler"].have_on && !st["young kessler"].on);
    /* bad priorities are ignored */
    put_file(g.state_path, "[A]\npriority = 0\n[B]\npriority = -4\n[C]\npriority = x\n[D]\npriority = 7\n");
    st = state_read();
    CHECK(!st["a"].have_prio && !st["b"].have_prio && !st["c"].have_prio && st["d"].have_prio && st["d"].prio == 7);
}

/* ---- preview pictures --------------------------------------------------------- */
static void test_images(const std::string& sample_png, const std::string& sample_jpg)
{
    const std::string mp = join(W, "imgmod");
    wipe(mp);
    mkdirs(join(mp, "sub")); mkdirs(join(mp, "dirpic.png"));
    put_file(join(mp, "pic.png"), get_file(sample_png));
    put_file(join(mp, "sub/pic.jpg"), get_file(sample_jpg));
    put_file(join(W, "outside.png"), get_file(sample_png));
    put_file(join(mp, "empty.png"), "");
    put_file(join(mp, "big.png"), std::string(9u << 20, 'b'));
    put_file(join(mp, "fake.png"), "GIF89a not a png at all....");
#ifdef _WIN32
    {   /* a junction inside the mod, to a folder outside it */
        mkdirs(join(W, "elsewhere"));
        put_file(join(W, "elsewhere/pic.png"), get_file(sample_png));
        std::string a = join(mp, "link"), b = join(W, "elsewhere");
        for (char& c : a) if (c == '/') c = '\\';
        for (char& c : b) if (c == '/') c = '\\';
        const std::string cmd = "cmd /c mklink /J \"" + a + "\" \"" + b + "\" >nul";
        system(cmd.c_str());
    }
#endif
    Mod m;
    m.path = mp;
    auto ok = [&](const char* img) { m.image = img; std::string out, why; return image_path(m, out, &why); };
    auto why_of = [&](const char* img) { m.image = img; std::string out, why; image_path(m, out, &why); return why; };
    CHECK(ok("pic.png"));
    CHECK(ok("sub/pic.jpg"));
    CHECK(ok("sub\\pic.jpg"));
    CHECK(ok("PIC.PNG"));
    CHECK_HAS(why_of("../outside.png"), "\"..\"");
    CHECK_HAS(why_of("sub/../../outside.png"), "\"..\"");
    CHECK_HAS(why_of("/outside.png"), "absolute");
    CHECK_HAS(why_of("\\\\server\\share\\x.png"), "absolute");
    CHECK_HAS(why_of("//server/share/x.png"), "absolute");
    CHECK_HAS(why_of("C:/x.png"), "drive");
    CHECK_HAS(why_of("C:x.png"), "drive");
    CHECK_HAS(why_of("pic.png:stream"), "':'");
    CHECK_HAS(why_of("CON.png"), "device");
    CHECK_HAS(why_of("sub/nul.jpg"), "device");
    CHECK_HAS(why_of("pic.png "), "dot or a space");
    CHECK_HAS(why_of("pic.gif"), "not a .png or .jpg");
    CHECK_HAS(why_of("pic.exe"), "not a .png or .jpg");
    CHECK_HAS(why_of("big.png"), "8 MB");
    CHECK_HAS(why_of("empty.png"), "8 MB");
    CHECK_HAS(why_of("missing.png"), "no such file");
    CHECK_HAS(why_of("dirpic.png"), "no such file");
    CHECK_HAS(why_of(""), "names no picture");
    {
        m.image = std::string("pic.png\0x", 9);
        std::string out, why;
        CHECK(!image_path(m, out, &why));
    }
#ifdef _WIN32
    CHECK(is_link(join(mp, "link")));
    CHECK_HAS(why_of("link/pic.png"), "junction or link");
#endif
    /* decoding: shrunk to 512 at most, aspect kept; refusals */
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    std::string why;
    CHECK(mod_preview_load(sample_png.c_str(), 512, px, w, h, &why) == 1);
    CHECK(w > 0 && h > 0 && w <= 512 && h <= 512 && px.size() == (size_t)w * h * 4);
    CHECK(mod_preview_load(sample_jpg.c_str(), 512, px, w, h, &why) == 1);
    CHECK(w <= 512 && h <= 512);
    CHECK(mod_preview_load(sample_png.c_str(), 64, px, w, h, &why) == 1 && (w == 64 || h == 64));
    CHECK(mod_preview_load(join(mp, "fake.png").c_str(), 512, px, w, h, &why) == 0);
    CHECK_HAS(why, "not a PNG or JPEG");
    CHECK(mod_preview_load(join(mp, "big.png").c_str(), 512, px, w, h, &why) == 0);
    CHECK_HAS(why, "8 MB");
    {   /* a PNG whose header says 10000 x 10000 (100 M pixels): refused before decoding */
        std::string png("\x89PNG\r\n\x1a\n", 8);
        std::string ihdr;
        p32(ihdr, 0); ihdr = std::string("\0\0\0\x0d", 4) + "IHDR";
        std::string body;
        auto be = [](std::string& s, uint32_t v) { s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v; };
        be(body, 10000); be(body, 10000); body += std::string("\x08\x06\x00\x00\x00", 5);
        const uint32_t crc = crc32_of((const uint8_t*)("IHDR" + body).data(), 4 + body.size());
        std::string c; be(c, crc);
        std::string idat = std::string("\0\0\0\0", 4) + "IDAT", ic;
        be(ic, crc32_of((const uint8_t*)"IDAT", 4));
        png += ihdr + body + c + idat + ic;
        put_file(join(mp, "huge.png"), png);
        CHECK(mod_preview_load(join(mp, "huge.png").c_str(), 512, px, w, h, &why) == 0);
        CHECK_HAS(why, "too big");
    }
}

/* ---- the real community zips (read only), and the version check on the game's files ---- */
static void test_real(const std::string& dl, const std::string& game)
{
    const std::string mods = join(W, "realmods");
    wipe(mods); mkdirs(mods);
    g.dir = mods; g.state_path = join(W, "real_state.ini"); g.cache = join(mods, ".port_cache");
    remove(g.state_path.c_str());
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list.clear(); g.start.clear(); g.start_sig.clear(); g.zip_dirs.clear(); g.gen++;
    }
    const char* zips[] = { "Karmic Logo TShirt.zip", "White Coat Reaper.zip", "Climbing Gloves.zip", "Sasha's Black Tar.zip",
                           "Mixed Electricity.zip", "Doom Kessler.zip", "Young Kessler.zip" };
    for (const char* zn : zips) {
        ZipPlan z;
        const bool ok = zip_check(join(dl, zn), z);
        fprintf(stderr, "  real %s: %s -> %s, %zu files, %zu left out (%s)\n", zn, ok ? "ok" : z.err.c_str(), z.folder.c_str(), z.files.size(), z.left_out, z.left_kinds.c_str());
        CHECK(ok);
        std::string err;
        CHECK(zip_install(z, false, err));
        if (!err.empty()) fprintf(stderr, "    %s\n", err.c_str());
    }
    {
        ZipPlan z;
        CHECK(!zip_check(join(dl, "inFAMOUS 2 Ability Manager Latest.zip"), z));
        CHECK_HAS(z.err, "contains a program, not a mod");
        fprintf(stderr, "  real Ability Manager: %s\n", z.err.c_str());
    }
    rescan_now();
    CHECK(ps3_game_mods_count() == 7);
    if (idx("Sasha's Black Tar") < 0 || idx("Doom Kessler") < 0) { CHECK(!"the real mods are installed"); return; }
    for (int i = 0; i < ps3_game_mods_count(); i++) {
        ps3_game_mod m;
        ps3_game_mods_get(i, &m);
        char img[600] = "";
        const int hi = ps3_game_mods_image(i, img, sizeof img);
        fprintf(stderr, "  %s: %s | picture: %s\n", m.name, m.status, hi ? img : "-");
        CHECK(m.for_game);
        if (m.has_image) {
            CHECK(hi == 1);
            std::vector<unsigned char> px; int w = 0, h = 0; std::string why;
            CHECK(mod_preview_load(img, 512, px, w, h, &why) == 1 && w <= 512 && h <= 512);
        }
    }
    CHECK_HAS(status_of("Young Kessler"), "same files as Doom Kessler");
    CHECK_HAS(status_of("Sasha's Black Tar"), "same files as Climbing Gloves, Karmic Logo Tshirt");
    ps3_game_mod s;
    ps3_game_mods_get(idx("Sasha's Black Tar"), &s);
    CHECK(s.clash && !strcmp(s.prefer, "Climbing Gloves and Karmic Logo Tshirt will be left out"));
    fprintf(stderr, "  Sasha's Use this one instead: %s\n", s.prefer);
    if (game.empty()) return;
#ifdef _WIN32
    _putenv_s("PS3_HDD0_ROOT", join(game, "dev_hdd0").c_str());
#endif
    static std::string vfs;
    vfs = join(game, "disc");
    ppu_vfs_root = vfs.c_str();
    Mod m;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        m = g.list[(size_t)idx("Sasha's Black Tar")];
    }
    Arcs arcs;
    const std::vector<std::string> r = check_alone(m, arcs);
    for (const std::string& x : r) fprintf(stderr, "  Sasha alone: %s\n", x.c_str());
    CHECK(r.size() == 1);
    if (!r.empty()) CHECK(r[0] == "15 textures were made for another version of cole_gold_amp.xpps: untick Solid Gold Amp");
    Mod d;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        d = g.list[(size_t)idx("Doom Kessler")];
    }
    CHECK(check_alone(d, arcs).empty());
}

/* ---- merging: merge patches ("patch_for") and automatic packed-file merges ------ */
static void seed_diff(const Mod& m, std::vector<int> changed)   /* what pf_diff would find against the game's file */
{
    PfDiff d;
    d.ok = true; d.changed = changed;
    const Part& f = m.files[0];
    g_pf["v2\n" + sig_line("mod", f.path) + "target " + f.target + "\n"] = d;
}

static void test_merge()
{
    g.dir = join(W, "plan"); g.state_path = join(W, "merge_state.ini");
    remove(g.state_path.c_str());
    g.start.clear(); g.start_sig.clear(); g.use = true;
    Mod a = mk("MA", {}, "i2.core.xpps"), b = mk("MB", {}, "i2.core.xpps"), p = mk("MP", {}, "i2.core.xpps");
    p.patch_for = { "MA", "MB" };
    Mod c1 = mk("MC1", {}, "zeke.xpps"), c2 = mk("MC2", {}, "zeke.xpps"), c3 = mk("MC3", {}, "zeke.xpps");
    Mod t = mk("MT", { { "zeke.xpps", 0x40 } });
    seed_diff(a, { 1, 2 }); seed_diff(b, { 2, 3 }); seed_diff(p, { 1, 2, 3 });
    seed_diff(c1, { 5 }); seed_diff(c2, { 6 }); seed_diff(c3, { 6, 7 });
    /* automatic: different packed files merge, the same one clashes, a texture patch always does */
    CHECK(!mods_clash(c1, c2) && !mods_clash(c2, c1));
    CHECK(mods_clash(c2, c3) && mods_clash(c3, c2));
    CHECK(mods_clash(c1, t) && mods_clash(t, c1));
    Xp x; x.target = "zeke.xpps"; x.index = 7; x.chunk = true; x.choff = 0; x.len = 4;
    Mod xp7 = mk("MX", {}); xp7.xp.push_back(x); xp7.targets.insert("zeke.xpps");
    CHECK(!mods_clash(c1, xp7) && mods_clash(c3, xp7));   /* a packed-file patch onto a file another mod replaces */
    CHECK(mods_clash(a, b));
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list = { a, b, c1, c2, p };
        g.gen++;
        plan_locked();
        CHECK(g.plan.next[0] && g.plan.next[1] && g.plan.next[2] && g.plan.next[3] && g.plan.next[4]);   /* MP merges MA + MB */
        std::vector<size_t> used = { 4, 0, 1 };
        const std::map<size_t, std::set<std::string>> cv = patch_cover(g.list, used);
        CHECK(cv.count(0) && cv.at(0).count("i2.core.xpps") && cv.count(1) && !cv.count(4));
        g.list[1].on = false;   /* one of its mods off: the patch steps aside */
        g.gen++;
        plan_locked();
        CHECK(g.plan.next[0] && !g.plan.next[1] && !g.plan.next[4]);
        CHECK_HAS(g.plan.why[4], "a merge patch, used only while MA and MB are used");
        g.list[1].on = true; g.list[4].on = false;   /* the patch off: they clash again */
        g.gen++;
        plan_locked();
        CHECK(g.plan.next[0] && !g.plan.next[1]);
        CHECK_HAS(g.plan.why[1], "same files as MA");
        g.list.clear();
        g.gen++;
    }
    g_pf.clear();
}


/* ---- conflicts: byte-by-byte merges and the player's choices, end to end ------- */
static void be32_put(std::string& s, size_t at, uint32_t v) { s[at] = (char)(v >> 24); s[at + 1] = (char)(v >> 16); s[at + 2] = (char)(v >> 8); s[at + 3] = (char)v; }
/* An XPPS PACK file: one chunk, packed files of the given bytes. */
static std::string mk_xpps(const std::vector<std::string>& files)
{
    const size_t tab = 48, nf = files.size(), data = tab + 24 + 28 + nf * 16;
    std::string d(data, '\0');
    memcpy(&d[0], "PACK", 4);
    uint32_t off = 0;
    for (size_t i = 0; i < nf; i++) {
        const size_t r = tab + 24 + 28 + i * 16;
        be32_put(d, r + 4, (uint32_t)files[i].size()); be32_put(d, r + 8, off);
        off += (uint32_t)files[i].size();
    }
    be32_put(d, 24, (uint32_t)tab); be32_put(d, 40, (uint32_t)data); be32_put(d, 44, off);
    be32_put(d, tab + 4, 1); be32_put(d, tab + 12, (uint32_t)nf);
    be32_put(d, tab + 24 + 4, off); be32_put(d, tab + 24 + 8, 0); be32_put(d, tab + 24 + 20, 0); be32_put(d, tab + 24 + 24, (uint32_t)nf);
    for (const std::string& f : files) d += f;
    return d;
}
/* A PSARC with these files, stored (not compressed). */
static std::string mk_psarc(const std::vector<std::pair<std::string, std::string>>& files)
{
    std::vector<std::string> ent;
    std::string names;
    for (auto& f : files) names += "/" + f.first + "\n";
    ent.push_back(names);
    for (auto& f : files) ent.push_back(f.second);
    const uint32_t bsz = 65536, n = (uint32_t)ent.size();
    std::vector<uint32_t> blocks;
    std::vector<uint32_t> first;
    for (const std::string& e : ent) { first.push_back((uint32_t)blocks.size()); for (size_t q = 0; q < e.size() || q == 0; q += bsz) { blocks.push_back((uint32_t)(e.size() - q < bsz ? e.size() - q : 0)); if (e.empty()) break; } }
    const uint32_t toclen = 32 + n * 30 + (uint32_t)blocks.size() * 2;
    std::string h(32, '\0');
    memcpy(&h[0], "PSAR", 4); be32_put(h, 4, 0x00010004); memcpy(&h[8], "zlib", 4);
    be32_put(h, 12, toclen); be32_put(h, 16, 30); be32_put(h, 20, n); be32_put(h, 24, bsz);
    std::string toc(n * 30, '\0'), tab;
    uint64_t off = toclen;
    for (uint32_t i = 0; i < n; i++) {
        std::string& r = toc;
        const size_t q = (size_t)i * 30;
        be32_put(r, q + 16, first[i]);
        const uint64_t usz = ent[i].size();
        for (int k = 0; k < 5; k++) { r[q + 20 + k] = (char)(usz >> (8 * (4 - k))); r[q + 25 + k] = (char)(off >> (8 * (4 - k))); }
        off += ent[i].size();
    }
    for (uint32_t b : blocks) { tab += (char)(b >> 8); tab += (char)b; }
    std::string out = h + toc + tab;
    for (const std::string& e : ent) out += e;
    return out;
}

static void test_conflicts()
{
    /* the byte merge itself */
    {
        const uint8_t o[4] = { 1, 2, 3, 4 };
        Bytes a = { 9, 2, 3, 4 }, b = { 1, 2, 3, 8 }, c = { 7, 2, 3, 4 }, m;
        CHECK(merge_bytes(o, 4, a, b, m) && m == Bytes({ 9, 2, 3, 8 }));
        CHECK(!merge_bytes(o, 4, a, c, m));           /* the same byte, two ways */
        CHECK(merge_bytes(o, 4, a, a, m) && m == a);  /* the same change */
        CHECK(!merge_bytes(o, 4, a, Bytes({ 1, 2, 3 }), m));
    }
    {   /* with changes of size: an insertion and a change elsewhere both kept; the same place: only with a preference */
        Bytes o(4000);
        uint32_t x = 12345;
        for (uint8_t& c : o) { x = x * 1103515245u + 12345u; c = (uint8_t)(x >> 16); }
        Bytes a = o, b = o, m, want = o;
        const Bytes ins(37, 0xEE);
        a.insert(a.begin() + 1000, ins.begin(), ins.end());     /* A inserts 37 bytes at 1000 */
        b[3000] ^= 0xFF; b.erase(b.begin() + 3500, b.begin() + 3510);   /* B changes 3000, removes 3500..3510 */
        want[3000] ^= 0xFF; want.erase(want.begin() + 3500, want.begin() + 3510); want.insert(want.begin() + 1000, ins.begin(), ins.end());
        CHECK(merge3(o.data(), o.size(), a, b, -1, m) && m == want);
        CHECK(merge3(o.data(), o.size(), b, a, -1, m) && m == want);
        Bytes c = o; c.insert(c.begin() + 1000, 5, 0x11);      /* C inserts elsewhere-sized bytes at the same place */
        int ov = 0;
        CHECK(!merge3(o.data(), o.size(), a, c, -1, m));
        CHECK(merge3(o.data(), o.size(), a, c, 1, m, &ov) && ov == 1 && m == c);
        CHECK(merge3(o.data(), o.size(), a, c, 0, m, &ov) && ov == 1 && m == a);
        Bytes d = o; d[10] = (uint8_t)~d[10];                  /* same size both, same byte: preferred one's */
        Bytes e = o; e[10] = (uint8_t)(d[10] ^ 1); e[20] ^= 1;
        CHECK(merge3(o.data(), o.size(), d, e, 0, m, &ov) && ov == 1 && m[10] == d[10] && m[20] == e[20]);
    }
    const std::string root = join(W, "cf");
    wipe(root);
    mkdirs(join(root, "game/PS3_GAME/USRDIR"));
    std::vector<std::string> pf;
    for (int i = 0; i < 3; i++) { std::string x(16, '\0'); for (int k = 0; k < 16; k++) x[k] = (char)(i * 16 + k); pf.push_back(x); }
    put_file(join(root, "game/PS3_GAME/USRDIR/infamous2.psarc_s"), mk_psarc({ { "zeke.xpps", mk_xpps(pf) } }));
    static std::string vfs;
    vfs = join(root, "game");
    ppu_vfs_root = vfs.c_str();
    { std::lock_guard<std::mutex> lk(g_pf_mx); g_pf_arcs = Arcs(); g_pf.clear(); g_bm.clear(); }
    g.dir = join(root, "mods"); g.cache = join(g.dir, ".port_cache"); g.state_path = join(root, "cf_state.ini");
    g.start.clear(); g.start_sig.clear(); g.use = true;
    std::vector<std::string> fa = pf, fb = pf;
    fa[1][0] = (char)0xA1; fa[2][3] = (char)0xA2;   /* A: packed file 1 byte 0, packed file 2 byte 3 */
    fb[1][5] = (char)0xB1; fb[2][3] = (char)0xB2;   /* B: packed file 1 byte 5 (merges), packed file 2 byte 3 (conflicts) */
    Mod a = mk("CA", {}, "zeke.xpps"), b = mk("CB", {}, "zeke.xpps");
    a.path = join(g.dir, "CA"); b.path = join(g.dir, "CB");
    a.files[0].path = join(a.path, "zeke.xpps"); b.files[0].path = join(b.path, "zeke.xpps");
    put_file(a.files[0].path, mk_xpps(fa)); put_file(b.files[0].path, mk_xpps(fb));
    std::vector<Clash> cl;
    clash_items(a, b, cl);
    CHECK(cl.size() == 2);
    if (cl.size() == 2) {
        CHECK(cl[0].item == "p1" && cl[0].merges);
        CHECK(cl[1].item == "p2" && !cl[1].merges);
    }
    CHECK(mods_clash(a, b));
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list = { a, b };
        g.gen++;
        plan_locked();
        CHECK(g.plan.next[0] && !g.plan.next[1]);
        CHECK(g.plan.unres[0] == 1 && g.plan.unres[1] == 1);
    }
    ps3_game_mod gm;
    CHECK(ps3_game_mods_get(1, &gm) && gm.conflicts == 1 && !strcmp(gm.conflicts_with, "CA"));
    ps3_game_mod_conflict c[8];
    CHECK(ps3_game_mods_conflicts(1, c, 8) == 2);
    CHECK(c[0].merges && !c[1].merges && c[1].winner == -1 && c[1].other == 0 && !strcmp(c[1].what, "zeke.xpps, packed file 2"));
    const int w1[2] = { -1, 0 };   /* CB's version of packed file 2 */
    CHECK(ps3_game_mods_resolve(1, w1, 2) == 2);   /* not live: a restart */
    CHECK(ps3_game_mods_conflicts(1, c, 8) == 2 && c[1].winner == 0);
    CHECK(ps3_game_mods_conflicts(0, c, 8) == 2 && c[1].winner == 1);
    CHECK(ps3_game_mods_get(1, &gm) && gm.conflicts == 0 && gm.next && !strcmp(gm.merged_with, "CA"));
    {   /* saved both ways in mods.ini, and read back */
        const std::map<std::string, Saved> st = state_read();
        CHECK(st.count("cb") && st.at("cb").res.count("ca|zeke.xpps|p2") && st.at("cb").res.at("ca|zeke.xpps|p2"));
        CHECK(st.count("ca") && st.at("ca").res.count("cb|zeke.xpps|p2") && !st.at("ca").res.at("cb|zeke.xpps|p2"));
        CHECK_HAS(get_file(g.state_path), "win = ca|zeke.xpps|p2");
    }
    auto served = [&]() {
        std::vector<Mod> ms;
        { std::lock_guard<std::mutex> lk(g.mx); ms = g.list; }
        resolve(ms);
        std::lock_guard<std::mutex> lk(g.mx);
        auto it = g.serve.find("zeke.xpps");
        return it == g.serve.end() ? std::string() : get_file(it->second);
    };
    {
        std::vector<std::string> want = pf;
        want[1][0] = (char)0xA1; want[1][5] = (char)0xB1; want[2][3] = (char)0xB2;
        const std::string got = served();
        CHECK(got == mk_xpps(want));
        CHECK(g.start["ca"].active && g.start["cb"].active);
    }
    const int w2[2] = { -1, 1 };   /* now CA's */
    CHECK(ps3_game_mods_resolve(1, w2, 2) == 2);
    {
        std::vector<std::string> want = pf;
        want[1][0] = (char)0xA1; want[1][5] = (char)0xB1; want[2][3] = (char)0xA2;
        CHECK(served() == mk_xpps(want));
        CHECK(!ps3_game_mods_restart_needed());   /* this "session" now uses what is saved */
    }
    {   /* packed files of other sizes combine too: CA grows packed file 0, CB changes it elsewhere */
        std::vector<std::string> base = pf;   /* packed file 0 made long and varied, as a real one */
        { uint32_t x = 777; base[0].resize(800); for (char& ch : base[0]) { x = x * 1103515245u + 12345u; ch = (char)(x >> 16); } }
        put_file(join(root, "game/PS3_GAME/USRDIR/infamous2.psarc_s"), mk_psarc({ { "zeke.xpps", mk_xpps(base) } }));
        { std::lock_guard<std::mutex> lk(g_pf_mx); g_pf_arcs = Arcs(); }
        std::vector<std::string> ga = base, gb = base;
        ga[0].insert(2, "INSERTED-BY-CA");
        gb[0][300] = (char)0xB0;
        /* new files (the caches key on name, size and time: a rewrite in the same second would look the same) */
        auto put2 = [&](int which, const std::string& d, const char* nm) {
            std::lock_guard<std::mutex> lk(g.mx);
            Mod& m = g.list[(size_t)which];
            m.files[0].path = join(m.path, nm);
            put_file(m.files[0].path, d);
        };
        put2(0, mk_xpps(ga), "v2/zeke.xpps"); put2(1, mk_xpps(gb), "v2/zeke.xpps");
        { std::lock_guard<std::mutex> lk(g_pf_mx); g_pf.clear(); g_bm.clear(); }
        {
            std::lock_guard<std::mutex> lk(g.mx);
            g.list[0].res.clear(); g.list[1].res.clear();
            g.gen++;
        }
        std::vector<Clash> c4;
        { std::lock_guard<std::mutex> lk(g.mx); clash_items(g.list[0], g.list[1], c4); }
        CHECK(c4.size() == 1 && c4[0].item == "*" && c4[0].merges);   /* whole copies: combined by the format-aware merge */
        std::vector<std::string> want = base;
        want[0][300] = (char)0xB0; want[0].insert(2, "INSERTED-BY-CA");
        CHECK(served() == mk_xpps(want));
        CHECK_HAS(get_file(join(g.dir, "merge_log.txt")), "zeke.xpps: CA and CB combined (each one's additions kept)");
        /* both change the same byte of packed file 0, CB resolved first: combined, CB's byte there */
        gb[0][500] = (char)0xBB; ga[0][500 + 14] = (char)0xAA;   /* the game's byte 500 (in CA's copy 14 further on) */
        put2(0, mk_xpps(ga), "v3/zeke.xpps"); put2(1, mk_xpps(gb), "v3/zeke.xpps");
        { std::lock_guard<std::mutex> lk(g.mx); g.gen++; }
        CHECK(ps3_game_mods_conflicts(1, c, 8) == 1 && !c[0].merges);
        const int w3[1] = { 0 };
        CHECK(ps3_game_mods_resolve(1, w3, 1) == 2);
        std::vector<std::string> want2 = base;
        want2[0][300] = (char)0xB0; want2[0][500] = (char)0xBB; want2[0].insert(2, "INSERTED-BY-CA");
        CHECK(served() == mk_xpps(want2));
        CHECK_HAS(get_file(join(g.dir, "merge_log.txt")), "CB's used there");
        /* CA adds packed files at the end (as a new power does): CB's changes go into CA's copy */
        std::vector<std::string> ga4 = base, gb4 = base;
        ga4.push_back("NEW-PACKED-FILE-ONE"); ga4.push_back("NEW-PACKED-FILE-TWO");
        ga4[1][1] = (char)0xA4;
        gb4[0][300] = (char)0xB4; gb4[1][4] = (char)0xB5;
        put2(0, mk_xpps(ga4), "v4/zeke.xpps"); put2(1, mk_xpps(gb4), "v4/zeke.xpps");
        { std::lock_guard<std::mutex> lk(g.mx); g.list[0].res.clear(); g.list[1].res.clear(); g.gen++; }
        std::vector<Clash> c5;
        { std::lock_guard<std::mutex> lk(g.mx); clash_items(g.list[0], g.list[1], c5); }
        CHECK(c5.size() == 1 && c5[0].item == "p1" && c5[0].merges);
        std::vector<std::string> want4 = ga4;
        want4[0][300] = (char)0xB4; want4[1][4] = (char)0xB5;
        CHECK(served() == mk_xpps(want4));
        CHECK_HAS(get_file(join(g.dir, "merge_log.txt")), "built from CA's copy (it adds 2 packed file(s))");
        {   /* both add packed files: the whole file, one of them */
            std::vector<std::string> gb5 = gb4;
            gb5.push_back("CB-ADDS-ONE");
            put2(1, mk_xpps(gb5), "v5/zeke.xpps");
            std::vector<Clash> c6;
            { std::lock_guard<std::mutex> lk(g.mx); g.gen++; clash_items(g.list[0], g.list[1], c6); }
            CHECK(c6.size() == 1 && c6[0].item == "*" && !c6[0].merges);
        }
    }
    {   /* a texture patch onto a file another mod replaces: the whole file is the part */
        Mod t = mk("CT", { { "zeke.xpps", 0x10 } });
        std::vector<Clash> c2;
        clash_items(a, t, c2);
        CHECK(c2.size() == 1 && c2[0].item == "*" && !c2[0].merges);
        t.res["ca|zeke.xpps|*"] = true;
        CHECK(!mods_clash(a, t));
        std::vector<Mod> fm = { a, t };
        merge_used(fm, { 0, 1 }, {});
        CHECK(fm[0].files.empty() && fm[1].tex.size() == 1);
    }
    {   /* a live mod against a regular one: no question asked, the regular mod keeps every part
           (the live mod leaves its overlapping windows out), even with an old "live wins" line */
        Mod l = b; l.live = true; l.folder = "CL"; l.name = "CL";
        std::vector<Clash> c3;
        clash_items(a, l, c3);
        CHECK(!c3.empty());
        bool all = true;
        for (const Clash& k : c3) all = all && k.merges;
        CHECK(all && !mods_clash(a, l));
        l.res["ca|zeke.xpps|p0"] = true; a.res["cl|zeke.xpps|p0"] = false;
        std::vector<Mod> fm = { a, l };
        merge_used(fm, { 0 }, {}, { 1 });
        CHECK(fm[0].files.size() == a.files.size());
        bool nodrop = true;
        for (const Part& p : fm[0].files) nodrop = nodrop && p.drop.empty();
        CHECK(nodrop);
    }
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list.clear(); g.gen++;
    }
    g.cache.clear();
    ppu_vfs_root = ".";
    { std::lock_guard<std::mutex> lk(g_pf_mx); g_pf_arcs = Arcs(); g_pf.clear(); g_bm.clear(); }
}

/* ---- "electric_dash": the Electric Dash power's engine settings ------------------ */
static void test_electric_dash()
{
    const std::string d = join(W, "ed");
    mkdirs(join(d, "Dash")); mkdirs(join(d, "Plain")); mkdirs(join(d, "Off"));
    put_file(join(d, "Dash/mod.json"), "{\"game\": \"inf2\", \"name\": \"Dash\", \"electric_dash\": {\"speed\": 3, \"anim\": 1.5, \"drain\": 20, \"glow\": false, \"glowskip\": 500}}");
    put_file(join(d, "Plain/mod.json"), "{\"game\": \"inf2\", \"name\": \"Plain\", \"electric_dash\": true}");
    put_file(join(d, "Off/mod.json"), "{\"game\": \"inf2\", \"name\": \"Off\"}");
    g.dir = d; g.state_path = join(W, "ed_state.ini");
    remove(g.state_path.c_str());
    std::vector<Mod> L = scan();
    const Mod *a = nullptr, *b = nullptr, *c = nullptr;
    for (const Mod& m : L) { if (m.folder == "Dash") a = &m; if (m.folder == "Plain") b = &m; if (m.folder == "Off") c = &m; }
    CHECK(a && b && c);
    if (!a || !b || !c) return;
    CHECK(a->ed && a->ed_speed == 3.0f && a->ed_anim == 1.5f && a->ed_drain == 20.0f && a->ed_glow == 0 && a->ed_glowskip == 500);
    CHECK(b->ed && b->ed_speed == 2.0f && b->ed_anim == 1.3f && b->ed_glow == 1);
    CHECK(!c->ed);
    float sp = 0, an = 0, dr = 0; int gl = -1, gs = -1;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list = L; g.start.clear();
        StartInfo si; si.active = true;
        g.start["off"] = si;
    }
    CHECK(ps3_game_mods_electric_dash(&sp, &an, &dr, &gl, &gs) == 0);   /* only a mod without it is used */
    {
        std::lock_guard<std::mutex> lk(g.mx);
        StartInfo si; si.active = true;
        g.start["dash"] = si;
    }
    CHECK(ps3_game_mods_electric_dash(&sp, &an, &dr, &gl, &gs) == 1);
    CHECK(sp == 3.0f && an == 1.5f && dr == 20.0f && gl == 0 && gs == 500);
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list.clear(); g.start.clear(); g.gen++;
    }
}

/* mod.json "file_alias": a USRDIR file the game lacks is read from another of
 * its files while the mod is on; never when the game has the file, never a
 * path out of the game folder. */
static void test_alias()
{
    g.dir = join(W, "alias"); g.state_path = join(W, "alias_state.ini");
    remove(g.state_path.c_str());
    g.start.clear(); g.start_sig.clear(); g.use = true;
    wipe(g.dir);
    put_file(join(g.dir, "P/mod.json"), "{\"game\": \"inf2\", \"name\": \"P\", \"file_alias\": {"
             "\"movies/ui/powerk_bolt9.bik\": \"movies/ui/powerk_fatbolt.bik\", "
             "\"Movies\\\\UI\\\\powerk_bolt0.bik\": \"movies/ui/powerk_autobolt.bik\", "
             "\"movies/ui/powerk_r2_8.bik\": \"movies/../../secret.bik\", "
             "\"movies/ui/powerk_r2_9.bik\": \"movies/ui/powerk_none.bik\", "
             "\"movies/ui/powerk_fatbolt.bik\": \"movies/ui/powerk_autobolt.bik\"}}");
    put_file(join(g.dir, "P/files/x.xpps"), "x");
    const std::string U = join(W, "alias_game/USRDIR/");
    put_file(U + "movies/ui/powerk_fatbolt.bik", "magnum");
    put_file(U + "movies/ui/powerk_autobolt.bik", "stream");
    { std::lock_guard<std::mutex> lk(g.mx); g.list = scan(); g.gen++; }
    CHECK(g.list.size() == 1 && g.list[0].err.empty() && g.list[0].alias.size() == 4);   /* the ../ one is refused */
    auto at = [&](const char* rel) {
        char out[1100];
        snprintf(out, sizeof out, "%s%s", U.c_str(), rel);
        redirect((std::string("/dev_bdvd/PS3_GAME/USRDIR/") + rel).c_str(), out, sizeof out);
        return std::string(out);
    };
    CHECK(at("movies/ui/powerk_bolt9.bik") == U + "movies/ui/powerk_fatbolt.bik");
    CHECK(at("movies/ui/POWERK_BOLT0.bik") == U + "movies/ui/powerk_autobolt.bik");   /* any case, '\\' in mod.json */
    CHECK(at("movies/ui/powerk_r2_9.bik") == U + "movies/ui/powerk_r2_9.bik");        /* the target is missing too */
    CHECK(at("movies/ui/powerk_fatbolt.bik") == U + "movies/ui/powerk_fatbolt.bik");  /* the game has it: kept */
    CHECK(at("movies/ui/powerk_bolt8.bik") == U + "movies/ui/powerk_bolt8.bik");
    { std::lock_guard<std::mutex> lk(g.mx); g.list[0].on = false; }
    CHECK(at("movies/ui/powerk_bolt9.bik") == U + "movies/ui/powerk_bolt9.bik");      /* mod off */
    { std::lock_guard<std::mutex> lk(g.mx); g.list[0].on = true; g.use = false; }
    CHECK(at("movies/ui/powerk_bolt9.bik") == U + "movies/ui/powerk_bolt9.bik");      /* mods off */
    { std::lock_guard<std::mutex> lk(g.mx); g.use = true; g.list.clear(); g.gen++; }
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_game_mods <work folder> [<downloads> [<game folder>] [<png> <jpg>]]\n"); return 2; }
    W = slashes(argv[1]);
    mkdirs(W);
    init();   /* PS3_GAME_MODS_DIR unset: nothing read; the tests set g themselves */
    test_zips();
    fprintf(stderr, "zips: %d ok, %d failed\n", g_pass, g_fail);
    test_plan();
    fprintf(stderr, "+plan: %d ok, %d failed\n", g_pass, g_fail);
    test_state();
    fprintf(stderr, "+mods.ini: %d ok, %d failed\n", g_pass, g_fail);
    test_merge();
    fprintf(stderr, "+merge: %d ok, %d failed\n", g_pass, g_fail);
    test_conflicts();
    fprintf(stderr, "+conflicts: %d ok, %d failed\n", g_pass, g_fail);
    test_electric_dash();
    fprintf(stderr, "+electric_dash: %d ok, %d failed\n", g_pass, g_fail);
    test_alias();
    fprintf(stderr, "+file_alias: %d ok, %d failed\n", g_pass, g_fail);
    if (argc >= 6) test_images(slashes(argv[4]), slashes(argv[5]));
    fprintf(stderr, "+images: %d ok, %d failed\n", g_pass, g_fail);
    if (argc >= 3) test_real(slashes(argv[2]), argc >= 4 ? slashes(argv[3]) : std::string());
    printf("test_game_mods: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
