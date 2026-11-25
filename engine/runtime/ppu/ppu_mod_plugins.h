/*
 * The mod manager's plugin host (include/ps3emu/ps3_mod_plugin.h): a mod's own code,
 * mod.json "plugin", loaded at start by runtime/ppu/ppu_fs_mods.h for a mod that is
 * used and whose code the player allowed. Included once, by ppu_loader.cpp (after the
 * hook slots: plugins chain onto them with ppu_hook_add).
 */
#include "../../include/ps3emu/ps3_mod_plugin.h"
#include "ppu_sha256.h"
#include "ppu_mod_pe.h"
#include <chrono>
#include <string>
#include <stdarg.h>
#include <vector>
#include <mutex>

extern "C" void* ps3_module_ppu_fn(uint32_t guest_addr);   /* ps3emu/ps3_module.h */

namespace modplug {

struct Plugin {
    ps3_mod_host h;                      /* first: the plugin's `self` is this */
    std::string folder, name, json_text, tag;
    std::string last_key, last_val;      /* json(): kept until the next call */
    void* lib = nullptr;
    ps3_mod_plugin_enable_fn enable = nullptr;
    bool on = true;
    std::vector<std::pair<uint32_t, ps3_mod_hook>> hooks;   /* what it added (taken out while off) */
    std::string data_dir;
    ps3_mod_save_read save_rd = nullptr;
    ps3_mod_save_write save_wr = nullptr;
    ps3_mod_rows_fn rows = nullptr;
    ps3_mod_press_fn press = nullptr;
};
static Plugin* self_of(const ps3_mod_host* h) { return (Plugin*)(void*)h; }

static void h_log(const ps3_mod_host* self, const char* fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    size_t n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    fprintf(stderr, "[%s] %s\n", self ? self_of(self)->tag.c_str() : "plugin", b);
}

/* The JSON text of a top-level key of the mod's mod.json ("" escapes and nesting skipped). */
static const char* skip_ws(const char* p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
static const char* skip_str(const char* p)   /* p at '"': past the closing one */
{
    for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
    return *p ? p + 1 : p;
}
static const char* skip_val(const char* p)
{
    p = skip_ws(p);
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        int d = 0;
        while (*p) {
            if (*p == '"') { p = skip_str(p); continue; }
            if (*p == '{' || *p == '[') d++;
            else if (*p == '}' || *p == ']') { if (--d == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}
static const char* h_json(const ps3_mod_host* self, const char* key)
{
    if (!self || !key) return nullptr;
    Plugin* P = self_of(self);
    const char* p = skip_ws(P->json_text.c_str());
    if (*p != '{') return nullptr;
    p = skip_ws(p + 1);
    while (*p == '"') {
        const char* k0 = p + 1;
        const char* k1 = skip_str(p);
        const std::string k(k0, k1 > k0 ? (size_t)(k1 - k0 - 1) : 0);
        p = skip_ws(k1);
        if (*p != ':') return nullptr;
        p = skip_ws(p + 1);
        const char* v1 = skip_val(p);
        if (k == key) {
            std::string v(p, (size_t)(v1 - p));
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
            P->last_key = k; P->last_val = v;
            return P->last_val.c_str();
        }
        p = skip_ws(v1);
        if (*p != ',') return nullptr;
        p = skip_ws(p + 1);
    }
    return nullptr;
}
static double h_now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/* 1: [ea, ea+len) is guest memory the game has mapped (committed and readable), all of it */
static int h_valid(uint32_t ea, uint32_t len)
{
    if (!vm_base || ea < 0x10000u || (uint64_t)ea + len > 0x100000000ull) return 0;
#ifdef _WIN32   /* the host's own view of guest memory: main memory, RSX, the stacks, and what the game maps later (heaps) */
    uint64_t a = ea;
    const uint64_t e = (uint64_t)ea + (len ? len : 1);
    while (a < e) {   /* each region the range crosses */
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        const DWORD pr = mi.Protect & 0xFF;
        if ((mi.Protect & PAGE_GUARD) || pr == PAGE_NOACCESS || pr == PAGE_EXECUTE) return 0;
        const uint64_t end = (uint64_t)((uint8_t*)mi.BaseAddress + mi.RegionSize - vm_base);
        if (end <= a) return 0;
        a = end;
    }
#else
    if (!vm_is_valid_addr(ea) || (len && !vm_is_valid_addr(ea + len - 1))) return 0;
#endif
    return 1;
}
static uint8_t h_read8(uint32_t ea) { return vm_read8(ea); }
static uint32_t h_read32(uint32_t ea) { return vm_read32(ea); }
static float h_readf(uint32_t ea) { const uint32_t u = vm_read32(ea); float f; memcpy(&f, &u, 4); return f; }
static void h_write8(uint32_t ea, uint8_t v) { vm_write8(ea, v); }
static void h_write32(uint32_t ea, uint32_t v) { vm_write32(ea, v); }
static void h_writef(uint32_t ea, float v) { uint32_t u; memcpy(&u, &v, 4); vm_write32(ea, u); }

static uint64_t h_gpr(ps3_ctx* c, int i) { return i >= 0 && i < 32 ? c->gpr[i] : 0; }
static void h_set_gpr(ps3_ctx* c, int i, uint64_t v) { if (i >= 0 && i < 32) c->gpr[i] = v; }
static double h_fpr(ps3_ctx* c, int i) { return i >= 0 && i < 32 ? c->fpr[i] : 0.0; }
static void h_set_fpr(ps3_ctx* c, int i, double v) { if (i >= 0 && i < 32) c->fpr[i] = v; }
/* vector registers hold the guest's big-endian bytes */
static void h_vr(ps3_ctx* c, int i, float out[4])
{
    for (int k = 0; k < 4; k++) { const uint32_t u = i >= 0 && i < 32 ? __builtin_bswap32(c->vr[i].w[k]) : 0; memcpy(&out[k], &u, 4); }
}
static void h_set_vr(ps3_ctx* c, int i, const float v[4])
{
    if (i < 0 || i >= 32) return;
    for (int k = 0; k < 4; k++) { uint32_t u; memcpy(&u, &v[k], 4); c->vr[i].w[k] = __builtin_bswap32(u); }
}
static uint64_t h_lr(ps3_ctx* c) { return c->lr; }

static int h_hook_exists(uint32_t a) { return ppu_hook_exists(a); }
static std::mutex& mx() { static std::mutex m; return m; }
static int h_hook_add(const ps3_mod_host* self, uint32_t a, ps3_mod_hook fn)
{
    if (!self || !fn) return 0;
    Plugin* P = self_of(self);
    std::lock_guard<std::mutex> lk(mx());
    for (const auto& h : P->hooks) if (h.first == a && h.second == fn) return 1;
    if (P->on && !ppu_hook_add(a, (ppu_hook_fn)fn)) return 0;
    P->hooks.push_back({ a, fn });
    return 1;
}
static int h_hook_remove(const ps3_mod_host* self, uint32_t a, ps3_mod_hook fn)
{
    if (!self) return 0;
    Plugin* P = self_of(self);
    std::lock_guard<std::mutex> lk(mx());
    for (size_t i = 0; i < P->hooks.size(); i++)
        if (P->hooks[i].first == a && P->hooks[i].second == fn) {
            P->hooks.erase(P->hooks.begin() + (ptrdiff_t)i);
            if (P->on) ppu_hook_remove(a, (ppu_hook_fn)fn);
            return 1;
        }
    return 0;
}
static ps3_mod_next h_game_fn(uint32_t a) { return (ps3_mod_next)ps3_module_ppu_fn(a); }
static int h_call_game(ps3_ctx* ctx, uint32_t a, void (*setup)(ps3_ctx*, void*), void (*result)(ps3_ctx*, void*), void* user)
{
    const ppu_hook_body f = (ppu_hook_body)ps3_module_ppu_fn(a);
    if (!f || !ctx) return 0;
    ppu_context* saved = new ppu_context;   /* (a hook's guest thread; the context is large and aligned) */
    memcpy(saved, ctx, sizeof *saved);
    if (setup) setup(ctx, user);
    ppu_hook_call(ctx, f);
    if (result) { try { result(ctx, user); } catch (...) {} }   /* its return value (r3, f1, ...) before ctx is put back */
    memcpy(ctx, saved, sizeof *saved);
    delete saved;
    return 1;
}

static std::vector<Plugin*>& list() { static std::vector<Plugin*> v; return v; }   /* (under mx) */

/* The game's save files, to every plugin that is on (cellSaveData's hooks) */
static void save_read(const char* file, unsigned char* buf, size_t got, unsigned long long off)
{
    std::vector<ps3_mod_save_read> fs;
    { std::lock_guard<std::mutex> lk(mx()); for (Plugin* P : list()) if (P->on && P->save_rd) fs.push_back(P->save_rd); }
    for (ps3_mod_save_read f : fs) { try { f(file, buf, got, off); } catch (...) {} }
}
static void save_write(const char* file, const unsigned char* buf, size_t n, unsigned long long off)
{
    std::vector<ps3_mod_save_write> fs;
    { std::lock_guard<std::mutex> lk(mx()); for (Plugin* P : list()) if (P->on && P->save_wr) fs.push_back(P->save_wr); }
    for (ps3_mod_save_write f : fs) { try { f(file, buf, n, off); } catch (...) {} }
}
static int h_on_save(const ps3_mod_host* self, ps3_mod_save_read rd, ps3_mod_save_write wr)
{
    if (!self) return 0;
    std::lock_guard<std::mutex> lk(mx());
    self_of(self)->save_rd = rd; self_of(self)->save_wr = wr;
    return 1;
}
static int h_menu(const ps3_mod_host* self, ps3_mod_rows_fn rows, ps3_mod_press_fn press)
{
    if (!self) return 0;
    std::lock_guard<std::mutex> lk(mx());
    self_of(self)->rows = rows; self_of(self)->press = press;
    return 1;
}

} // namespace modplug

extern "C" void (*ps3_savedata_read_hook)(const char* file, unsigned char* buf, size_t got, unsigned long long off);
extern "C" void (*ps3_savedata_write_hook)(const char* file, const unsigned char* buf, size_t n, unsigned long long off);

#ifdef _WIN32
namespace modplug {
static std::wstring wide(const std::string& u)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}
/* The whole file through a handle that lets nobody write, rename or delete it while it is open. */
static HANDLE open_locked(const std::wstring& path, std::vector<uint8_t>& bytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64ll << 20)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    bytes.resize((size_t)sz.QuadPart);
    size_t got = 0;
    while (got < bytes.size()) {
        DWORD r = 0;
        if (!ReadFile(h, bytes.data() + got, (DWORD)(bytes.size() - got), &r, nullptr) || !r) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
        got += r;
    }
    return h;
}
/* A Windows system DLL by that name (System32, or an API set): never a file next to the plugin. */
static bool system_dll(const std::string& name)
{
    if (!modpe::plain_name(name)) return false;
    if (modpe::api_set(name)) return true;
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    const std::wstring p = std::wstring(sys) + L"\\" + wide(name);
    const DWORD at = GetFileAttributesW(p.c_str());
    return at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY);
}
}   /* namespace modplug */
#endif

/* ppu_fs_mods.h: load <dll> for the mod at <folder> -- only if the file is still the one the player
 * allowed (sha256_hex) and every DLL it imports is a Windows system DLL. The verified bytes are copied to
 * <copy_dir>/<sha256>.dll and that copy, held open so nobody can change it, is what is loaded (its imports
 * from System32 only). json_text: its mod.json, for json(); data_dir: its own folder for files (made).
 * 1 = set up. */
extern "C" int ps3_mod_plugin_load(const char* dll_u8, const char* sha256_hex, const char* copy_dir_u8, const char* folder_u8,
                                   const char* name_u8, const char* json_text, const char* data_dir_u8, char* err, size_t cap)
{
    using namespace modplug;
    auto fail = [&](const std::string& e) { if (err && cap) snprintf(err, cap, "%s", e.c_str()); return 0; };
#ifdef _WIN32
    if (!dll_u8 || !sha256_hex || !copy_dir_u8 || !*copy_dir_u8) return fail("no file to load");
    std::vector<uint8_t> b;
    HANDLE src = open_locked(wide(dll_u8), b);
    if (src == INVALID_HANDLE_VALUE) return fail("could not read it");
    const std::string got = sha256::hex(b.data(), b.size());
    CloseHandle(src);
    if (got != sha256_hex) return fail("its file changed since its code was allowed: allow it again");
    std::vector<std::string> imp;
    std::string pe_err;
    if (!modpe::imports(b.data(), b.size(), imp, pe_err)) return fail(pe_err);
    for (const std::string& d : imp)
        if (!system_dll(d)) return fail("it needs " + d + ", which is not a Windows system DLL (a plugin may only use those)");
    const std::wstring copy = wide(std::string(copy_dir_u8) + "/" + got + ".dll");
    {   /* the verified bytes, as this host's own copy */
        std::vector<uint8_t> have;
        HANDLE c = open_locked(copy, have);
        const bool same = c != INVALID_HANDLE_VALUE && have == b;
        if (c != INVALID_HANDLE_VALUE) CloseHandle(c);
        if (!same) {
            HANDLE w = CreateFileW(copy.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (w == INVALID_HANDLE_VALUE) return fail("could not copy it to " + std::string(copy_dir_u8));
            DWORD wr = 0;
            const bool ok = WriteFile(w, b.data(), (DWORD)b.size(), &wr, nullptr) && wr == b.size();
            CloseHandle(w);
            if (!ok) return fail("could not copy it to " + std::string(copy_dir_u8));
        }
    }
    std::vector<uint8_t> again;
    HANDLE held = open_locked(copy, again);   /* held (nobody may write it) until it is loaded */
    if (held == INVALID_HANDLE_VALUE || sha256::hex(again.data(), again.size()) != got) {
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        return fail("its copy changed while it was loaded");
    }
    /* its imports (all system DLLs, checked above) from System32 only: not the copy's folder, the game's, or PATH */
    HMODULE lib = LoadLibraryExW(copy.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const DWORD le = GetLastError();
    CloseHandle(held);
    if (!lib) return fail("could not load it (Windows error " + std::to_string((unsigned long)le) + ")");
    const ps3_mod_plugin_init_fn init = (ps3_mod_plugin_init_fn)(void*)GetProcAddress(lib, "ps3_mod_plugin_init");
    if (!init) { FreeLibrary(lib); return fail("it has no ps3_mod_plugin_init"); }
    const ps3_mod_plugin_enable_fn enable = (ps3_mod_plugin_enable_fn)(void*)GetProcAddress(lib, "ps3_mod_plugin_enable");
#else
    (void)dll_u8; (void)sha256_hex; (void)copy_dir_u8;
    return fail("plugins are Windows DLLs in this port");
    ps3_mod_plugin_init_fn init = nullptr; ps3_mod_plugin_enable_fn enable = nullptr; void* lib = nullptr;
#endif
    Plugin* P = new Plugin();
    P->folder = folder_u8 ? folder_u8 : ""; P->name = name_u8 ? name_u8 : ""; P->json_text = json_text ? json_text : "";
    P->tag = P->name.empty() ? std::string("plugin") : P->name;
    P->lib = (void*)lib;
    P->enable = enable;
    P->data_dir = data_dir_u8 ? data_dir_u8 : "";
    ps3_mod_host& h = P->h;
    memset(&h, 0, sizeof h);
    h.api = PS3_MOD_PLUGIN_API; h.size = sizeof h;
    h.mod_folder = P->folder.c_str(); h.mod_name = P->name.c_str();
    h.log = h_log; h.json = h_json; h.now = h_now;
    h.valid = h_valid; h.read8 = h_read8; h.read32 = h_read32; h.readf = h_readf;
    h.write8 = h_write8; h.write32 = h_write32; h.writef = h_writef;
    h.gpr = h_gpr; h.set_gpr = h_set_gpr; h.fpr = h_fpr; h.set_fpr = h_set_fpr; h.vr = h_vr; h.set_vr = h_set_vr; h.lr = h_lr;
    h.hook_exists = h_hook_exists; h.hook_add = h_hook_add; h.hook_remove = h_hook_remove;
    h.game_fn = h_game_fn; h.call_game = h_call_game;
    h.data_dir = P->data_dir.c_str(); h.on_save = h_on_save; h.menu = h_menu;
    ps3_savedata_read_hook = save_read;     /* (the saves go through the plugins that are on) */
    ps3_savedata_write_hook = save_write;
    int ok = 0;
    try { ok = init(&h); } catch (...) { ok = 0; }
    if (!ok) {   /* kept loaded (nothing may still point into it), its hooks out */
        std::lock_guard<std::mutex> lk(mx());
        for (const auto& hk : P->hooks) ppu_hook_remove(hk.first, (ppu_hook_fn)hk.second);
        P->hooks.clear(); P->on = false;
        return fail("its setup said it is not used");
    }
    std::lock_guard<std::mutex> lk(mx());
    list().push_back(P);
    return 1;
}

/* The player switched a loaded plugin's mod off (0) or on (1) while the game runs (ppu_fs_mods.h).
 * Its hooks go out after its enable(0) and back in before its enable(1). 1 = such a plugin is loaded. */
extern "C" int ps3_mod_plugin_set_on(const char* folder_u8, int on)
{
    using namespace modplug;
    Plugin* P = nullptr;
    {
        std::lock_guard<std::mutex> lk(mx());
        for (Plugin* q : list()) if (folder_u8 && q->folder == folder_u8) P = q;
    }
    if (!P) return 0;
    if ((on != 0) == P->on) return 1;
    if (!on) {
        if (P->enable) { try { P->enable(0); } catch (...) {} }
        std::lock_guard<std::mutex> lk(mx());
        for (const auto& hk : P->hooks) ppu_hook_remove(hk.first, (ppu_hook_fn)hk.second);
        P->on = false;
    } else {
        {
            std::lock_guard<std::mutex> lk(mx());
            for (const auto& hk : P->hooks) ppu_hook_add(hk.first, (ppu_hook_fn)hk.second);
            P->on = true;
        }
        if (P->enable) { try { P->enable(1); } catch (...) {} }
    }
    fprintf(stderr, "[mods] plugin of \"%s\" %s\n", P->name.c_str(), on ? "on again" : "off (its hooks are out)");
    return 1;
}

/* A loaded plugin's rows for the menu (its mod's folder); the count (0: none, or it is off) */
extern "C" int ps3_mod_plugin_rows(const char* folder_u8, ps3_mod_row* out, int cap)
{
    using namespace modplug;
    ps3_mod_rows_fn f = nullptr;
    {
        std::lock_guard<std::mutex> lk(mx());
        for (Plugin* q : list()) if (folder_u8 && q->folder == folder_u8 && q->on) f = q->rows;
    }
    if (!f || !out || cap <= 0) return 0;
    memset(out, 0, sizeof *out * (size_t)cap);
    int n = 0;
    try { n = f(out, cap); } catch (...) { n = 0; }
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {   /* the plugin's strings, ended whatever it wrote */
        out[i].id[sizeof out[i].id - 1] = 0; out[i].label[sizeof out[i].label - 1] = 0; out[i].button[sizeof out[i].button - 1] = 0;
        out[i].desc[sizeof out[i].desc - 1] = 0; out[i].status[sizeof out[i].status - 1] = 0;
    }
    return n < 0 ? 0 : n;
}
extern "C" void ps3_mod_plugin_press(const char* folder_u8, const char* id)
{
    using namespace modplug;
    ps3_mod_press_fn f = nullptr;
    std::string name;
    {
        std::lock_guard<std::mutex> lk(mx());
        for (Plugin* q : list()) if (folder_u8 && q->folder == folder_u8 && q->on) { f = q->press; name = q->name; }
    }
    if (!f || !id) return;
    fprintf(stderr, "[mods] plugin of \"%s\": %s pressed\n", name.c_str(), id);
    try { f(id); } catch (...) {}
}
