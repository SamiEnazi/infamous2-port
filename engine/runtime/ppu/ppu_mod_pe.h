/*
 * The DLLs a Windows x64 DLL (PE32+) imports, read from its bytes: the plugin host loads a mod's plugin
 * only when every one of them is a Windows system DLL, so the player's consent (the plugin file's hash)
 * covers all the code a plugin brings. Portable (the tests run it anywhere); the file is untrusted input:
 * every offset is checked against its size.
 */
#ifndef PPU_MOD_PE_H
#define PPU_MOD_PE_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace modpe {

static inline uint32_t le16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* names: the imported DLL names (normal and delay-load imports, as written). false: not a DLL it can read (err). */
static inline bool imports(const uint8_t* d, size_t n, std::vector<std::string>& names, std::string& err)
{
    names.clear();
    auto in = [&](uint64_t off, uint64_t len) { return off <= n && len <= n - off; };
    if (!in(0, 0x40) || d[0] != 'M' || d[1] != 'Z') { err = "it is not a Windows DLL"; return false; }
    const uint32_t pe = le32(d + 0x3C);
    if (!in(pe, 24) || d[pe] != 'P' || d[pe + 1] != 'E' || d[pe + 2] || d[pe + 3]) { err = "it is not a Windows DLL"; return false; }
    if (le16(d + pe + 4) != 0x8664) { err = "it is not a 64-bit (x64) DLL"; return false; }
    const uint32_t nsec = le16(d + pe + 6), optsz = le16(d + pe + 20);
    const uint64_t opt = (uint64_t)pe + 24;
    if (!in(opt, optsz) || optsz < 112 || le16(d + opt) != 0x20B) { err = "it is not a 64-bit (x64) DLL"; return false; }
    const uint32_t ndir = le32(d + opt + 108);
    const uint64_t sec = opt + optsz;
    if (nsec > 96 || !in(sec, 40ull * nsec)) { err = "its section table is broken"; return false; }
    auto file_of = [&](uint32_t rva, uint64_t& off) {   /* an RVA's place in the file */
        for (uint32_t i = 0; i < nsec; i++) {
            const uint8_t* s = d + sec + 40ull * i;
            const uint32_t vsz = le32(s + 8), va = le32(s + 12), rsz = le32(s + 16), raw = le32(s + 20);
            const uint32_t span = vsz > rsz ? vsz : rsz;
            if (rva >= va && (uint64_t)rva < (uint64_t)va + span) {
                if ((uint64_t)rva - va >= rsz) return false;   /* (not in the file: zero-filled) */
                off = (uint64_t)raw + (rva - va);
                return off < n;
            }
        }
        return false;
    };
    auto name_at = [&](uint32_t rva, std::string& out) {
        uint64_t off;
        if (!file_of(rva, off)) return false;
        out.clear();
        while (off < n && d[off] && out.size() < 260) out += (char)d[off++];
        return off < n && !d[off] && !out.empty();
    };
    auto dir = [&](uint32_t k, uint32_t& rva, uint32_t& size) {
        rva = size = 0;
        if (k >= ndir || !in(opt + 112 + 8ull * k, 8)) return;
        rva = le32(d + opt + 112 + 8ull * k); size = le32(d + opt + 116 + 8ull * k);
    };
    uint32_t rva, size;
    dir(1, rva, size);   /* the import table: 20-byte descriptors, the name at +12, ended by a zero one */
    if (rva) {
        uint64_t off;
        if (!file_of(rva, off)) { err = "its import table is broken"; return false; }
        for (uint32_t i = 0;; i++) {
            if (i >= 4096 || !in(off + 20ull * i, 20)) { err = "its import table is broken"; return false; }
            const uint8_t* e = d + off + 20ull * i;
            bool zero = true;
            for (int k = 0; k < 20; k++) if (e[k]) zero = false;
            if (zero) break;
            std::string nm;
            if (!name_at(le32(e + 12), nm)) { err = "its import table is broken"; return false; }
            names.push_back(nm);
        }
    }
    dir(13, rva, size);   /* delay-load imports: 32-byte descriptors, the name at +4, ended by a zero one */
    if (rva) {
        uint64_t off;
        if (!file_of(rva, off)) { err = "its delay-load table is broken"; return false; }
        for (uint32_t i = 0;; i++) {
            if (i >= 4096 || !in(off + 32ull * i, 32)) { err = "its delay-load table is broken"; return false; }
            const uint8_t* e = d + off + 32ull * i;
            bool zero = true;
            for (int k = 0; k < 32; k++) if (e[k]) zero = false;
            if (zero) break;
            std::string nm;
            if (!name_at(le32(e + 4), nm)) { err = "its delay-load table is broken"; return false; }
            names.push_back(nm);
        }
    }
    return true;
}

/* A name that can only mean a DLL in System32 (or an API set), never one next to the plugin. */
static inline bool plain_name(const std::string& s)
{
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 32) return false;
    return s.find("..") == std::string::npos;
}
static inline bool api_set(const std::string& s)
{
    std::string l;
    for (char c : s) l += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return l.compare(0, 11, "api-ms-win-") == 0 || l.compare(0, 11, "ext-ms-win-") == 0;
}

}   /* namespace modpe */

#endif
