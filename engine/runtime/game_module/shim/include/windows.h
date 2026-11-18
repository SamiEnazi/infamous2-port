/* The game module's <windows.h> (slim split): only what the module's sources use -- spu_dma.h's
 * demand-commit of guest pages (VirtualQuery / VirtualAlloc). The calls go through the module's
 * import slots to the engine's own kernel32 (runtime/game_module/ps3_module_imports.c). */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef size_t SIZE_T;
typedef struct _MEMORY_BASIC_INFORMATION {
    void* BaseAddress; void* AllocationBase; DWORD AllocationProtect; unsigned short PartitionId;
    SIZE_T RegionSize; DWORD State; DWORD Protect; DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
#define MEM_COMMIT     0x00001000
#define MEM_RESERVE    0x00002000
#define PAGE_NOACCESS  0x01
#define PAGE_READONLY  0x02
#define PAGE_READWRITE 0x04
#define PAGE_GUARD     0x100
#define WINAPI __stdcall
__declspec(dllimport) LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
__declspec(dllimport) SIZE_T WINAPI VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION info, SIZE_T len);
#ifdef __cplusplus
}
#endif
