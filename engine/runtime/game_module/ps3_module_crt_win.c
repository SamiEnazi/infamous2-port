/* Windows: what the C runtime would give a DLL, for the game module linked by lld-link with
 * /NODEFAULTLIB (no MSVC, no Windows SDK, no import library at all): the TLS directory, the
 * constructor run (the lifted code's self-registration, .CRT$XCU), __chkstk, _fltused, the /GS
 * cookie, MSVC's on-demand TLS initialisation. The C library the lifted code calls comes from the
 * engine (ps3_module_imports.c). (slim split, 2026-11-14; after work/slim/shim/module_support.c) */
#include <stdint.h>

/* ---- TLS: the module's thread-locals (g_trampoline_fn) in its own TLS block ----------------- */
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLZ", long, read)
__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;
typedef void (__stdcall *tls_cb)(void*, unsigned long, void*);
__declspec(allocate(".CRT$XLA")) tls_cb __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) tls_cb __xl_z = 0;
unsigned long _tls_index = 0;
typedef struct { uint64_t start, end, index, callbacks; uint32_t zero_fill, characteristics; } tls_dir64;
const tls_dir64 _tls_used = { (uint64_t)&_tls_start, (uint64_t)&_tls_end, (uint64_t)&_tls_index,
                              (uint64_t)(&__xl_a + 1), 0, 0 };

/* MSVC's on-demand TLS init: a C++ thread_local without constinit (the firmware lift's
 * g_trampoline_fn declaration) tests __tls_guard and calls this. The module has no dynamic TLS
 * initialiser (g_trampoline_fn is constant-initialised), so this thread is ready. */
__declspec(thread) unsigned char __tls_guard = 0;
void __dyn_tls_on_demand_init(void) { __tls_guard = 1; }

/* ---- constructors: C++ dynamic init and __attribute__((constructor)) both land in .CRT$XCU --- */
typedef void (*pvfv)(void);
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)
__declspec(allocate(".CRT$XCA")) const pvfv __xc_a = 0;
__declspec(allocate(".CRT$XCZ")) const pvfv __xc_z = 0;

/* ---- floating point marker, /GS cookie -------------------------------------------------- */
int _fltused = 0x9875;
uint64_t __security_cookie = 0x00002B992DDFA232ull;
void __security_check_cookie(uint64_t c) { if (c != __security_cookie) __builtin_trap(); }

static int s_ran;
void ps3_module_run_ctors(void)   /* the engine calls it once its imports are bound (init) */
{
    if (s_ran) return;
    s_ran = 1;
    /* a cookie of this run before any /GS function has a frame */
    __security_cookie ^= (uint64_t)__builtin_ia32_rdtsc() ^ ((uint64_t)(uintptr_t)&s_ran << 16);
    __security_cookie &= 0x0000FFFFFFFFFFFFull;
    for (const pvfv* p = &__xc_a + 1; p < &__xc_z; ++p)
        if (*p) (*p)();
}

/* The Windows loader sets a DLL's thread-locals up (its TLS index, every thread's copy) only for an
 * image that imports something: a DLL with no import directory loads fine, but each
 * __declspec(thread) access then reads the EXE's TLS block (index 0) -- found 2026-11-14 on Windows
 * 11 (build 26300): the SPU natives' per-thread buffers read the engine's thread-locals. So the
 * module imports one real function, kernel32's GetCurrentThreadId, through a one-entry import
 * library tools/build_module.py makes with lld's own librarian (lld-link /lib /def:, no SDK). */
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
static volatile unsigned long s_load_tid;

int __stdcall ps3_module_dll_entry(void* self, unsigned long reason, void* reserved)
{
    (void)self; (void)reserved;
    if (reason == 1) s_load_tid = GetCurrentThreadId();   /* (the import above; nothing else at load:
                                                           * the engine binds the module, then calls init) */
    return 1;
}

/* ---- __chkstk: rax = frame size; touch every page below rsp (the stack grows page by page) -- */
__asm__(
    ".att_syntax prefix\n.text\n.globl __chkstk\n.p2align 4\n__chkstk:\n"
    "  push %rcx\n  push %rax\n"
    "  cmp $0x1000, %rax\n  lea 24(%rsp), %rcx\n  jb 1f\n"
    "2:\n  sub $0x1000, %rcx\n  test %rcx, (%rcx)\n  sub $0x1000, %rax\n  cmp $0x1000, %rax\n  ja 2b\n"
    "1:\n  sub %rax, %rcx\n  test %rcx, (%rcx)\n"
    "  pop %rax\n  pop %rcx\n  ret\n");
