/* [about] supporters.json, the list F2 > About shows. tools/update_check.ps1 fetches it from the
 * public repository with the update check and keeps it in work/update/supporters.json; the copy
 * that came with this install (the port folder's supporters.json) is the fallback.
 *
 *   {"v":1, "supporters":["Name", ...], "contributors":["Name", ...], "support_url":"https://..."}
 *
 * The file is text from the network: nothing in it is trusted.
 *   - more than 64 KB, not one JSON object, or broken JSON: the whole file is refused (false);
 *   - at most 500 names per list, each cut to 40 characters; control characters, newlines,
 *     invisible / direction-changing characters and broken UTF-8 are dropped; empty names are skipped;
 *   - support_url is kept only when it starts with "https://", is at most 200 characters and holds
 *     nothing but plain URL characters; else it is "" (no row in the menu);
 *   - unknown keys are skipped (nesting up to 16 levels).
 * The names are shown with AddText / "%s" only, never as a format string.
 * Pure C++ (no ImGui, no files): libs/video/tests/test_about_file.cpp. */
#pragma once

#include <stddef.h>
#include <string.h>
#include <string>
#include <vector>

enum { ABOUT_FILE_MAX = 64 << 10, ABOUT_NAMES_MAX = 500, ABOUT_NAME_CHARS = 40, ABOUT_URL_MAX = 200 };

struct AboutFile {
    std::vector<std::string> supporters, contributors;
    std::string support_url;     /* "" or https://... */
};

struct AboutParse_ {
    const unsigned char* p;
    const unsigned char* e;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool lit(const char* s) { const size_t n = strlen(s); if ((size_t)(e - p) < n || memcmp(p, s, n)) return false; p += n; return true; }
    static void utf8(std::string* o, unsigned c)
    {
        if (c < 0x80) o->push_back((char)c);
        else if (c < 0x800) { o->push_back((char)(0xC0 | c >> 6)); o->push_back((char)(0x80 | (c & 63))); }
        else if (c < 0x10000) { o->push_back((char)(0xE0 | c >> 12)); o->push_back((char)(0x80 | (c >> 6 & 63))); o->push_back((char)(0x80 | (c & 63))); }
        else { o->push_back((char)(0xF0 | c >> 18)); o->push_back((char)(0x80 | (c >> 12 & 63))); o->push_back((char)(0x80 | (c >> 6 & 63))); o->push_back((char)(0x80 | (c & 63))); }
    }
    bool hex4(unsigned* v)
    {
        if (e - p < 4) return false;
        unsigned x = 0;
        for (int i = 0; i < 4; i++) {
            const unsigned char c = p[i];
            x = x << 4 | (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 256u);
            if (x & ~0xFFFFu) return false;
        }
        p += 4; *v = x;
        return true;
    }
    /* a JSON string, raw (escapes decoded; still untrusted). `out` may be NULL. */
    bool str(std::string* out)
    {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e) {
            const unsigned char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { if (out) out->push_back((char)c); continue; }
            if (p >= e) return false;
            const unsigned char k = *p++;
            unsigned u = 0;
            switch (k) {
            case '"': u = '"'; break;   case '\\': u = '\\'; break; case '/': u = '/'; break;
            case 'b': u = 8; break;     case 'f': u = 12; break;    case 'n': u = 10; break;
            case 'r': u = 13; break;    case 't': u = 9; break;
            case 'u': {
                if (!hex4(&u)) return false;
                if (u >= 0xD800 && u < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    const unsigned char* keep = p;
                    unsigned lo = 0;
                    p += 2;
                    if (hex4(&lo) && lo >= 0xDC00 && lo < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    else { p = keep; u = 0xFFFD; }
                } else if (u >= 0xD800 && u < 0xE000) u = 0xFFFD;
                break; }
            default: return false;
            }
            if (out) utf8(out, u);
        }
        return false;
    }
    bool number()
    {
        const unsigned char* s = p;
        while (p < e && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) p++;
        return p > s;
    }
    bool skip(int depth)
    {
        ws();
        if (p >= e || depth > 16) return false;
        if (*p == '"') return str(NULL);
        if (*p == '{' || *p == '[') {
            const bool obj = *p == '{';
            p++; ws();
            if (p < e && *p == (obj ? '}' : ']')) { p++; return true; }
            for (;;) {
                ws();
                if (obj) { if (!str(NULL)) return false; ws(); if (p >= e || *p++ != ':') return false; }
                if (!skip(depth + 1)) return false;
                ws();
                if (p >= e) return false;
                if (*p == ',') { p++; continue; }
                if (*p == (obj ? '}' : ']')) { p++; return true; }
                return false;
            }
        }
        if (lit("true") || lit("false") || lit("null")) return true;
        return number();
    }
};

/* One name as it may be shown: valid UTF-8 only, no control / invisible / direction characters,
 * no leading or trailing spaces, at most ABOUT_NAME_CHARS characters. */
static inline std::string about_clean_name(const std::string& raw)
{
    std::string o;
    int chars = 0;
    const unsigned char* s = (const unsigned char*)raw.data();
    const size_t n = raw.size();
    for (size_t i = 0; i < n && chars < ABOUT_NAME_CHARS;) {
        unsigned c = s[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > n) { i++; continue; }
        if (len > 1) {
            c &= 0xFF >> (len + 1);
            bool good = true;
            for (size_t k = 1; k < len; k++) { if ((s[i + k] & 0xC0) != 0x80) { good = false; break; } c = c << 6 | (s[i + k] & 63); }
            static const unsigned kMin[5] = { 0, 0, 0x80, 0x800, 0x10000 };
            if (!good || c < kMin[len] || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) { i++; continue; }
        }
        const bool drop = c < 0x20 || (c >= 0x7F && c < 0xA0) || c == 0xAD || (c >= 0x200B && c <= 0x200F) ||
                          (c >= 0x2028 && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF ||
                          (c >= 0xFFF9 && c <= 0xFFFF) || (c >= 0xE0000 && c <= 0xE0FFF);
        if (!drop && !(c == ' ' && (o.empty() || o.back() == ' '))) { o.append((const char*)s + i, len); chars++; }
        i += len;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

/* "" unless `raw` is an https:// address of plain URL characters, at most ABOUT_URL_MAX long. */
static inline std::string about_clean_url(const std::string& raw)
{
    if (raw.size() <= 8 || raw.size() > ABOUT_URL_MAX || memcmp(raw.data(), "https://", 8)) return "";
    for (const char ch : raw) {
        const unsigned char c = (unsigned char)ch;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("-._~:/?#[]@!$&()*+,;=%", c));
        if (!ok) return "";
    }
    const unsigned char h = (unsigned char)raw[8];   /* a host name starts it */
    if (!((h >= 'a' && h <= 'z') || (h >= 'A' && h <= 'Z') || (h >= '0' && h <= '9'))) return "";
    return raw;
}

/* Parses `text` (len bytes) into *out. false = the file is refused and *out is left empty:
 * the caller keeps the copy it had. */
static inline bool about_file_parse(const char* text, size_t len, AboutFile* out)
{
    out->supporters.clear(); out->contributors.clear(); out->support_url.clear();
    if (!text || len > (size_t)ABOUT_FILE_MAX) return false;
    AboutParse_ j = { (const unsigned char*)text, (const unsigned char*)text + len };
    if (len >= 3 && j.p[0] == 0xEF && j.p[1] == 0xBB && j.p[2] == 0xBF) j.p += 3;
    AboutFile f;
    bool ok = false;
    j.ws();
    if (j.p < j.e && *j.p == '{') {
        j.p++; j.ws();
        if (j.p < j.e && *j.p == '}') { j.p++; ok = true; }
        else for (;;) {
            std::string key;
            j.ws();
            if (!j.str(&key)) break;
            j.ws();
            if (j.p >= j.e || *j.p++ != ':') break;
            j.ws();
            std::vector<std::string>* list = key == "supporters" ? &f.supporters : key == "contributors" ? &f.contributors : NULL;
            if (list && j.p < j.e && *j.p == '[') {
                list->clear();
                j.p++; j.ws();
                bool good = true;
                if (j.p < j.e && *j.p == ']') j.p++;
                else for (;;) {
                    j.ws();
                    if (j.p < j.e && *j.p == '"') {
                        std::string raw;
                        if (!j.str(&raw)) { good = false; break; }
                        if (list->size() < (size_t)ABOUT_NAMES_MAX) {
                            std::string nm = about_clean_name(raw);
                            if (!nm.empty()) list->push_back(std::move(nm));
                        }
                    } else if (!j.skip(1)) { good = false; break; }   /* not a name: skipped */
                    j.ws();
                    if (j.p >= j.e) { good = false; break; }
                    if (*j.p == ',') { j.p++; continue; }
                    if (*j.p == ']') { j.p++; break; }
                    good = false; break;
                }
                if (!good) break;
            } else if (key == "support_url" && j.p < j.e && *j.p == '"') {
                std::string raw;
                if (!j.str(&raw)) break;
                f.support_url = about_clean_url(raw);
            } else if (!j.skip(1)) break;
            j.ws();
            if (j.p >= j.e) break;
            if (*j.p == ',') { j.p++; continue; }
            if (*j.p == '}') { j.p++; ok = true; }
            break;
        }
    }
    if (ok) { j.ws(); ok = j.p == j.e; }   /* nothing after the object */
    if (!ok) return false;
    *out = std::move(f);
    return true;
}
