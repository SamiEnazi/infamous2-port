/* Engine side: the C library (and the two OS calls) the game module uses, which on Windows is
 * the engine's own -- the module links no C runtime (ps3emu/ps3_module.h PS3_MODULE_LIBC). The
 * real functions where the CRT exports them, so a module calls exactly what the single exe called
 * (sqrt, exp2f, log2f: bit for bit); wrappers only for the stdio calls the UCRT has as inline
 * functions. (slim split, 2026-11-14)
 *
 * EVERY name of PS3_MODULE_LIBC and PS3_MODULE_PGO_RT is bound here, at compile time, to this
 * exe's own C runtime (or to kernel32): the table below is made from those two lists, so a name
 * added to a list without a C runtime function behind it does not compile. There is no lookup by
 * name in another DLL: the release engine has its C runtime inside the exe (/MT), and
 * ucrtbase.dll -- in the process anyway, some system DLL loads it -- is then a SECOND runtime
 * with its own FILE table, file descriptors and errno. The PGO profile writer once got
 * _open_osfhandle / _fdopen / fseek from there and fwrite / fclose from here: an invalid
 * parameter, and the training game was gone (0xC0000409) at its first profile dump.
 * tests/module_libc checks the table. */
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <malloc.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <errno.h>
#include "ps3emu/ps3_module.h"

static int lc_fprintf(FILE* f, const char* fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
static int lc_printf(const char* fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vprintf(fmt, ap); va_end(ap); return r; }
static int lc_snprintf(char* b, size_t n, const char* fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsnprintf(b, n, fmt, ap); va_end(ap); return r; }
static int lc_vsnprintf(char* b, size_t n, const char* fmt, va_list ap) { return vsnprintf(b, n, fmt, ap); }
static int lc_vfprintf(FILE* f, const char* fmt, va_list ap) { return vfprintf(f, fmt, ap); }
static int lc_puts(const char* s) { return puts(s); }
static int lc_fputs(const char* s, FILE* f) { return fputs(s, f); }
static int lc_abs(int v) { return abs(v); }
/* the PGO profile runtime registers its at-exit writer (the UCRT exports _crt_atexit, not atexit):
 * the engine's, so it runs when the game exits, the module still loaded */
static int lc_atexit(void (*f)(void)) { return atexit(f); }

static int lc_open(const char* path, int flags, int mode) { return _open(path, flags, mode); }
/* the names whose function is the wrapper above, and the POSIX spellings (the UCRT's are _name;
 * its plain ones live in oldnames.lib, which nothing else here needs) */
#define fprintf lc_fprintf
#define printf lc_printf
#define snprintf lc_snprintf
#define vsnprintf lc_vsnprintf
#define vfprintf lc_vfprintf
#define puts lc_puts
#define fputs lc_fputs
#define abs lc_abs
#define atexit lc_atexit
#define open lc_open
#define fdopen _fdopen
#define fileno _fileno
#define getpid _getpid
#define strdup _strdup
typedef struct { const char* name; const void* addr; } lc_sym;
#define ROW(n) { #n, (const void*)&n },
static const lc_sym s_libc[] = { PS3_MODULE_LIBC(ROW) PS3_MODULE_PGO_RT(ROW) { 0, 0 } };
#undef ROW

const void* ps3_module_libc_lookup(const char* n)
{
    for (const lc_sym* s = s_libc; s->name; ++s)
        if (!strcmp(s->name, n)) return s->addr;
    return 0;   /* the caller says which name the module wanted (ps3_module_host.c: "engine has no ...") */
}
#else
/* a Linux module has no slots: the dynamic linker binds it to the C library the engine uses */
const void* ps3_module_libc_lookup(const char* n) { (void)n; return 0; }
#endif
