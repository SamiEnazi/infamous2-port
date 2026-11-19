/* Every C library and OS name a game module may import (ps3emu/ps3_module.h PS3_MODULE_LIBC,
 * PS3_MODULE_PGO_RT) must come from the ENGINE's own C runtime -- with a static runtime that is
 * code inside the exe -- or from Windows itself, never from another C runtime in the process
 * (ucrtbase.dll, loaded by some system DLL): a FILE*, a file descriptor or errno handed from one
 * runtime to the other is an invalid parameter, and the process fast-fails (0xC0000409).
 * Then the calls the PGO profile writer makes, in its order, through the bound names.
 *   libc_bind_test.exe <scratch file>      exit 0 = fine */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "ps3emu/ps3_module.h"

const void* ps3_module_libc_lookup(const char* name);

static const char* const s_names[] = {
#define ROW(n) #n,
    PS3_MODULE_LIBC(ROW) PS3_MODULE_PGO_RT(ROW)
#undef ROW
    0 };

static const char* base_name(const char* p)
{
    const char* b = p;
    for (; *p; ++p) if (*p == 92 || *p == 47) b = p + 1;   /* a backslash or a slash */
    return b;
}
/* a C runtime that is not the exe's own */
static int is_foreign_crt(const char* dll)
{
    char l[64]; size_t i = 0;
    for (; dll[i] && i + 1 < sizeof l; ++i) l[i] = (char)tolower((unsigned char)dll[i]);
    l[i] = 0;
    return !strncmp(l, "ucrtbase", 8) || !strncmp(l, "vcruntime", 9) || !strncmp(l, "msvcr", 5)
        || !strncmp(l, "msvcp", 5) || !strncmp(l, "api-ms-win-crt", 14);
}
#define FN(type, name) ((type)ps3_module_libc_lookup(name))

int main(int argc, char** argv)
{
    int bad = 0, in_exe = 0, in_os = 0;
    LoadLibraryA("ucrtbase.dll");   /* as in the game, where a system DLL has it loaded */
    HMODULE self = GetModuleHandleA(0);
    /* the runtime the exe was linked with: static = fopen is code of the exe, not an import */
    HMODULE crt = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void*)&fopen, &crt);
    int static_crt = crt == self;
    for (int i = 0; s_names[i]; ++i) {
        const void* a = ps3_module_libc_lookup(s_names[i]);
        if (!a) { printf("MISSING  %s\n", s_names[i]); ++bad; continue; }
        HMODULE h = 0; char path[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &h);
        if (h) GetModuleFileNameA(h, path, sizeof path);
        if (h == self) { ++in_exe; continue; }
        if (static_crt && is_foreign_crt(base_name(path))) { printf("FOREIGN  %s -> %s\n", s_names[i], base_name(path)); ++bad; continue; }
        ++in_os;
    }
    printf("%d names: %d in the exe, %d in Windows DLLs, %d wrong (%s C runtime)\n", in_exe + in_os + bad, in_exe, in_os, bad, static_crt ? "static" : "DLL");
    /* a name nobody listed has no fallback */
    if (ps3_module_libc_lookup("_wfopen") || ps3_module_libc_lookup("fgetc")) { printf("an unlisted name was found by a fallback\n"); ++bad; }
    if (bad || argc < 2) return bad ? 1 : 0;
    /* the profile writer's calls (compiler-rt lprofOpenFileEx, then its file writer) */
    fflush(stdout);
    HANDLE hf = FN(HANDLE (*)(LPCSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE), "CreateFileA")
        (argv[1], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (hf == INVALID_HANDLE_VALUE) { printf("CreateFileA failed\n"); return 1; }
    int fd = FN(int (*)(intptr_t, int), "_open_osfhandle")((intptr_t)hf, 0);
    FILE* f = FN(FILE* (*)(int, const char*), "_fdopen")(fd, "r+b");
    if (fd < 0 || !f) { printf("_open_osfhandle / _fdopen failed\n"); return 1; }
    static const char data[4096] = "profile";
    size_t w = FN(size_t (*)(const void*, size_t, size_t, FILE*), "fwrite")(data, 1, sizeof data, f);
    int sk = FN(int (*)(FILE*, long, int), "fseek")(f, 0, SEEK_END);
    long at = FN(long (*)(FILE*), "ftell")(f);
    int fn = FN(int (*)(FILE*), "fileno")(f);
    int eo = FN(int (*)(FILE*), "feof")(f);
    FN(int (*)(FILE*), "fflush")(f);
    FN(int (*)(FILE*, long, int), "fseek")(f, 0, SEEK_SET);
    char back[8] = { 0 };
    size_t r = FN(size_t (*)(void*, size_t, size_t, FILE*), "fread")(back, 1, 7, f);
    int cl = FN(int (*)(FILE*), "fclose")(f);
    char* dup = FN(char* (*)(const char*), "strdup")("x");
    FN(void (*)(void*), "free")(dup);
    *FN(int* (*)(void), "_errno")() = 0;
    int ok = w == sizeof data && !sk && at == (long)sizeof data && fn == fd && !eo && r == 7 && !memcmp(back, "profile", 7) && !cl;
    printf("profile writer calls: %s\n", ok ? "OK" : "WRONG RESULT");
    return ok ? 0 : 1;
}
