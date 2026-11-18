/* slim prototype: the C library names the lifted code uses, without the Windows SDK. The game module
   gets them from the engine (it exports them) or as compiler builtins. */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void*  memcpy(void* __restrict d, const void* __restrict s, size_t n);
void*  memmove(void* d, const void* s, size_t n);
void*  memset(void* d, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strncpy(char* d, const char* s, size_t n);
char*  strchr(const char* s, int c);
#ifdef __cplusplus
}
#endif
