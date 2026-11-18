#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) void* _aligned_malloc(size_t n, size_t align);
__declspec(dllimport) void  _aligned_free(void* p);
#ifdef __cplusplus
}
#endif
