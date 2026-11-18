/* [about] vk_overlay_about_file.h: supporters.json, text from the network.
 *   clang-cl /std:c++17 /EHsc /W4 libs/video/tests/test_about_file.cpp /Fe:test_about_file.exe
 *   (or: c++ -std=c++17 -Wall libs/video/tests/test_about_file.cpp -o test_about_file)
 *   test_about_file [file]   with a file: parses it and prints what the menu would show.
 * Prints one line per failed check and "N checks, M failed"; exit code = failures. */
#include "../vk_overlay_about_file.h"

#include <stdio.h>
#include <string.h>
#include <string>

static int g_n, g_bad;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static bool parse(const std::string& t, AboutFile* f) { return about_file_parse(t.data(), t.size(), f); }

int main(int argc, char** argv)
{
    AboutFile f;
    if (argc > 1) {
        std::string t;
        FILE* fp = fopen(argv[1], "rb");
        if (!fp) { printf("cannot open %s\n", argv[1]); return 2; }
        char buf[8192]; size_t n;
        while (t.size() <= (size_t)ABOUT_FILE_MAX && (n = fread(buf, 1, sizeof buf, fp)) > 0) t.append(buf, n);
        fclose(fp);
        const bool ok = parse(t, &f);
        printf("%s: %s, %d supporter(s), %d contributor(s), support_url=[%s]\n", argv[1], ok ? "taken" : "REFUSED",
               (int)f.supporters.size(), (int)f.contributors.size(), f.support_url.c_str());
        for (size_t i = 0; i < f.supporters.size() && i < 8; i++) printf("  supporter [%s]\n", f.supporters[i].c_str());
        for (size_t i = 0; i < f.contributors.size() && i < 8; i++) printf("  contributor [%s]\n", f.contributors[i].c_str());
        return ok ? 0 : 1;
    }

    /* the file as shipped */
    CHECK(parse("{\"v\":1, \"supporters\":[], \"contributors\":[\"ANewJanuary\"], \"support_url\":\"https://paypal.me/samiEnazi\"}\n", &f));
    CHECK(f.supporters.empty());
    CHECK(f.contributors.size() == 1 && f.contributors[0] == "ANewJanuary");
    CHECK(f.support_url == "https://paypal.me/samiEnazi");

    /* a good file: order kept, a BOM, CRLF, unknown keys of any shape, UTF-8 and \u escapes */
    CHECK(parse("\xEF\xBB\xBF{\r\n \"v\": 1,\r\n \"later\": {\"a\":[1,2,{\"b\":null}], \"c\":true, \"d\":-1.5e3},\r\n"
                " \"supporters\": [\"Zed\", \"Al\\u00e9 \\ud83d\\ude00\", \"\xD8\xB3\xD8\xA7\xD9\x85\xD9\x8A\"],\r\n \"support_url\": \"\"\r\n}\r\n", &f));
    CHECK(f.supporters.size() == 3);
    CHECK(f.supporters[0] == "Zed");
    CHECK(f.supporters[1] == "Al\xC3\xA9 \xF0\x9F\x98\x80");
    CHECK(f.supporters[2] == "\xD8\xB3\xD8\xA7\xD9\x85\xD9\x8A");
    CHECK(f.contributors.empty() && f.support_url.empty());

    /* empty */
    CHECK(parse("{}", &f) && f.supporters.empty() && f.support_url.empty());
    CHECK(parse("{\"v\":1,\"supporters\":[]}", &f) && f.supporters.empty());

    /* refused whole: not an object, cut, trailing text, too deep, too big, empty */
    CHECK(!parse("", &f));
    CHECK(!parse("[\"a\"]", &f));
    CHECK(!parse("<html>404</html>", &f));
    CHECK(!parse("{\"supporters\":[\"a\",", &f));
    CHECK(!parse("{\"supporters\":[\"a\"]", &f));
    CHECK(!parse("{\"supporters\":[\"a\"]} x", &f));
    CHECK(!parse("{\"supporters\":[\"a\\q\"]}", &f));
    CHECK(!parse("{\"supporters\":[\"a\" \"b\"]}", &f));
    CHECK(f.supporters.empty());   /* a refused file leaves nothing behind */
    { std::string deep = "{\"x\":";
      for (int i = 0; i < 40; i++) deep += "[";
      for (int i = 0; i < 40; i++) deep += "]";
      CHECK(!parse(deep + "}", &f)); }
    { std::string big = "{\"supporters\":[\"a\"";
      while (big.size() < (size_t)ABOUT_FILE_MAX) big += ",\"abcdefgh\"";
      CHECK(!parse(big + "]}", &f)); }

    /* at most 500 names */
    { std::string many = "{\"supporters\":[\"n0\"";
      for (int i = 1; i < 2000; i++) many += ",\"n" + std::to_string(i) + "\"";
      CHECK(parse(many + "]}", &f));
      CHECK(f.supporters.size() == (size_t)ABOUT_NAMES_MAX);
      CHECK(f.supporters[499] == "n499"); }

    /* names: 40 characters (not bytes), control characters and newlines dropped, format text stays text */
    CHECK(parse("{\"supporters\":[\"0123456789012345678901234567890123456789XYZ\"]}", &f));
    CHECK(f.supporters[0] == "0123456789012345678901234567890123456789");
    { std::string s = "{\"supporters\":[\"";
      for (int i = 0; i < 60; i++) s += "\xC3\xA9";
      CHECK(parse(s + "\"]}", &f));
      CHECK(f.supporters[0].size() == 80); }
    CHECK(parse("{\"supporters\":[\"a\\nb\\r\\tc\\u0000d\\u001b[31me\\u007f\", \"  spaced   out  \", \"\\n\\t\", \"\", 5, null, [\"x\"], {\"k\":\"v\"}, \"last\"]}", &f));
    CHECK(f.supporters.size() == 3);
    CHECK(f.supporters[0] == "abcd[31me");
    CHECK(f.supporters[1] == "spaced out");
    CHECK(f.supporters[2] == "last");
    CHECK(parse("{\"supporters\":[\"%s%s%s%n %d %x\", \"100%\"]}", &f));
    CHECK(f.supporters[0] == "%s%s%s%n %d %x" && f.supporters[1] == "100%");
    /* raw control bytes, broken UTF-8, direction overrides, zero-width, a lone surrogate */
    CHECK(parse(std::string("{\"supporters\":[\"a\x01\x1F") + "b\xFF\xC3z\xE2\x80\xAE" "c\xE2\x80\x8B" "d\\ud800e\xC0\x80\"]}", &f));
    CHECK(f.supporters[0] == "abzcde");
    /* "supporters" that is not a list: no names, the file still taken */
    CHECK(parse("{\"supporters\":\"x\",\"contributors\":{\"a\":1}}", &f) && f.supporters.empty() && f.contributors.empty());

    /* support_url: https only, 200 characters, plain URL characters */
    CHECK(parse("{\"support_url\":\"http://paypal.me/x\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"javascript:alert(1)\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"file:///C:/Windows/System32/calc.exe\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"C:\\\\Windows\\\\System32\\\\calc.exe\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"HTTPS://paypal.me/x\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"https://\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"https://-x\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"https://a.b/x y\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"https://a.b/\\\" & calc\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"https://a.b/%s%n\\n\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":\"https://a.b/\xC3\xA9\"}", &f) && f.support_url.empty());
    CHECK(parse("{\"support_url\":5}", &f) && f.support_url.empty());
    { std::string u = "https://a.b/";
      while (u.size() < 200) u += "x";
      CHECK(parse("{\"support_url\":\"" + u + "\"}", &f) && f.support_url == u);
      CHECK(parse("{\"support_url\":\"" + u + "x\"}", &f) && f.support_url.empty()); }
    CHECK(parse("{\"support_url\":\"https://github.com/sponsors/SamiEnazi?o=1&x=%20#top\"}", &f) && !f.support_url.empty());

    printf("%d checks, %d failed\n", g_n, g_bad);
    return g_bad;
}
