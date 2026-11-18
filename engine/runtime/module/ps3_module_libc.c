/* Engine side: the C library (and the two OS calls) the game module uses, which on Windows is
 * the engine's own -- the module links no C runtime (ps3emu/ps3_module.h PS3_MODULE_LIBC). The
 * real functions where the CRT exports them, so a module calls exactly what the single exe called
 * (sqrt, exp2f, log2f: bit for bit); wrappers only for the stdio calls the UCRT has as inline
 * functions. (slim split, 2026-11-14) */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <malloc.h>
#endif

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

const void* ps3_module_libc_lookup(const char* n)
{
#define R(name, addr) if (!strcmp(n, name)) return (const void*)(addr);
    R("memcpy", &memcpy) R("memmove", &memmove) R("memset", &memset) R("memcmp", &memcmp)
    R("strcmp", &strcmp) R("strncmp", &strncmp) R("strlen", &strlen) R("strchr", &strchr)
    R("strncpy", &strncpy) R("getenv", &getenv) R("atoi", &atoi) R("strtol", &strtol)
    R("strtoul", &strtoul) R("strtoull", &strtoull) R("malloc", &malloc) R("calloc", &calloc)
    R("realloc", &realloc) R("free", &free) R("abort", &abort) R("exit", &exit) R("abs", &lc_abs)
    R("fprintf", &lc_fprintf) R("printf", &lc_printf) R("snprintf", &lc_snprintf)
    R("vsnprintf", &lc_vsnprintf) R("vfprintf", &lc_vfprintf) R("fwrite", &fwrite)
    R("fputs", &lc_fputs) R("fputc", &fputc) R("puts", &lc_puts) R("fflush", &fflush)
    R("fopen", &fopen) R("fclose", &fclose)
    R("sqrt", &sqrt) R("sqrtf", &sqrtf) R("exp2f", &exp2f) R("log2f", &log2f) R("exp2", &exp2)
    R("log2", &log2) R("expf", &expf) R("exp", &exp) R("ldexp", &ldexp) R("ldexpf", &ldexpf)
    R("frexp", &frexp) R("frexpf", &frexpf) R("floor", &floor) R("floorf", &floorf)
    R("ceil", &ceil) R("ceilf", &ceilf) R("trunc", &trunc) R("truncf", &truncf)
    R("nearbyint", &nearbyint) R("nearbyintf", &nearbyintf) R("rint", &rint) R("rintf", &rintf)
    R("fma", &fma) R("fmaf", &fmaf) R("fmin", &fmin) R("fmax", &fmax) R("fminf", &fminf)
    R("fmaxf", &fmaxf) R("copysign", &copysign) R("copysignf", &copysignf) R("fabs", &fabs)
    R("fabsf", &fabsf) R("atexit", &lc_atexit)
#if defined(_WIN32)
    R("__acrt_iob_func", &__acrt_iob_func) R("_aligned_malloc", &_aligned_malloc)
    R("_aligned_free", &_aligned_free) R("VirtualAlloc", &VirtualAlloc) R("VirtualQuery", &VirtualQuery)
#endif
#undef R
#if defined(_WIN32)
    /* anything else (the PGO runtime's needs: ps3emu/ps3_module.h PS3_MODULE_PGO_RT): the engine's
     * own C runtime DLLs and kernel32, by name, the POSIX spelling as the UCRT's _name */
    static const char* const dlls[] = { "ucrtbase.dll", "vcruntime140.dll", "kernel32.dll", 0 };
    for (int pass = 0; pass < 2; ++pass) {
        char alt[128];
        const char* want = n;
        if (pass) { if (n[0] == '_' || strlen(n) + 2 > sizeof alt) break; alt[0] = '_'; strcpy(alt + 1, n); want = alt; }
        for (int i = 0; dlls[i]; ++i) {
            HMODULE h = GetModuleHandleA(dlls[i]);
            FARPROC f = h ? GetProcAddress(h, want) : 0;
            if (f) return (const void*)f;
        }
    }
#endif
    return 0;
}
