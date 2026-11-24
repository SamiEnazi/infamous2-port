/*
 * ps3recomp - game mods in the inFAMOUS Mod Manager's format, served under the
 * guest (included once, by ppu_fs.cpp, after ppu_fs_lang.h)
 *
 * The community's inFAMOUS 2 mods, made for tiffsomniac's "inFAMOUS Mod
 * Manager" 0.2 (docs/MODS.md), are folders:
 *
 *   <mod>/mod.json         {"game": "inf2", "name", "author", "version", ...}
 *   <mod>/files/ (.xpps)   whole files that replace the game's, in any
 *                          subfolder (a mod with no files/ folder keeps them
 *                          in the mod folder itself)
 *   <files>/texture_patches/<target>.xpps/ ... data<OFFSET>_ ... <W>x<H>_<DXTn>_m<mips>.dds
 *                          a texture written over the target's bytes OFFSET
 *                          past its data start (the u32 at +40), its block
 *                          rows flipped back (the exporter flips them)
 *   <files>/xpps_patches/<target>.xpps/file_<i>_<abs>[_offset_<o>].bin
 *                          packed file i of the target replaced (its size and
 *                          the later files' and chunks' offsets fixed up), or
 *                          these bytes written at o inside it
 *   <mod>/modular.json     options: "modules" (toggles) and "groups" (toggle
 *                          or dropdown, of "options") whose "paths" pick the
 *                          parts of the mod that are used
 *
 * The Manager copies a mod into the game's USRDIR, after its "Unlocker" has
 * unpacked the five big archives (infamous1/2, patch1/3/4 .psarc_s) into
 * loose files there and moved the archives away: the game reads a file out
 * of its archives when one holds it, and the loose USRDIR file when none
 * does. Here nothing is unpacked, moved or written into the game's folders:
 *
 *  - Every file a mod changes is hidden from the archives the game opens:
 *    the archive's TOC record of it gets an MD5 (the game looks a file up by
 *    the MD5 of its upper-case name) one larger, in the read buffer, never
 *    on disk (lang_apply's per-descriptor patches). The TOC is not sorted,
 *    so no other lookup moves.
 *  - The game then asks for the loose file under USRDIR, and that open/stat
 *    gets the mod's file -- or, for patches, the original (read out of the
 *    highest archive that has it: patch4 > patch3 > patch1 > infamous2 >
 *    infamous1, the order in which the Unlocker's copies overwrite each
 *    other) with the patches written in, built once into <mods>/.port_cache.
 *
 * What a mod changes is decided at start. Mods are applied in order: the
 * ones the player put first (mods.ini "priority", the menu's "Use this one
 * instead"), the higher priority first, then the rest in folder order
 * (case-insensitive). A mod that changes what an earlier one already
 * changes is left out whole, as the Manager refuses to enable a conflicting
 * mod: the same whole file, a texture at the same offset, a packed file that
 * overlaps, or a patch onto a file another mod replaces (mods_clash) -- unless
 * it merges: different packed files, or different bytes of one (automatic
 * merges), a merge patch ("patch_for"), or parts whose version the player
 * picked when the menu asked (clash_items, res_winner, merge_used). The
 * menu shows the same rule over the saved choices at once (plan_locked: no
 * builds); the real resolve() still builds everything at start.
 *
 * The menu also installs mods from zip files (zip_check / zip_install, the
 * reader in ppu_fs_mods_zip.h): every entry checked before anything is
 * written, extracted into a dot folder inside the mods folder, then renamed.
 * mod.json "image" names the mod's preview picture (image_path).
 *
 *   PS3_GAME_MODS_DIR=<dir>    the mods folder: <dir>/<mod>/mod.json, or one
 *                              folder deeper (an unzipped "<name>/<name>/").
 *                              Unset: no mods (tests, the harness).
 *   PS3_GAME_MODS=0            use none of them (the menu's "Use mods").
 *   PS3_GAME_MODS_STATE=<file> the mods the menu turned off and the options
 *                              picked (default: mods.ini beside the menu's
 *                              settings.ini). A mod not in it is on, with
 *                              its default options (the Manager's: every
 *                              toggle, a dropdown's first option).
 *   PS3_GAME_MODS_CACHE=<dir>  where patched files are built
 *                              (default <mods dir>/.port_cache).
 *   PS3_GAME_MODS_ZIP_DIRS=<d;d> more folders the zip list looks in (tests).
 *   PS3_GAME_MODS_TEST=<steps> test hook, run once (not after a restart),
 *                              PS3_GAME_MODS_TEST_AT s (default 15) after
 *                              init: "install=<zip>|replace=<zip>|
 *                              pick=<folder>=<id,id>|prefer=<folder>|plan|
 *                              zips|restart", each logged "[mods] test: ...".
 *
 * Log lines start "[mods]".
 */
#ifndef PPU_FS_MODS_H
#define PPU_FS_MODS_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <tuple>
#include <array>
#include <deque>
#include <vector>
#include <ctype.h>
#include "ps3emu/ps3_game_mods.h"
#include "ppu_fs_xcombine.h"   /* format-aware combine of two mods' whole copies of one PACK file */
#ifdef _WIN32
#include <direct.h>
#include <shellapi.h>
#include <commdlg.h>    /* OPENFILENAMEW: "Choose another file..." (comdlg32 is loaded when used) */
#endif

/* libs/system/cellSysutil.c: the test hook's "restart" step ends the session
 * the way the menu's "Restart to apply" does (the overlay saves its settings
 * first, then calls this) */
extern "C" void ps3_host_restart(const char* why);

namespace gmods {

typedef std::vector<uint8_t> Bytes;

/* ---- small helpers --------------------------------------------------------- */

static std::string lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
static std::string upper(std::string s)
{
    for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}
static bool ieq(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
/* The Manager's StringComparer.OrdinalIgnoreCase: upper-cased, so '_' and
 * the other characters between 'Z' and 'a' sort after the letters. */
static bool ilt(const std::string& a, const std::string& b) { return upper(a) < upper(b); }
static bool ends_i(const std::string& s, const char* suf)
{
    const size_t n = strlen(suf);
    return s.size() >= n && lang_ieq_n(s.c_str() + s.size() - n, suf, n);
}
static std::string slashes(std::string s) { for (char& c : s) if (c == '\\') c = '/'; return s; }
static std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}
static std::string base_of(const std::string& p)
{
    const size_t k = p.find_last_of('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}
static std::string dir_of(const std::string& p)
{
    const size_t k = p.find_last_of('/');
    return k == std::string::npos ? std::string() : p.substr(0, k);
}
/* The Manager's NormalizeRelativePath: trimmed, '/' separators, no leading
 * '.' or '/', no trailing '/'. */
static std::string norm_rel(const std::string& in)
{
    size_t a = 0, b = in.size();
    while (a < b && isspace((unsigned char)in[a])) a++;
    while (b > a && isspace((unsigned char)in[b - 1])) b--;
    std::string s = slashes(in.substr(a, b - a));
    size_t i = 0;
    while (i < s.size() && (s[i] == '.' || s[i] == '/')) i++;
    s.erase(0, i);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int imax(int a, int b) { return a > b ? a : b; }
static size_t imax(size_t a, size_t b) { return a > b ? a : b; }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

struct FInfo { bool ok = false, dir = false; uint64_t size = 0; int64_t mtime = 0; };
static FInfo finfo(const std::string& p)
{
    FInfo f;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p.c_str(), &st) != 0) return f;
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return f;
#endif
    f.ok = true;
    f.dir = (st.st_mode & S_IFMT) == S_IFDIR;
    f.size = (uint64_t)st.st_size;
    f.mtime = (int64_t)st.st_mtime;
    return f;
}
static bool is_dir(const std::string& p) { const FInfo f = finfo(p); return f.ok && f.dir; }
/* A junction or symbolic link (on Windows: a reparse point). A mod's folders
 * are walked without them: a link in a downloaded mod could lead the scan
 * anywhere on the PC, or round in a loop. */
static bool is_link(const std::string& p)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

/* Entries of a folder (no "." / ".."), sorted case-insensitively. */
static std::vector<std::string> list_dir(const std::string& dir)
{
    std::vector<std::string> v;
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) v.push_back(e->d_name);
        closedir(d);
    }
    std::stable_sort(v.begin(), v.end(), ilt);
    return v;
}
/* Every file below dir whose name ends in ext (case-insensitive), full paths.
 * no_links: junctions and symbolic links are skipped (a mod's own folders). */
static void walk(const std::string& dir, const char* ext, std::vector<std::string>& out, int depth = 0, bool no_links = false)
{
    if (depth > 16) return;
    for (const std::string& n : list_dir(dir)) {
        const std::string p = join(dir, n);
        if (no_links && is_link(p)) continue;
        const FInfo f = finfo(p);
        if (!f.ok) continue;
        if (f.dir) walk(p, ext, out, depth + 1, no_links);
        else if (ends_i(n, ext)) out.push_back(p);
    }
}
/* p relative to root (both '/'-separated; p below root), else "". */
static std::string rel_to(const std::string& root, const std::string& p)
{
    if (p.size() > root.size() && lang_ieq_n(p.c_str(), root.c_str(), root.size()) && p[root.size()] == '/')
        return p.substr(root.size() + 1);
    return std::string();
}
static std::vector<std::string> split_parts(const std::string& rel)
{
    std::vector<std::string> v;
    size_t s = 0;
    for (size_t i = 0; i <= rel.size(); i++)
        if (i == rel.size() || rel[i] == '/') { if (i > s) v.push_back(rel.substr(s, i - s)); s = i + 1; }
    return v;
}

/* A mod's file bigger than this is refused, not read: a huge (or broken)
 * download must not take the game's memory at every start. The game's own
 * largest .xpps is far below it. */
static const uint64_t kMaxModFile = 512ull << 20;
static const uint64_t kMaxModJson = 1ull << 20;
/* The file from `at` on (at most `max` bytes); false when it cannot be read or
 * what would be read is more than `limit`. */
static bool read_all(const std::string& p, Bytes& out, uint64_t at = 0, uint64_t max = ~0ull, uint64_t limit = kMaxModFile)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    if (LANG_FSEEK(f, 0) == 0) {
#ifdef _WIN32
        _fseeki64(f, 0, SEEK_END); const int64_t n = _ftelli64(f);
#else
        fseeko(f, 0, SEEK_END); const int64_t n = (int64_t)ftello(f);
#endif
        if (n >= 0 && (uint64_t)n >= at) {
            uint64_t len = (uint64_t)n - at;
            if (len > max) len = max;
            if (len <= limit) {
                out.resize((size_t)len);
                ok = LANG_FSEEK(f, at) == 0 && fread(out.data(), 1, out.size(), f) == out.size();
            }
        }
    }
    fclose(f);
    return ok;
}
static void mkdirs(const std::string& dir)
{
    std::string d;
    for (const std::string& part : split_parts(dir)) {
        d = d.empty() ? (dir[0] == '/' ? "/" + part : part) : join(d, part);
        if (d.size() == 2 && d[1] == ':') continue;   /* "C:" */
        if (!is_dir(d)) mkdir(d.c_str(), 0777);
    }
}
static bool write_all(const std::string& p, const Bytes& d)
{
    mkdirs(dir_of(p));
    const std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return false; }
    remove(p.c_str());
    return rename(tmp.c_str(), p.c_str()) == 0;
}

/* ---- JSON (mod.json, modular.json: comments and trailing commas allowed) ---- */

struct J {
    int t = 0;   /* 0 null, 1 bool, 2 number, 3 string, 4 array, 5 object */
    std::string s;
    std::vector<J> a;
    /* An object's keys and values, in order. Two vectors, not one of pair<string, J>: a pair holding a J
     * inside J is a cycle GCC 15's C++ library rejects ("incomplete type ... used in type trait expression",
     * GitHub #24); a vector of J inside J is fine (C++17). */
    std::vector<std::string> ok;
    std::vector<J> ov;
    const J* get(const char* k) const
    {
        if (t != 5) return nullptr;
        for (size_t i = 0; i < ok.size(); i++) if (ok[i] == k) return &ov[i];
        return nullptr;
    }
    /* The Manager's ReadString: the trimmed string, else the fallback. */
    std::string str(const char* k, const std::string& fallback = std::string()) const
    {
        const J* v = get(k);
        if (!v || (v->t != 3 && v->t != 2)) return fallback;
        size_t a0 = 0, b0 = v->s.size();
        while (a0 < b0 && isspace((unsigned char)v->s[a0])) a0++;
        while (b0 > a0 && isspace((unsigned char)v->s[b0 - 1])) b0--;
        return v->s.substr(a0, b0 - a0);
    }
};

struct JParse {
    const char* p; const char* e;
    void ws()
    {
        for (;;) {
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (e - p >= 2 && p[0] == '/' && p[1] == '/') { while (p < e && *p != '\n') p++; continue; }
            if (e - p >= 2 && p[0] == '/' && p[1] == '*') {
                p += 2;
                while (e - p >= 2 && !(p[0] == '*' && p[1] == '/')) p++;
                p = (e - p >= 2) ? p + 2 : e;
                continue;
            }
            return;
        }
    }
    static void utf8(std::string& s, uint32_t c)
    {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    bool hex4(uint32_t& v)
    {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& s)
    {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p != '\\') { s += *p++; continue; }
            if (++p >= e) return false;
            const char c = *p++;
            switch (c) {
            case 'n': s += '\n'; break; case 't': s += '\t'; break; case 'r': s += '\r'; break;
            case 'b': s += '\b'; break; case 'f': s += '\f'; break;
            case 'u': {
                uint32_t u;
                if (!hex4(u)) return false;
                if (u >= 0xD800 && u < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(s, u);
                break;
            }
            default: s += c; break;   /* \" \\ \/ */
            }
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool val(J& v, int depth)
    {
        if (depth > 64) return false;
        ws();
        if (p >= e) return false;
        if (*p == '{') {
            v.t = 5; p++;
            for (;;) {
                ws();
                if (p < e && *p == '}') { p++; return true; }
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                p++;
                J x;
                if (!val(x, depth + 1)) return false;
                v.ok.push_back(std::move(k));
                v.ov.push_back(std::move(x));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            v.t = 4; p++;
            for (;;) {
                ws();
                if (p < e && *p == ']') { p++; return true; }
                J x;
                if (!val(x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.t = 3; return str(v.s); }
        if (e - p >= 4 && !strncmp(p, "true", 4)) { v.t = 1; v.s = "true"; p += 4; return true; }
        if (e - p >= 5 && !strncmp(p, "false", 5)) { v.t = 1; v.s = "false"; p += 5; return true; }
        if (e - p >= 4 && !strncmp(p, "null", 4)) { v.t = 0; p += 4; return true; }
        const char* s0 = p;
        while (p < e && (isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) p++;
        if (p == s0) return false;
        v.t = 2; v.s.assign(s0, p);
        return true;
    }
};

/* The bytes' object (a UTF-8 BOM skipped); false if they are not a JSON object. */
static bool parse_json(const Bytes& b, J& out)
{
    const char* p = (const char*)b.data();
    const char* e = p + b.size();
    if (b.size() >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    JParse jp{ p, e };
    J v;
    if (!b.empty() && jp.val(v, 0) && v.t == 5) { out = std::move(v); return true; }
    return false;
}

/* The file's object; an empty one if it is missing or not JSON (err says why). */
static J read_json(const std::string& path, std::string* err = nullptr)
{
    J j;
    Bytes b;
    if (!read_all(path, b, 0, ~0ull, kMaxModJson)) {
        if (err && finfo(path).ok) *err = base_of(slashes(path)) + " cannot be read or is bigger than 1 MB";
        return j;
    }
    J v;
    if (parse_json(b, v)) return v;
    if (err) *err = base_of(slashes(path)) + " is not valid JSON";
    return j;
}

/* ---- paths from untrusted places (a zip's entry names, mod.json "image") ---- */

/* One part of such a path: printable ASCII only (the file layer's names are
 * narrow), no character Windows refuses or reads specially (<>:"|?* -- ':' is
 * a drive letter or an alternate data stream), not ".", "..", empty, ending in
 * a dot or a space (Windows drops those: two names, one file), and not a
 * device name (CON, NUL, COM1 ... with any extension). */
static bool part_name_ok(const std::string& p, std::string* why)
{
    auto no = [&](const char* w) { if (why) *why = w; return false; };
    if (p.empty()) return no("an empty folder name (a doubled or trailing '/')");
    if (p == "." || p == "..") return no("a \"..\" or \".\" part (it would leave its folder)");
    if (p.size() > 160) return no("a name longer than 160 characters");
    for (char c : p) {
        const unsigned char u = (unsigned char)c;
        if (u < 0x20 || u > 0x7E) return no("a character other than plain ASCII letters, digits and signs");
        if (c == ':') return no("a ':' (a drive letter or a file stream)");
        if (strchr("<>\"|?*", c)) return no("a character Windows does not allow in names (<>\"|?*)");
    }
    if (p.back() == '.' || p.back() == ' ') return no("a name that ends in a dot or a space");
    std::string base = upper(p.substr(0, p.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    static const char* const kDev[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$" };
    for (const char* d : kDev) if (base == d) return no("a Windows device name (CON, NUL, COM1, ...)");
    if (base.size() == 4 && (!base.compare(0, 3, "COM") || !base.compare(0, 3, "LPT")) && isdigit((unsigned char)base[3]))
        return no("a Windows device name (CON, NUL, COM1, ...)");
    return true;
}

/* A relative path that stays inside its folder: '\' read as '/' (zips made on
 * Windows), then not absolute ('/' first: also "//server" and "\\?\"), no
 * drive letter, and every part part_name_ok. out: the '/'-separated path. */
static bool safe_rel(const std::string& in, std::string& out, std::string* why)
{
    auto no = [&](const char* w) { if (why) *why = w; return false; };
    if (in.empty()) return no("an empty name");
    if (in.size() > 240) return no("a path longer than 240 characters");
    std::string s = in;
    for (char& c : s) if (c == '\\') c = '/';
    if (s[0] == '/') return no("an absolute path (it starts with a slash, or names a network share)");
    if (s.size() >= 2 && s[1] == ':') return no("a drive letter");
    size_t a = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i < s.size() && s[i] != '/') continue;
        if (!part_name_ok(s.substr(a, i - a), why)) return false;
        a = i + 1;
    }
    out = s;
    return true;
}

/* ---- PSARC (the game's archives) ------------------------------------------- */

struct Psarc {
    FILE* f = nullptr;
    std::string path;
    uint32_t toclen = 0, esz = 0, n = 0, bsz = 0;
    int nb = 2;
    size_t nblocks = 0;
    Bytes toc;                       /* the TOC after the 32-byte header */
    std::vector<std::string> names;  /* entry i's name, lower case, no leading '/' ("" for 0) */
    ~Psarc() { if (f) fclose(f); }
    uint64_t rd(const uint8_t* q, int k) const { uint64_t v = 0; for (int i = 0; i < k; i++) v = (v << 8) | q[i]; return v; }
    const uint8_t* rec(uint32_t i) const { return toc.data() + (size_t)i * esz; }
    bool entry(uint32_t i, Bytes& out)
    {
        if (i >= n) return false;
        const uint8_t* r = rec(i);
        uint64_t blk = rd(r + 16, 4);
        const uint64_t usz = rd(r + 20, 5), off = rd(r + 25, 5);
        if (usz > (2ull << 30)) return false;
        out.clear();
        out.reserve((size_t)usz);
        Bytes in(bsz), dec(bsz);
        const size_t tab = (size_t)n * esz;
        if (LANG_FSEEK(f, off) != 0) return false;
        while (out.size() < usz) {
            if (blk >= nblocks) return false;
            const uint32_t c = (uint32_t)rd(toc.data() + tab + blk * (size_t)nb, nb);
            blk++;
            const uint32_t take = c ? c : bsz;
            if (take > bsz || fread(in.data(), 1, take, f) != take) return false;
            const size_t want = (size_t)(usz - out.size()) < bsz ? (size_t)(usz - out.size()) : bsz;
            int got = -1;
            if (c && in[0] == 0x78)
                got = stbi_zlib_decode_buffer((char*)dec.data(), (int)bsz, (const char*)in.data(), (int)take);
            if (got > 0) out.insert(out.end(), dec.data(), dec.data() + ((size_t)got < want ? (size_t)got : want));
            else out.insert(out.end(), in.data(), in.data() + (take < want ? take : want));
        }
        return true;
    }
    bool open(const std::string& p)
    {
        path = p;
        f = fopen(p.c_str(), "rb");
        if (!f) return false;
        uint8_t h[32];
        if (fread(h, 1, 32, f) != 32 || memcmp(h, "PSAR", 4) || memcmp(h + 8, "zlib", 4)) return false;
        toclen = be32(h + 12); esz = be32(h + 16); n = be32(h + 20); bsz = be32(h + 24);
        if (esz != 30 || n < 2 || n > 1000000 || toclen < 32 + n * esz || toclen > (64u << 20) || !bsz || bsz > (16u << 20))
            return false;
        toc.resize(toclen - 32);
        if (fread(toc.data(), 1, toc.size(), f) != toc.size()) return false;
        nb = bsz <= 0x10000 ? 2 : bsz <= 0x1000000 ? 3 : 4;
        nblocks = (toc.size() - (size_t)n * esz) / (size_t)nb;
        Bytes list;
        if (!entry(0, list)) return false;
        names.assign(n, std::string());
        uint32_t idx = 1;
        for (size_t s = 0; s < list.size() && idx < n; idx++) {
            size_t q = s;
            while (q < list.size() && list[q] != '\n') q++;
            std::string nm((const char*)list.data() + s, q - s);
            if (!nm.empty() && nm.back() == '\r') nm.pop_back();
            if (!nm.empty() && nm[0] == '/') nm.erase(0, 1);
            names[idx] = lower(nm);
            s = q + 1;
        }
        return true;
    }
    int find(const std::string& lname) const
    {
        for (uint32_t i = 1; i < n; i++) if (names[i] == lname) return (int)i;
        return -1;
    }
};

/* ---- the Manager's patch formats ------------------------------------------- */

static int dxt_block(const std::string& fmt) { return fmt == "DXT1" ? 8 : 16; }
static size_t dxt_size(int w, int h, const std::string& fmt, int mips)
{
    size_t total = 0;
    for (int i = 0; i < mips; i++) {
        total += (size_t)imax(1, (w + 3) / 4) * (size_t)imax(1, (h + 3) / 4) * (size_t)dxt_block(fmt);
        w = imax(1, w / 2); h = imax(1, h / 2);
    }
    return total;
}
static void flip_dxt1(uint8_t* b) { std::swap(b[4], b[7]); std::swap(b[5], b[6]); }
static void flip_dxt3(uint8_t* b)
{
    uint8_t t[8]; memcpy(t, b, 8);
    memcpy(b + 0, t + 6, 2); memcpy(b + 2, t + 4, 2); memcpy(b + 4, t + 2, 2); memcpy(b + 6, t + 0, 2);
    flip_dxt1(b + 8);
}
static void flip_dxt5(uint8_t* b)
{
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    uint8_t px[16], fl[16];
    for (int j = 0; j < 16; j++) px[j] = (uint8_t)((bits >> (3 * j)) & 7);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) fl[r * 4 + c] = px[(3 - r) * 4 + c];
    bits = 0;
    for (int j = 0; j < 16; j++) bits |= (uint64_t)fl[j] << (3 * j);
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
    flip_dxt1(b + 8);
}
/* The exporter's (and the Manager's) vertical flip of every mip: block rows
 * reversed, each block's own rows reversed. Its own inverse. */
static void flip_payload(uint8_t* data, int w, int h, const std::string& fmt, int mips)
{
    const int bs = dxt_block(fmt);
    size_t at = 0;
    for (int i = 0; i < mips; i++) {
        const int bw = imax(1, (w + 3) / 4), bh = imax(1, (h + 3) / 4);
        const size_t len = (size_t)bw * bh * bs;
        Bytes tmp(len);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                uint8_t* d = tmp.data() + ((size_t)y * bw + x) * bs;
                memcpy(d, data + at + ((size_t)(bh - 1 - y) * bw + x) * bs, (size_t)bs);
                if (fmt == "DXT1") flip_dxt1(d); else if (fmt == "DXT3") flip_dxt3(d); else flip_dxt5(d);
            }
        memcpy(data + at, tmp.data(), len);
        at += len;
        w = imax(1, w / 2); h = imax(1, h / 2);
    }
}

static bool is_hex8(const std::string& s, size_t at)
{
    if (at + 8 > s.size()) return false;
    for (size_t i = at; i < at + 8; i++) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}
/* The Manager's texture-patch name, (?:^|_)(?:data)?([0-9a-f]{8})_.*?(\d+)x(\d+)_(DXT[135])_m(\d+)\.dds$
 * case-insensitively: the offset, size, format and mip count. */
static bool parse_tex_name(const std::string& nm, uint32_t& off, int& w, int& h, std::string& fmt, int& mips)
{
    const std::string s = lower(nm);
    if (!ends_i(s, ".dds")) return false;
    size_t q = s.size() - 4, m_end = q;
    while (q > 0 && isdigit((unsigned char)s[q - 1])) q--;
    if (q == m_end || q < 2 || s.compare(q - 2, 2, "_m")) return false;
    mips = atoi(s.substr(q, m_end - q).c_str());
    q -= 2;
    if (q < 5 || s.compare(q - 4, 3, "dxt") || (s[q - 1] != '1' && s[q - 1] != '3' && s[q - 1] != '5') || s[q - 5] != '_') return false;
    fmt = std::string("DXT") + s[q - 1];
    q -= 5;
    const size_t h_end = q;
    while (q > 0 && isdigit((unsigned char)s[q - 1])) q--;
    if (q == h_end || q < 1 || s[q - 1] != 'x') return false;
    h = atoi(s.substr(q, h_end - q).c_str());
    const size_t x_at = q - 1;
    size_t wq = x_at;
    while (wq > 0 && isdigit((unsigned char)s[wq - 1])) wq--;
    if (wq == x_at) return false;
    for (size_t i = 0; i < s.size(); i++) {
        if (i && s[i - 1] != '_') continue;
        size_t hx = std::string::npos;
        if (!s.compare(i, 4, "data") && is_hex8(s, i + 4) && i + 12 < s.size() && s[i + 12] == '_') hx = i + 4;
        else if (is_hex8(s, i) && i + 8 < s.size() && s[i + 8] == '_') hx = i;
        if (hx == std::string::npos) continue;
        const size_t region = hx + 9;
        if (region >= x_at) continue;
        const size_t ws = imax(region, wq);
        w = atoi(s.substr(ws, x_at - ws).c_str());
        off = (uint32_t)strtoul(s.substr(hx, 8).c_str(), nullptr, 16);
        return true;
    }
    return false;
}
/* ^file_(\d+)_([0-9a-f]{8})(?:_offset_([0-9a-f]{8}))?\.bin$, case-insensitively. */
static bool parse_xp_name(const std::string& nm, int& index, uint32_t& abs, bool& chunk, uint32_t& choff)
{
    const std::string s = lower(nm);
    if (s.compare(0, 5, "file_") || !ends_i(s, ".bin")) return false;
    size_t q = 5;
    const size_t d0 = q;
    while (q < s.size() && isdigit((unsigned char)s[q])) q++;
    if (q == d0 || q >= s.size() || s[q] != '_' || !is_hex8(s, q + 1)) return false;
    index = atoi(s.substr(d0, q - d0).c_str());
    abs = (uint32_t)strtoul(s.substr(q + 1, 8).c_str(), nullptr, 16);
    q += 9;
    chunk = false; choff = 0;
    if (q == s.size() - 4) return true;
    if (s.compare(q, 8, "_offset_") || !is_hex8(s, q + 8) || q + 16 != s.size() - 4) return false;
    chunk = true;
    choff = (uint32_t)strtoul(s.substr(q + 8, 8).c_str(), nullptr, 16);
    return true;
}

struct XpFile { uint32_t size, off; size_t size_at, off_at; };
struct XpChunk { uint32_t size, off, file_index, file_count; size_t size_at, off_at; };
struct XpLayout {
    uint32_t data_off = 0, data_size = 0;
    uint64_t tables_end = 48;            /* the header and both tables end here */
    std::vector<XpChunk> chunks; std::vector<XpFile> files;
};

static bool xp_layout(const Bytes& d, XpLayout& L, std::string& err)
{
    if (d.size() < 48 || memcmp(d.data(), "PACK", 4)) { err = "not an XPPS/XPP PACK file"; return false; }
    const uint32_t tab = be32(&d[24]);
    L.data_off = be32(&d[40]); L.data_size = be32(&d[44]);
    if ((uint64_t)tab + 24 > d.size()) { err = "its table header is out of range"; return false; }
    const uint32_t nc = be32(&d[tab + 4]), nf = be32(&d[tab + 12]);
    const uint64_t c0 = (uint64_t)tab + 24, f0 = c0 + (uint64_t)nc * 28;
    if (f0 + (uint64_t)nf * 16 > d.size()) { err = "its file table is out of range"; return false; }
    L.tables_end = f0 + (uint64_t)nf * 16;
    if (L.tables_end < 48) L.tables_end = 48;
    for (uint32_t i = 0; i < nc; i++) {
        const size_t r = (size_t)(c0 + (uint64_t)i * 28);
        L.chunks.push_back({ be32(&d[r + 4]), be32(&d[r + 8]), be32(&d[r + 20]), be32(&d[r + 24]), r + 4, r + 8 });
    }
    for (uint32_t i = 0; i < nf; i++) {
        const size_t r = (size_t)(f0 + (uint64_t)i * 16);
        L.files.push_back({ be32(&d[r + 4]), be32(&d[r + 8]), r + 4, r + 8 });
    }
    return true;
}

/* The textures of an XPPS/XPP file, as xpps-texture-tools 0.2's exporter finds
 * them (the names texture patches carry come from it): (offset from the data
 * start, width, height, format, mips fitted to the owning packed file). Used to
 * check that a texture patch fits THIS copy of the file: a mod made from
 * another version of it (the US digital game's cole_gold_amp.xpps has its
 * textures 0x110 bytes further on than the EU disc's) would otherwise write
 * beside the texture. */
typedef std::tuple<uint32_t, int, int, std::string, int> TexKey;
static std::set<TexKey> xp_textures(const Bytes& d)
{
    std::set<TexKey> out;
    if (d.size() < 48 || memcmp(d.data(), "PACK", 4)) return out;
    const uint32_t tab = be32(&d[24]), start = be32(&d[40]);
    if ((uint64_t)tab + 24 > d.size()) return out;
    const uint32_t nc = be32(&d[tab + 4]), nf = be32(&d[tab + 12]);
    const uint64_t f0 = (uint64_t)tab + 24 + (uint64_t)nc * 28;
    if (f0 + (uint64_t)nf * 16 > d.size()) return out;
    struct PF { uint32_t i, off, size; };
    std::vector<PF> files;
    for (uint32_t i = 0; i < nf; i++) files.push_back({ i, be32(&d[(size_t)(f0 + i * 16ull + 8)]), be32(&d[(size_t)(f0 + i * 16ull + 4)]) });
    std::vector<PF> banks;
    for (const PF& f : files) if (f.size >= 65536) banks.push_back(f);
    std::stable_sort(banks.begin(), banks.end(), [](const PF& a, const PF& b) { return a.size > b.size; });
    auto pow2 = [](uint32_t v) { return v == 16 || v == 32 || v == 64 || v == 128 || v == 256 || v == 512 || v == 1024; };
    auto bitlen = [](uint32_t v) { int n = 0; while (v) { n++; v >>= 1; } return n; };
    std::set<TexKey> seen;
    for (const PF& bank : banks)
        for (const PF& f : files) {
            if (f.i == bank.i || f.size < 82 || f.size > 65536) continue;
            const uint64_t b = (uint64_t)start + f.off;
            if (b + f.size > d.size()) continue;
            const uint8_t* p = &d[(size_t)b];
            struct C { uint32_t doff, w, h, m, fc; } cands[2];
            int nc2 = 0;
            cands[nc2++] = { be32(p + 32), (uint32_t)((p[56] << 8) | p[57]), (uint32_t)((p[58] << 8) | p[59]), p[80], p[81] };
            if (f.size >= 100) cands[nc2++] = { be32(p + 64), be32(p + 36), be32(p + 40), be32(p + 44), be32(p + 48) };
            for (int k = 0; k < nc2; k++) {
                const C& c = cands[k];
                const char* fmt = c.fc == 3 ? "DXT1" : c.fc == 4 ? "DXT3" : c.fc == 5 ? "DXT5" : nullptr;
                if (!fmt || !pow2(c.w) || !pow2(c.h)) continue;
                if (!(c.m > 0 && (int)c.m <= bitlen(c.w > c.h ? c.w : c.h)) || c.doff < bank.off) continue;
                if (c.doff - bank.off >= bank.size) continue;
                const TexKey key((uint32_t)c.doff, (int)c.w, (int)c.h, std::string(fmt), (int)c.m);
                if (seen.count(key)) break;
                seen.insert(key);
                /* the exporter's fitted mip count (its arithmetic, kept as it is) */
                const PF* owner = nullptr;
                for (const PF& x : files) if (x.off <= c.doff && (uint64_t)c.doff < (uint64_t)x.off + x.size) { owner = &x; break; }
                const int64_t avail = owner ? (int64_t)owner->size - ((int64_t)c.doff - bank.off) : (int64_t)d.size() - ((int64_t)start + c.doff);
                int mm = (int)c.m;
                while (mm > 1 && (int64_t)dxt_size((int)c.w, (int)c.h, fmt, mm) > avail) mm--;
                out.insert(TexKey((uint32_t)c.doff, (int)c.w, (int)c.h, std::string(fmt), mm));
                break;
            }
        }
    return out;
}

/* ---- mods ------------------------------------------------------------------ */

struct Module { std::string id, name, group_id, group_name; std::vector<std::string> paths; bool dropdown = false, single = false; };
struct Part { std::string path, target; std::set<int> drop; };      /* a mod file and the game file it changes (lower case);
                                                                      drop: packed files of it left out (conflicts the player resolved) */
struct Tex : Part { uint32_t off = 0; int w = 0, h = 0, mips = 0; std::string fmt, name; };
struct Xp : Part { int index = 0; uint32_t abs = 0, choff = 0, len = 0; bool chunk = false; std::string name; };
/* Cole's karma levels, as the game orders them (its rank table, live_karma_level) */
static const char* const kKarmaLevels[7] = { "Infamous", "Outlaw", "Thug", "Neutral", "Guardian", "Champion", "Hero" };
struct Mod {
    std::string folder, path, name, author, version, game, err;
    std::string image;                   /* mod.json "image", as written (image_path checks it) */
    bool for_game = false;
    bool live = false;                   /* mod.json "live": true -- patched in memory, on and off at once (live_*) */
    std::vector<std::pair<int, std::string>> levels;   /* a live mod's "karma_levels": karma level (0-6) -> its folder */
    int dark = 0;                        /* a live mod's "dark_bolts": bit r = karma level r, 0x80 = always */
    std::vector<std::string> patch_for;  /* "patch_for": a patch that makes these mods (folder or name) work together */
    std::vector<int> discover;           /* "discover_powers": powers (0-95) this mod adds, shown as discovered in any save */
    struct KarmaPatch { std::vector<int> find, rep; int levels = 0; };   /* bytes; -1 = ?? */
    std::vector<KarmaPatch> kpatch;      /* "karma_patches": bytes of the mod's own that change with Cole's karma level */
    std::vector<std::pair<std::string, std::string>> alias;   /* "file_alias": lower USRDIR path the game lacks -> the
                                                                 game file read in its place (alias_redirect) */
    std::map<std::string, bool> res;     /* conflicts the player resolved: "<other folder>|<file>|<part>" (lower) -> this mod's
                                            version is used (mods.ini win / lose, res_winner) */
    bool ed = false;                     /* "electric_dash": the Electric Dash power's engine side (cheats.cpp ed_*) */
    float ed_speed = 2.0f, ed_anim = 1.3f, ed_drain = 15.0f;
    int ed_glow = 1, ed_glowskip = 11000;
    std::vector<Module> modules;
    bool on = true;                      /* the saved choice */
    bool have_pick = false;
    std::vector<std::string> pick;       /* the saved option ids */
    bool have_prio = false;              /* mods.ini "priority": put first by the player */
    int prio = 0;                        /* the higher, the earlier */
    std::vector<Part> files;
    std::vector<Tex> tex;
    std::vector<Xp> xp;
    std::set<std::string> targets;
    uint64_t stamp = 0;                  /* the used files' names, sizes and times (read_parts) */
};

/* What this session does with a mod (resolve, at start). key: mod_key then;
 * fail: left out because a file could not be built -- alone (fails by itself)
 * or with `partners` (lower folder -> mod_key), the other mods on that file. */
struct StartInfo {
    bool active = false, fail = false, alone = false;
    std::string why, key;
    std::map<std::string, std::string> partners;
};

/* The menu's view of the next start (plan_locked): the clash rule over the
 * saved choices, by index into g.list. */
struct Plan {
    uint64_t gen = ~0ull;                 /* g.gen it was made for */
    bool use = true;                      /* PS3_GAME_MODS as it is now */
    std::vector<char> next;               /* used from the next start */
    std::vector<std::string> why;         /* else why not ("" = off, broken, not for the game) */
    std::vector<std::string> key;         /* mod_key */
    std::vector<int> prefer;              /* left out by a clash: the priority "Use this one instead" gives (0 = none) */
    std::vector<std::string> prefer_text; /* ... and what that does to the others */
    std::set<std::string> sig;            /* mod_key of every mod used from the next start */
    std::vector<int> unres;               /* parts it and another mod that is on both change, not resolved */
    std::vector<std::vector<std::string>> unres_with, merged_with;   /* ... with whom; the mods it is merged with */
    std::map<size_t, std::set<std::string>> cover;   /* patch_cover of the next start's mods */
};

static struct {
    std::once_flag once;
    std::mutex mx;                                   /* list, start, state, pending, plan, gen, zip_dirs */
    std::string dir, cache, state_path;
    bool use = true;                                 /* PS3_GAME_MODS */
    std::vector<Mod> list;                           /* what the menu shows */
    uint64_t gen = 0;                                /* bumped by every change of list (the plan is redone) */
    Plan plan;
    std::set<std::string> start_sig;                 /* mod_key of every mod this session uses */
    std::vector<std::string> zip_dirs;               /* folders "Choose another file" took a zip from (mods.ini [.zips]) */
    std::map<std::string, StartInfo> start;          /* by lower(folder): what this session uses */
    std::map<std::string, std::string> serve;        /* lower file name -> host file the game gets */
    std::map<std::string, std::string> serve_mod;    /* lower file name -> the mod's name, for the log */
    std::atomic<int> nserve{0};
    std::mutex log_mx;                               /* logged */
    std::set<std::string> logged;
} g;

static void log_once(const std::string& key, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_once(const std::string& key, const char* fmt, ...)
{
    {
        std::lock_guard<std::mutex> lk(g.log_mx);
        if (!g.logged.insert(key).second) return;
    }
    char b[1400];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "[mods] %s\n", b);
}

static std::string game_key(const std::string& v)
{
    const std::string s = lower(v);
    if (s == "inf2" || s == "infamous2" || s == "i2") return "inf2";
    if (s == "inf1" || s == "infamous1" || s == "infamous") return "inf1";
    if (s == "fob" || s == "festivalofblood" || s == "festival_of_blood") return "fob";
    return s;
}
static const char* game_label(const std::string& k)
{
    return k == "inf1" ? "inFAMOUS" : k == "fob" ? "inFAMOUS Festival of Blood" : k == "inf2" ? "inFAMOUS 2" : "another game";
}

/* modular.json, as the Manager reads it (ListModules). */
static void read_modules(Mod& m)
{
    const J j = read_json(join(m.path, "modular.json"));
    std::set<std::string> used;
    /* ids are saved in mods.ini as "pick = a,b": no separators, brackets or
     * line breaks in them (a crafted id could otherwise add lines there) */
    auto clean = [](std::string s) {
        for (char& c : s) if ((unsigned char)c < 32 || c == ',' || c == '[' || c == ']' || c == '=' || c == '#' || c == ';') c = '_';
        return s;
    };
    auto parse = [&](const J& node, const std::string& fallback, bool dropdown, const std::string& gid, const std::string& gname) {
        std::string id = clean(node.str("id", node.str("name", fallback)));
        if (id.empty()) id = fallback;
        std::string uid = id;
        for (int k = 2; !used.insert(lower(uid)).second; k++) uid = id + "_" + std::to_string(k);
        Module mod;
        mod.id = uid;
        mod.name = node.str("name", uid);
        if (mod.name.empty()) mod.name = uid;
        mod.dropdown = dropdown; mod.group_id = gid; mod.group_name = gname;
        std::set<std::string> seen;
        if (const J* ps = node.get("paths")) if (ps->t == 4)
            for (const J& x : ps->a) {
                if (x.t != 3) continue;
                const std::string r = norm_rel(x.s);
                if (!r.empty() && seen.insert(lower(r)).second) mod.paths.push_back(r);
            }
        m.modules.push_back(mod);
    };
    if (const J* ms = j.get("modules")) if (ms->t == 4) {
        int k = 0;
        for (const J& x : ms->a) { k++; if (x.t == 5) parse(x, "module_" + std::to_string(k), false, "", ""); }
    }
    if (const J* gs = j.get("groups")) if (gs->t == 4) {
        int k = 0;
        for (const J& gr : gs->a) {
            k++;
            if (gr.t != 5) continue;
            std::string gid = clean(gr.str("id", "group_" + std::to_string(k)));
            if (gid.empty()) gid = "group_" + std::to_string(k);
            const std::string gname = gr.str("name", gid);
            const bool dd = ieq(gr.str("type", "toggle"), "dropdown");
            const J* os = gr.get("options");
            if (!os || os->t != 4) continue;
            int n = 0;
            const size_t first = m.modules.size();
            for (const J& o : os->a) { n++; if (o.t == 5) parse(o, gid + "_option_" + std::to_string(n), dd, gid, gname); }
            /* A "toggle" group whose options ALL change the same file (every two of them have
             * a file at the same place below their folders: the Karmic T-shirt's 7 designs of
             * one texture, the two glove colours) can only ever show one of them; ticking
             * several just lets one win at random. Such a group is one at a time: picked like
             * a dropdown (the saved option, else the first by name), shown as radio buttons.
             * Groups whose options change different files (Sasha's sword, amps and outfits)
             * stay check boxes. */
            if (!dd && m.modules.size() - first >= 2) {
                std::vector<std::set<std::string>> spots;
                for (size_t q = first; q < m.modules.size(); q++) {
                    std::set<std::string> s;
                    for (const std::string& r : m.modules[q].paths) {
                        const std::string root = join(m.path, r);
                        if (!is_dir(root) || is_link(root)) continue;
                        std::vector<std::string> v;
                        walk(root, "", v, 0, true);
                        for (const std::string& p : v) {
                            const std::string key = lower(rel_to(root, p));
                            if (key.find('/') != std::string::npos) s.insert(key);   /* a file inside a folder: not a picture beside it */
                        }
                    }
                    spots.push_back(std::move(s));
                }
                bool all = true;
                for (size_t a = 0; a < spots.size() && all; a++)
                    for (size_t b = a + 1; b < spots.size() && all; b++) {
                        bool meet = false;
                        for (const std::string& key : spots[a]) if (spots[b].count(key)) { meet = true; break; }
                        all = meet;
                    }
                if (all)
                    for (size_t q = first; q < m.modules.size(); q++) { m.modules[q].dropdown = true; m.modules[q].single = true; }
            }
        }
    }
    std::stable_sort(m.modules.begin(), m.modules.end(), [](const Module& a, const Module& b) { return ilt(a.name, b.name); });
}

/* The chosen options (the Manager's SelectedModuleIdsFromState): with no
 * saved choice (or an empty one) every toggle; each dropdown group's saved
 * option, else its first. */
static std::set<std::string> picked_ids(const Mod& m)
{
    std::set<std::string> out, saved;
    for (const std::string& s : m.pick) saved.insert(lower(s));
    for (const Module& x : m.modules)
        if (!x.dropdown && (!m.have_pick || m.pick.empty() || saved.count(lower(x.id)))) out.insert(lower(x.id));
    std::vector<std::string> groups;
    for (const Module& x : m.modules)
        if (x.dropdown && std::find_if(groups.begin(), groups.end(), [&](const std::string& s) { return ieq(s, x.group_id); }) == groups.end())
            groups.push_back(x.group_id);
    for (const std::string& gid : groups) {
        const Module* first = nullptr; const Module* chosen = nullptr;
        for (const Module& x : m.modules) {
            if (!x.dropdown || !ieq(x.group_id, gid)) continue;
            if (!first) first = &x;
            if (!chosen && saved.count(lower(x.id))) chosen = &x;
        }
        if (chosen || first) out.insert(lower((chosen ? chosen : first)->id));
    }
    return out;
}

/* Whether a path of the mod is in a chosen option (PathEnabledForMod).
 * sel == nullptr: the mod has no options, everything is used. */
static bool path_on(const std::vector<std::string>* sel, const std::string& rel)
{
    if (!sel) return true;
    const std::string n = lower(norm_rel(rel));
    for (const std::string& p : *sel)
        if (n == p || (n.size() > p.size() && !n.compare(0, p.size(), p) && n[p.size()] == '/')) return true;
    return false;
}

/* A path below the mod no part of which is a dot folder, backups or a patch folder. */
static bool part_ok(const std::string& rel)
{
    for (const std::string& part : split_parts(rel))
        if (part[0] == '.' || ieq(part, "backups") || ieq(part, "texture_patches") || ieq(part, "xpps_patches")) return false;
    return true;
}

/* What the mod changes with its chosen options: the Manager's ListModFiles,
 * ListTexturePatches and ListXppsPatches. A file it cannot use makes the
 * whole mod unusable (the Manager refuses to enable it). */
static void read_parts(Mod& m)
{
    /* m.err holds only this function's verdict (a mod.json error never gets
     * here), so a pick that fixes the options clears an old one */
    m.err.clear();
    m.files.clear(); m.tex.clear(); m.xp.clear(); m.targets.clear();
    std::vector<std::string> selv;
    const std::vector<std::string>* sel = nullptr;
    std::vector<std::string> roots;   /* the chosen options' folders */
    if (!m.modules.empty()) {
        const std::set<std::string> ids = picked_ids(m);
        std::set<std::string> ps;
        for (const Module& x : m.modules) if (ids.count(lower(x.id))) for (const std::string& p : x.paths) ps.insert(lower(p));
        selv.assign(ps.begin(), ps.end());
        sel = &selv;
        for (const Module& x : m.modules) {
            if (!ids.count(lower(x.id))) continue;
            for (const std::string& p : x.paths) {
                if (p.find("..") != std::string::npos) continue;
                const std::string full = join(m.path, p);
                if (is_dir(full) && !is_link(full) && std::find_if(roots.begin(), roots.end(), [&](const std::string& r) { return ieq(r, full); }) == roots.end())
                    roots.push_back(full);
            }
        }
        std::stable_sort(roots.begin(), roots.end(), ilt);
    }
    if (is_link(m.path)) { m.err = "its folder is a link (junction or symbolic link); copy the mod in instead"; return; }
    const std::string files_root = is_dir(join(m.path, "files")) && !is_link(join(m.path, "files")) ? join(m.path, "files") : m.path;
    const std::string backups = join(m.path, "backups");
    std::set<std::string> seen;

    /* whole files */
    struct F { std::string path, rel; };
    std::vector<F> fl;
    auto add_files = [&](const std::string& root, const std::string& rel_root) {
        std::vector<std::string> v;
        walk(root, ".xpps", v, 0, true);
        for (const std::string& p : v) {
            if (!ends_i(p, ".xpps") || !seen.insert(lower(p)).second) continue;
            if (!rel_to(backups, p).empty()) continue;
            const std::string rel = rel_to(rel_root, p);
            if (rel.empty() || !part_ok(rel) || !path_on(sel, rel)) continue;
            fl.push_back({ p, rel });
        }
    };
    add_files(files_root, files_root);
    for (const std::string& r : roots) add_files(r, m.path);
    std::stable_sort(fl.begin(), fl.end(), [](const F& a, const F& b) { return ilt(a.rel, b.rel); });
    for (const F& f : fl) {
        const std::string t = lower(base_of(f.rel));
        bool dup = false;
        for (const Part& q : m.files) if (q.target == t) dup = true;
        if (dup) { log_once("dup:" + m.folder + t, "%s: two files named %s; using the first (%s)", m.name.c_str(), t.c_str(), f.rel.c_str()); continue; }
        m.files.push_back({ f.path, t });
    }

    /* patches below <root>/texture_patches and <root>/xpps_patches */
    auto target_of = [](const std::string& patch_root, const std::string& p) {
        const std::vector<std::string> parts = split_parts(rel_to(patch_root, p));
        for (size_t i = 0; i + 1 < parts.size(); i++)
            if (ends_i(parts[i], ".xpps") || ends_i(parts[i], ".xpp")) return lower(parts[i]);
        return std::string();
    };
    std::set<std::string> seen_tex, seen_xp;
    auto add_patches = [&](const std::string& root, bool rel_mod) {
        const std::string rel_root = rel_mod ? m.path : root;
        const std::string tr = join(root, "texture_patches"), xr = join(root, "xpps_patches");
        std::vector<std::string> v;
        if (is_dir(tr) && !is_link(tr)) walk(tr, ".dds", v, 0, true);
        std::vector<Tex> tl;
        for (const std::string& p : v) {
            if (!seen_tex.insert(lower(p)).second || !path_on(sel, rel_to(rel_root, p))) continue;
            const std::string t = target_of(tr, p);
            if (t.empty()) continue;
            Tex x;
            x.path = p; x.target = t; x.name = base_of(p);
            if (!parse_tex_name(x.name, x.off, x.w, x.h, x.fmt, x.mips)) { m.err = x.name + " does not use the exported texture name format"; return; }
            if (x.w < 1 || x.h < 1 || x.w > 16384 || x.h > 16384 || x.mips < 1 || x.mips > 16) {
                m.err = x.name + " names a size or mip count no texture has";
                return;
            }
            Bytes hd;
            const FInfo fi = finfo(p);
            if (!read_all(p, hd, 0, 128) || hd.size() < 128 || memcmp(hd.data(), "DDS ", 4)) { m.err = x.name + " is not a DDS file"; return; }
            const int hh = (int)le32(&hd[12]), ww = (int)le32(&hd[16]), mm = (int)le32(&hd[28]);
            const std::string four((const char*)&hd[84], 4);
            if (ww != x.w || hh != x.h || upper(four) != x.fmt || mm != x.mips) {   /* the Manager upper-cases the fourcc */
                m.err = x.name + " header does not match its name";
                return;
            }
            if (fi.size - 128 != dxt_size(x.w, x.h, x.fmt, x.mips)) { m.err = x.name + " is not the size its name says"; return; }
            tl.push_back(x);
        }
        std::stable_sort(tl.begin(), tl.end(), [](const Tex& a, const Tex& b) {
            if (a.target != b.target) return a.target < b.target;
            if (a.off != b.off) return a.off < b.off;
            return ilt(a.name, b.name); });
        for (Tex& x : tl) m.tex.push_back(x);
        v.clear();
        if (is_dir(xr) && !is_link(xr)) walk(xr, ".bin", v, 0, true);
        std::vector<Xp> xl;
        for (const std::string& p : v) {
            if (!seen_xp.insert(lower(p)).second || !path_on(sel, rel_to(rel_root, p))) continue;
            const std::string t = target_of(xr, p);
            if (t.empty()) continue;
            Xp x;
            x.path = p; x.target = t; x.name = base_of(p);
            if (!parse_xp_name(x.name, x.index, x.abs, x.chunk, x.choff)) { m.err = x.name + " is not named file_<index>_<offset>[_offset_<offset>].bin"; return; }
            x.len = (uint32_t)finfo(p).size;
            xl.push_back(x);
        }
        std::stable_sort(xl.begin(), xl.end(), [](const Xp& a, const Xp& b) {
            if (a.target != b.target) return a.target < b.target;
            if (a.index != b.index) return a.index < b.index;
            if (a.abs != b.abs) return a.abs < b.abs;
            return ilt(a.name, b.name); });
        for (Xp& x : xl) m.xp.push_back(x);
    };
    add_patches(files_root, false);
    for (const std::string& r : roots) { if (!m.err.empty()) break; add_patches(r, true); }
    if (!m.err.empty()) { m.files.clear(); m.tex.clear(); m.xp.clear(); return; }
    for (const Part& p : m.files) m.targets.insert(p.target);
    for (const Tex& p : m.tex) m.targets.insert(p.target);
    for (const Xp& p : m.xp) m.targets.insert(p.target);
    if (m.targets.empty()) m.err = "it has no .xpps files, texture patches or xpps patches (with these options)";
    if (m.live && m.err.empty()) {   /* live: bytes written over what the game loaded, so nothing may move */
        if (!m.files.empty()) m.err = "a live mod can only patch bytes (texture patches, xpps _offset_ patches), not replace whole files";
        for (const Xp& x : m.xp) if (m.err.empty() && !x.chunk) m.err = "a live mod cannot replace a packed file whole (" + x.name + "): only _offset_ patches";
    }
    /* what the files are now (a mod replaced from a zip under the same folder
     * name changes it): the menu compares it with the start's */
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string& s, uint64_t a, int64_t b) {
        for (char c : lower(s)) { h ^= (uint8_t)c; h *= 1099511628211ull; }
        for (int k = 0; k < 8; k++) { h ^= (uint8_t)(a >> (8 * k)); h *= 1099511628211ull; }
        for (int k = 0; k < 8; k++) { h ^= (uint8_t)((uint64_t)b >> (8 * k)); h *= 1099511628211ull; }
    };
    for (const Part& p : m.files) { const FInfo f = finfo(p.path); mix(p.path, f.size, f.mtime); }
    for (const Tex& p : m.tex) { const FInfo f = finfo(p.path); mix(p.path, f.size, f.mtime); }
    for (const Xp& p : m.xp) { const FInfo f = finfo(p.path); mix(p.path, f.size, f.mtime); }
    m.stamp = h;
}

/* ---- the saved choices (mods.ini) ------------------------------------------ */

/* mods.ini: a section per mod folder with anything not at its default --
 *   [Sasha's Black Tar]
 *   on = 0                 turned off
 *   pick = default,jbintro the options picked
 *   priority = 2           put first by the player ("Use this one instead"): a
 *                          mod with a priority comes before every mod without
 *                          one, a higher priority before a lower one
 *   win = Climbing Gloves|cole.xpps|p80
 *   lose = Climbing Gloves|cole.xpps|t1a000
 *                          a conflict the player resolved (the menu's merge
 *                          dialog): for that part of that file this mod's
 *                          version is used (win) or the other mod's (lose);
 *                          the other mod's section says the opposite
 * and the folders the zip list also looks in ("Choose another file"):
 *   [.zips]
 *   folder = D:/Downloads
 * ([.zips] can be no mod's folder: the scan skips dot folders.) Older builds
 * read the file as before: they skip the keys and the section they do not know. */
struct Saved { bool have_on = false, on = true, have_pick = false, have_prio = false; int prio = 0; std::vector<std::string> pick;
               std::map<std::string, bool> res; };

static const char* const kZipSection = ".zips";

static std::map<std::string, Saved> state_read(std::vector<std::string>* zip_dirs = nullptr)
{
    std::map<std::string, Saved> st;
    FILE* f = g.state_path.empty() ? nullptr : fopen(g.state_path.c_str(), "r");
    if (!f) return st;
    char line[2048];
    std::string cur;
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) a++;
        s.erase(0, a);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        if (s[0] == '[' && s.back() == ']') { cur = lower(s.substr(1, s.size() - 2)); continue; }
        const size_t eq = s.find('=');
        if (cur.empty() || eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
        if (cur == kZipSection) {
            if (k == "folder" && !v.empty() && zip_dirs && zip_dirs->size() < 8) zip_dirs->push_back(v);
            continue;
        }
        Saved& sv = st[cur];
        if (k == "priority") {
            const long p = strtol(v.c_str(), nullptr, 10);
            if (p >= 1) { sv.have_prio = true; sv.prio = p > 1000000 ? 1000000 : (int)p; }
        }
        else if (k == "on") { sv.have_on = true; sv.on = v != "0"; }
        else if (k == "win" || k == "lose") {   /* <other folder>|<file>|<part>: the file and part hold no '|' */
            const size_t b2 = v.rfind('|'), b1 = b2 == std::string::npos || b2 == 0 ? std::string::npos : v.rfind('|', b2 - 1);
            if (b1 != std::string::npos && b1 > 0 && b2 + 1 < v.size()) sv.res[lower(v)] = k == "win";
        }
        else if (k == "pick") {
            sv.have_pick = true; sv.pick.clear();
            size_t s0 = 0;
            for (size_t i = 0; i <= v.size(); i++)
                if (i == v.size() || v[i] == ',') { if (i > s0) sv.pick.push_back(v.substr(s0, i - s0)); s0 = i + 1; }
        }
    }
    fclose(f);
    return st;
}

/* Called with g.mx held. */
static void state_write_locked()
{
    if (g.state_path.empty()) return;
    std::string out = "# inFAMOUS 2 port: the game mods the menu turned off, the options picked and the mods\n"
                      "# put first (F2 > Mods). A mod not listed here is on, with its default options, in\n"
                      "# folder order.\n";
    for (const Mod& m : g.list) {
        if (m.on && !m.have_pick && !m.have_prio && m.res.empty()) continue;
        out += "[" + m.folder + "]\n";
        if (!m.on) out += "on = 0\n";
        if (m.have_pick) {
            out += "pick = ";
            for (size_t i = 0; i < m.pick.size(); i++) out += (i ? "," : "") + m.pick[i];
            out += "\n";
        }
        if (m.have_prio) out += "priority = " + std::to_string(m.prio) + "\n";
        for (const auto& r : m.res) out += (r.second ? "win = " : "lose = ") + r.first + "\n";
    }
    if (!g.zip_dirs.empty()) {
        out += std::string("[") + kZipSection + "]\n";
        for (const std::string& d : g.zip_dirs) out += "folder = " + d + "\n";
    }
    Bytes b(out.begin(), out.end());
    if (!write_all(g.state_path, b)) fprintf(stderr, "[mods] could not save %s\n", g.state_path.c_str());
}

/* A live mod's karma level: its patches, out of <mod>/<folder> (files/ in it, as a mod's). */
static Mod live_level_mod(const Mod& m, const std::string& folder)
{
    Mod x = m;
    x.path = join(m.path, folder);
    x.modules.clear(); x.levels.clear();
    if (folder.find("..") != std::string::npos || !is_dir(x.path)) { x.err = "its folder (" + folder + ") is not in the mod"; return x; }
    read_parts(x);
    return x;
}

/* The mod folders: <dir>/<x>/mod.json, or <dir>/<x>/<y>/mod.json (a zip
 * unpacked into a folder of its own name). Dot folders are skipped. */
static std::vector<Mod> scan()
{
    std::vector<Mod> out;
    if (g.dir.empty()) return out;
    const std::map<std::string, Saved> st = state_read();
    auto add = [&](const std::string& path, const std::string& folder) {
        Mod m;
        m.path = path; m.folder = folder;
        std::string jerr;
        const J j = read_json(join(path, "mod.json"), &jerr);
        m.name = j.str("name", base_of(folder));
        if (m.name.empty()) m.name = base_of(folder);
        m.author = j.str("author", "unknown");
        m.version = j.str("version", "unknown");
        m.game = game_key(j.str("game"));
        m.for_game = m.game == "inf2";
        m.image = j.str("image");
        if (const J* lj = j.get("live")) m.live = lj->t == 1 ? lj->s == "true" : (lj->t == 2 && lj->s != "0");
        if (const J* kj = j.get("karma_levels"))
            if (kj->t == 5)
                for (size_t i = 0; i < kj->ok.size(); i++)
                    for (int q = 0; q < 7; q++)
                        if (ieq(kj->ok[i], kKarmaLevels[q]) && kj->ov[i].t == 3 && !kj->ov[i].s.empty()) m.levels.push_back({ q, kj->ov[i].s });
        if (const J* dp = j.get("discover_powers"))   /* the power numbers a mod adds: a save made before it never
                                                         * reaches their story moment, so they would stay undiscovered */
            if (dp->t == 4)
                for (const J& a : dp->a)
                    if (a.t == 2) { const int v = atoi(a.s.c_str()); if (v >= 0 && v < 96) m.discover.push_back(v); }
        if (const J* kj = j.get("karma_patches"))   /* [{"levels": ["Thug", ...], "find": "<hex>", "replace": "<hex>"}] */
            if (kj->t == 4)
                for (const J& e : kj->a) {
                    auto hex = [](const J* h, std::vector<int>& out) {
                        if (!h || h->t != 3) return false;
                        auto dig = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
                        std::string t;
                        for (char c : h->s) if (c != ' ') t += c;
                        if (t.size() % 2) return false;
                        for (size_t i = 0; i < t.size(); i += 2) {
                            if (t[i] == '?' && t[i + 1] == '?') { out.push_back(-1); continue; }
                            const int a = dig(t[i]), b = dig(t[i + 1]);
                            if (a < 0 || b < 0) return false;
                            out.push_back(a * 16 + b);
                        }
                        return true;
                    };
                    Mod::KarmaPatch kp;
                    bool ok = e.t == 5;
                    if (ok) {
                        if (const J* lj = e.get("levels"))
                            if (lj->t == 4)
                                for (const J& a : lj->a)
                                    for (int q = 0; q < 7; q++) if (a.t == 3 && ieq(a.s, kKarmaLevels[q])) kp.levels |= 1 << q;
                        ok = kp.levels && hex(e.get("find"), kp.find) && hex(e.get("replace"), kp.rep) && kp.find.size() == kp.rep.size() &&
                             kp.find.size() >= 16 && kp.find.size() <= 4096 && kp.find.size() % 4 == 0;
                        bool key = false, any = false;
                        for (size_t i = 0; ok && i < kp.find.size(); i++) {
                            if (kp.rep[i] >= 0) { any = true; if (kp.find[i] < 0) ok = false; }   /* a replaced byte must be known */
                            if (i % 4 == 0 && i + 8 <= kp.find.size()) {
                                bool k8 = true;
                                for (size_t q = i; q < i + 8; q++) if (kp.find[q] < 0 || kp.rep[q] >= 0) k8 = false;
                                key = key || k8;
                            }
                        }
                        ok = ok && any && key;
                    }
                    if (ok) m.kpatch.push_back(std::move(kp));
                    else fprintf(stderr, "[mods] %s: a karma_patches entry is not used (it needs \"levels\", and \"find\" and \"replace\" "
                                         "as hex of the same length, 16-4096 bytes, 8 known bytes in a row that replace leaves alone)\n", m.name.c_str());
                }
        if (const J* ej = j.get("electric_dash")) {   /* true, or {"speed", "anim", "drain", "glow", "glowskip"} */
            if (ej->t == 1) m.ed = ej->s == "true";
            else if (ej->t == 5) {
                m.ed = true;
                for (size_t i = 0; i < ej->ok.size(); i++) {
                    const J& v = ej->ov[i];
                    const double x = v.t == 2 ? atof(v.s.c_str()) : v.t == 1 ? (v.s == "true" ? 1.0 : 0.0) : -1.0;
                    if (x < 0) continue;
                    if (ieq(ej->ok[i], "speed")) m.ed_speed = (float)x;
                    else if (ieq(ej->ok[i], "anim")) m.ed_anim = (float)x;
                    else if (ieq(ej->ok[i], "drain")) m.ed_drain = (float)x;
                    else if (ieq(ej->ok[i], "glow")) m.ed_glow = x != 0.0;
                    else if (ieq(ej->ok[i], "glowskip")) m.ed_glowskip = (int)x;
                }
            }
        }
        if (const J* aj = j.get("file_alias"))     /* {"movies/ui/powerk_bolt9.bik": "movies/ui/powerk_fatbolt.bik"}: a file
                                                      * the game asks for and does not have, read from another of its files */
            if (aj->t == 5)
                for (size_t i = 0; i < aj->ok.size(); i++) {
                    std::string from, to;
                    if (aj->ov[i].t != 3 || !safe_rel(norm_rel(aj->ok[i]), from, nullptr) || !safe_rel(norm_rel(aj->ov[i].s), to, nullptr)) {
                        fprintf(stderr, "[mods] %s: file_alias \"%s\" not used (not a path inside the game folder)\n", m.name.c_str(), aj->ok[i].c_str());
                        continue;
                    }
                    m.alias.push_back({ lower(from), to });
                }
        if (const J* pj = j.get("patch_for"))      /* the mods this patch merges: used only while all of them are */
            if (pj->t == 4)
                for (const J& a : pj->a)
                    if (a.t == 3 && !a.s.empty()) m.patch_for.push_back(a.s);
        if (const J* dj = j.get("dark_bolts")) {   /* true, or the karma levels it is for */
            if (dj->t == 1 && dj->s == "true") m.dark = 0x80;
            else if (dj->t == 4)
                for (const J& a : dj->a)
                    for (int q = 0; q < 7; q++)
                        if (a.t == 3 && ieq(a.s, kKarmaLevels[q])) m.dark |= 1 << q;
        }
        auto it = st.find(lower(folder));
        if (it != st.end()) {
            if (it->second.have_on) m.on = it->second.on;
            m.have_pick = it->second.have_pick; m.pick = it->second.pick;
            m.have_prio = it->second.have_prio; m.prio = it->second.prio;
            m.res = it->second.res;
        }
        if (!jerr.empty()) m.err = jerr;
        else if (m.for_game) { read_modules(m); read_parts(m); }
        if (m.live && !m.levels.empty() && m.err.empty())
            for (const auto& l : m.levels) {   /* every level's patches must read; their files are part of the mod's key */
                const Mod x = live_level_mod(m, l.second);
                if (!x.err.empty()) { m.err = std::string("karma level ") + kKarmaLevels[l.first] + ": " + x.err; break; }
                m.stamp = (m.stamp ^ x.stamp) * 1099511628211ull + (uint64_t)l.first;
            }
        out.push_back(std::move(m));
    };
    for (const std::string& n : list_dir(g.dir)) {
        if (n[0] == '.') continue;
        const std::string p = join(g.dir, n);
        if (!is_dir(p)) continue;
        if (finfo(join(p, "mod.json")).ok) { add(p, n); continue; }
        for (const std::string& n2 : list_dir(p)) {
            if (n2[0] == '.') continue;
            const std::string p2 = join(p, n2);
            if (is_dir(p2) && finfo(join(p2, "mod.json")).ok) add(p2, n + "/" + n2);
        }
    }
    return out;
}

/* ---- building what the game gets ------------------------------------------- */

struct Arc { std::string path; int prio; };

static int arc_prio(const std::string& name)
{
    const std::string s = lower(name);
    if (!s.compare(0, 5, "patch")) return 100 + atoi(s.c_str() + 5);
    if (s == "infamous2.psarc_s") return 2;
    if (s == "infamous1.psarc_s") return 1;
    return 0;
}

/* The game's big archives, highest first (patchN by N, then infamous2, infamous1). */
static std::vector<Arc> archives()
{
    std::vector<std::string> v;
    if (const char* h = getenv("PS3_HDD0_ROOT")) {
        /* Only the running game's folders: a shared dev_hdd0 can hold other
         * games' patchN.psarc_s, which would outrank this game's. The game's id
         * is PS3_TITLE_ID, else the first 9 characters of the folder that holds
         * infamous2.psarc_s (BCES01143_INSTALL -> BCES01143, whose patches are
         * in BCES01143); none found: every folder, as before. */
        const std::string gd = join(slashes(h), "game");
        const std::vector<std::string> titles = list_dir(gd);
        std::string id;
        if (const char* t = getenv("PS3_TITLE_ID")) id = t;
        for (size_t i = 0; id.empty() && i < titles.size(); i++) {
            std::vector<std::string> w;
            walk(join(join(gd, titles[i]), "USRDIR"), ".psarc_s", w);
            for (const std::string& p : w)
                if (ieq(base_of(p), "infamous2.psarc_s")) { id = titles[i].substr(0, 9); break; }
        }
        for (const std::string& t : titles)
            if (id.empty() || (t.size() >= id.size() && lang_ieq_n(t.c_str(), id.c_str(), id.size())))
                walk(join(join(gd, t), "USRDIR"), ".psarc_s", v);
    }
    walk(join(slashes(ppu_vfs_root ? ppu_vfs_root : "."), "PS3_GAME/USRDIR"), ".psarc_s", v);
    std::vector<Arc> a;
    std::set<std::string> seen;
    for (const std::string& p : v) if (seen.insert(lower(p)).second) a.push_back({ p, arc_prio(base_of(p)) });
    std::stable_sort(a.begin(), a.end(), [](const Arc& x, const Arc& y) { return x.prio > y.prio; });
    return a;
}

/* The game's big archives, opened on first need (highest first). */
struct Arcs {
    bool loaded = false;
    std::vector<std::unique_ptr<Psarc>> v;
    Psarc* find(const std::string& name, int& idx)
    {
        if (!loaded) {
            loaded = true;
            for (const Arc& a : archives()) {
                std::unique_ptr<Psarc> p(new Psarc());
                if (p->open(a.path)) v.push_back(std::move(p));
                else fprintf(stderr, "[mods] cannot read the archive %s\n", a.path.c_str());
            }
        }
        for (auto& p : v) if ((idx = p->find(name)) >= 0) return p.get();
        return nullptr;
    }
};

static std::string sig_line(const char* kind, const std::string& p)
{
    const FInfo f = finfo(p);
    char b[64];
    snprintf(b, sizeof b, " %llu %lld\n", (unsigned long long)f.size, (long long)f.mtime);
    return std::string(kind) + " " + p + b;
}

/* The game file `name` with the patches written in (base: a mod's whole file,
 * else the original), built into the cache unless the cache already holds
 * this exact build. dry: only whether it builds (nothing read from or
 * written to the cache). */
static bool build(const std::string& name, const Part* base, const std::vector<const Tex*>& tex,
                  const std::vector<const Xp*>& xp, Arcs& arcs, std::string& out_path, std::string& err, bool dry = false,
                  Bytes* keep = nullptr)
{
    out_path = join(g.cache, name);
    std::string sig = "gmods 1\n";
    std::string from;
    Psarc* arc = nullptr;
    int idx = -1;
    if (base) sig += sig_line("base", base->path);
    else {
        arc = arcs.find(name, idx);
        if (!arc) { err = name + " is not in the game's archives"; return false; }
        from = arc->path;
        sig += sig_line("archive", from);
    }
    for (const Tex* t : tex) sig += sig_line("tex", t->path);
    for (const Xp* x : xp) sig += sig_line("xpps", x->path);
    Bytes old;
    if (!dry && !keep && finfo(out_path).ok && read_all(out_path + ".sig", old) && std::string(old.begin(), old.end()) == sig) return true;

    Bytes d;
    if (base) { if (!read_all(base->path, d)) { err = "cannot read " + base->path; return false; } }
    else if (!arc->entry((uint32_t)idx, d)) { err = "cannot read " + name + " out of " + base_of(slashes(from)); return false; }
    if (!tex.empty()) {
        if (d.size() < 48 || memcmp(d.data(), "PACK", 4)) { err = name + " is not an XPPS/XPP PACK file"; return false; }
        const uint64_t ds = be32(&d[40]);
        const std::set<TexKey> known = xp_textures(d);
        size_t misfit = 0;
        const Tex* first = nullptr;
        for (const Tex* t : tex)
            if (!known.count(TexKey(t->off, t->w, t->h, t->fmt, t->mips))) { misfit++; if (!first) first = t; }
        if (misfit == 1) {
            err = first->name + " was made for another version of " + name + ": this game's copy has no such texture there";
            return false;
        }
        if (misfit) {
            err = std::to_string(misfit) + " textures (" + first->name + " and more) were made for another version of " + name +
                  ": this game's copy has no such textures there";
            return false;
        }
        for (const Tex* t : tex) {
            Bytes pl;
            if (!read_all(t->path, pl, 128)) { err = "cannot read " + t->path; return false; }
            /* the size the scan checked; the file may have changed since */
            if (pl.size() != dxt_size(t->w, t->h, t->fmt, t->mips)) { err = t->name + " is not the size its name says"; return false; }
            const uint64_t at = ds + t->off;
            if (at + pl.size() > d.size()) { err = t->name + " would patch outside " + name; return false; }
            flip_payload(pl.data(), t->w, t->h, t->fmt, t->mips);
            memcpy(d.data() + at, pl.data(), pl.size());
        }
    }
    if (!xp.empty()) {
        XpLayout L;
        if (!xp_layout(d, L, err)) { err = name + ": " + err; return false; }
        for (const Xp* x : xp) {
            Bytes pl;
            if (!read_all(x->path, pl)) { err = "cannot read " + x->path; return false; }
            int fi = -1;
            if (x->index >= 0 && x->index < (int)L.files.size()) fi = x->index;
            else {
                int hits = 0;
                for (size_t i = 0; i < L.files.size(); i++)
                    if ((uint64_t)L.data_off + L.files[i].off == x->abs) { fi = (int)i; hits++; }
                if (hits != 1) { err = x->name + " does not match any file entry in " + name; return false; }
            }
            XpFile& F = L.files[(size_t)fi];
            const uint64_t a0 = (uint64_t)L.data_off + F.off, a1 = a0 + F.size;
            if (a1 > d.size()) { err = x->name + " points outside " + name; return false; }
            if (x->chunk) {
                if ((uint64_t)x->choff + pl.size() > F.size) { err = x->name + " points outside file_" + std::to_string(fi) + " in " + name; return false; }
                memcpy(d.data() + a0 + x->choff, pl.data(), pl.size());
                continue;
            }
            /* the whole packed file: later files, the chunks after it and the data size move.
             * The table entries are rewritten at their places in the file, which
             * only stay put when the replaced bytes lie after the header and both
             * tables -- as in every one of the game's own files. A file whose
             * header puts a packed file over its tables (a mod's own copy can) is
             * refused, and every table write is bounds-checked, as the Manager's
             * WriteXppsU32 is. */
            if (a0 < L.tables_end) { err = x->name + " would replace a part of " + name + " that holds its tables"; return false; }
            const int64_t delta = (int64_t)pl.size() - (int64_t)F.size;
            Bytes nd;
            nd.reserve((size_t)((int64_t)d.size() + delta));
            nd.insert(nd.end(), d.begin(), d.begin() + (ptrdiff_t)a0);
            nd.insert(nd.end(), pl.begin(), pl.end());
            nd.insert(nd.end(), d.begin() + (ptrdiff_t)a1, d.end());
            d.swap(nd);
            bool in_range = true;
            auto put = [&](uint64_t at, uint32_t v) {
                if (at + 4 > d.size()) { in_range = false; return; }
                put_be32(&d[(size_t)at], v);
            };
            const uint32_t old_off = F.off;
            F.size = (uint32_t)pl.size();
            put(F.size_at, F.size);
            if (delta) {
                for (size_t i = (size_t)fi + 1; i < L.files.size(); i++) {
                    L.files[i].off = (uint32_t)((int64_t)L.files[i].off + delta);
                    put(L.files[i].off_at, L.files[i].off);
                }
                int ci = -1;
                for (size_t i = 0; i < L.chunks.size(); i++)
                    if ((int64_t)L.chunks[i].file_index <= fi && fi < (int64_t)L.chunks[i].file_index + L.chunks[i].file_count) { ci = (int)i; break; }
                if (ci >= 0) {
                    L.chunks[(size_t)ci].size = (uint32_t)((int64_t)L.chunks[(size_t)ci].size + delta);
                    put(L.chunks[(size_t)ci].size_at, L.chunks[(size_t)ci].size);
                }
                for (size_t i = 0; i < L.chunks.size(); i++)
                    if ((int)i != ci && L.chunks[i].off > old_off) {
                        L.chunks[i].off = (uint32_t)((int64_t)L.chunks[i].off + delta);
                        put(L.chunks[i].off_at, L.chunks[i].off);
                    }
                L.data_size = (uint32_t)((int64_t)L.data_size + delta);
                put(44, L.data_size);
            }
            if (!in_range) { err = x->name + ": a table entry of " + name + " lies outside it"; return false; }
        }
    }
    if (keep) { keep->swap(d); return true; }   /* a live mod's pairs (live_build) */
    if (dry) return true;
    if (!write_all(out_path, d)) { err = "cannot write " + out_path; return false; }
    Bytes sb(sig.begin(), sig.end());
    write_all(out_path + ".sig", sb);
    fprintf(stderr, "[mods] built %s (%zu bytes: %zu texture patch(es), %zu xpps patch(es) on %s)\n", name.c_str(), d.size(),
            tex.size(), xp.size(), base ? "the mod's file" : base_of(from).c_str());
    return true;
}

static bool xp_overlap(const Xp& a, const Xp& b)
{
    if (a.target != b.target || a.index != b.index) return false;
    if (!a.chunk || !b.chunk) return true;
    return a.choff < b.choff + b.len && b.choff < a.choff + a.len;
}

/* Whether two mods change the same thing, so only one of them can be used:
 * the same whole file, a whole file one replaces and the other patches, a
 * texture at the same offset, or packed-file patches that overlap. The same
 * rule resolve() always had, both ways round. */
/* ---- automatic merges ------------------------------------------------------------
 * Two mods that replace the same whole file, or one replaces it and the other
 * patches packed files in it, are both used when they change different packed
 * files. Each mod's copy is compared with the game's, packed file by packed file
 * (same number of packed files, else it can't merge); the file the game gets is
 * the game's own with each mod's changed packed files put in, as an
 * xpps_patches file_<i>_... patch would, and the other mods' packed-file patches
 * on top. Texture patches onto such a file still clash: their offsets move with
 * the packed files. Comparisons are cached in <cache>/merge by the mod file's
 * path, size and time, so only the first start after a change reads the files. */
struct PfDiff { bool ok = false; std::vector<int> changed; int extra = 0; };   /* extra: packed files it adds at the end */
static std::mutex g_pf_mx;
static std::map<std::string, PfDiff> g_pf;
static Arcs g_pf_arcs;   /* under g_pf_mx: the game's archives, for the comparisons */

static uint64_t fnv64(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}
static std::string hex64(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%016llx", (unsigned long long)v); return b; }

/* The packed files f's copy of its target changes against the game's. */
static PfDiff pf_diff(const Part& f)
{
    const std::string key = "v2\n" + sig_line("mod", f.path) + "target " + f.target + "\n";
    std::lock_guard<std::mutex> lk(g_pf_mx);
    auto it = g_pf.find(key);
    if (it != g_pf.end()) return it->second;
    PfDiff r;
    const std::string dir = join(g.cache, "merge"), cp = join(dir, hex64(fnv64(key)) + ".txt");
    Bytes cb;
    if (!g.cache.empty() && read_all(cp, cb)) {
        const std::string c(cb.begin(), cb.end());
        if (c.size() > key.size() && !c.compare(0, key.size(), key)) {
            const std::string rest = c.substr(key.size());
            if (!rest.compare(0, 2, "ok")) {
                r.ok = true;
                const char* p = rest.c_str() + 2;
                while (*p) {
                    char* e = nullptr;
                    const long v = strtol(p, &e, 10);
                    if (e == p) break;
                    if (v < 0) r.extra = (int)-v; else r.changed.push_back((int)v);
                    p = e;
                }
            }
            g_pf[key] = r;
            return r;
        }
    }
    int idx = -1;
    Psarc* a = g_pf_arcs.find(f.target, idx);
    Bytes o, m;
    XpLayout Lo, Lm;
    std::string e;
    if (a && a->entry((uint32_t)idx, o) && read_all(f.path, m) && xp_layout(o, Lo, e) && xp_layout(m, Lm, e) &&
        Lo.files.size() <= Lm.files.size()) {   /* the same packed files, or the game's and more at the end */
        r.ok = true;
        r.extra = (int)(Lm.files.size() - Lo.files.size());
        for (size_t i = 0; i < Lo.files.size() && r.ok; i++) {
            const uint64_t ao = (uint64_t)Lo.data_off + Lo.files[i].off, am = (uint64_t)Lm.data_off + Lm.files[i].off;
            const uint32_t so = Lo.files[i].size, sm = Lm.files[i].size;
            if (ao + so > o.size() || am + sm > m.size()) { r.ok = false; break; }
            if (so != sm || memcmp(o.data() + ao, m.data() + am, so)) r.changed.push_back((int)i);
        }
        if (!r.ok) { r.changed.clear(); r.extra = 0; }
    }
    if (!g.cache.empty()) {
        std::string c = key + (r.ok ? "ok" : "no");
        for (int i : r.changed) c += " " + std::to_string(i);
        if (r.extra) c += " -" + std::to_string(r.extra);
        c += "\n";
        mkdirs(dir);
        write_all(cp, Bytes(c.begin(), c.end()));
    }
    fprintf(stderr, "[mods] merge: %s changes %s\n", f.path.c_str(),
            r.ok ? (std::to_string(r.changed.size()) + " of its packed files" + (r.extra ? " and adds " + std::to_string(r.extra) : std::string())).c_str()
                 : "a file that can't be compared packed file by packed file");
    g_pf[key] = r;
    return r;
}

static const Part* whole_of(const Mod& m, const std::string& t)
{
    for (const Part& f : m.files) if (f.target == t) return &f;
    return nullptr;
}

/* Whether m's whole copy of t adds packed files (only one mod's can be the base). */
static bool adds(const Mod& m, const std::string& t)
{
    const Part* f = whole_of(m, t);
    return f && pf_diff(*f).extra > 0;
}

/* The packed files m changes in t: its whole file's changed ones, and its
 * packed-file patches. false: not known (a texture patch, or a file that
 * doesn't compare). */
static bool touched(const Mod& m, const std::string& t, std::set<int>& out)
{
    for (const Tex& x : m.tex) if (x.target == t) return false;
    if (const Part* f = whole_of(m, t)) {
        const PfDiff d = pf_diff(*f);
        if (!d.ok) return false;
        for (int i : d.changed) if (!f->drop.count(i)) out.insert(i);
    }
    for (const Xp& x : m.xp) if (x.target == t) out.insert(x.index);
    return true;
}

/* ---- conflicts: merged byte by byte, or resolved by the player -------------------
 * What two mods both change, part by part: a packed file ("p<i>"), a texture at
 * an offset in the file's data ("t<hex>"), or the whole file ("*": a file that
 * can't be compared packed file by packed file, or a texture patch onto a file
 * the other mod replaces). A packed file both change is merged byte by byte when
 * their changes keep its size and touch different bytes (a three-way merge
 * against the game's copy): nothing to choose. For any other part the player
 * picks whose version is used (F2 > Mods asks when a mod is turned on; mods.ini
 * win / lose); the other mod's change to that part is left out and everything
 * else of both mods is used. A conflict nobody resolved leaves the later mod out
 * whole, as before. Live mods are never merged byte by byte (their bytes are
 * found and written in memory), but their parts can be given to one of them. */
struct Clash { std::string target, item; bool merges = false; };

static std::string res_key(const Mod& other, const std::string& t, const std::string& item)
{
    return lower(other.folder) + "|" + t + "|" + item;
}
/* Whose version of the part is used: 0 a's, 1 b's, -1 not resolved. */
static int res_winner(const Mod& a, const Mod& b, const std::string& t, const std::string& item)
{
    auto it = a.res.find(res_key(b, t, item));
    if (it != a.res.end()) return it->second ? 0 : 1;
    it = b.res.find(res_key(a, t, item));
    if (it != b.res.end()) return it->second ? 1 : 0;
    return -1;
}
static std::string item_label(const std::string& item)
{
    if (item == "*") return "the whole file";
    if (!item.empty() && item[0] == 't') return "the texture at 0x" + upper(item.substr(1));
    return "packed file " + item.substr(1);
}

/* What m puts into packed file i of t: the files that decide it, for cache keys. */
static std::string part_sig(const Mod& m, const std::string& t, int i)
{
    std::string s;
    if (const Part* f = whole_of(m, t)) if (!f->drop.count(i)) s += sig_line("mod", f->path);
    for (const Xp& x : m.xp)
        if (x.target == t && x.index == i) s += sig_line(x.chunk ? ("xp@" + std::to_string(x.choff)).c_str() : "xp", x.path);
    return s;
}

/* m's copy of packed file i of t (o: the game's file, Lo its layout; md: m's whole
 * file, read once by the caller): its whole file's copy, else the game's, with its
 * packed-file patches on top. */
static bool pf_version(const Mod& m, const std::string& t, int i, const Bytes& o, const XpLayout& Lo, const Bytes* md, Bytes& v)
{
    if (i < 0 || (size_t)i >= Lo.files.size()) return false;
    const Part* f = whole_of(m, t);
    if (f && !f->drop.count(i)) {
        XpLayout Lm;
        std::string e;
        if (!md || !xp_layout(*md, Lm, e) || Lm.files.size() < Lo.files.size()) return false;
        const uint64_t a = (uint64_t)Lm.data_off + Lm.files[(size_t)i].off;
        if (a + Lm.files[(size_t)i].size > md->size()) return false;
        v.assign(md->begin() + (ptrdiff_t)a, md->begin() + (ptrdiff_t)(a + Lm.files[(size_t)i].size));
    } else {
        const uint64_t a = (uint64_t)Lo.data_off + Lo.files[(size_t)i].off;
        if (a + Lo.files[(size_t)i].size > o.size()) return false;
        v.assign(o.begin() + (ptrdiff_t)a, o.begin() + (ptrdiff_t)(a + Lo.files[(size_t)i].size));
    }
    for (const Xp& x : m.xp) {
        if (x.target != t || x.index != i) continue;
        Bytes d;
        if (!read_all(x.path, d)) return false;
        if (!x.chunk) { v.swap(d); continue; }
        if ((uint64_t)x.choff + d.size() > v.size()) return false;
        memcpy(v.data() + x.choff, d.data(), d.size());
    }
    return true;
}

/* The three-way merge of one packed file: a's and b's copies against the game's
 * (po, n bytes). false: a size changed, or both changed a byte differently. */
static bool merge_bytes(const uint8_t* po, size_t n, const Bytes& va, const Bytes& vb, Bytes& out)
{
    if (va.size() != n || vb.size() != n) return false;
    out.assign(po, po + n);
    for (size_t k = 0; k < n; k++) {
        const bool ca = va[k] != po[k], cb = vb[k] != po[k];
        if (ca && cb && va[k] != vb[k]) return false;
        if (ca) out[k] = va[k];
        else if (cb) out[k] = vb[k];
    }
    return true;
}

/* ---- the three-way merge with changes of size ------------------------------------
 * A mod's copy of a packed file against the game's, as hunks: o[o0, o1) became
 * v[v0, v1). Same size: the runs of changed bytes. Else the common start and end
 * are cut off and the middle is matched on 16-byte windows that occur once in
 * both copies (a patience diff: the longest run of them in the same order), so
 * bytes a mod inserted or removed move everything after them as one block. */
struct Hunk { size_t o0, o1, v0, v1; };
static const size_t kDiffMax = 16u << 20;   /* bigger packed files are not diffed: one hunk */

static void diff_hunks(const uint8_t* o, size_t no, const Bytes& v, std::vector<Hunk>& out)
{
    out.clear();
    const size_t nv = v.size();
    size_t p = 0;
    while (p < no && p < nv && o[p] == v[p]) p++;
    if (p == no && p == nv) return;
    size_t s = 0;
    while (s < no - p && s < nv - p && o[no - 1 - s] == v[nv - 1 - s]) s++;
    const size_t ob = p, oe = no - s, vb = p, ve = nv - s;
    if (no == nv) {   /* in place: runs of changed bytes, joined over gaps under 8 */
        size_t i = ob;
        while (i < oe) {
            if (o[i] == v[i]) { i++; continue; }
            size_t e = i + 1, j = i + 1;
            while (j < oe && j - e < 8) { if (o[j] != v[j]) e = j + 1; j++; }
            out.push_back({ i, e, i, e });
            i = e;
        }
        return;
    }
    const size_t K = 16;
    if (oe - ob < K * 2 || ve - vb < K * 2 || oe - ob > kDiffMax || ve - vb > kDiffMax) { out.push_back({ ob, oe, vb, ve }); return; }
    auto key = [&](const uint8_t* q) { uint64_t x, y; memcpy(&x, q, 8); memcpy(&y, q + 8, 8); return x * 0x9E3779B97F4A7C15ull ^ (y + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full; };
    std::unordered_map<uint64_t, int64_t> po, pv;   /* window -> its place, -1 if more than one */
    /* only windows whose key ends in three 0 bits (the same ones in both copies, as they go by content) */
    po.reserve((oe - ob) / 4); pv.reserve((ve - vb) / 4);
    for (size_t i = ob; i + K <= oe; i++) { const uint64_t h = key(o + i); if (h & 7) continue; auto r = po.emplace(h, (int64_t)i); if (!r.second) r.first->second = -1; }
    for (size_t i = vb; i + K <= ve; i++) { const uint64_t h = key(v.data() + i); if (h & 7) continue; auto r = pv.emplace(h, (int64_t)i); if (!r.second) r.first->second = -1; }
    std::vector<std::pair<size_t, size_t>> an;   /* (o place, v place), by v place */
    for (size_t i = vb; i + K <= ve; i++) {
        if (key(v.data() + i) & 7) continue;
        auto a = pv.find(key(v.data() + i));
        if (a == pv.end() || a->second != (int64_t)i) continue;
        auto b = po.find(a->first);
        if (b == po.end() || b->second < 0 || memcmp(o + b->second, v.data() + i, K)) continue;
        an.push_back({ (size_t)b->second, i });
    }
    /* the longest run in the same order in both (patience sorting on the o places) */
    std::vector<size_t> tails, prev(an.size(), (size_t)-1);
    for (size_t k = 0; k < an.size(); k++) {
        size_t lo = 0, hi = tails.size();
        while (lo < hi) { const size_t m = (lo + hi) / 2; if (an[tails[m]].first < an[k].first) lo = m + 1; else hi = m; }
        if (lo) prev[k] = tails[lo - 1];
        if (lo == tails.size()) tails.push_back(k); else tails[lo] = k;
    }
    std::vector<size_t> chain;
    for (size_t k = tails.empty() ? (size_t)-1 : tails.back(); k != (size_t)-1; k = prev[k]) chain.push_back(k);
    std::reverse(chain.begin(), chain.end());
    size_t co = ob, cv = vb;
    auto gap = [&](size_t o0, size_t o1, size_t v0, size_t v1) {
        while (o0 < o1 && v0 < v1 && o[o0] == v[v0]) { o0++; v0++; }
        while (o0 < o1 && v0 < v1 && o[o1 - 1] == v[v1 - 1]) { o1--; v1--; }
        if (o0 < o1 || v0 < v1) out.push_back({ o0, o1, v0, v1 });
    };
    for (size_t k : chain) {
        const size_t ao = an[k].first, av = an[k].second;
        if (ao < co || av < cv) continue;   /* inside the match before it */
        if (ao - co != av - cv || memcmp(o + co, v.data() + cv, ao - co)) gap(co, ao, cv, av);
        size_t len = 0;
        while (ao + len < oe && av + len < ve && o[ao + len] == v[av + len]) len++;
        co = ao + len; cv = av + len;
    }
    gap(co, oe, cv, ve);
}

/* Both mods' changes to one packed file (po, n bytes: the game's) in one copy.
 * Changes to different places are all kept, with what either inserted or removed.
 * Where both change the same place differently, prefer (0 a, 1 b) says whose
 * change is used there; -1: that fails. overlaps: how many such places. */
static bool merge3(const uint8_t* po, size_t n, const Bytes& va, const Bytes& vb, int prefer, Bytes& out, int* overlaps = nullptr)
{
    if (overlaps) *overlaps = 0;
    if (va.size() == n && vb.size() == n && prefer < 0) return merge_bytes(po, n, va, vb, out);
    std::vector<Hunk> ha, hb;
    diff_hunks(po, n, va, ha);
    diff_hunks(po, n, vb, hb);
    struct E { Hunk h; int side; };
    std::vector<E> ev;
    for (const Hunk& h : ha) ev.push_back({ h, 0 });
    for (const Hunk& h : hb) ev.push_back({ h, 1 });
    std::stable_sort(ev.begin(), ev.end(), [](const E& x, const E& y) { return x.h.o0 < y.h.o0; });
    std::vector<E> use;
    size_t k = 0;
    while (k < ev.size()) {   /* clusters of hunks over the same bytes */
        size_t j = k + 1, end = ev[k].h.o1;
        bool sides[2] = { false, false };
        sides[ev[k].side] = true;
        while (j < ev.size() && (ev[j].h.o0 < end || (ev[j].h.o0 == end && ev[j].h.o0 == ev[j].h.o1 && ev[k].h.o0 == end))) {
            end = (std::max)(end, ev[j].h.o1); sides[ev[j].side] = true; j++;
        }
        if (sides[0] && sides[1]) {
            const size_t c0 = ev[k].h.o0;
            auto side_bytes = [&](int sd) {   /* that side's version of [c0, end) */
                const Bytes& v = sd ? vb : va;
                Bytes r;
                size_t cur = c0;
                for (size_t q = k; q < j; q++) {
                    if (ev[q].side != sd) continue;
                    r.insert(r.end(), po + cur, po + ev[q].h.o0);
                    r.insert(r.end(), v.begin() + (ptrdiff_t)ev[q].h.v0, v.begin() + (ptrdiff_t)ev[q].h.v1);
                    cur = ev[q].h.o1;
                }
                r.insert(r.end(), po + cur, po + end);
                return r;
            };
            const bool same = side_bytes(0) == side_bytes(1);
            if (!same) {
                if (prefer < 0) return false;
                if (overlaps) ++*overlaps;
            }
            const int keep = same ? 0 : prefer;
            for (size_t q = k; q < j; q++) if (ev[q].side == keep) use.push_back(ev[q]);
        } else
            for (size_t q = k; q < j; q++) use.push_back(ev[q]);
        k = j;
    }
    out.clear();
    out.reserve((std::max)(va.size(), vb.size()));
    size_t cur = 0;
    for (const E& e : use) {
        const Bytes& v = e.side ? vb : va;
        out.insert(out.end(), po + cur, po + e.h.o0);
        out.insert(out.end(), v.begin() + (ptrdiff_t)e.h.v0, v.begin() + (ptrdiff_t)e.h.v1);
        cur = e.h.o1;
    }
    out.insert(out.end(), po + cur, po + n);
    return true;
}

struct BmRes { bool ok = false; std::string path; int overlaps = 0; };
static std::map<std::string, BmRes> g_bm;   /* under g_pf_mx: pf_merge's answers by key */

static std::string bm_key(const Mod& a, const Mod& b, const std::string& t, int i, int prefer)
{
    std::string sa = part_sig(a, t, i), sb = part_sig(b, t, i);
    if (prefer == 1) std::swap(sa, sb);   /* the preferred one first */
    else if (prefer < 0 && sb < sa) std::swap(sa, sb);   /* the same either way round */
    return std::string(prefer < 0 ? "merge2 " : "merge2 prefer first ") + t + " " + std::to_string(i) + "\n" + sa + sb;
}

/* Whether a's and b's changes to the packed files `idx` of t merge byte by byte
 * (ok[k] for idx[k]); paths[k]: the merged copy in <cache>/merge. The game's file
 * and each mod's are read once for all of them; answers are cached (memory, and
 * <cache>/merge/<hash>.m by the files' names, sizes and times). Without a cache
 * folder nothing merges (the merged copy has nowhere to go). */
static void pf_merge(const Mod& a, const Mod& b, const std::string& t, const std::vector<int>& idx,
                     std::vector<char>& ok, std::vector<std::string>* paths = nullptr, int prefer = -1,
                     std::vector<int>* overlaps = nullptr)
{
    if (overlaps) overlaps->assign(idx.size(), 0);
    ok.assign(idx.size(), 0);
    if (paths) paths->assign(idx.size(), std::string());
    std::lock_guard<std::mutex> lk(g_pf_mx);
    const std::string dir = join(g.cache, "merge");
    std::vector<std::string> keys(idx.size());
    std::vector<size_t> todo;
    for (size_t k = 0; k < idx.size(); k++) {
        keys[k] = bm_key(a, b, t, idx[k], prefer);
        auto it = g_bm.find(keys[k]);
        if (it == g_bm.end() && !g.cache.empty()) {
            const std::string h = hex64(fnv64(keys[k]));
            Bytes cb;
            if (read_all(join(dir, h + ".m"), cb)) {
                const std::string c(cb.begin(), cb.end());
                if (c.size() >= keys[k].size() + 2 && !c.compare(0, keys[k].size(), keys[k])) {
                    BmRes r;
                    r.ok = !c.compare(keys[k].size(), 2, "ok");
                    if (r.ok) r.path = join(dir, h + ".bin");
                    if (r.ok) r.overlaps = atoi(c.c_str() + keys[k].size() + 2);
                    if (!r.ok || finfo(r.path).ok) it = g_bm.emplace(keys[k], r).first;   /* "ok" with its copy gone: again */
                }
            }
        }
        if (it == g_bm.end()) { todo.push_back(k); continue; }
        ok[k] = it->second.ok;
        if (paths) (*paths)[k] = it->second.path;
        if (overlaps) (*overlaps)[k] = it->second.overlaps;
    }
    if (todo.empty()) return;
    int ai = -1;
    Psarc* ar = g.cache.empty() ? nullptr : g_pf_arcs.find(t, ai);
    Bytes o, ma, mb;
    XpLayout Lo;
    std::string e;
    const Part* fa = whole_of(a, t);
    const Part* fb = whole_of(b, t);
    const bool have = ar && ar->entry((uint32_t)ai, o) && xp_layout(o, Lo, e) &&
                      (!fa || read_all(fa->path, ma)) && (!fb || read_all(fb->path, mb));
    for (size_t k : todo) {
        BmRes r;
        const int i = idx[k];
        Bytes va, vb, mg;
        if (have && pf_version(a, t, i, o, Lo, fa ? &ma : nullptr, va) && pf_version(b, t, i, o, Lo, fb ? &mb : nullptr, vb)) {
            const uint64_t a0 = (uint64_t)Lo.data_off + Lo.files[(size_t)i].off;
            if (merge3(o.data() + a0, Lo.files[(size_t)i].size, va, vb, prefer, mg, &r.overlaps)) {
                const std::string bp = join(dir, hex64(fnv64(keys[k])) + ".bin");
                mkdirs(dir);
                if (write_all(bp, mg)) { r.ok = true; r.path = bp; }
            }
        }
        if (!g.cache.empty()) {
            const std::string c = keys[k] + (r.ok ? "ok " + std::to_string(r.overlaps) + "\n" : std::string("no\n"));
            mkdirs(dir);
            write_all(join(dir, hex64(fnv64(keys[k])) + ".m"), Bytes(c.begin(), c.end()));
        }
        fprintf(stderr, "[mods] merge: %s packed file %d of %s and %s: %s\n", t.c_str(), i, a.name.c_str(), b.name.c_str(),
                r.ok ? (r.overlaps ? (std::to_string(r.overlaps) + " place(s) both change: " + (prefer ? b : a).name + "'s used there").c_str()
                                   : "combined")
                     : have ? "both change the same bytes: combined once the conflict is resolved"
                            : "can't be combined (a file could not be read or compared)");
        g_bm[keys[k]] = r;
        ok[k] = r.ok;
        if (paths) (*paths)[k] = r.path;
        if (overlaps) (*overlaps)[k] = r.overlaps;
    }
}

/* Two mods' whole copies of t, combined by what each added and changed (ppu_fs_xcombine.h):
 * pointers, lists both extended, counters both raised, the same insert from both.
 * Only for whole copies with nothing else on t. The pair is taken in a fixed order
 * (by their files), so a and b either way round give the same file. Cached like
 * pf_merge: memory, and <cache>/merge/<hash>.x (the answer and its log) + .xpps. */
struct XcRes { bool ok = false; std::string path, err; std::vector<std::string> log; };
static std::map<std::string, XcRes> g_xc;   /* under g_pf_mx */
static bool xc_fit(const Mod& m, const std::string& t)
{
    const Part* f = whole_of(m, t);
    if (!f || !f->drop.empty() || m.live) return false;
    for (const Tex& x : m.tex) if (x.target == t) return false;
    for (const Xp& x : m.xp) if (x.target == t) return false;
    return true;
}
static const XcRes* xc_combine(const Mod& a, const Mod& b, const std::string& t)
{
    if (!xc_fit(a, t) || !xc_fit(b, t) || g.cache.empty()) return nullptr;
    std::string sa = sig_line("mod", whole_of(a, t)->path), sb = sig_line("mod", whole_of(b, t)->path);
    const bool swap = sb < sa;
    if (swap) std::swap(sa, sb);
    const std::string key = "xcombine 2 " + t + "\n" + sa + sb;
    std::lock_guard<std::mutex> lk(g_pf_mx);
    auto it = g_xc.find(key);
    if (it != g_xc.end()) return &it->second;
    const std::string dir = join(g.cache, "merge"), h = hex64(fnv64(key));
    {
        Bytes cb;
        if (read_all(join(dir, h + ".x"), cb)) {
            const std::string c(cb.begin(), cb.end());
            if (c.size() > key.size() && !c.compare(0, key.size(), key)) {
                XcRes r;
                size_t p = key.size();
                auto line = [&]() { const size_t e = c.find('\n', p); std::string l = c.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? c.size() : e + 1; return l; };
                const std::string st = line();
                r.ok = st == "ok";
                if (!r.ok) r.err = st.size() > 3 ? st.substr(3) : st;
                while (p < c.size()) r.log.push_back(line());
                if (r.ok) r.path = join(dir, h + ".xpps");
                if (!r.ok || finfo(r.path).ok) return &g_xc.emplace(key, r).first->second;
            }
        }
    }
    XcRes r;
    int ai = -1;
    Psarc* ar = g_pf_arcs.find(t, ai);
    Bytes o, ma, mb;
    const Mod& A = swap ? b : a; const Mod& B = swap ? a : b;
    if (!ar || !ar->entry((uint32_t)ai, o) || !read_all(whole_of(A, t)->path, ma) || !read_all(whole_of(B, t)->path, mb)) r.err = "a file could not be read";
    else {
        xm::Arc G, MA, MB;
        xm::Delta da, db;
        Bytes out;
        if (!G.load(o)) r.err = "the game's file: " + G.err;
        else if (!MA.load(ma)) r.err = A.name + "'s file: " + MA.err;
        else if (!MB.load(mb)) r.err = B.name + "'s file: " + MB.err;
        else if (!xm::make_delta(G, MA, A.name, da)) r.err = A.name + ": " + da.err;
        else if (!xm::make_delta(G, MB, B.name, db)) r.err = B.name + ": " + db.err;
        else {
            /* each reading checked: its delta gives its file back */
            Bytes chk;
            std::string e;
            if (!xm::apply_delta(G, da, chk, e) || chk != ma) r.err = A.name + "'s file is not read exactly";
            else if (!xm::apply_delta(G, db, chk, e) || chk != mb) r.err = B.name + "'s file is not read exactly";
            else if (xm::combine_files(G, da, db, 0, out, r.log, e)) {
                mkdirs(dir);
                r.path = join(dir, h + ".xpps");
                r.ok = write_all(r.path, out);
                if (!r.ok) r.err = "cannot write " + r.path;
            } else r.err = e;
        }
    }
    std::string c = key + (r.ok ? std::string("ok") : "no " + r.err) + "\n";
    for (const std::string& l : r.log) c += l + "\n";
    mkdirs(dir);
    write_all(join(dir, h + ".x"), Bytes(c.begin(), c.end()));
    fprintf(stderr, "[mods] combine: %s of %s and %s: %s\n", t.c_str(), A.name.c_str(), B.name.c_str(), r.ok ? "combined" : r.err.c_str());
    for (const std::string& l : r.log) fprintf(stderr, "[mods] combine:   %s\n", l.c_str());
    return &g_xc.emplace(key, r).first->second;
}

/* Every part a and b both change (skip: files a used merge patch carries for
 * them). can_merge: packed files may merge byte by byte (never with a live mod). */
static void clash_items(const Mod& a, const Mod& b, std::vector<Clash>& out, const std::set<std::string>* skip = nullptr,
                        bool can_merge = true)
{
    for (const std::string& t : a.targets) {
        if (!b.targets.count(t) || (skip && skip->count(t))) continue;
        std::vector<int> pi;   /* packed files both change */
        if (can_merge && !a.live && !b.live) {   /* two whole copies: combined by what each added and changed */
            const XcRes* x = xc_combine(a, b, t);
            if (x && x->ok) { out.push_back({ t, "*", true }); continue; }
        }
        if (whole_of(a, t) || whole_of(b, t)) {   /* merged when they change different packed files */
            std::set<int> sa, sb;
            if (!touched(a, t, sa) || !touched(b, t, sb) || (adds(a, t) && adds(b, t))) { out.push_back({ t, "*", a.live != b.live }); continue; }
            for (int i : sa) if (sb.count(i)) pi.push_back(i);
        } else {
            std::set<uint32_t> offs;
            for (const Tex& x : a.tex)
                for (const Tex& y : b.tex)
                    if (x.target == t && y.target == t && x.off == y.off) offs.insert(x.off);
            for (uint32_t off : offs) { char h[16]; snprintf(h, sizeof h, "t%x", (unsigned)off); out.push_back({ t, h, a.live != b.live }); }
            std::set<int> ps;
            for (const Xp& x : a.xp)
                for (const Xp& y : b.xp)
                    if (x.target == t && xp_overlap(x, y)) ps.insert(x.index);
            pi.assign(ps.begin(), ps.end());
        }
        if (pi.empty()) continue;
        std::vector<char> ok(pi.size(), 0);
        if (can_merge && !a.live && !b.live) pf_merge(a, b, t, pi, ok);
        if (a.live != b.live) ok.assign(pi.size(), 1);   /* a live mod and a regular one: both kept, no question (the live
                                                            mod's bytes go in where its windows are found in memory) */
        for (size_t k = 0; k < pi.size(); k++) out.push_back({ t, "p" + std::to_string(pi[k]), ok[k] != 0 });
    }
}

/* Whether two mods change the same thing so that only one of them can be used:
 * a part both change that neither merges byte by byte nor has a winner the
 * player picked. */
static bool mods_clash(const Mod& a, const Mod& b, const std::set<std::string>* skip = nullptr)
{
    std::vector<Clash> c;
    clash_items(a, b, c, skip);
    for (const Clash& x : c) if (!x.merges && res_winner(a, b, x.target, x.item) < 0) return true;
    return false;
}

/* Leaves m's change to one part of t out. */
static void drop_item(Mod& m, const std::string& t, const std::string& item)
{
    if (item == "*") {
        m.files.erase(std::remove_if(m.files.begin(), m.files.end(), [&](const Part& f) { return f.target == t; }), m.files.end());
        m.tex.erase(std::remove_if(m.tex.begin(), m.tex.end(), [&](const Tex& x) { return x.target == t; }), m.tex.end());
        m.xp.erase(std::remove_if(m.xp.begin(), m.xp.end(), [&](const Xp& x) { return x.target == t; }), m.xp.end());
        return;
    }
    if (item.size() < 2) return;
    if (item[0] == 't') {
        const uint32_t off = (uint32_t)strtoul(item.c_str() + 1, nullptr, 16);
        m.tex.erase(std::remove_if(m.tex.begin(), m.tex.end(), [&](const Tex& x) { return x.target == t && x.off == off; }), m.tex.end());
        return;
    }
    const int i = atoi(item.c_str() + 1);
    for (Part& f : m.files) if (f.target == t) f.drop.insert(i);
    m.xp.erase(std::remove_if(m.xp.begin(), m.xp.end(), [&](const Xp& x) { return x.target == t && x.index == i; }), m.xp.end());
}

/* ---- merge patches (mod.json "patch_for") ---------------------------------------
 * Two mods that change the same parts of a file can't both be used. A third mod,
 * a merge patch, can carry the file with both mods' changes: "patch_for": ["Mod A",
 * "Mod B"] (folder or name). It is used only while every mod it names is used;
 * then, for the files it has, its copy replaces theirs, and those files no longer
 * count as a clash between them (or with the patch). Their other files are used
 * as always. The Mods page lists it like any mod; turned off, the named mods clash
 * as before. */
static std::string and_list(const std::vector<std::string>& v);
static bool patch_names(const Mod& p, const Mod& m)
{
    if (&p == &m) return false;
    for (const std::string& s : p.patch_for) if (ieq(s, m.folder) || ieq(s, m.name)) return true;
    return false;
}

/* The files the used patches carry for each mod they name (by index). */
static std::map<size_t, std::set<std::string>> patch_cover(const std::vector<Mod>& mods, const std::vector<size_t>& used)
{
    std::map<size_t, std::set<std::string>> c;
    for (size_t p : used) {
        if (mods[p].patch_for.empty()) continue;
        for (size_t i : used)
            if (patch_names(mods[p], mods[i])) c[i].insert(mods[p].targets.begin(), mods[p].targets.end());
    }
    return c;
}

/* The merge log: what each combined file got from which mod, written at every start
 * to <mods>/merge_log.txt (resolve). Nothing is written into the game's or the mods'
 * files: every start builds them again from the game's own and the mods that are on,
 * so a mod turned off has its changes taken back out. */
static std::vector<std::string> g_merge_log;   /* under g.mx's caller: resolve() only (one thread at start) */
static void merge_note(const std::string& s)
{
    fprintf(stderr, "[mods] merge: %s\n", s.c_str());
    g_merge_log.push_back(s);
}

/* The used mods' copies as the game gets them (fm: a copy of the list, by the same
 * indices): for each pair of used mods, the packed files they both change merged
 * byte by byte (the merged copy goes to the first of them as a packed-file patch,
 * both lose their own), and each part the player gave to one of them left out of
 * the other. live_on: the live mods that are on; a part the player gave to one
 * of them is left out of the used mod too (the live mod writes it in memory). */
static void merge_used(std::vector<Mod>& fm, const std::vector<size_t>& used, const std::map<size_t, std::set<std::string>>& cover,
                       const std::vector<size_t>& live_on = {})
{
    for (size_t x = 0; x < used.size(); x++)
        for (size_t y = x + 1; y < used.size(); y++) {
            const size_t a = used[x], b = used[y];
            std::set<std::string> sk;
            for (size_t q : { a, b }) { auto c = cover.find(q); if (c != cover.end()) sk.insert(c->second.begin(), c->second.end()); }
            std::vector<Clash> cl;
            clash_items(fm[a], fm[b], cl, &sk);
            /* the packed files to combine, asked of pf_merge per file and preference at once
               (it reads the game's file and both mods' once per call, not once per packed file) */
            std::map<std::pair<std::string, int>, std::vector<int>> ask;   /* (target, prefer) -> packed files */
            for (const Clash& k : cl) {
                if (k.item.size() < 2 || k.item[0] != 'p') continue;
                const int w = k.merges ? -1 : res_winner(fm[a], fm[b], k.target, k.item);
                if (!k.merges && w < 0) continue;
                ask[{ k.target, w }].push_back(atoi(k.item.c_str() + 1));
            }
            std::map<std::pair<std::string, int>, std::map<int, std::pair<bool, std::pair<std::string, int>>>> got;
            for (const auto& q : ask) {
                std::vector<char> ok;
                std::vector<std::string> path;
                std::vector<int> ov;
                pf_merge(fm[a], fm[b], q.first.first, q.second, ok, &path, q.first.second, &ov);
                for (size_t n = 0; n < q.second.size(); n++) got[q.first][q.second[n]] = { ok[n] != 0, { path[n], ov[n] } };
            }
            auto answer = [&](const std::string& t, int prefer, int i, std::vector<char>& ok, std::vector<std::string>& path, std::vector<int>& ov) {
                const auto& r = got[{ t, prefer }][i];
                ok.assign(1, r.first ? 1 : 0); path.assign(1, r.second.first); ov.assign(1, r.second.second);
            };
            for (const Clash& k : cl) {
                if (k.merges && k.item == "*" && !fm[a].live && !fm[b].live) {   /* whole copies combined */
                    const XcRes* x = xc_combine(fm[a], fm[b], k.target);
                    if (!x || !x->ok) continue;
                    drop_item(fm[a], k.target, "*");
                    drop_item(fm[b], k.target, "*");
                    Part f; f.target = k.target; f.path = x->path;
                    fm[a].files.push_back(f);
                    merge_note(k.target + ": " + fm[a].name + " and " + fm[b].name + " combined (each one's additions kept)");
                    for (const std::string& l : x->log) merge_note("  " + l);
                    continue;
                }
                if (k.merges) {
                    if (k.item.size() < 2 || k.item[0] != 'p') continue;   /* (a live mod's part: kept by both) */
                    const int i = atoi(k.item.c_str() + 1);
                    std::vector<char> ok;
                    std::vector<std::string> path;
                    std::vector<int> ov0;
                    answer(k.target, -1, i, ok, path, ov0);
                    if (!ok[0]) continue;
                    drop_item(fm[a], k.target, k.item);
                    drop_item(fm[b], k.target, k.item);
                    Xp m;
                    m.target = k.target; m.index = i; m.chunk = false; m.path = path[0];
                    m.name = fm[a].name + " + " + fm[b].name + " (merged) file_" + std::to_string(i);
                    fm[a].xp.push_back(m);
                    merge_note(k.target + ", " + item_label(k.item) + ": " + fm[a].name + " and " + fm[b].name + " combined (different places)");
                    continue;
                }
                const int w = res_winner(fm[a], fm[b], k.target, k.item);
                if (w < 0) continue;   /* (plan_pass never uses both then) */
                if (k.item[0] == 'p') {   /* resolved: combined, the winner's bytes where both change the same place */
                    const int i = atoi(k.item.c_str() + 1);
                    std::vector<char> ok;
                    std::vector<std::string> path;
                    std::vector<int> ov;
                    answer(k.target, w, i, ok, path, ov);
                    if (ok[0]) {
                        drop_item(fm[a], k.target, k.item);
                        drop_item(fm[b], k.target, k.item);
                        Xp m;
                        m.target = k.target; m.index = i; m.chunk = false; m.path = path[0];
                        m.name = fm[a].name + " + " + fm[b].name + " (combined) file_" + std::to_string(i);
                        fm[a].xp.push_back(m);
                        merge_note(k.target + ", " + item_label(k.item) + ": " + fm[a].name + " and " + fm[b].name + " combined; " +
                                   std::to_string(ov[0]) + " place(s) both change, " + (w ? fm[b] : fm[a]).name + "'s used there");
                        continue;
                    }
                }
                drop_item(w ? fm[a] : fm[b], k.target, k.item);
                merge_note(k.target + ", " + item_label(k.item) + ": " + (w ? fm[b] : fm[a]).name + "'s version (it can't be combined), not " +
                           (w ? fm[a] : fm[b]).name + "'s");
            }
        }
    (void)live_on;   /* a live mod never takes a part from a regular mod: it patches on top in memory */
}

/* The order mods are applied in: the ones with a priority first, the higher
 * one first, then the rest; equals in folder order (their order in the list,
 * the scan's). Deterministic: the priorities live in mods.ini. */
struct Prio { bool have; int prio; };
static std::vector<size_t> mod_order(const std::vector<Prio>& p)
{
    std::vector<size_t> o(p.size());
    for (size_t i = 0; i < o.size(); i++) o[i] = i;
    std::stable_sort(o.begin(), o.end(), [&](size_t a, size_t b) {
        if (p[a].have != p[b].have) return p[a].have;
        return p[a].have && p[a].prio > p[b].prio; });
    return o;
}
static std::vector<Prio> prios(const std::vector<Mod>& mods)
{
    std::vector<Prio> p;
    for (const Mod& m : mods) p.push_back({ m.have_prio, m.prio });
    return p;
}

/* The clash rule over `order`: a mod is used unless it changes what a mod
 * used before it changes; then it is left out whole and why[lower(folder)]
 * says with whom (lost_to: their indices). Mods that are off, not for this
 * game, broken, or in `out` (a build that failed) are not used. Returns the
 * used mods in order. resolve() and the menu's plan both run it. */
static std::vector<size_t> plan_pass(const std::vector<Mod>& mods, const std::vector<size_t>& order,
                                     const std::map<std::string, std::string>& out, std::map<std::string, std::string>& why,
                                     std::map<size_t, std::vector<size_t>>* lost_to = nullptr)
{
    auto can = [&](const Mod& m) { return !m.live && m.on && m.for_game && m.err.empty() && !out.count(lower(m.folder)); };   /* live: not served at load */
    const std::map<std::string, std::string> why0 = why;
    std::set<size_t> dropped;   /* merge patches whose mods were not all used: tried again without them */
    for (size_t round = 0; round <= mods.size() + 1; round++) {
        why = why0;
        std::map<size_t, std::vector<size_t>> lost;
        /* merge patches whose mods are all on go first, then the rest in order */
        std::vector<size_t> ready, rest;
        for (size_t i : order) {
            const Mod& m = mods[i];
            if (!can(m)) continue;
            if (m.patch_for.empty()) { rest.push_back(i); continue; }
            std::vector<std::string> names;
            bool all = !dropped.count(i);
            for (const std::string& nm : m.patch_for) {
                bool got = false;
                for (size_t j = 0; j < mods.size() && !got; j++)
                    if (j != i && mods[j].patch_for.empty() && (ieq(nm, mods[j].folder) || ieq(nm, mods[j].name)) && can(mods[j])) got = true;
                if (!got) all = false;
                names.push_back(nm);
            }
            if (all) ready.push_back(i);
            else why[lower(m.folder)] = "a merge patch, used only while " + and_list(names) + (names.size() > 1 ? " are" : " is") + " used";
        }
        std::vector<size_t> used;
        auto skip_for = [&](size_t a, size_t b) {   /* the files a used patch merges for a and b */
            std::set<std::string> s;
            for (size_t p : used) {
                if (mods[p].patch_for.empty()) continue;
                const bool ca = p == a || patch_names(mods[p], mods[a]), cb = p == b || patch_names(mods[p], mods[b]);
                if (ca && cb) s.insert(mods[p].targets.begin(), mods[p].targets.end());
            }
            return s;
        };
        for (const std::vector<size_t>* list : { &ready, &rest })
            for (size_t i : *list) {
                const Mod& m = mods[i];
                std::vector<size_t> clash;
                for (size_t u : used) {
                    const std::set<std::string> sk = skip_for(i, u);
                    if (mods_clash(m, mods[u], &sk)) clash.push_back(u);
                }
                if (clash.empty()) { used.push_back(i); continue; }
                std::string s = "it changes the same files as ";
                for (size_t k = 0; k < clash.size(); k++) s += (k ? ", " : "") + mods[clash[k]].name;
                why[lower(m.folder)] = s;
                lost[i] = clash;
            }
        /* a used patch needs every mod it names used too */
        bool again = false;
        for (size_t p : used) {
            if (mods[p].patch_for.empty()) continue;
            for (size_t j = 0; j < mods.size(); j++)
                if (patch_names(mods[p], mods[j]) && mods[j].patch_for.empty() && can(mods[j]) &&
                    std::find(used.begin(), used.end(), j) == used.end()) { dropped.insert(p); again = true; }
        }
        if (again) continue;
        if (lost_to) for (auto& l : lost) (*lost_to)[l.first] = l.second;
        return used;
    }
    return {};
}

/* A mod as it would be used: its folder, the options picked and its files. */
static std::string mod_key(const Mod& m)
{
    std::string k = lower(m.folder) + "|";
    if (!m.modules.empty()) for (const std::string& id : picked_ids(m)) k += id + ",";
    char b[32];
    snprintf(b, sizeof b, "|%016llx", (unsigned long long)m.stamp);
    k += b;
    for (const auto& r : m.res) k += (r.second ? "|+" : "|-") + r.first;   /* resolved conflicts: what it serves changes */
    return k;
}

/* Which mods are used, and what the game gets for each file they change. */
static void resolve(std::vector<Mod>& mods)
{
    Arcs arcs;
    const std::vector<size_t> order = mod_order(prios(mods));
    std::vector<size_t> rank(mods.size());
    for (size_t r = 0; r < order.size(); r++) rank[order[r]] = r;
    std::map<std::string, std::string> blamed;      /* lower(folder) -> why its file could not be built */
    std::map<std::string, StartInfo> fails;         /* ... alone, or with which mods */
    /* every failed round leaves out at least one more mod, so this ends */
    for (size_t round = 0; round <= mods.size() + 1; round++) {
        std::map<std::string, std::string> why = blamed;    /* lower(folder) -> why it is left out */
        const std::vector<size_t> used = plan_pass(mods, order, blamed, why);
        /* each changed file: a whole file as it is, or a build */
        std::map<std::string, std::string> serve, by;
        bool again = false;
        std::set<std::string> targets;
        for (size_t i : used) for (const std::string& t : mods[i].targets) targets.insert(t);
        const std::map<size_t, std::set<std::string>> cover = patch_cover(mods, used);   /* a merge patch's files replace theirs */
        std::vector<size_t> live_on;
        for (size_t i = 0; i < mods.size(); i++) if (mods[i].live && mods[i].on && mods[i].for_game && mods[i].err.empty()) live_on.push_back(i);
        std::vector<Mod> fm = mods;   /* as the game gets them: merged parts, the player's choices */
        g_merge_log.clear();
        merge_used(fm, used, cover, live_on);
        for (const std::string& t : targets) {
            const Part* base = nullptr;
            std::vector<const Tex*> tl;
            std::vector<const Xp*> xl;
            std::set<size_t> who;
            std::vector<const Part*> bases;
            for (size_t i : used) {
                auto cv = cover.find(i);
                if (cv != cover.end() && cv->second.count(t)) continue;
                for (const Part& f : fm[i].files) if (f.target == t) { base = &f; bases.push_back(&f); who.insert(i); }
                for (const Tex& x : fm[i].tex) if (x.target == t) { tl.push_back(&x); who.insert(i); }
                for (const Xp& x : fm[i].xp) if (x.target == t) { xl.push_back(&x); who.insert(i); }
            }
            std::string bym;
            for (size_t i : who) bym += (bym.empty() ? "" : " + ") + mods[i].name;
            std::deque<Xp> merged;   /* an automatic merge: each mod's changed packed files, onto the game's file */
            bool dropped = false;   /* a whole file with packed files the player gave to another mod */
            for (const Part* b : bases) if (!b->drop.empty()) dropped = true;
            if (bases.size() > 1 || dropped) {
                std::string merr;
                const Part* grown = nullptr;   /* a copy that adds packed files: the others' go into it, not the game's */
                for (const Part* b : bases)
                    if (pf_diff(*b).extra > 0) {
                        if (grown) { merr = "both " + grown->path + " and " + b->path + " add packed files"; break; }
                        grown = b;
                    }
                for (const Part* b : bases) {
                    if (!merr.empty()) break;
                    if (b == grown) continue;   /* the base itself */
                    const PfDiff d = pf_diff(*b);
                    Bytes md;
                    XpLayout Lm;
                    if (!d.ok || !read_all(b->path, md) || !xp_layout(md, Lm, merr)) { merr = "cannot merge " + b->path; break; }
                    for (int pi : d.changed) {
                        if (b->drop.count(pi)) continue;
                        if (pi < 0 || (size_t)pi >= Lm.files.size()) { merr = "cannot merge " + b->path; break; }
                        const XpFile& F = Lm.files[(size_t)pi];
                        const uint64_t a0 = (uint64_t)Lm.data_off + F.off;
                        if (a0 + F.size > md.size()) { merr = "cannot merge " + b->path; break; }
                        Xp x;
                        x.target = t; x.index = pi; x.chunk = false;
                        x.name = base_of(b->path) + " file_" + std::to_string(pi);
                        x.path = join(join(g.cache, "merge"), hex64(fnv64(sig_line("mod", b->path) + t + " " + std::to_string(pi))) + ".bin");
                        if (!finfo(x.path).ok) {
                            mkdirs(join(g.cache, "merge"));
                            if (!write_all(x.path, Bytes(md.begin() + (ptrdiff_t)a0, md.begin() + (ptrdiff_t)(a0 + F.size)))) { merr = "cannot write " + x.path; break; }
                        }
                        merged.push_back(x);
                    }
                    if (!merr.empty()) break;
                }
                if (merr.empty() && grown)   /* a packed file of the base given to another mod must be replaced by one */
                    for (int pi : grown->drop) {
                        bool got = false;
                        for (const Xp& x : merged) if (x.index == pi && !x.chunk) got = true;
                        for (const Xp* x : xl) if (x->index == pi && !x->chunk) got = true;
                        if (!got) { merr = "packed file " + std::to_string(pi) + " of " + grown->path + " is left out with nothing in its place"; break; }
                    }
                if (merr.empty()) {
                    std::vector<const Xp*> all;
                    for (const Xp& x : merged) all.push_back(&x);
                    all.insert(all.end(), xl.begin(), xl.end());
                    xl.swap(all);
                    base = grown;
                    if (grown) {
                        std::string gm;
                        for (size_t i : who) for (const Part& f : fm[i].files) if (&f == grown) gm = mods[i].name;
                        merge_note(t + ": built from " + gm + "'s copy (it adds " + std::to_string(pf_diff(*grown).extra) + " packed file(s)) + " + bym +
                                   " (" + std::to_string(merged.size()) + " changed packed file(s) of the others)");
                    } else
                        merge_note(t + ": built from the game's copy + " + bym + " (" + std::to_string(merged.size()) + " changed packed file(s))");
                } else {   /* leave out the last of them, as a failed build does */
                    fprintf(stderr, "[mods] %s: %s\n", t.c_str(), merr.c_str());
                    size_t last = *who.begin();
                    for (size_t i : who) if (rank[i] > rank[last]) last = i;
                    blamed[lower(mods[last].folder)] = "could not merge " + t + " with the mods before it: " + merr;
                    StartInfo& f = fails[lower(mods[last].folder)];
                    for (size_t i : who) if (i != last) f.partners[lower(mods[i].folder)] = mod_key(mods[i]);
                    again = true;
                    break;
                }
            }
            if (!base && tl.empty() && xl.empty()) continue;   /* every change to it went to another mod's part: the game's own */
            if (tl.empty() && xl.empty()) { serve[t] = base->path; by[t] = bym; continue; }
            std::string path, err;
            if (!build(t, base, tl, xl, arcs, path, err)) {
                fprintf(stderr, "[mods] %s: %s\n", t.c_str(), err.c_str());
                /* Who is at fault: each mod on this file alone (a dry build). The
                 * ones that fail alone are left out; if each builds alone, the
                 * last one in the order, which came after the others (as the
                 * Manager refuses the mod being turned on, not the ones before). */
                bool blamed_one = false;
                if (who.size() > 1)
                    for (size_t i : who) {
                        const Part* b1 = nullptr;
                        std::vector<const Tex*> t1;
                        std::vector<const Xp*> x1;
                        for (const Part& f : fm[i].files) if (f.target == t) b1 = &f;
                        for (const Tex& x : fm[i].tex) if (x.target == t) t1.push_back(&x);
                        for (const Xp& x : fm[i].xp) if (x.target == t) x1.push_back(&x);
                        std::string p1, e1;
                        if (!build(t, b1, t1, x1, arcs, p1, e1, true)) {
                            blamed[lower(mods[i].folder)] = "could not build " + t + ": " + e1;
                            fails[lower(mods[i].folder)].alone = true;
                            blamed_one = true;
                        }
                    }
                if (!blamed_one) {
                    size_t last = *who.begin();
                    for (size_t i : who) if (rank[i] > rank[last]) last = i;
                    blamed[lower(mods[last].folder)] = (who.size() > 1 ? "could not build " + t + " together with the mods before it: "
                                                                       : "could not build " + t + ": ") + err;
                    StartInfo& f = fails[lower(mods[last].folder)];
                    f.alone = who.size() == 1;
                    for (size_t i : who) if (i != last) f.partners[lower(mods[i].folder)] = mod_key(mods[i]);
                }
                again = true;
                break;
            }
            serve[t] = path; by[t] = bym;
        }
        if (again) continue;
        if (!g.dir.empty()) {   /* the merge log, for the player (and the next start rebuilds it) */
            std::string L = "# inFAMOUS 2 port: how the mods that are on were combined at the last start.\n"
                            "# Nothing is written into the game's or the mods' files: each start builds them again\n"
                            "# from the game's own and the mods that are on, so a mod turned off is taken back out.\n";
            for (const std::string& x : g_merge_log) L += x + "\n";
            if (g_merge_log.empty()) L += "(no files combined)\n";
            write_all(join(g.dir, "merge_log.txt"), Bytes(L.begin(), L.end()));
        }
        std::lock_guard<std::mutex> lk(g.mx);
        g.serve.swap(serve);
        g.serve_mod.swap(by);
        g.start_sig.clear();
        for (Mod& m : mods) {
            const std::string lf = lower(m.folder);
            StartInfo si;
            auto fi = fails.find(lf);
            if (fi != fails.end()) si = fi->second;
            si.fail = blamed.count(lf) != 0;
            si.active = std::find(used.begin(), used.end(), (size_t)(&m - mods.data())) != used.end();
            si.key = mod_key(m);
            auto w = why.find(lf);
            if (w != why.end()) si.why = w->second;
            if (si.active) g.start_sig.insert(si.key);
            g.start[lf] = si;
        }
        g.nserve.store((int)g.serve.size(), std::memory_order_release);
        return;
    }
    fprintf(stderr, "[mods] gave up building the mods' files; no mod is used\n");
    std::lock_guard<std::mutex> lk(g.mx);
    g.serve.clear();
    g.serve_mod.clear();
    g.start_sig.clear();
    for (const Mod& m : mods) {
        StartInfo si;
        auto w = blamed.find(lower(m.folder));
        si.why = w != blamed.end() ? w->second : "the mods' files could not be built (see the session log)";
        si.key = mod_key(m);
        g.start[lower(m.folder)] = si;
    }
    g.nserve.store(0, std::memory_order_release);
}

static std::string settings_dir()
{
#ifdef _WIN32
    const char* a = getenv("APPDATA");
    return a && *a ? join(slashes(a), "inFAMOUS2-port") : std::string();
#else
    const char* x = getenv("XDG_CONFIG_HOME");
    const char* h = getenv("HOME");
    if (x && *x) return join(x, "infamous2-port");
    return h && *h ? join(join(h, ".config"), "infamous2-port") : std::string();
#endif
}

/* ---- live mods (mod.json "live": true) -------------------------------------
 * A live mod's texture patches and xpps chunk patches (file_<i>_<abs>_offset_<o>)
 * are not served to the game at load. The game loads its own files, and a
 * worker thread finds the patched bytes' originals in guest memory (main
 * memory 0x30000000-0x50000000, where the game keeps what it loaded, and RSX
 * local memory 0xC0000000-0xD0000000, where the textures are) and writes the
 * mod's bytes over them; turned off, it writes the originals back. So the
 * menu's switch works at once, without a restart, and what the game loads
 * later (another part of the city) is patched within a few seconds.
 *
 * Each patch is diffed against the game's original file (out of the
 * archives) into windows: a changed run, 4-byte aligned, with at least 32
 * bytes in all, matched exactly. Writes are bracketed for the write tracker
 * (g_vm_host_io_hook), as the file layer's reads into guest memory are, so
 * the draw engine sees the new texture bytes.
 *
 *   PS3_GAME_MODS_LIVE_SECS    seconds between passes (default 4; a pass
 *                              takes longer apart from that when one pass
 *                              is slow: at most ~5% of a core)
 * mod.json "karma_levels": { "Hero": "<folder>", ... } (the 7 levels:
 * Infamous, Outlaw, Thug, Neutral, Guardian, Champion, Hero) gives a live mod
 * other patches for each of Cole's karma levels, in <mod>/<folder>/files as a
 * mod's own; the worker reads the karma level every half second and puts the
 * level's bytes in when it changes (a level not named uses the mod's own).
 * Log lines start "[mods] live:". */

extern "C" { extern uint8_t* vm_base; extern void (*g_vm_host_io_hook)(uint32_t ea, uint32_t len, int end); }

static double g_live_last_ms = 0;   /* the last pass (the worker paces itself by it) */

/* v[0]: the game's bytes; v[i]: variant i's (k: their keys). weak[i]: v[i] is too plain to be looked for (zeros,
 * one flat colour: it is everywhere in memory, so a search for it would write into other things). Such a pair is
 * placed from the other windows of its texture or packed file (grp; off = its offset in the game's file): where
 * one of them is found, this one is at the same distance. places: where it was found (kept across passes). */
struct LivePair { std::vector<Bytes> v; std::vector<uint32_t> k; std::vector<char> weak; bool any_weak = false;
                  uint32_t grp = 0; uint64_t off = 0; bool grp_weak = false; std::vector<uint64_t> places; };
struct LiveMod {
    std::string key, name;
    std::vector<LivePair> pairs;
    std::vector<int> var;               /* "karma_levels": karma level (0-6) -> variant; empty: one variant (1) */
    int dark = 0;                       /* "dark_bolts" (Mod::dark) */
    bool want = false, built = false, building = false;
    int revert = 0;                     /* passes left that write the originals back */
    int target = 0;                     /* the variant the last pass put in (0: the game's own) */
    std::string err;
    uint64_t in_place = 0, wrote = 0;   /* the last pass: windows found in the mod's bytes; written in total */
    uint64_t tmp = 0;                   /* (the pass's count)*/
};

static struct {
    std::mutex mx;
    std::condition_variable cv;
    std::map<std::string, LiveMod> mods;   /* lower(folder) */
    std::vector<LiveMod> retired;          /* an old build of a mod (options or files changed), still to undo */
    bool started = false, kick = false;
    int rank = -1;                         /* Cole's karma level as last read (-1: not known) */
} lv;

/* Too plain to look for: fewer than 4 different byte values in the whole window (a black or zeroed run of
 * texture blocks, an empty table) -- such bytes are found all over the guest's memory. */
static bool live_weak(const Bytes& b)
{
    bool seen[256] = {};
    int n = 0;
    for (uint8_t c : b) if (!seen[c]) { seen[c] = true; if (++n >= 4) return false; }
    return true;
}

static uint32_t live_key_at(const Bytes& b)
{
    uint32_t best = 0;
    int bn = -1;
    for (uint32_t k = 0; k + 8 <= b.size() && k < 4096; k += 4) {
        bool seen[256] = {};
        int n = 0;
        for (int i = 0; i < 8; i++) if (!seen[b[k + i]]) { seen[b[k + i]] = true; n++; }
        if (n > bn) { bn = n; best = k; if (n == 8) break; }
    }
    return best;
}

/* A variant's changes to a game file: its changed runs (start -> bytes). */
struct LiveDiff { std::map<size_t, Bytes> runs; };

static void live_diff(const Bytes& o, const Bytes& n, LiveDiff& d, std::vector<bool>& dm)
{
    size_t i = 0;
    while (i < o.size()) {
        if (o[i] == n[i]) { i++; continue; }
        size_t e = i;
        while (e < o.size() && o[e] != n[e]) dm[e++] = true;
        d.runs[i].assign(n.begin() + (ptrdiff_t)i, n.begin() + (ptrdiff_t)e);
        i = e;
    }
}

/* w: the game's bytes at [a, a + w.size()); puts the variant's changes in. */
static void live_apply(const LiveDiff& d, size_t a, Bytes& w)
{
    auto it = d.runs.upper_bound(a);
    if (it != d.runs.begin()) --it;
    for (; it != d.runs.end() && it->first < a + w.size(); ++it) {
        const size_t r0 = it->first, r1 = r0 + it->second.size();
        const size_t c0 = (std::max)(r0, a), c1 = (std::min)(r1, a + w.size());
        for (size_t i = c0; i < c1; i++) w[i - a] = it->second[i - r0];
    }
}

/* The windows of [r0, r1) (a patch) where any variant differs from the game's
 * file o (dm); a window may reach out to [lim0, lim1) (the texture or packed
 * file) to be 32 bytes long. */
static void live_windows(const Bytes& o, const std::vector<bool>& dm, const std::vector<LiveDiff>& ds,
                         size_t r0, size_t r1, size_t lim0, size_t lim1, std::vector<LivePair>& out, std::set<Bytes>& seen,
                         uint32_t grp)
{
    r1 = (std::min)(r1, o.size()); lim1 = (std::min)(lim1, o.size());
    size_t i = r0;
    while (i < r1) {
        if (!dm[i]) { i++; continue; }
        size_t e = i + 1, j = i + 1;
        while (j < r1 && j - e < 16) { if (dm[j]) e = j + 1; j++; }
        size_t a = i & ~(size_t)3, b = e;
        if (b - a < 32) b = (std::min)(a + 32, lim1);
        if (b - a < 32) { const size_t a2 = b >= 32 ? ((b - 32) & ~(size_t)3) : 0; if (a2 >= lim0) a = a2; }
        while (b < o.size() && dm[b]) b++;
        b = (std::min)((b + 3) & ~(size_t)3, o.size());
        LivePair p;
        p.grp = grp; p.off = a;
        p.v.emplace_back(o.begin() + (ptrdiff_t)a, o.begin() + (ptrdiff_t)b);
        if (p.v[0].size() >= 8 && seen.insert(p.v[0]).second) {
            for (const LiveDiff& d : ds) { Bytes w = p.v[0]; live_apply(d, a, w); p.v.push_back(std::move(w)); }
            for (const Bytes& x : p.v) {
                p.k.push_back(live_key_at(x));
                p.weak.push_back(live_weak(x) ? 1 : 0);
                if (p.weak.back()) p.any_weak = true;
            }
            out.push_back(std::move(p));
        }
        i = b;
    }
}

/* A live mod's windows: variant 1 is the mod's own patches; a mod with
 * "karma_levels" adds one variant per level (var: level -> variant; a level
 * it does not name uses variant 1). */
static bool live_build(const Mod& m, std::vector<LivePair>& out, std::vector<int>& var, std::string& err)
{
    std::vector<Mod> vm(1, m);
    var.clear();
    if (!m.levels.empty()) {
        var.assign(7, 1);
        for (const auto& l : m.levels) {
            Mod x = live_level_mod(m, l.second);
            if (!x.err.empty()) { err = std::string("karma level ") + kKarmaLevels[l.first] + ": " + x.err; return false; }
            var[(size_t)l.first] = (int)vm.size() + 1;   /* vm[i] is variant i + 1 (v[0]: the game's) */
            vm.push_back(std::move(x));
        }
    }
    std::set<std::string> targets;
    for (const Mod& x : vm) targets.insert(x.targets.begin(), x.targets.end());
    Arcs arcs;
    std::set<Bytes> seen;
    uint32_t grp = 0;   /* one per texture / packed file range (LivePair::grp) */
    for (const std::string& t : targets) {
        int idx = -1;
        Psarc* arc = arcs.find(t, idx);
        if (!arc) { err = t + " is not in the game's archives"; return false; }
        Bytes o;
        if (!arc->entry((uint32_t)idx, o)) { err = "cannot read " + t + " out of the archives"; return false; }
        std::vector<LiveDiff> ds(vm.size());
        std::vector<bool> dm(o.size(), false);
        std::set<std::array<size_t, 4>> rs;
        XpLayout L;
        bool have_l = false;
        for (size_t vi = 0; vi < vm.size(); vi++) {
            const Mod& x = vm[vi];
            std::vector<const Tex*> tl;
            std::vector<const Xp*> xl;
            for (const Tex& q : x.tex) if (q.target == t) tl.push_back(&q);
            for (const Xp& q : x.xp) if (q.target == t) xl.push_back(&q);
            if (tl.empty() && xl.empty()) continue;
            Bytes n;
            std::string path;
            if (!build(t, nullptr, tl, xl, arcs, path, err, false, &n)) return false;
            if (n.size() != o.size()) { err = t + ": a live mod must not change the file's size"; return false; }
            live_diff(o, n, ds[vi], dm);
            if (!tl.empty()) {
                const size_t dso = be32(&o[40]);
                for (const Tex* q : tl) {
                    const size_t at = dso + q->off, sz = dxt_size(q->w, q->h, q->fmt, q->mips);
                    rs.insert({ at, at + sz, at, at + sz });
                }
            }
            if (!xl.empty()) {
                if (!have_l) { if (!xp_layout(o, L, err)) { err = t + ": " + err; return false; } have_l = true; }
                for (const Xp* q : xl) {
                    int fi = -1;
                    if (q->index >= 0 && q->index < (int)L.files.size()) fi = q->index;
                    else for (size_t i = 0; i < L.files.size(); i++) if ((uint64_t)L.data_off + L.files[i].off == q->abs) fi = (int)i;
                    if (fi < 0) continue;
                    const size_t a0 = (size_t)L.data_off + L.files[(size_t)fi].off, a1 = a0 + L.files[(size_t)fi].size;
                    rs.insert({ a0 + q->choff, a0 + q->choff + q->len, a0, a1 });
                }
            }
        }
        for (const auto& r : rs) live_windows(o, dm, ds, r[0], r[1], r[2], r[3], out, seen, ++grp);
    }
    if (out.empty()) { err = "its patches change nothing in the game's files"; return false; }
    std::set<uint32_t> wg;
    for (const LivePair& p : out) if (p.any_weak) wg.insert(p.grp);
    for (LivePair& p : out) p.grp_weak = wg.count(p.grp) != 0;
    return true;
}

static bool use_next();

/* With g.mx held: whether mod o is in the game now, for a live mod's resolved
 * conflicts: a live mod while it is on, any other while this session uses it. */
static bool live_partner_now_locked(const Mod& o)
{
    if (!o.for_game || !o.err.empty() || !o.on) return false;
    if (o.live) return true;
    auto it = g.start.find(lower(o.folder));
    return it != g.start.end() && it->second.active;
}
/* With g.mx held: a live mod's build key: mod_key, and the mods in the game now
 * that it resolved a conflict with (their parts it leaves out change with them). */
static std::string live_key_locked(const Mod& m)
{
    std::string k = mod_key(m);
    for (const Mod& o : g.list) {
        if (&o == &m || !live_partner_now_locked(o)) continue;
        const std::string f = lower(o.folder) + "|";
        bool any = false;
        for (const auto& r : m.res) if (!r.first.compare(0, f.size(), f)) { any = true; break; }
        if (any) k += "|with " + lower(o.folder);
    }
    return k;
}

/* With g.mx held: what each live mod should be now (the saved switch and "Use mods"). */
static void live_sync_locked()
{
    std::lock_guard<std::mutex> lk(lv.mx);
    const bool use = use_next();
    std::set<std::string> present;
    for (const Mod& m : g.list) {
        if (!m.live) continue;
        const std::string f = lower(m.folder);
        present.insert(f);
        const bool ok = m.for_game && m.err.empty();
        const std::string key = live_key_locked(m);
        LiveMod& L = lv.mods[f];
        if (L.key != key && !L.key.empty()) {   /* other options or files: undo the old build, make the new one */
            if (L.built && (L.want || L.revert)) { LiveMod old = L; old.want = false; old.revert = 3; lv.retired.push_back(std::move(old)); }
            L = LiveMod();
        }
        L.key = key; L.name = m.name; L.dark = m.dark;
        const bool want = ok && m.on && use;
        if (L.want && !want) L.revert = 3;
        if (want) L.revert = 0;
        L.want = want;
    }
    for (auto& kv : lv.mods)
        if (!present.count(kv.first) && (kv.second.want || kv.second.revert)) {   /* its folder is gone */
            kv.second.want = false;
            if (!kv.second.revert) kv.second.revert = 3;
        }
}

/* "dark_bolts": the arcs' dark shader, as the Black lightning cheat draws them (cheats.cpp sets the hook) */
extern "C" void (*g_mods_dark_bolts_hook)(int on);
void (*g_mods_dark_bolts_hook)(int on) = nullptr;

static bool live_dark_for(const LiveMod& m, int rank)
{
    return m.want && m.built && m.err.empty() && !m.pairs.empty() &&
           ((m.dark & 0x80) || (rank >= 0 && ((m.dark >> rank) & 1)));
}

static void live_dark_update()
{
    static int last = -1;
    int on = 0;
    {
        std::lock_guard<std::mutex> lk(lv.mx);
        for (auto& kv : lv.mods) if (live_dark_for(kv.second, lv.rank)) on = 1;
    }
    if (on != last && g_mods_dark_bolts_hook) {
        g_mods_dark_bolts_hook(on);
        fprintf(stderr, "[mods] live: dark bolts %s\n", on ? "on" : "off");
        last = on;
    }
}

#ifdef _WIN32
struct LiveEnt { LivePair* p; LiveMod* m; int from, to; };   /* v[from] -> v[to]; from == to: count only (in place) */

/* Cole's karma level (0-6) as the game finds it: the first of its 7 ranges
 * (0x00745EC8, (min, max) big-endian floats) that holds the karma
 * (0x00A64E40), else Neutral; -1 when it cannot be read. */
static int live_karma_level()
{
    uint8_t t[56], k[4];
    SIZE_T g1 = 0, g2 = 0;
    const HANDLE me = GetCurrentProcess();
    if (!vm_base || !ReadProcessMemory(me, vm_base + 0x00745EC8u, t, sizeof t, &g1) || g1 != sizeof t) return -1;
    if (!ReadProcessMemory(me, vm_base + 0x00A64E40u, k, sizeof k, &g2) || g2 != sizeof k) return -1;
    auto f = [](const uint8_t* p) { const uint32_t u = be32(p); float x; memcpy(&x, &u, 4); return x; };
    const float kv = f(k);
    if (kv != kv) return -1;
    for (int i = 0; i < 7; i++) if (!(f(t + 8 * i) <= f(t + 8 * i + 4))) return -1;   /* not the table (yet) */
    for (int i = 0; i < 7; i++) if (f(t + 8 * i) <= kv && kv <= f(t + 8 * i + 4)) return i;
    return 3;
}

/* mod.json "discover_powers": while a save is being played (Cole's character object at 0x00A6E9E4 is set),
 * each listed power whose word (0x00A64E60 + 4 * power, the save's power state: 1 discovered, 2 its
 * requirement met, 4 bought, 0x20 new) lacks the 1 bit gets 0x23: discovered, buyable, marked new. The game
 * saves it with the rest, so the power stays found when the mod is later turned off. Only mods in use. */
static void live_discover()
{
    std::set<int> want;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        if (!g.use) return;
        for (const Mod& m : g.list)
            if (m.on && m.for_game && m.err.empty()) for (int p : m.discover) want.insert(p);
    }
    if (want.empty() || !vm_base) return;
    const HANDLE me = GetCurrentProcess();
    uint8_t c[4], w[4 * 96];
    SIZE_T got = 0;
    if (!ReadProcessMemory(me, vm_base + 0x00A6E9E4u, c, sizeof c, &got) || got != sizeof c || !be32(c)) return;
    if (!ReadProcessMemory(me, vm_base + 0x00A64E60u, w, sizeof w, &got) || got != sizeof w) return;
    for (int p : want) {
        const uint32_t v = be32(w + 4 * p);
        if (v & 1) continue;
        uint8_t nv[4];
        put_be32(nv, v | 0x23);
        SIZE_T put = 0;
        const bool ok = WriteProcessMemory(me, vm_base + 0x00A64E60u + 4u * (uint32_t)p, nv, sizeof nv, &put) && put == sizeof nv;
        fprintf(stderr, "[mods] discover_powers: power %d %08x -> %08x%s\n", p, v, v | 0x23, ok ? "" : " (write failed)");
    }
}

/* mod.json "karma_patches": [{"levels": ["Thug", "Neutral", ...], "find": "<hex>", "replace": "<hex>"}]. While
 * a save is being played and Cole's karma level is one of "levels", the place in guest memory that holds "find"
 * gets "replace"'s bytes; at the other levels it gets "find"'s bytes back. ?? in "find" is any byte (bytes the
 * loader fills in: pointers, native calls); ?? in "replace" leaves that byte as it is. For a mod's own script or
 * data that should behave differently with karma (Custom Powers: Lightning Bolt's head explosion only at Outlaw
 * and Infamous). "find" must be unique to the mod's bytes; the place is looked for in either form, remembered,
 * and checked again every half second (looked for again at most every 5 s while it is not found). */
static std::atomic<bool> g_kp_any{false};   /* a mod in use has "karma_patches" (the worker ticks every half second) */
struct KpPlace { std::vector<uint32_t> at; std::chrono::steady_clock::time_point next{}; int last = -2; };
static std::map<std::string, KpPlace> g_kp;
static void live_karma_patches()
{
    std::vector<std::pair<std::string, Mod::KarmaPatch>> want;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        if (!g.use) return;
        for (const Mod& m : g.list)
            if (m.on && m.for_game && m.err.empty())
                for (size_t i = 0; i < m.kpatch.size(); i++) want.push_back({ m.name + " #" + std::to_string(i + 1), m.kpatch[i] });
    }
    g_kp_any.store(!want.empty(), std::memory_order_relaxed);
    if (want.empty() || !vm_base) return;
    const HANDLE me = GetCurrentProcess();
    uint8_t c[4];
    SIZE_T got = 0;
    if (!ReadProcessMemory(me, vm_base + 0x00A6E9E4u, c, sizeof c, &got) || got != sizeof c || !be32(c)) return;
    const int rank = live_karma_level();
    if (rank < 0) return;
    const auto now = std::chrono::steady_clock::now();
    void (*const io)(uint32_t, uint32_t, int) = g_vm_host_io_hook;
    for (auto& w : want) {
        const Mod::KarmaPatch& p = w.second;
        KpPlace& st = g_kp[w.first];
        const size_t n = p.find.size();
        auto form = [&](const uint8_t* b) {   /* 1: find's form, 2: replace's, 0: neither */
            bool f = true, r = true;
            for (size_t i = 0; i < n && (f || r); i++) {
                if (p.find[i] >= 0 && b[i] != p.find[i]) f = false;
                const int x = p.rep[i] >= 0 ? p.rep[i] : p.find[i];
                if (x >= 0 && b[i] != x) r = false;
            }
            return f ? 1 : r ? 2 : 0;
        };
        std::vector<uint8_t> cur(n);
        std::vector<uint32_t> keep;
        for (uint32_t a : st.at)
            if (ReadProcessMemory(me, vm_base + a, cur.data(), n, &got) && got == n && form(cur.data())) keep.push_back(a);
        st.at.swap(keep);
        if (st.at.empty()) {
            if (now < st.next) continue;
            st.next = now + std::chrono::seconds(5);
            size_t k = 0;   /* 8 known bytes that are the same in both forms */
            for (size_t i = 0; i + 8 <= n; i += 4) {
                bool ok = true;
                for (size_t q = i; q < i + 8; q++) if (p.find[q] < 0 || p.rep[q] >= 0) ok = false;
                if (ok) { k = i; break; }
            }
            uint8_t key[8];
            for (int q = 0; q < 8; q++) key[q] = (uint8_t)p.find[k + (size_t)q];
            static const uint32_t kRegions[][2] = { { 0x30000000u, 0x50000000u }, { 0xC0000000u, 0xD0000000u } };
            const size_t kChunk = 4u << 20;
            std::vector<uint8_t> buf(kChunk + 8);
            for (const auto& r : kRegions) {
                uint64_t ea = r[0];
                while (ea < r[1] && st.at.size() < 8) {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(vm_base + ea, &mbi, sizeof mbi)) break;
                    const uint64_t rb = (uint64_t)((uint8_t*)mbi.BaseAddress - vm_base), re = (std::min<uint64_t>)(rb + mbi.RegionSize, r[1]);
                    const DWORD pr = mbi.Protect;
                    const bool readable = mbi.State == MEM_COMMIT && !(pr & (PAGE_GUARD | PAGE_NOACCESS)) &&
                                          (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
                    if (readable)
                        for (uint64_t cb = (std::max)(ea, rb); cb < re; cb += kChunk) {
                            const size_t len = (size_t)(std::min<uint64_t>)(kChunk, re - cb);
                            if (!ReadProcessMemory(me, vm_base + cb, buf.data(), len, &got) || got < 8) continue;
                            for (size_t o = 0; o + 8 <= got; o += 4) {
                                if (memcmp(buf.data() + o, key, 8)) continue;
                                if (cb + o < k) continue;
                                const uint64_t at = cb + o - k;
                                if (at < r[0] || at + n > r[1]) continue;
                                if (!ReadProcessMemory(me, vm_base + at, cur.data(), n, &got) || got != n || !form(cur.data())) continue;
                                if (std::find(st.at.begin(), st.at.end(), (uint32_t)at) == st.at.end()) st.at.push_back((uint32_t)at);
                            }
                        }
                    ea = re;
                }
            }
            if (st.at.empty()) continue;
            fprintf(stderr, "[mods] karma_patches: %s found at 0x%08X%s\n", w.first.c_str(), (unsigned)st.at[0],
                    st.at.size() > 1 ? " (and more places)" : "");
        }
        const bool r = (p.levels >> rank) & 1;
        for (uint32_t a : st.at) {
            if (!ReadProcessMemory(me, vm_base + a, cur.data(), n, &got) || got != n) continue;
            std::vector<uint8_t> t(cur);
            for (size_t i = 0; i < n; i++) {
                const int x = r && p.rep[i] >= 0 ? p.rep[i] : p.rep[i] >= 0 ? p.find[i] : -1;
                if (x >= 0) t[i] = (uint8_t)x;
            }
            if (t == cur) continue;
            if (io) io(a, (uint32_t)n, 0);
            SIZE_T put = 0;
            const bool ok = WriteProcessMemory(me, vm_base + a, t.data(), n, &put) && put == n;
            if (io) io(a, (uint32_t)n, 1);
            fprintf(stderr, "[mods] karma_patches: %s at 0x%08X -> %s (karma level %s)%s\n", w.first.c_str(), (unsigned)a,
                    r ? "replace" : "find", kKarmaLevels[rank], ok ? "" : " (write failed)");
        }
    }
}

static void live_pass()
{
    const auto t0 = std::chrono::steady_clock::now();
    std::unordered_multimap<uint64_t, LiveEnt> tab;
    std::vector<uint64_t> bloom((size_t)1 << 18, 0);   /* 2^24 bits */
    auto hsh = [](uint64_t v) { return (uint32_t)((v * 0x9E3779B97F4A7C15ull) >> 40); };
    auto add = [&](LivePair& p, LiveMod& m, int from, int to) {
        if (p.weak[(size_t)from]) return;   /* never looked for (see LivePair) */
        uint64_t v;
        memcpy(&v, p.v[(size_t)from].data() + p.k[(size_t)from], 8);
        tab.insert({ v, LiveEnt{ &p, &m, from, to } });
        const uint32_t h = hsh(v);
        bloom[h >> 6] |= 1ull << (h & 63);
    };
    std::unique_lock<std::mutex> lk(lv.mx);
    std::vector<LiveMod*> ms;
    for (auto& kv : lv.mods) ms.push_back(&kv.second);
    for (LiveMod& m : lv.retired) ms.push_back(&m);
    bool any = false;
    std::vector<std::pair<LiveMod*, int>> act;   /* the mods this pass serves, and the variant each puts in */
    std::map<std::pair<const LiveMod*, uint32_t>, std::vector<uint64_t>> bases;   /* where a group's file starts */
    for (LiveMod* m : ms) {
        if (!m->built) continue;
        m->tmp = 0;
        if (m->pairs.empty()) continue;
        int t;
        if (m->want) t = m->var.empty() ? 1 : lv.rank >= 0 ? m->var[(size_t)lv.rank] : m->target > 0 ? m->target : 1;
        else if (m->revert > 0) t = 0;
        else continue;
        m->target = t;
        any = true;
        act.push_back({ m, t });
        for (LivePair& p : m->pairs) {
            const Bytes& w = p.v[(size_t)t];
            for (size_t i = 0; i < p.v.size(); i++) {
                if ((int)i == t) { if (t) add(p, *m, t, t); continue; }
                if (p.v[i] == w) continue;
                bool dup = false;
                for (size_t q = 0; q < i && !dup; q++) dup = (int)q != t && p.v[q] == p.v[i];
                if (!dup) add(p, *m, (int)i, t);
            }
        }
    }
    if (!any) return;
    /* the pairs stay put while the pass runs: only this thread changes them */
    lk.unlock();
    const HANDLE me = GetCurrentProcess();
    void (*const io)(uint32_t, uint32_t, int) = g_vm_host_io_hook;
    static const uint32_t kRegions[][2] = { { 0x30000000u, 0x50000000u }, { 0xC0000000u, 0xD0000000u } };
    const size_t kChunk = 4u << 20;
    std::vector<uint8_t> buf(kChunk + 8), cur;
    uint64_t wrote = 0, scanned = 0;
    for (const auto& r : kRegions) {
        uint64_t ea = r[0];
        while (ea < r[1]) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(vm_base + ea, &mbi, sizeof mbi)) break;
            const uint64_t rb = (uint64_t)((uint8_t*)mbi.BaseAddress - vm_base), re = (std::min<uint64_t>)(rb + mbi.RegionSize, r[1]);
            const DWORD pr = mbi.Protect;
            const bool readable = mbi.State == MEM_COMMIT && !(pr & (PAGE_GUARD | PAGE_NOACCESS)) &&
                                  (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
            if (readable) {
                for (uint64_t c = (std::max)(ea, rb); c < re; c += kChunk) {
                    const size_t n = (size_t)(std::min<uint64_t>)(kChunk, re - c);
                    SIZE_T got = 0;
                    const size_t want = (size_t)(std::min<uint64_t>)(n + 8, r[1] - c);
                    if (!ReadProcessMemory(me, vm_base + c, buf.data(), want, &got) || got < 8) {
                        if (!ReadProcessMemory(me, vm_base + c, buf.data(), n, &got) || got < 8) continue;
                    }
                    scanned += n;
                    const size_t lim = (std::min<size_t>)(n, got - 7);
                    for (size_t p = 0; p < lim; p += 4) {
                        uint64_t v;
                        memcpy(&v, buf.data() + p, 8);
                        const uint32_t h = hsh(v);
                        if (!((bloom[h >> 6] >> (h & 63)) & 1)) continue;
                        auto range = tab.equal_range(v);
                        for (auto it = range.first; it != range.second; ++it) {
                            const LiveEnt& e = it->second;
                            const uint32_t k = e.p->k[(size_t)e.from];
                            const Bytes& f = e.p->v[(size_t)e.from];
                            if (c + p < k) continue;
                            const uint64_t at = c + p - k;
                            if (at < r[0] || at + f.size() > r[1]) continue;
                            cur.resize(f.size());
                            SIZE_T g2 = 0;
                            if (!ReadProcessMemory(me, vm_base + at, cur.data(), cur.size(), &g2) || g2 != cur.size()) continue;
                            if (memcmp(cur.data(), f.data(), f.size())) continue;
                            if (e.p->any_weak) {   /* remembered: it may be found by its plain form only later */
                                auto& pl = e.p->places;
                                if (std::find(pl.begin(), pl.end(), at) == pl.end() && pl.size() < 16) pl.push_back(at);
                            }
                            if (e.p->grp_weak && at >= e.p->off) {   /* its group's plain windows are placed from here */
                                auto& bl = bases[{ e.m, e.p->grp }];
                                const uint64_t base = at - e.p->off;
                                if (std::find(bl.begin(), bl.end(), base) == bl.end() && bl.size() < 16) bl.push_back(base);
                            }
                            if (e.from == e.to) { e.m->tmp++; continue; }
                            const Bytes& w = e.p->v[(size_t)e.to];
                            if (io) io((uint32_t)at, (uint32_t)w.size(), 0);
                            SIZE_T put = 0;
                            const bool okw = WriteProcessMemory(me, vm_base + at, w.data(), w.size(), &put) && put == w.size();
                            if (io) io((uint32_t)at, (uint32_t)w.size(), 1);
                            if (okw) { wrote++; e.m->wrote++; if (e.to) e.m->tmp++; }
                            break;
                        }
                    }
                }
            }
            ea = re;
        }
    }
    /* The pairs with a plain form: at the places they were found, whatever form they hold now becomes the wanted
     * one; a place that holds none of their forms any more (the memory went to something else) is forgotten. */
    for (auto& mt : act) {
        LiveMod* m = mt.first;
        const int t = mt.second;
        for (LivePair& p : m->pairs) {
            if (!p.any_weak) continue;
            const Bytes& w = p.v[(size_t)t];
            auto bi = bases.find({ m, p.grp });
            if (bi != bases.end())
                for (uint64_t base : bi->second) {
                    const uint64_t at = base + p.off;
                    if (std::find(p.places.begin(), p.places.end(), at) != p.places.end() || p.places.size() >= 16) continue;
                    cur.resize(w.size());
                    SIZE_T g2 = 0;
                    if (!ReadProcessMemory(me, vm_base + at, cur.data(), cur.size(), &g2) || g2 != cur.size()) continue;
                    for (const Bytes& x : p.v)
                        if (x.size() == cur.size() && !memcmp(cur.data(), x.data(), x.size())) { p.places.push_back(at); break; }
                }
            for (size_t j = 0; j < p.places.size();) {
                const uint64_t at = p.places[j];
                cur.resize(w.size());
                SIZE_T g2 = 0;
                if (!ReadProcessMemory(me, vm_base + at, cur.data(), cur.size(), &g2) || g2 != cur.size()) {
                    p.places.erase(p.places.begin() + (ptrdiff_t)j);
                    continue;
                }
                if (!memcmp(cur.data(), w.data(), w.size())) { if (t && p.weak[(size_t)t]) m->tmp++; j++; continue; }
                bool known = false;
                for (const Bytes& x : p.v) if (x.size() == cur.size() && !memcmp(cur.data(), x.data(), x.size())) known = true;
                if (!known) { p.places.erase(p.places.begin() + (ptrdiff_t)j); continue; }
                if (io) io((uint32_t)at, (uint32_t)w.size(), 0);
                SIZE_T put = 0;
                const bool okw = WriteProcessMemory(me, vm_base + at, w.data(), w.size(), &put) && put == w.size();
                if (io) io((uint32_t)at, (uint32_t)w.size(), 1);
                if (okw) { wrote++; m->wrote++; if (t) m->tmp++; }
                j++;
            }
        }
    }
    lk.lock();
    for (LiveMod* m : ms) { if (m->built) m->in_place = m->tmp; if (!m->want && m->revert > 0) m->revert--; }
    lv.retired.erase(std::remove_if(lv.retired.begin(), lv.retired.end(), [](const LiveMod& m) { return m.revert <= 0; }), lv.retired.end());
    lk.unlock();
    const double ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static double last_logged = -1;
    if (wrote || last_logged < 0) {
        fprintf(stderr, "[mods] live: pass over %llu MB in %.0f ms, %llu place(s) written\n",
                (unsigned long long)(scanned >> 20), ms_, (unsigned long long)wrote);
        last_logged = ms_;
    }
    g_live_last_ms = ms_;
}
#else
static int live_karma_level() { return -1; }
static void live_discover() {}
static std::atomic<bool> g_kp_any{false};
static void live_karma_patches() {}
static void live_pass() {}
#endif

static void live_worker()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    const char* s = getenv("PS3_GAME_MODS_LIVE_SECS");
    const double secs = s && atof(s) > 0.2 ? atof(s) : 4.0;
    for (;;) {
        {
            /* a mod with karma levels on: the karma level is read every half second,
             * and a change is put in at once */
            const double wait = (std::max)(secs, g_live_last_ms * 20.0 / 1000.0);
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int64_t)(wait * 1000));
            std::unique_lock<std::mutex> lk(lv.mx);
            while (!lv.kick) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= until) break;
                bool lev = g_kp_any.load(std::memory_order_relaxed);   /* "karma_patches" are checked every half second too */
                for (auto& kv : lv.mods) if (kv.second.want && kv.second.built && (!kv.second.var.empty() || (kv.second.dark & 0x7F))) lev = true;
                const auto step = lev ? (std::min)(until, now + std::chrono::milliseconds(500)) : until;
                lv.cv.wait_until(lk, step, [] { return lv.kick; });
                if (lev && !lv.kick) {
                    lk.unlock();
                    const int r = live_karma_level();
                    live_karma_patches();
                    lk.lock();
                    if (r >= 0 && r != lv.rank) { lv.rank = r; break; }
                }
            }
            lv.kick = false;
        }
        { const int r = live_karma_level(); if (r >= 0) { std::lock_guard<std::mutex> lk(lv.mx); lv.rank = r; } }
        { std::lock_guard<std::mutex> lk(g.mx); live_sync_locked(); }
        /* builds: every live mod's pairs, once, on or off (reading the archives takes a few
         * seconds each). The mods switched on are built first and put in by a pass at once;
         * then one switched-off mod is built a round, so a switch turned on later is quick.
         * (Building them all before the first pass kept a mod that was on out for a minute.) */
        auto build_one = [](bool on_only) -> bool {
            std::string f, key;
            {
                std::lock_guard<std::mutex> lk(lv.mx);
                for (int want = 1; want >= (on_only ? 1 : 0) && f.empty(); want--)
                    for (auto& kv : lv.mods)
                        if (!kv.second.built && !kv.second.building && kv.second.want == (want != 0)) {
                            f = kv.first; key = kv.second.key; kv.second.building = true; break;
                        }
            }
            if (f.empty()) return false;
            Mod m;
            bool found = false;
            std::vector<Mod> partners;   /* the mods in the game now it resolved a conflict with */
            {
                std::lock_guard<std::mutex> lk(g.mx);
                for (const Mod& x : g.list) if (lower(x.folder) == f && live_key_locked(x) == key) { m = x; found = true; break; }
                if (found)
                    for (const Mod& o : g.list) if (lower(o.folder) != f && live_partner_now_locked(o)) partners.push_back(o);
            }
            for (const Mod& o : partners) {   /* the parts the player gave to them are left out */
                std::vector<Clash> cl;
                try { clash_items(m, o, cl, nullptr, false); } catch (const std::exception&) {}
                for (const Clash& k : cl)
                    if (o.live && res_winner(m, o, k.target, k.item) == 1) {   /* live vs live: the player's choice */
                        drop_item(m, k.target, k.item);
                        fprintf(stderr, "[mods] live: \"%s\": %s, %s left to %s (the player's choice)\n", m.name.c_str(), k.target.c_str(),
                                item_label(k.item).c_str(), o.name.c_str());
                    }
            }
            std::vector<LivePair> pairs;
            std::vector<int> var;
            std::string err;
            bool ok = false;
            if (found) {
                try { ok = live_build(m, pairs, var, err); }
                catch (const std::exception& e) { err = std::string("could not be read (") + e.what() + ")"; }
            }
            std::lock_guard<std::mutex> lk(lv.mx);
            auto it = lv.mods.find(f);
            if (it == lv.mods.end() || it->second.key != key) return true;
            LiveMod& L = it->second;
            L.building = false;
            L.built = true;
            if (ok) {
                L.pairs.swap(pairs);
                L.var.swap(var);
                L.err.clear();
                size_t bytes = 0;
                for (const LivePair& p : L.pairs) bytes += p.v[0].size();
                fprintf(stderr, "[mods] live: \"%s\" ready: %zu place(s) to find (%zu bytes)%s\n", L.name.c_str(), L.pairs.size(), bytes,
                        L.var.empty() ? "" : ", with karma levels");
            } else {
                L.err = found ? err : "it changed while it was read";
                fprintf(stderr, "[mods] live: \"%s\" not used: %s\n", L.name.c_str(), L.err.c_str());
            }
            return true;
        };
        while (build_one(true)) {}
        live_pass();
        live_dark_update();
        live_discover();
        live_karma_patches();
        if (build_one(false)) { std::lock_guard<std::mutex> lk(lv.mx); lv.kick = true; }   /* the next one soon */
    }
}

/* Wake the worker (a switch changed, the list was read again); start it on first need. */
static void live_kick()
{
    std::lock_guard<std::mutex> lk(lv.mx);
    lv.kick = true;
    if (!lv.started) {
        lv.started = true;
        std::thread(live_worker).detach();
    }
    lv.cv.notify_one();
}

/* With g.mx held: the menu's line for a live mod. */
static std::string live_status_locked(const Mod& m)
{
    if (!m.err.empty()) return "Not used: " + m.err;
    if (!m.for_game) return "Made for " + std::string(game_label(m.game)) + ", not inFAMOUS 2";
    if (!use_next()) return "Not used: mods are off";
    std::lock_guard<std::mutex> lk(lv.mx);
    auto it = lv.mods.find(lower(m.folder));
    const LiveMod* L = it != lv.mods.end() && it->second.key == live_key_locked(m) ? &it->second : nullptr;
    if (!m.on) return L && L->revert ? "Off: putting the game's own back" : "Off. A live mod: it changes the game at once, no restart";
    if (!L || !L->built) return "On: getting ready (a few seconds after the game starts)";
    if (!L->err.empty()) return "Not used: " + L->err;
    std::string lvl;
    if (!L->var.empty() || (L->dark & 0x7F)) lvl = lv.rank >= 0 ? std::string(" (karma level: ") + kKarmaLevels[lv.rank] + ")" : " (karma level not known yet)";
    if (!L->in_place) return "On" + lvl + ": waiting for the game to load what it changes";
    return "On now" + lvl + ": " + std::to_string(L->in_place) + " place(s) in memory changed" +
           (live_dark_for(*L, lv.rank) ? ", black bolts" : "");
}

static void init_body();
static void test_start();   /* PS3_GAME_MODS_TEST, below */
static void init()
{
    std::call_once(g.once, [] {
        /* a mod the scan or a build cannot hold in memory must not crash the
         * game at every start: no mod is used then, and the log says why */
        try { init_body(); }
        catch (const std::exception& e) {
            fprintf(stderr, "[mods] could not read the mods (%s); none is used this session\n", e.what());
            std::lock_guard<std::mutex> lk(g.mx);
            g.serve.clear(); g.serve_mod.clear();
            g.nserve.store(0, std::memory_order_release);
        }
    });
}
static void init_body()
{
    {
        const char* d = getenv("PS3_GAME_MODS_DIR");
        if (!d || !*d) return;
        g.dir = slashes(d);
        while (g.dir.size() > 1 && g.dir.back() == '/') g.dir.pop_back();
        const char* u = getenv("PS3_GAME_MODS");
        g.use = !(u && u[0] == '0' && !u[1]);
        const char* s = getenv("PS3_GAME_MODS_STATE");
        g.state_path = s ? slashes(s) : (settings_dir().empty() ? std::string() : join(settings_dir(), "mods.ini"));
        const char* c = getenv("PS3_GAME_MODS_CACHE");
        g.cache = c && *c ? slashes(c) : join(g.dir, ".port_cache");
        {
            std::vector<std::string> zd;
            state_read(&zd);
            std::lock_guard<std::mutex> lk(g.mx);
            g.zip_dirs = zd;
        }
        std::vector<Mod> mods = scan();
        int n_for = 0;
        for (const Mod& m : mods) if (m.for_game) n_for++;
        fprintf(stderr, "[mods] %s: %zu mod(s), %d for inFAMOUS 2%s\n", g.dir.c_str(), mods.size(), n_for,
                g.use ? "" : "; PS3_GAME_MODS=0: none used");
        {
            std::string first;
            for (size_t i : mod_order(prios(mods)))
                if (mods[i].have_prio) first += (first.empty() ? "" : ", ") + mods[i].name + " (priority " + std::to_string(mods[i].prio) + ")";
            if (!first.empty()) fprintf(stderr, "[mods] put first by the player: %s; then the rest by folder name\n", first.c_str());
        }
        if (g.use) resolve(mods);
        for (const Mod& m : mods) {
            const StartInfo si = g.start.count(lower(m.folder)) ? g.start[lower(m.folder)] : StartInfo();
            std::string t;
            for (const std::string& x : m.targets) t += (t.empty() ? "" : ", ") + x;
            if (si.active)
                fprintf(stderr, "[mods] using \"%s\" %s by %s: %zu file(s), %zu texture patch(es), %zu xpps patch(es) -> %s\n",
                        m.name.c_str(), m.version.c_str(), m.author.c_str(), m.files.size(), m.tex.size(), m.xp.size(), t.c_str());
            else
                fprintf(stderr, "[mods] not using \"%s\": %s\n", m.name.c_str(),
                        !m.err.empty() ? m.err.c_str() : !m.for_game ? "not made for inFAMOUS 2" : !m.on ? "turned off" :
                        !g.use ? "mods are off" : si.why.c_str());
        }
        {
            std::lock_guard<std::mutex> lk(g.mx);
            g.list.swap(mods);
            g.gen++;
        }
        {
            bool any = false;
            { std::lock_guard<std::mutex> lk(g.mx); for (const Mod& m : g.list) if (m.live || !m.discover.empty() || !m.kpatch.empty()) any = true; }
            if (any) live_kick();   /* (the worker also finds "discover_powers" powers and puts "karma_patches" in) */
        }
        test_start();
    }
}

/* The name whose mod file the game gets for `k` (lower case). With a text
 * language chosen (PS3_TEXT_LANG, ppu_fs_lang.h), every lang_*.xpps entry of
 * an archive is pointed at lang_<text>.xpps: whatever name the game asks for,
 * it reads that text. So a mod's lang_<text>.xpps stands for every
 * lang_*.xpps name, and a mod's other lang_*.xpps (text the player did not
 * choose) is not used. Without a chosen text language, names are as they are. */
static std::string serve_key(const std::string& k)
{
    if (k.size() <= 10 || k.compare(0, 5, "lang_") || !ends_i(k, ".xpps")) return k;
    lang_init();
    if (!g_lang.text[0]) return k;
    return lower(std::string("lang_") + g_lang.text + ".xpps");
}

/* mod.json "file_alias": a loose USRDIR file the game asks for and does not
 * have (out names no file) is read from the game file a mod in use names
 * instead. A power a mod adds has no preview movie (movies/ui/powerk_<its
 * name in the game's power table>.bik), and one failed open leaves the Powers
 * menu's movie player showing nothing for every power until a restart. */
static void alias_redirect(const char* u, char* out, size_t cap)
{
    const std::string rel = lower(u + 8);   /* past "/USRDIR/" */
    std::string to, by;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        if (!g.use) return;
        for (const Mod& m : g.list) {
            if (!m.on || !m.for_game || !m.err.empty()) continue;
            for (const auto& a : m.alias) if (a.first == rel) { to = a.second; by = m.name; break; }
            if (!to.empty()) break;
        }
    }
    if (to.empty() || finfo(out).ok) return;   /* the game has the file: it is used */
    const std::string o = out;
    if (o.size() < rel.size() || lower(o.substr(o.size() - rel.size())) != rel) return;
    const std::string host = o.substr(0, o.size() - rel.size()) + to;
    const FInfo f = finfo(host);
    if (!f.ok || f.dir || host.size() >= cap) {
        log_once("alias-miss:" + rel, "file_alias of %s: %s -> %s, which the game does not have either; not used", by.c_str(), rel.c_str(), to.c_str());
        return;
    }
    snprintf(out, cap, "%s", host.c_str());
    log_once("alias:" + rel, "%s -> %s (file_alias of %s)", rel.c_str(), to.c_str(), by.c_str());
}

/* A guest open (read-only) or stat of a loose file under USRDIR whose name a
 * mod changes: the mod's file (or the build) instead. */
static void redirect(const char* guest, char* out, size_t cap)
{
    init();
    const char* u = nullptr;
    for (const char* p = guest; *p; p++) if (lang_ieq_n(p, "/USRDIR/", 8)) { u = p; break; }
    if (!u) return;
    alias_redirect(u, out, cap);
    if (!g.nserve.load(std::memory_order_acquire)) return;
    const char* b = strrchr(guest, '/');
    const std::string key = serve_key(lower(b + 1));
    std::string host, by;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        auto it = g.serve.find(key);
        if (it == g.serve.end()) return;
        host = it->second; by = g.serve_mod[key];
    }
    if (host.size() >= cap) {   /* cut short, it would name another file */
        log_once("long:" + host, "%s: the path is too long to hand to the game; not used: %s", guest, host.c_str());
        return;
    }
    snprintf(out, cap, "%s", host.c_str());
    log_once("serve:" + std::string(guest), "%s -> %s's %s", guest, by.c_str(), host.c_str());
}

/* An archive was opened on fd: hide the files the mods change from its TOC. */
static void on_open(int fd, const char* hpath)
{
    init();
    if (fd < 0 || fd >= LANG_FD_MAX || !g.nserve.load(std::memory_order_acquire)) return;
    const std::string hp = hpath;
    if (!ends_i(hp, ".psarc") && !ends_i(hp, ".psarc_s")) return;
    Psarc p;
    if (!p.open(hp)) return;
    std::vector<LangPatch> add;
    std::string hidden;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        for (uint32_t i = 1; i < p.n; i++) {
            if (!g.serve.count(serve_key(p.names[i]))) continue;
            uint8_t md5[16];
            memcpy(md5, p.rec(i), 16);
            for (int k = 15; k >= 0 && ++md5[k] == 0; k--) { }   /* one larger: the lookup misses */
            LangPatch lp; lp.off = 32 + (uint64_t)i * p.esz; lp.len = 16; memcpy(lp.bytes, md5, 16);
            add.push_back(lp);
            hidden += (hidden.empty() ? "" : ", ") + p.names[i];
        }
    }
    if (add.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_lang_patch_mx);
        for (const LangPatch& lp : add) g_lang_patch[fd].push_back(lp);
        g_lang_has_patch[fd].store(1, std::memory_order_release);
    }
    log_once("hide:" + hp, "%s: %zu file(s) hidden, the mods' are read instead: %s", base_of(slashes(hp)).c_str(), add.size(), hidden.c_str());
}

/* ---- the menu's plan: the next start from the saved choices ------------------- */

/* "Use mods" as the next start will read it (the menu sets PS3_GAME_MODS at once). */
static bool use_next()
{
    const char* u = getenv("PS3_GAME_MODS");
    return !(u && u[0] == '0' && !u[1]);
}

static std::string and_list(const std::vector<std::string>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i == 0 ? "" : i + 1 == v.size() ? " and " : ", ") + v[i];
    return s;
}

/* Called with g.mx held: g.plan for the list as it is now -- the clash rule
 * (plan_pass) over the saved choices, no builds. A mod whose file could not
 * be built at this start stays out while it, and the mods it failed with,
 * are unchanged (the same build would fail again). */
static bool pair_items_locked(size_t i, size_t j, std::vector<Clash>& cl);
static void plan_locked()
{
    const bool use = use_next();
    Plan& P = g.plan;
    if (P.gen == g.gen && P.use == use) return;
    const std::vector<Mod>& L = g.list;
    const size_t n = L.size();
    P = Plan();
    P.gen = g.gen; P.use = use;
    P.next.assign(n, 0); P.why.assign(n, std::string()); P.key.assign(n, std::string());
    P.prefer.assign(n, 0); P.prefer_text.assign(n, std::string());
    P.unres.assign(n, 0); P.unres_with.assign(n, {}); P.merged_with.assign(n, {});
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < n; i++) { P.key[i] = mod_key(L[i]); at[lower(L[i].folder)] = i; }
    std::map<std::string, std::string> out;
    for (size_t i = 0; i < n; i++) {
        auto s = g.start.find(lower(L[i].folder));
        if (s == g.start.end() || !s->second.fail || s->second.key != P.key[i]) continue;
        bool same = true;
        for (const auto& pk : s->second.partners) {
            auto a = at.find(pk.first);
            if (a == at.end() || !L[a->second].on || P.key[a->second] != pk.second) same = false;
        }
        if (same) out[lower(L[i].folder)] = s->second.why;
    }
    if (!use) return;
    const std::vector<Prio> pr = prios(L);
    std::map<std::string, std::string> why = out;
    std::map<size_t, std::vector<size_t>> lost;
    const std::vector<size_t> used = plan_pass(L, mod_order(pr), out, why, &lost);
    for (size_t i : used) { P.next[i] = 1; P.sig.insert(P.key[i]); }
    P.cover = patch_cover(L, used);
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            std::vector<Clash> cl;
            if (!pair_items_locked(i, j, cl) || cl.empty()) continue;
            int open = 0;
            for (const Clash& c : cl) if (!c.merges && res_winner(L[i], L[j], c.target, c.item) < 0) open++;
            if (open) {
                P.unres[i] += open; P.unres[j] += open;
                P.unres_with[i].push_back(L[j].name); P.unres_with[j].push_back(L[i].name);
            } else { P.merged_with[i].push_back(L[j].name); P.merged_with[j].push_back(L[i].name); }
        }
    for (size_t i = 0; i < n; i++) {
        auto w = why.find(lower(L[i].folder));
        if (w != why.end()) P.why[i] = w->second;
    }
    /* "Use this one instead": a priority above every mod it clashes with, and
     * what that does to the others (the same pass with that priority) */
    for (const auto& lt : lost) {
        const size_t i = lt.first;
        int p = 1;
        for (size_t j = 0; j < n; j++)
            if (j != i && L[j].have_prio && L[j].prio >= p && mods_clash(L[i], L[j])) p = L[j].prio + 1;
        std::vector<Prio> p2 = pr;
        p2[i] = { true, p };
        std::map<std::string, std::string> w2 = out;
        const std::vector<size_t> u2 = plan_pass(L, mod_order(p2), out, w2);
        if (std::find(u2.begin(), u2.end(), i) == u2.end()) continue;
        std::vector<std::string> lose, gain;
        for (size_t u : used) if (std::find(u2.begin(), u2.end(), u) == u2.end()) lose.push_back(L[u].name);
        for (size_t u : u2) if (u != i && !P.next[u]) gain.push_back(L[u].name);
        if (lose.empty()) continue;
        P.prefer[i] = p;
        P.prefer_text[i] = and_list(lose) + " will be left out" + (gain.empty() ? std::string() : "; " + and_list(gain) + " will be used");
    }
}

/* Called with g.mx held, after plan_locked: the parts mods i and j (both on)
 * both change, as the menu's conflict dialog lists them. false: no such pair
 * (one is off, broken, or a merge patch for the other). */
static bool pair_items_locked(size_t i, size_t j, std::vector<Clash>& cl)
{
    const std::vector<Mod>& L = g.list;
    auto ok = [](const Mod& m) { return m.on && m.for_game && m.err.empty(); };
    if (i == j || !ok(L[i]) || !ok(L[j]) || patch_names(L[i], L[j]) || patch_names(L[j], L[i])) return false;
    bool common = false;
    for (const std::string& t : L[i].targets) if (L[j].targets.count(t)) { common = true; break; }
    if (!common) return false;
    std::set<std::string> sk;
    for (size_t q : { i, j }) { auto c = g.plan.cover.find(q); if (c != g.plan.cover.end()) sk.insert(c->second.begin(), c->second.end()); }
    clash_items(L[i], L[j], cl, &sk);
    return true;
}

/* Called with g.mx held, after plan_locked: the mod's line for the menu.
 * `changes`: the game files it changes, for the line. */
static std::string status_locked(size_t i, const std::string& changes)
{
    const Mod& m = g.list[i];
    if (m.live) return live_status_locked(m);
    const Plan& P = g.plan;
    auto it = g.start.find(lower(m.folder));
    const bool active = it != g.start.end() && it->second.active;
    if (!m.err.empty()) return "Not used: " + m.err;   /* a broken mod.json names no game: its error first */
    if (!m.for_game)
        return m.game.empty() ? "Not used: its mod.json does not name the game (\"game\": \"inf2\")"
                              : std::string("Made for ") + game_label(m.game) + ", not inFAMOUS 2";
    if (active) {
        if (!m.on) return "Off from the next start (in use now: changes " + changes + ")";
        if (!P.use) return "In use now; not used from the next start: mods are off";
        if (!P.next[i]) return "In use now; left out from the next start: " + P.why[i];
        if (it->second.key != P.key[i]) return "In use: changes " + changes + ". The changes apply from the next start";
        return "In use: changes " + changes;
    }
    if (!m.on) return "Off";
    if (!P.use) return "Not used: mods are off";
    if (P.next[i]) return "Used from the next start: changes " + changes;
    if (it != g.start.end() && g.use && it->second.why == P.why[i]) return "Not used: " + P.why[i];
    return "Left out from the next start: " + P.why[i];
}

/* ---- preview pictures (mod.json "image") --------------------------------------- */

static const uint64_t kMaxImage = 8ull << 20;

/* The mod's picture: a PNG or JPG inside the mod's folder -- a relative path
 * (safe_rel: no "..", absolute path, drive, share, stream or device name),
 * reached through no junction or link, a regular file of at most 8 MB.
 * out: its host path. */
static bool image_path(const Mod& m, std::string& out, std::string* why = nullptr)
{
    std::string rel;
    if (m.image.empty()) { if (why) *why = "mod.json names no picture"; return false; }
    if (!safe_rel(m.image, rel, why)) return false;
    if (!ends_i(rel, ".png") && !ends_i(rel, ".jpg") && !ends_i(rel, ".jpeg")) { if (why) *why = "not a .png or .jpg file"; return false; }
    if (is_link(m.path)) { if (why) *why = "the mod's folder is a link"; return false; }
    std::string p = m.path;
    for (const std::string& part : split_parts(rel)) {
        p = join(p, part);
        if (is_link(p)) { if (why) *why = "it goes through a junction or link"; return false; }
    }
    const FInfo f = finfo(p);
    if (!f.ok || f.dir) { if (why) *why = "no such file in the mod's folder"; return false; }
    if (!f.size || f.size > kMaxImage) { if (why) *why = "empty or bigger than 8 MB"; return false; }
    out = p;
    return true;
}

/* ---- one mod on its own (a mod just added from a zip) ------------------------- */

/* What the existing checks say about m alone: for each game file it patches,
 * its textures against this copy of the file (xp_textures, as build() checks
 * them; the options holding the misfits are named), then a dry build. Empty:
 * every file builds. */
static std::vector<std::string> check_alone(const Mod& m, Arcs& arcs)
{
    std::vector<std::string> out;
    if (!m.err.empty() || !m.for_game) return out;
    std::vector<std::pair<std::string, std::vector<std::string>>> opts;   /* chosen options: name, lower paths */
    if (!m.modules.empty()) {
        const std::set<std::string> ids = picked_ids(m);
        for (const Module& x : m.modules) {
            if (!ids.count(lower(x.id))) continue;
            std::vector<std::string> ps;
            for (const std::string& p : x.paths) ps.push_back(lower(p));
            opts.push_back({ x.name, ps });
        }
    }
    for (const std::string& t : m.targets) {
        const Part* base = nullptr;
        std::vector<const Tex*> tl;
        std::vector<const Xp*> xl;
        for (const Part& f : m.files) if (f.target == t) base = &f;
        for (const Tex& x : m.tex) if (x.target == t) tl.push_back(&x);
        for (const Xp& x : m.xp) if (x.target == t) xl.push_back(&x);
        if (tl.empty() && xl.empty()) continue;
        if (!tl.empty()) {
            Bytes d;
            int idx = -1;
            Psarc* arc = base ? nullptr : arcs.find(t, idx);
            const bool got = base ? read_all(base->path, d) : (arc && arc->entry((uint32_t)idx, d));
            if (!got) { out.push_back(base ? "cannot read " + base_of(base->path) : t + " is not in the game's archives"); continue; }
            const std::set<TexKey> known = xp_textures(d);
            size_t misfit = 0;
            bool all_in = true;
            std::vector<std::string> names;
            for (const Tex* x : tl) {
                if (known.count(TexKey(x->off, x->w, x->h, x->fmt, x->mips))) continue;
                misfit++;
                const std::string rel = lower(rel_to(m.path, x->path));
                bool in = false;
                for (const auto& o : opts)
                    if (path_on(&o.second, rel)) {
                        in = true;
                        if (std::find(names.begin(), names.end(), o.first) == names.end()) names.push_back(o.first);
                    }
                if (!in) all_in = false;
            }
            if (misfit) {
                std::string s = (misfit == 1 ? std::string("1 texture was") : std::to_string(misfit) + " textures were") +
                                " made for another version of " + t;
                s += !names.empty() && all_in ? ": untick " + and_list(names) : std::string(": the mod does not fit this copy of the game");
                out.push_back(s);
                continue;
            }
        }
        std::string p, e;
        if (!build(t, base, tl, xl, arcs, p, e, true)) out.push_back("could not build " + t + ": " + e);
    }
    return out;
}

#include "ppu_fs_mods_zip.h"   /* zip_check / zip_install */

/* ---- the zip list, the installs and the file dialog (one worker at a time) ------ */

struct ZipRow { std::string path, file, where; int64_t mtime = 0; uint64_t size = 0; bool chosen = false; ZipPlan plan; };
static struct {
    std::mutex mx;                    /* everything here */
    std::vector<ZipRow> rows;         /* the zips that look like mods (and why one cannot be installed) */
    int not_mods = 0;                 /* zips listed that are no mod at all */
    bool busy = false;                /* a worker runs */
    std::string busy_what;
    std::atomic<uint64_t> done{0};
    uint64_t total = 0;
    std::string result;               /* the last install's report */
    int result_kind = 0;              /* 1 installed, -1 not */
    uint64_t seq = 0;                 /* bumped when rows/result change */
} gz;

static bool zip_job(const std::string& what, std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lk(gz.mx);
        if (gz.busy) return false;
        gz.busy = true; gz.busy_what = what; gz.done = 0; gz.total = 0;
    }
    std::thread([fn] {
        try { fn(); }
        catch (const std::exception& e) {
            fprintf(stderr, "[mods] zip: %s\n", e.what());
            std::lock_guard<std::mutex> lk(gz.mx);
            gz.result = std::string("Something went wrong (") + e.what() + ")"; gz.result_kind = -1; gz.seq++;
        }
        std::lock_guard<std::mutex> lk(gz.mx);
        gz.busy = false; gz.busy_what.clear(); gz.seq++;
    }).detach();
    return true;
}

/* The folders the list looks in: Downloads, the mods folder, the folders a
 * zip was chosen from before, PS3_GAME_MODS_ZIP_DIRS. (UTF-8 path, label) */
static std::vector<std::pair<std::string, std::string>> zip_folders()
{
    std::vector<std::pair<std::string, std::string>> v;
#ifdef _WIN32
    {   /* FOLDERID_Downloads, through shell32 as it is loaded (no COM headers here) */
        typedef HRESULT(WINAPI* GetKF)(const GUID&, DWORD, HANDLE, PWSTR*);
        typedef void(WINAPI* MemFree)(LPVOID);
        static const GUID kDownloads = { 0x374DE290, 0x123F, 0x4565, { 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B } };
        HMODULE sh = LoadLibraryA("shell32.dll"), ole = LoadLibraryA("ole32.dll");
        GetKF gk = sh ? (GetKF)(void*)GetProcAddress(sh, "SHGetKnownFolderPath") : nullptr;
        MemFree mf = ole ? (MemFree)(void*)GetProcAddress(ole, "CoTaskMemFree") : nullptr;
        PWSTR w = nullptr;
        if (gk && mf && gk(kDownloads, 0, NULL, &w) == S_OK && w) v.push_back({ slashes(utf8_of(w)), "Downloads" });
        if (w && mf) mf(w);
    }
#else
    if (const char* h = getenv("HOME")) v.push_back({ join(h, "Downloads"), "Downloads" });
#endif
    std::vector<std::string> extra;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        extra = g.zip_dirs;
    }
    if (!g.dir.empty()) v.push_back({ utf8_of_ansi(g.dir), "the mods folder" });
    for (const std::string& d : extra) v.push_back({ d, base_of(d) });
    if (const char* e = getenv("PS3_GAME_MODS_ZIP_DIRS")) {
        std::string s = e;
        size_t a = 0;
        for (size_t i = 0; i <= s.size(); i++)
            if (i == s.size() || s[i] == ';') {
                if (i > a) { const std::string d = slashes(utf8_of_ansi(s.substr(a, i - a))); v.push_back({ d, base_of(d) }); }
                a = i + 1;
            }
    }
    std::vector<std::pair<std::string, std::string>> u;
    for (auto& x : v) {
        while (x.first.size() > 3 && x.first.back() == '/') x.first.pop_back();
        bool dup = false;
        for (const auto& y : u) if (ieq(y.first, x.first)) dup = true;
        if (!dup && !x.first.empty()) u.push_back(x);
    }
    return u;
}

/* The .zip files directly in dir (no links), newest first. */
static void zips_in(const std::string& dir, const std::string& label, std::vector<ZipRow>& out)
{
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(dir + "/*.zip").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        ZipRow r;
        r.file = utf8_of(fd.cFileName);
        if (!ends_i(r.file, ".zip")) continue;
        r.path = join(dir, r.file); r.where = label;
        r.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        const uint64_t ft = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        r.mtime = (int64_t)(ft / 10000000ull) - 11644473600ll;
        out.push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    for (const std::string& n : list_dir(dir)) {
        if (!ends_i(n, ".zip")) continue;
        const std::string p = join(dir, n);
        if (is_link(p)) continue;
        const FInfo f = finfo(p);
        if (!f.ok || f.dir) continue;
        ZipRow r; r.file = n; r.path = p; r.where = label; r.size = f.size; r.mtime = f.mtime;
        out.push_back(r);
    }
#endif
}

static const size_t kZipListMax = 30;

/* The list: the newest zips of those folders, each checked (zip_check, read only). */
static void zip_list_run()
{
    std::vector<ZipRow> all;
    for (const auto& d : zip_folders()) zips_in(d.first, d.second, all);
    std::stable_sort(all.begin(), all.end(), [](const ZipRow& a, const ZipRow& b) { return a.mtime > b.mtime; });
    if (all.size() > kZipListMax) all.resize(kZipListMax);
    {
        std::lock_guard<std::mutex> lk(gz.mx);
        std::vector<ZipRow> keep;   /* a file chosen with the dialog stays at the top */
        for (ZipRow& r : gz.rows) if (r.chosen) keep.push_back(r);
        gz.rows.swap(keep);
        gz.not_mods = 0;
        gz.total = all.size();
        gz.seq++;
    }
    for (ZipRow& r : all) {
        zip_check(r.path, r.plan);
        std::lock_guard<std::mutex> lk(gz.mx);
        gz.done++;
        bool have = false;
        for (const ZipRow& x : gz.rows) if (ieq(x.path, r.path)) have = true;
        if (have) continue;
        if (r.plan.not_mod) gz.not_mods++;
        else gz.rows.push_back(r);
        gz.seq++;
    }
}

/* "Use mods" and the rest of the menu see the new folder at once. */
static void rescan_now();

/* Installs the zip at path_u8 (checked again first), then reports what the
 * mods' own checks say about it. Runs on the zip worker. */
static void zip_install_run(const std::string& path_u8, bool replace)
{
    ZipPlan z;
    std::string report;
    int kind = -1;
    if (!zip_check(path_u8, z)) report = base_of(slashes(path_u8)) + " was not installed: " + z.err;
    else if (z.exists && !replace) report = "mods/" + z.folder + " is there already: use Replace to put this one in its place";
    else {
        { std::lock_guard<std::mutex> lk(gz.mx); gz.total = z.total; }
        std::string err;
        if (!zip_install(z, replace, err, &gz.done)) report = base_of(slashes(path_u8)) + " was not installed: " + err;
        else {
            kind = 1;
            rescan_now();
            const std::string who = z.name + (z.version.empty() ? "" : " " + z.version) + (z.author.empty() ? "" : " by " + z.author);
            report = (z.exists ? "Replaced " : "Installed ") + who + " (mods/" + z.folder + ", " + std::to_string(z.files.size()) + " file(s)).";
            if (z.left_out) report += "\nLeft out " + std::to_string(z.left_out) + " file(s) that are not mod files: " + z.left_kinds + ".";
            Mod m;
            bool found = false;
            std::string status;
            {
                std::lock_guard<std::mutex> lk(g.mx);
                plan_locked();
                for (size_t i = 0; i < g.list.size(); i++) {
                    if (!ieq(g.list[i].folder, z.folder)) continue;
                    m = g.list[i];
                    std::string t;
                    for (const std::string& x : m.targets) t += (t.empty() ? "" : ", ") + x;
                    status = status_locked(i, t);
                    found = true;
                    break;
                }
            }
            if (!found) report += "\nThe mods folder does not list it (see the session log).";
            else {
                report += "\n" + status;
                Arcs arcs;
                for (const std::string& s : check_alone(m, arcs)) report += "\n" + s;
            }
        }
    }
    fprintf(stderr, "[mods] zip: %s\n", report.c_str());
    ZipPlan again;   /* its row: "there already" now */
    zip_check(path_u8, again);
    std::lock_guard<std::mutex> lk(gz.mx);
    gz.result = report; gz.result_kind = kind;
    for (ZipRow& r : gz.rows) if (ieq(r.path, path_u8)) r.plan = again;
    gz.seq++;
}

#ifdef _WIN32
/* The open dialog shows over the game: its frame is put on top as it opens. */
static UINT_PTR CALLBACK zip_dialog_hook(HWND child, UINT msg, WPARAM, LPARAM)
{
    if (msg == WM_INITDIALOG) {
        HWND dlg = GetParent(child);
        SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(dlg);
    }
    return 0;
}
#endif

/* "Choose another file...": the Windows open dialog (on the worker; the game
 * goes on), the zip it gives checked and put at the top of the list, and its
 * folder remembered for the list (mods.ini [.zips]). */
static void zip_choose_run()
{
#ifdef _WIN32
    typedef BOOL(WINAPI* GetOpen)(LPOPENFILENAMEW);
    HMODULE dl = LoadLibraryA("comdlg32.dll");
    GetOpen go = dl ? (GetOpen)(void*)GetProcAddress(dl, "GetOpenFileNameW") : nullptr;
    if (!go) { std::lock_guard<std::mutex> lk(gz.mx); gz.result = "The file window is not available here."; gz.result_kind = -1; gz.seq++; return; }
    std::wstring init;
    for (const auto& d : zip_folders()) { init = widen(d.first); break; }
    for (wchar_t& c : init) if (c == L'/') c = L'\\';
    wchar_t file[1024] = L"";
    OPENFILENAMEW o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.lpstrFilter = L"Zip files (*.zip)\0*.zip\0";
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    o.lpstrInitialDir = init.empty() ? NULL : init.c_str();
    o.lpstrTitle = L"Add an inFAMOUS 2 mod from a zip";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY | OFN_DONTADDTORECENT |
              OFN_EXPLORER | OFN_ENABLEHOOK | OFN_ENABLESIZING;
    o.lpfnHook = zip_dialog_hook;
    if (!go(&o)) return;   /* cancelled */
    ZipRow r;
    r.path = slashes(utf8_of(file));
    r.file = base_of(r.path);
    r.where = "chosen";
    r.chosen = true;
    const FInfo fi = finfo_u8(r.path);
    r.size = fi.size; r.mtime = fi.mtime;
    zip_check(r.path, r.plan);
    {
        std::lock_guard<std::mutex> lk(g.mx);
        const std::string d = dir_of(r.path);
        std::vector<std::string> v{ d };
        for (const std::string& x : g.zip_dirs) if (!ieq(x, d) && v.size() < 6) v.push_back(x);
        g.zip_dirs = v;
        state_write_locked();
    }
    std::lock_guard<std::mutex> lk(gz.mx);
    for (size_t i = 0; i < gz.rows.size(); i++) if (ieq(gz.rows[i].path, r.path)) { gz.rows.erase(gz.rows.begin() + (ptrdiff_t)i); break; }
    gz.rows.insert(gz.rows.begin(), r);
    gz.seq++;
#endif
}

static void rescan_now()
{
    if (g.dir.empty()) return;
    std::vector<Mod> mods;
    try { mods = scan(); }
    catch (const std::exception& e) { fprintf(stderr, "[mods] could not read the mods folder again (%s)\n", e.what()); return; }
    bool live = false;
    for (const Mod& m : mods) if (m.live) live = true;
    std::lock_guard<std::mutex> lk(g.mx);
    g.list.swap(mods);
    g.gen++;
    if (live || lv.started) live_kick();
}

/* ---- PS3_GAME_MODS_TEST: the menu's actions from a script (tests) ---------------- */

static int find_folder(const std::string& folder)
{
    std::lock_guard<std::mutex> lk(g.mx);
    for (size_t i = 0; i < g.list.size(); i++) if (ieq(g.list[i].folder, folder)) return (int)i;
    return -1;
}

static void test_log_plan(const char* when)
{
    const int n = ps3_game_mods_count();
    for (int i = 0; i < n; i++) {
        ps3_game_mod m;
        if (!ps3_game_mods_get(i, &m)) continue;
        fprintf(stderr, "[mods] test: %s: %s%s%s -- %s%s%s\n", when, m.name, m.priority ? " (priority " : "",
                m.priority ? (std::to_string(m.priority) + ")").c_str() : "", m.status,
                m.prefer[0] ? " | Use this one instead: " : "", m.prefer);
        char img[600];
        if (ps3_game_mods_image(i, img, sizeof img)) fprintf(stderr, "[mods] test: %s: picture %s\n", m.name, img);
        for (int k = 0; k < m.n_modules; k++) {   /* the options as the page shows them */
            ps3_game_mod_option o;
            if (!ps3_game_mods_option(i, k, &o)) continue;
            fprintf(stderr, "[mods] test: %s: option %s%s%s [%s] %s\n", when, o.group, o.group[0] ? " / " : "", o.name,
                    o.single ? "one at a time" : o.dropdown ? "dropdown" : "check box", o.picked ? "ON" : "off");
        }
    }
    fprintf(stderr, "[mods] test: %s: restart needed: %s\n", when, ps3_game_mods_restart_needed() ? "yes" : "no");
}

static void test_wait_zip()
{
    for (int k = 0; k < 6000; k++) {
        { std::lock_guard<std::mutex> lk(gz.mx); if (!gz.busy) return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

static void test_run(std::string steps)
{
    fprintf(stderr, "[mods] test: %s\n", steps.c_str());
    size_t a = 0;
    for (size_t i = 0; i <= steps.size(); i++) {
        if (i < steps.size() && steps[i] != '|') continue;
        const std::string st = steps.substr(a, i - a);
        a = i + 1;
        const size_t eq = st.find('=');
        const std::string k = st.substr(0, eq), v = eq == std::string::npos ? std::string() : st.substr(eq + 1);
        if (k == "install" || k == "replace") {
            const std::string p = slashes(utf8_of_ansi(v));
            ZipPlan z;
            zip_check(p, z);
            fprintf(stderr, "[mods] test: check %s: %s\n", base_of(p).c_str(),
                    !z.err.empty() ? z.err.c_str() : (z.name + " -> mods/" + z.folder + (z.exists ? " (there already)" : "") + ", " +
                                                      std::to_string(z.files.size()) + " file(s)" +
                                                      (z.left_out ? ", " + std::to_string(z.left_out) + " left out (" + z.left_kinds + ")" : "")).c_str());
            if (ps3_game_mods_zip_install(p.c_str(), k == "replace")) test_wait_zip();
        } else if (k == "pick") {
            const size_t e2 = v.find('=');
            const int mi = find_folder(v.substr(0, e2));
            if (mi < 0 || e2 == std::string::npos) { fprintf(stderr, "[mods] test: pick: no mod %s\n", v.c_str()); continue; }
            std::set<std::string> want;
            const std::string ids = v.substr(e2 + 1);
            size_t b = 0;
            for (size_t j = 0; j <= ids.size(); j++)
                if (j == ids.size() || ids[j] == ',') { if (j > b) want.insert(lower(ids.substr(b, j - b))); b = j + 1; }
            ps3_game_mod m;
            ps3_game_mods_get(mi, &m);
            for (int pass = 0; pass < 2; pass++)   /* the wanted ones on first: the choice never goes empty */
                for (int o = 0; o < m.n_modules; o++) {
                    ps3_game_mod_option opt;
                    if (!ps3_game_mods_option(mi, o, &opt)) continue;
                    const bool w = want.count(lower(opt.id)) != 0;
                    if (pass == 0 && w && !opt.picked) ps3_game_mods_pick(mi, o, 1);
                    if (pass == 1 && !w && opt.picked && !opt.dropdown) ps3_game_mods_pick(mi, o, 0);
                }
            fprintf(stderr, "[mods] test: picked %s\n", v.c_str());
        } else if (k == "prefer") {
            const int mi = find_folder(v);
            ps3_game_mod m;
            if (mi < 0 || !ps3_game_mods_get(mi, &m)) { fprintf(stderr, "[mods] test: prefer: no mod %s\n", v.c_str()); continue; }
            fprintf(stderr, "[mods] test: before \"Use this one instead\" on %s: %s\n", m.name, m.prefer[0] ? m.prefer : "(no button: not left out by a clash)");
            fprintf(stderr, "[mods] test: prefer %s: %s\n", m.name, ps3_game_mods_prefer(mi) ? "done" : "nothing to do");
        } else if (k == "plan") {
            test_log_plan("plan");
        } else if (k == "zips") {
            ps3_game_mods_zips_refresh();
            test_wait_zip();
            const int n = ps3_game_mods_zip_count();
            for (int z = 0; z < n; z++) {
                ps3_game_mod_zip r;
                if (ps3_game_mods_zip_get(z, &r))
                    fprintf(stderr, "[mods] test: zip %s (%s): %s%s\n", r.file, r.where, r.ok ? "" : "cannot install: ", r.what);
            }
            fprintf(stderr, "[mods] test: %d other zip file(s) are not mods\n", ps3_game_mods_zip_not_mods());
        } else if (k == "restart") {
            const int need = ps3_game_mods_restart_needed();
            fprintf(stderr, "[mods] test: restart needed: %s%s\n", need ? "yes" : "no", need ? "; restarting (as \"Restart to apply\")" : "");
            if (need) ps3_host_restart("mods test: Restart to apply");
        } else if (!st.empty()) fprintf(stderr, "[mods] test: unknown step %s\n", st.c_str());
    }
}

static void test_start()
{
    const char* t = getenv("PS3_GAME_MODS_TEST");
    if (!t || !*t || getenv("PS3_RESTARTED")) return;   /* once: not again in the game a restart started */
    const char* at = getenv("PS3_GAME_MODS_TEST_AT");
    const double secs = at && *at ? atof(at) : 15.0;
    const std::string steps = t;
    std::thread([steps, secs] {
        std::this_thread::sleep_for(std::chrono::milliseconds((long long)(secs * 1000.0)));
        try { test_run(steps); }
        catch (const std::exception& e) { fprintf(stderr, "[mods] test: %s\n", e.what()); }
    }).detach();
}

} /* namespace gmods */

/* ---- for the in-game menu (vk_overlay.cpp) --------------------------------- */

extern "C" const char* ps3_game_mods_dir(void) { gmods::init(); return gmods::g.dir.c_str(); }

/* Electric Dash (cheats.cpp): 1 and its settings when a mod this session uses has "electric_dash". */
extern "C" int ps3_game_mods_electric_dash(float* speed, float* anim, float* drain, int* glow, int* glowskip)
{
    gmods::init();
    std::lock_guard<std::mutex> lk(gmods::g.mx);
    for (const gmods::Mod& m : gmods::g.list) {
        if (!m.ed) continue;
        auto it = gmods::g.start.find(gmods::lower(m.folder));
        if (it == gmods::g.start.end() || !it->second.active) continue;
        *speed = m.ed_speed; *anim = m.ed_anim; *drain = m.ed_drain; *glow = m.ed_glow; *glowskip = m.ed_glowskip;
        return 1;
    }
    return 0;
}

extern "C" int ps3_game_mods_count(void)
{
    gmods::init();
    std::lock_guard<std::mutex> lk(gmods::g.mx);
    return (int)gmods::g.list.size();
}

extern "C" int ps3_game_mods_in_use(void) { gmods::init(); return gmods::g.nserve.load(std::memory_order_acquire); }

extern "C" unsigned ps3_game_mods_generation(void)
{
    gmods::init();
    std::lock_guard<std::mutex> lk(gmods::g.mx);
    return (unsigned)gmods::g.gen;
}

extern "C" int ps3_game_mods_get(int i, ps3_game_mod* o)
{
    using namespace gmods;
    init();
    std::lock_guard<std::mutex> lk(g.mx);
    if (i < 0 || i >= (int)g.list.size() || !o) return 0;
    plan_locked();
    const Mod& m = g.list[(size_t)i];
    memset(o, 0, sizeof *o);
    snprintf(o->folder, sizeof o->folder, "%s", m.folder.c_str());
    snprintf(o->name, sizeof o->name, "%s", m.name.c_str());
    snprintf(o->author, sizeof o->author, "%s", m.author.c_str());
    snprintf(o->version, sizeof o->version, "%s", m.version.c_str());
    o->on = m.on; o->for_game = m.for_game;
    o->n_files = (int)m.files.size(); o->n_tex = (int)m.tex.size(); o->n_xpps = (int)m.xp.size();
    o->n_modules = (int)m.modules.size();
    o->live = m.live;
    auto it = g.start.find(lower(m.folder));
    o->active = it != g.start.end() && it->second.active;
    o->next = g.plan.next[(size_t)i];
    o->priority = m.have_prio ? m.prio : 0;
    o->has_image = !m.image.empty();
    if (g.plan.prefer[(size_t)i] > 0) {
        o->clash = 1;
        snprintf(o->prefer, sizeof o->prefer, "%s", g.plan.prefer_text[(size_t)i].c_str());
    }
    std::string t;
    int k = 0;
    for (const std::string& x : m.targets) { if (k++ < 4) t += (t.empty() ? "" : ", ") + x; }
    if (k > 4) t += ", +" + std::to_string(k - 4) + " more";
    snprintf(o->status, sizeof o->status, "%s", status_locked((size_t)i, t).c_str());
    o->conflicts = g.plan.unres[(size_t)i];
    snprintf(o->conflicts_with, sizeof o->conflicts_with, "%s", and_list(g.plan.unres_with[(size_t)i]).c_str());
    snprintf(o->merged_with, sizeof o->merged_with, "%s", and_list(g.plan.merged_with[(size_t)i]).c_str());
    return 1;
}

extern "C" int ps3_game_mods_option(int i, int k, ps3_game_mod_option* o)
{
    using namespace gmods;
    init();
    std::lock_guard<std::mutex> lk(g.mx);
    if (i < 0 || i >= (int)g.list.size() || !o) return 0;
    const Mod& m = g.list[(size_t)i];
    if (k < 0 || k >= (int)m.modules.size()) return 0;
    const Module& x = m.modules[(size_t)k];
    memset(o, 0, sizeof *o);
    snprintf(o->id, sizeof o->id, "%s", x.id.c_str());
    snprintf(o->name, sizeof o->name, "%s", x.name.c_str());
    snprintf(o->group, sizeof o->group, "%s", x.group_name.c_str());
    snprintf(o->group_id, sizeof o->group_id, "%s", x.group_id.c_str());
    o->dropdown = x.dropdown;
    o->single = x.single;
    o->picked = picked_ids(m).count(lower(x.id)) ? 1 : 0;
    return 1;
}

extern "C" void ps3_game_mods_set_on(int i, int on)
{
    using namespace gmods;
    init();
    std::lock_guard<std::mutex> lk(g.mx);
    if (i < 0 || i >= (int)g.list.size()) return;
    g.list[(size_t)i].on = on != 0;
    g.gen++;
    state_write_locked();
    if (g.list[(size_t)i].live) live_kick();   /* a live mod: on or off now */
}

extern "C" void ps3_game_mods_pick(int i, int k, int on)
{
    using namespace gmods;
    init();
    /* a copy, read without the lock (read_parts walks the mod's folders, and
     * every guest USRDIR open waits on g.mx), then put back */
    Mod m;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        if (i < 0 || i >= (int)g.list.size()) return;
        m = g.list[(size_t)i];
    }
    if (k < 0 || k >= (int)m.modules.size()) return;
    const Module x = m.modules[(size_t)k];
    std::set<std::string> ids = picked_ids(m);
    if (x.dropdown) {
        for (const Module& y : m.modules) if (y.dropdown && ieq(y.group_id, x.group_id)) ids.erase(lower(y.id));
        ids.insert(lower(x.id));
    } else if (on) ids.insert(lower(x.id));
    else ids.erase(lower(x.id));
    /* the Manager's "Select at least one module": an empty choice is never
     * saved (read back, it would mean every option) */
    if (ids.empty()) return;
    m.pick.clear();
    for (const Module& y : m.modules) if (ids.count(lower(y.id))) m.pick.push_back(y.id);
    m.have_pick = true;
    try { read_parts(m); }
    catch (const std::exception&) {
        m.files.clear(); m.tex.clear(); m.xp.clear(); m.targets.clear();
        m.err = "its files could not be read (too big?)";
    }
    std::lock_guard<std::mutex> lk(g.mx);
    if (i >= (int)g.list.size() || g.list[(size_t)i].folder != m.folder) return;   /* rescanned meanwhile */
    const bool live = m.live;
    g.list[(size_t)i] = std::move(m);
    g.gen++;
    state_write_locked();
    if (live) live_kick();
}

/* "Use this one instead" (a mod left out by a clash): a priority above every
 * mod it clashes with, saved. 1 = done. */
extern "C" int ps3_game_mods_prefer(int i)
{
    using namespace gmods;
    init();
    std::lock_guard<std::mutex> lk(g.mx);
    if (i < 0 || i >= (int)g.list.size()) return 0;
    plan_locked();
    const int p = g.plan.prefer[(size_t)i];
    if (p <= 0) return 0;
    Mod& m = g.list[(size_t)i];
    fprintf(stderr, "[mods] \"%s\" put first (priority %d): %s\n", m.name.c_str(), p, g.plan.prefer_text[(size_t)i].c_str());
    m.have_prio = true;
    m.prio = p;
    g.gen++;
    state_write_locked();
    return 1;
}

/* With g.mx held: the parts mod i and the mods that are on both change, in a
 * fixed order (the other mods' order, then file and part). */
static std::vector<std::pair<size_t, gmods::Clash>> conflicts_locked(size_t i)
{
    using namespace gmods;
    std::vector<std::pair<size_t, Clash>> r;
    plan_locked();
    for (size_t j = 0; j < g.list.size(); j++) {
        std::vector<Clash> cl;
        if (!pair_items_locked(i, j, cl)) continue;
        for (const Clash& c : cl) r.push_back({ j, c });
    }
    return r;
}

extern "C" int ps3_game_mods_conflicts(int i, ps3_game_mod_conflict* out, int cap)
{
    using namespace gmods;
    init();
    std::lock_guard<std::mutex> lk(g.mx);
    if (i < 0 || i >= (int)g.list.size()) return 0;
    const std::vector<std::pair<size_t, Clash>> r = conflicts_locked((size_t)i);
    const Mod& m = g.list[(size_t)i];
    for (size_t k = 0; out && k < r.size() && (int)k < cap; k++) {
        const Mod& o = g.list[r[k].first];
        const Clash& c = r[k].second;
        ps3_game_mod_conflict& x = out[k];
        memset(&x, 0, sizeof x);
        x.other = (int)r[k].first;
        snprintf(x.other_name, sizeof x.other_name, "%s", o.name.c_str());
        snprintf(x.file, sizeof x.file, "%s", c.target.c_str());
        snprintf(x.part, sizeof x.part, "%s", c.item.c_str());
        snprintf(x.what, sizeof x.what, "%s, %s", c.target.c_str(), item_label(c.item).c_str());
        x.merges = c.merges;
        x.winner = c.merges ? -1 : res_winner(m, o, c.target, c.item);
        x.live = m.live || o.live;
    }
    return (int)r.size();
}

extern "C" int ps3_game_mods_resolve(int i, const int* winners, int n)
{
    using namespace gmods;
    init();
    std::lock_guard<std::mutex> lk(g.mx);
    if (i < 0 || i >= (int)g.list.size() || !winners) return 0;
    const std::vector<std::pair<size_t, Clash>> r = conflicts_locked((size_t)i);
    bool any = false, live = false;
    for (size_t k = 0; k < r.size() && (int)k < n; k++) {
        const Clash& c = r[k].second;
        if (c.merges || (winners[k] != 0 && winners[k] != 1)) continue;
        Mod& m = g.list[(size_t)i];
        Mod& o = g.list[r[k].first];
        m.res[res_key(o, c.target, c.item)] = winners[k] == 0;
        o.res[res_key(m, c.target, c.item)] = winners[k] != 0;
        fprintf(stderr, "[mods] conflict resolved: %s, %s: %s's version, not %s's\n", c.target.c_str(), item_label(c.item).c_str(),
                (winners[k] ? o : m).name.c_str(), (winners[k] ? m : o).name.c_str());
        any = true;
        if (m.live || o.live) live = true;
    }
    if (!any) return 0;
    g.gen++;
    state_write_locked();
    if (live) live_kick();
    plan_locked();
    return g.plan.sig != g.start_sig ? 2 : 1;
}

/* The saved choices would make the next start use other mods (or the same
 * mods otherwise) than this session does. */
extern "C" int ps3_game_mods_restart_needed(void)
{
    using namespace gmods;
    init();
    if (g.dir.empty()) return 0;
    std::lock_guard<std::mutex> lk(g.mx);
    plan_locked();
    return g.plan.sig != g.start_sig ? 1 : 0;
}

/* The mod's preview picture (image_path's rules), its host path. 1 = one to show. */
extern "C" int ps3_game_mods_image(int i, char* out, size_t cap)
{
    using namespace gmods;
    init();
    if (!out || !cap) return 0;
    out[0] = 0;
    Mod m;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        if (i < 0 || i >= (int)g.list.size() || g.list[(size_t)i].image.empty()) return 0;
        m.path = g.list[(size_t)i].path; m.image = g.list[(size_t)i].image;
    }
    std::string p, why;
    if (!image_path(m, p, &why)) {
        log_once("img:" + m.path, "%s: no preview picture: \"%s\" -- %s", m.path.c_str(), m.image.c_str(), why.c_str());
        return 0;
    }
    if (p.size() >= cap) return 0;
    snprintf(out, cap, "%s", p.c_str());
    return 1;
}

extern "C" void ps3_game_mods_rescan(void)
{
    gmods::init();
    gmods::rescan_now();
}

/* Opens the mods folder in the file manager (Windows; elsewhere nothing). */
extern "C" void ps3_game_mods_open_folder(void)
{
    gmods::init();
    if (gmods::g.dir.empty()) return;
#ifdef _WIN32
    gmods::mkdirs(gmods::g.dir);
    std::string w = gmods::g.dir;
    for (char& c : w) if (c == '/') c = '\\';
    ShellExecuteA(NULL, "open", w.c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif
}

/* ---- zips ---- */

extern "C" void ps3_game_mods_zips_refresh(void)
{
    gmods::init();
    if (gmods::g.dir.empty()) return;
    gmods::zip_job("Looking at the zip files", [] { gmods::zip_list_run(); });
}

extern "C" int ps3_game_mods_zip_count(void)
{
    std::lock_guard<std::mutex> lk(gmods::gz.mx);
    return (int)gmods::gz.rows.size();
}

extern "C" int ps3_game_mods_zip_not_mods(void)
{
    std::lock_guard<std::mutex> lk(gmods::gz.mx);
    return gmods::gz.not_mods;
}

extern "C" int ps3_game_mods_zip_get(int i, ps3_game_mod_zip* o)
{
    using namespace gmods;
    std::lock_guard<std::mutex> lk(gz.mx);
    if (!o || i < 0 || i >= (int)gz.rows.size()) return 0;
    const ZipRow& r = gz.rows[(size_t)i];
    memset(o, 0, sizeof *o);
    snprintf(o->path, sizeof o->path, "%s", r.path.c_str());
    snprintf(o->file, sizeof o->file, "%s", r.file.c_str());
    snprintf(o->where, sizeof o->where, "%s", r.where.c_str());
    o->size = (long long)r.size;
    o->mtime = (long long)r.mtime;
    o->ok = r.plan.err.empty();
    o->exists = r.plan.exists;
    snprintf(o->folder, sizeof o->folder, "%s", r.plan.folder.c_str());
    if (o->ok) {
        std::string w = r.plan.name + (r.plan.version.empty() ? "" : " " + r.plan.version) + (r.plan.author.empty() ? "" : " by " + r.plan.author);
        w += r.plan.exists ? " -- mods/" + r.plan.folder + " is there already" : " -> mods/" + r.plan.folder;
        snprintf(o->what, sizeof o->what, "%s", w.c_str());
    } else snprintf(o->what, sizeof o->what, "%s", r.plan.err.c_str());
    return 1;
}

extern "C" int ps3_game_mods_zip_install(const char* path_u8, int replace)
{
    gmods::init();
    if (!path_u8 || !*path_u8 || gmods::g.dir.empty()) return 0;
    const std::string p = path_u8;
    const bool rep = replace != 0;
    return gmods::zip_job("Installing " + gmods::base_of(gmods::slashes(p)), [p, rep] { gmods::zip_install_run(p, rep); }) ? 1 : 0;
}

extern "C" int ps3_game_mods_zip_choose(void)
{
    gmods::init();
    if (gmods::g.dir.empty()) return 0;
    return gmods::zip_job("Waiting for the file window", [] { gmods::zip_choose_run(); }) ? 1 : 0;
}

extern "C" int ps3_game_mods_zip_busy(char* what, size_t cap, float* progress)
{
    using namespace gmods;
    std::lock_guard<std::mutex> lk(gz.mx);
    if (what && cap) snprintf(what, cap, "%s", gz.busy_what.c_str());
    if (progress) *progress = gz.total ? (float)((double)gz.done.load() / (double)gz.total) : 0.0f;
    return gz.busy ? 1 : 0;
}

extern "C" int ps3_game_mods_zip_result(char* out, size_t cap)
{
    using namespace gmods;
    std::lock_guard<std::mutex> lk(gz.mx);
    if (out && cap) snprintf(out, cap, "%s", gz.result.c_str());
    return gz.result_kind;
}

extern "C" unsigned ps3_game_mods_zip_seq(void)
{
    std::lock_guard<std::mutex> lk(gmods::gz.mx);
    return (unsigned)gmods::gz.seq;
}

#endif /* PPU_FS_MODS_H */
