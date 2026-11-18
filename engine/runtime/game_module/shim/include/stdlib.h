/* The game module's <stdlib.h> (slim split): getenv / atoi / strtoul (switches read once), abort,
 * the allocator. dllimport as in the UCRT's header (the engine's own C runtime answers them). */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) char* getenv(const char* name);
__declspec(dllimport) int atoi(const char* s);
__declspec(dllimport) long atol(const char* s);
__declspec(dllimport) unsigned long strtoul(const char* s, char** end, int base);
__declspec(dllimport) long strtol(const char* s, char** end, int base);
__declspec(dllimport) unsigned long long strtoull(const char* s, char** end, int base);
__declspec(dllimport) __declspec(noreturn) void abort(void);
__declspec(dllimport) __declspec(noreturn) void exit(int);
__declspec(dllimport) void* malloc(size_t n);
__declspec(dllimport) void* calloc(size_t n, size_t sz);
__declspec(dllimport) void* realloc(void* p, size_t n);
__declspec(dllimport) void  free(void* p);
int abs(int);
#ifdef __cplusplus
}
#endif
