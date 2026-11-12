/* [update] What tools/update_check.ps1 found, as tools/play.ps1 hands it to the game:
 * PS3_UPDATE_DIR/status.txt, UTF-8 "key=value" lines (vk_overlay_update.inc reads it).
 *
 *   state=available|current|unknown|off|error   the answer (error: the last check failed)
 *   installed=v0.4.2        this copy's version, as the player sees it
 *   latest=v0.4.3           the newest release
 *   title=..., published=2026-11-14, url=https://github.com/..., checked=2026-11-12 14:03
 *   message=...             one line for the player ("GitHub could not be reached ...")
 *   notes=...               the release notes, one line each, in order ("notes=" alone: a blank line)
 *
 * Unknown keys, blank lines and "#" lines are skipped; a value is cut at its field's size;
 * the notes stop at 16 KB. Pure C++ (no ImGui, no files): libs/video/tests/test_update_status.cpp. */
#pragma once

#include <stdio.h>
#include <string.h>
#include <string>

struct UpdateStatus {
    char state[16];
    char installed[96];
    char latest[64];
    char title[200];
    char published[32];
    char url[300];
    char checked[48];
    char message[400];
    std::string notes;
};

static inline void update_status_clear(UpdateStatus* s)
{
    s->state[0] = s->installed[0] = s->latest[0] = s->title[0] = 0;
    s->published[0] = s->url[0] = s->checked[0] = s->message[0] = 0;
    s->notes.clear();
}

/* Parses `text` (len bytes, need not end in 0) into *s. Returns the number of keys taken. */
static inline int update_status_parse(const char* text, size_t len, UpdateStatus* s)
{
    update_status_clear(s);
    static const size_t kNotesMax = 16u << 10;
    int n = 0;
    size_t i = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        i = 3;   /* a BOM (Windows PowerShell writes one when told "utf8") */
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        size_t end = e;
        if (end > i && text[end - 1] == '\r') end--;
        const char* ln = text + i;
        const size_t ll = end - i;
        i = e + 1;
        if (!ll || ln[0] == '#') continue;
        const char* eq = (const char*)memchr(ln, '=', ll);
        if (!eq) continue;
        const size_t kl = (size_t)(eq - ln);
        const char* v = eq + 1;
        const size_t vl = ll - kl - 1;
        struct { const char* k; char* dst; size_t cap; } f[] = {
            { "state", s->state, sizeof s->state },       { "installed", s->installed, sizeof s->installed },
            { "latest", s->latest, sizeof s->latest },    { "title", s->title, sizeof s->title },
            { "published", s->published, sizeof s->published }, { "url", s->url, sizeof s->url },
            { "checked", s->checked, sizeof s->checked }, { "message", s->message, sizeof s->message },
        };
        if (kl == 5 && !memcmp(ln, "notes", 5)) {
            if (s->notes.size() + vl + 1 <= kNotesMax) { s->notes.append(v, vl); s->notes.push_back('\n'); }
            n++;
            continue;
        }
        for (auto& x : f) {
            if (strlen(x.k) != kl || memcmp(ln, x.k, kl)) continue;
            size_t c = vl < x.cap - 1 ? vl : x.cap - 1;
            /* never cut inside a UTF-8 character */
            while (c > 0 && c < vl && ((unsigned char)v[c] & 0xC0) == 0x80) c--;
            memcpy(x.dst, v, c);
            x.dst[c] = 0;
            n++;
            break;
        }
    }
    while (!s->notes.empty() && (s->notes.back() == '\n' || s->notes.back() == ' ')) s->notes.pop_back();
    return n;
}
