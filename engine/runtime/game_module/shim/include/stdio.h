/* The game module's <stdio.h> (slim split): the lifted code only prints diagnostics to stderr.
 * FILE is opaque; the engine hands over its own C runtime's functions, so the module and the
 * engine share one stderr. dllimport where the UCRT's header has it (the same call the single exe
 * made: call [__imp_x]); the UCRT's inline printf family are plain functions here. */
#pragma once
#include <stddef.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _iobuf { void* _Placeholder; } FILE;
__declspec(dllimport) FILE* __acrt_iob_func(unsigned);
#define stdin  (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
#define EOF (-1)
int    fprintf(FILE* f, const char* fmt, ...);
int    printf(const char* fmt, ...);
int    snprintf(char* b, size_t n, const char* fmt, ...);
int    vsnprintf(char* b, size_t n, const char* fmt, va_list ap);
int    vfprintf(FILE* f, const char* fmt, va_list ap);
__declspec(dllimport) int    fflush(FILE* f);
__declspec(dllimport) size_t fwrite(const void* p, size_t sz, size_t n, FILE* f);
__declspec(dllimport) int    fputs(const char* s, FILE* f);
__declspec(dllimport) int    fputc(int c, FILE* f);
__declspec(dllimport) int    puts(const char* s);
__declspec(dllimport) FILE*  fopen(const char* name, const char* mode);
__declspec(dllimport) int    fclose(FILE* f);
#ifdef __cplusplus
}
#endif
