/* [update] vk_overlay_update_status.h: the status file tools/update_check.ps1 writes.
 *   clang-cl /std:c++17 /EHsc /W4 libs/video/tests/test_update_status.cpp /Fe:test_update_status.exe
 *   (or: c++ -std=c++17 -Wall libs/video/tests/test_update_status.cpp -o test_update_status)
 * Prints one line per failed check and "N checks, M failed"; exit code = failures. */
#include "../vk_overlay_update_status.h"

#include <stdio.h>
#include <string.h>
#include <string>

static int g_n, g_bad;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int parse(const std::string& t, UpdateStatus* s) { return update_status_parse(t.data(), t.size(), s); }

int main()
{
    UpdateStatus s;

    /* what update_check.ps1 writes (UTF-8 with a BOM and CRLF, as Windows PowerShell may) */
    std::string a = "\xEF\xBB\xBFstate=available\r\ninstalled=v0.4.2\r\nlatest=v0.4.3\r\ntitle=v0.4.3: an updater\r\n"
                    "published=2026-11-14\r\nurl=https://github.com/SamiEnazi/infamous2-port/releases/tag/v0.4.3\r\n"
                    "checked=2026-11-12 14:03\r\nmessage=\r\nnotes=What's new\r\nnotes=\r\nnotes=- the game tells you \xE2\x80\x94 F2\r\n"
                    "notes=\r\nnotes=\r\n";
    CHECK(parse(a, &s) == 13);
    CHECK(!strcmp(s.state, "available"));
    CHECK(!strcmp(s.installed, "v0.4.2"));
    CHECK(!strcmp(s.latest, "v0.4.3"));
    CHECK(!strcmp(s.title, "v0.4.3: an updater"));
    CHECK(!strcmp(s.published, "2026-11-14"));
    CHECK(!strcmp(s.url, "https://github.com/SamiEnazi/infamous2-port/releases/tag/v0.4.3"));
    CHECK(!strcmp(s.checked, "2026-11-12 14:03"));
    CHECK(s.message[0] == 0);
    CHECK(s.notes == "What's new\n\n- the game tells you \xE2\x80\x94 F2");   /* trailing blank lines dropped */

    /* LF only, no BOM, a value with '=' in it, unknown keys, comments, a line without '=' */
    std::string b = "# made by update_check.ps1\nstate=error\nfuture=1\nmessage=a=b, c\nno equals sign here\n\nnotes=x";
    CHECK(parse(b, &s) == 3);
    CHECK(!strcmp(s.state, "error"));
    CHECK(!strcmp(s.message, "a=b, c"));
    CHECK(s.latest[0] == 0 && s.installed[0] == 0);
    CHECK(s.notes == "x");

    /* a parse starts from nothing: no field of the earlier file stays */
    CHECK(parse("state=current\n", &s) == 1);
    CHECK(!strcmp(s.state, "current") && s.message[0] == 0 && s.notes.empty());

    /* an empty or truncated file: no state, so the reader waits for a whole one */
    CHECK(parse("", &s) == 0 && s.state[0] == 0);
    CHECK(parse("stat", &s) == 0 && s.state[0] == 0);

    /* too long a value is cut at its field, never inside a UTF-8 character */
    std::string longv = "latest=";
    for (int i = 0; i < 40; i++) longv += "\xC3\xA9";   /* 80 bytes of "e acute" */
    CHECK(parse(longv + "\n", &s) == 1);
    CHECK(strlen(s.latest) <= sizeof s.latest - 1);
    CHECK(strlen(s.latest) % 2 == 0);                   /* whole characters only */
    CHECK(((unsigned char)s.latest[strlen(s.latest) - 1] & 0xC0) == 0x80);   /* ends on a continuation byte = a whole char */

    /* the notes stop at 16 KB */
    std::string many;
    for (int i = 0; i < 2000; i++) many += "notes=0123456789abcdef\n";
    parse(many, &s);
    CHECK(s.notes.size() <= (16u << 10));
    CHECK(s.notes.size() > (15u << 10));

    /* no 0 at the end of the buffer: only len bytes are read */
    const char raw[] = { 's', 't', 'a', 't', 'e', '=', 'o', 'f', 'f', 'X', 'Y' };
    CHECK(update_status_parse(raw, 9, &s) == 1 && !strcmp(s.state, "off"));

    printf("%d checks, %d failed\n", g_n, g_bad);
    return g_bad;
}
