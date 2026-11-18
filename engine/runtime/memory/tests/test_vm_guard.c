/* test_vm_guard: the PPU_LEAN_MEM guard pages (runtime/memory/vm_guard.c) -- the decoder on fixed
 * encodings, then real faults: stores into the null page dropped, raw-SPU window loads and stores
 * finished through the callbacks (plain mov, movbe, movzx/movsx, mov-immediate, 8/16/32/64-bit,
 * ah..bh), an instruction the guard cannot finish opening its page.
 * Windows; build from engine/:
 *   clang-cl /O2 /arch:AVX2 -Iruntime/memory runtime/memory/tests/test_vm_guard.c runtime/memory/vm_guard.c
 * Exit 0 = every check passed. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "vm_guard.h"

static int g_fail, g_n;
#define CHECK(c, ...) do { g_n++; if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the raw-SPU register file the callbacks stand for: big-endian bytes, one window's worth */
static uint8_t g_regs[6u << 20];
static uint32_t g_last_st_ea; static uint64_t g_last_st_val; static unsigned g_last_st_n; static int g_loads;
static int t_load(uint32_t ea, unsigned n, uint64_t* v)
{
    const uint8_t* p = g_regs + (ea - 0xE0000000u);
    uint64_t x = 0;
    for (unsigned i = 0; i < n; i++) x = (x << 8) | p[i];
    *v = x; g_loads++;
    return 1;
}
static int t_store(uint32_t ea, unsigned n, uint64_t v)
{
    uint8_t* p = g_regs + (ea - 0xE0000000u);
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
    g_last_st_ea = ea; g_last_st_val = v; g_last_st_n = n;
    return 1;
}

static uint8_t* g_vb;
static LONG WINAPI commit_veh(EXCEPTION_POINTERS* ep)   /* = the port's vm_commit_veh */
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR f = ep->ExceptionRecord->ExceptionInformation[1];
        if (g_vb && f >= (ULONG_PTR)g_vb && f < (ULONG_PTR)g_vb + 0x100010000ull &&
            VirtualAlloc((void*)(f & ~(ULONG_PTR)0xFFFF), 0x10000, MEM_COMMIT, PAGE_READWRITE))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void dec(const char* name, const uint8_t* code, int len, int op, int width, int reg, int high8)
{
    vmg_insn d;
    int ok = vm_guard_decode(code, &d);
    CHECK(ok && d.len == len && d.op == op && (!width || d.width == width) && (reg < 0 || d.reg == reg) &&
          d.high8 == high8, "%s: ok %d len %d op %d width %d reg %d high8 %d", name, ok, d.len, d.op, d.width, d.reg, d.high8);
}

int main(void)
{
    /* ---- the decoder ---- */
    { static const uint8_t c[] = { 0x89, 0x08 };                        dec("mov [rax],ecx", c, 2, VMG_STORE, 4, 1, 0); }
    { static const uint8_t c[] = { 0x48, 0x89, 0x0C, 0x08 };            dec("mov [rax+rcx],rcx", c, 4, VMG_STORE, 8, 1, 0); }
    { static const uint8_t c[] = { 0x66, 0x89, 0x08 };                  dec("mov [rax],cx", c, 3, VMG_STORE, 2, 1, 0); }
    { static const uint8_t c[] = { 0x88, 0x24, 0x08 };                  dec("mov [rax+rcx],ah", c, 3, VMG_STORE, 1, 0, 1); }
    { static const uint8_t c[] = { 0x40, 0x88, 0x34, 0x08 };            dec("mov [rax+rcx],sil", c, 4, VMG_STORE, 1, 6, 0); }
    { static const uint8_t c[] = { 0x0F, 0x38, 0xF1, 0x04, 0x0B };      dec("movbe [rbx+rcx],eax", c, 5, VMG_STORE, 4, 0, 0); }
    { static const uint8_t c[] = { 0x4C, 0x0F, 0x38, 0xF0, 0x44, 0x0B, 0x10 }; dec("movbe r8,[rbx+rcx+16]", c, 7, VMG_LOAD, 8, 8, 0); }
    { static const uint8_t c[] = { 0xC7, 0x04, 0x08, 0x78, 0x56, 0x34, 0x12 }; dec("mov dword [rax+rcx],imm", c, 7, VMG_STORE_IMM, 4, -1, 0); }
    { static const uint8_t c[] = { 0xC6, 0x00, 0x05 };                  dec("mov byte [rax],5", c, 3, VMG_STORE_IMM, 1, -1, 0); }
    { static const uint8_t c[] = { 0x0F, 0xB6, 0x04, 0x08 };            dec("movzx eax,byte [rax+rcx]", c, 4, VMG_LOAD, 1, 0, 0); }
    { static const uint8_t c[] = { 0x44, 0x8B, 0x84, 0x08, 0x00, 0x01, 0x00, 0x00 }; dec("mov r8d,[rax+rcx+256]", c, 8, VMG_LOAD, 4, 8, 0); }
    { static const uint8_t c[] = { 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00 }; dec("mov eax,[rip+16]", c, 6, VMG_LOAD, 4, 0, 0); }
    { static const uint8_t c[] = { 0xC5, 0xFA, 0x7F, 0x04, 0x08 };      dec("vmovdqu [rax+rcx],xmm0", c, 5, VMG_WRITE_OTHER, 0, -1, 0); }
    { static const uint8_t c[] = { 0xF3, 0x0F, 0x7F, 0x04, 0x08 };      dec("movdqu [rax+rcx],xmm0", c, 5, VMG_WRITE_OTHER, 0, -1, 0); }
    { static const uint8_t c[] = { 0x83, 0x00, 0x01 };                  dec("add dword [rax],1", c, 3, VMG_WRITE_OTHER, 0, -1, 0); }
    { static const uint8_t c[] = { 0x81, 0x7C, 0x24, 0x08, 1, 2, 3, 4 }; dec("cmp dword [rsp+8],imm32", c, 8, VMG_READ_OTHER, 0, -1, 0); }
    { static const uint8_t c[] = { 0xF0, 0x0F, 0xB1, 0x0A };            dec("lock cmpxchg [rdx],ecx", c, 4, VMG_WRITE_OTHER, 0, -1, 0); }
    { vmg_insn d; static const uint8_t c[] = { 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0, 0, 0 };   /* mov rax, gs:[0x58] */
      CHECK(!vm_guard_decode(c, &d), "gs: access must not decode"); }
    { vmg_insn d; static const uint8_t c[] = { 0xF3, 0xA4 };            /* rep movsb */
      CHECK(!vm_guard_decode(c, &d), "rep movsb must not decode"); }
    { vmg_insn d; static const uint8_t c[] = { 0x89, 0xC8 };            /* mov eax, ecx (no memory) */
      CHECK(!vm_guard_decode(c, &d), "a register move must not decode"); }

    /* ---- real faults on a guest window ---- */
    uint8_t* vb = (uint8_t*)VirtualAlloc(NULL, 0x100010000ull, MEM_RESERVE, PAGE_READWRITE);
    CHECK(vb != NULL, "reserve");
    if (!vb) return 1;
    /* the host's own demand commit (main.cpp vm_commit_veh), registered first as the port does */
    g_vb = vb;
    AddVectoredExceptionHandler(1, commit_veh);
    CHECK(vm_guard_install(vb, VMG_NULL_PAGE | VMG_RAW_SPU, t_load, t_store) == 0, "install");
    CHECK(vm_guard_armed() == (VMG_NULL_PAGE | VMG_RAW_SPU), "armed %u", vm_guard_armed());

    volatile uint32_t* n32 = (volatile uint32_t*)(vb + 0x100);
    volatile uint8_t*  n8  = (volatile uint8_t*)(vb + 0x0FFF);
    volatile uint64_t* n64 = (volatile uint64_t*)(vb + 0x800);
    *n32 = 0xDEADBEEFu;                 /* dropped */
    *n8 = 0x55;                         /* dropped */
    *n64 = 0x1122334455667788ull;       /* dropped */
    CHECK(*n32 == 0 && *n8 == 0 && *n64 == 0, "null page stores must be dropped (%08X %02X %016llX)", *n32, *n8, (unsigned long long)*n64);
    volatile uint32_t* above = (volatile uint32_t*)(vb + 0x1000);
    *above = 7;                        /* 0x1000.. is ordinary memory, as vm_null_store leaves it */
    CHECK(*above == 7, "0x1000 must be writable");

    /* raw SPU 1, register at +0x4400C (In_MBox): 32-bit store, plain mov (raw bytes = bswap of the guest value) */
    const uint32_t ea = 0xE0100000u + 0x4400Cu;
    volatile uint32_t* r32 = (volatile uint32_t*)(vb + ea);
    *r32 = 0x78563412u;                /* memory bytes 12 34 56 78 = guest value 0x12345678 */
    CHECK(g_last_st_ea == ea && g_last_st_n == 4 && g_last_st_val == 0x12345678u,
          "raw store: ea %08X n %u val %llX", g_last_st_ea, g_last_st_n, (unsigned long long)g_last_st_val);
    uint32_t back = *r32;               /* plain load: raw bytes */
    CHECK(back == 0x78563412u, "raw load back %08X", back);
    uint32_t bev = __builtin_bswap32(*r32);   /* load + bswap (movbe with /arch:AVX2) */
    CHECK(bev == 0x12345678u, "raw movbe load %08X", bev);
    /* movbe store */
    { uint32_t v = 0xCAFEF00Du; uint8_t* p = vb + ea + 4;
      __asm__ volatile("movbe %0, (%1)" :: "r"(v), "r"(p) : "memory");
      CHECK(g_last_st_val == 0xCAFEF00Du && g_last_st_ea == ea + 4, "movbe store %llX", (unsigned long long)g_last_st_val); }
    /* mov-immediate, 8 and 64 bits */
    { uint8_t* p = vb + ea + 8;
      __asm__ volatile("movl $0x11223344, (%0)" :: "r"(p) : "memory");
      CHECK(g_last_st_val == 0x44332211u, "imm32 store %llX", (unsigned long long)g_last_st_val);
      __asm__ volatile("movb $0x5A, (%0)" :: "r"(p) : "memory");
      CHECK(g_last_st_val == 0x5Au && g_last_st_n == 1, "imm8 store %llX", (unsigned long long)g_last_st_val);
      __asm__ volatile("movq $-2, (%0)" :: "r"(p) : "memory");
      CHECK(g_last_st_val == 0xFEFFFFFFFFFFFFFFull && g_last_st_n == 8, "imm32->64 store %llX", (unsigned long long)g_last_st_val); }
    /* movzx / movsx / 8-bit high register */
    { uint8_t* p = vb + ea + 16;
      g_regs[ea + 16 - 0xE0000000u] = 0x80; g_regs[ea + 17 - 0xE0000000u] = 0x01;
      uint32_t zx; int32_t sx; uint64_t hi = 0x1111111111111111ull;
      __asm__ volatile("movzbl (%1), %0" : "=r"(zx) : "r"(p) : "memory");
      __asm__ volatile("movsbl (%1), %0" : "=r"(sx) : "r"(p) : "memory");
      __asm__ volatile("movb (%1), %%ah" : "+a"(hi) : "S"(p) : "memory");   /* (no REX: rsi) */
      CHECK(zx == 0x80u, "movzx %X", zx);
      CHECK(sx == -128, "movsx %d", sx);
      CHECK(hi == 0x1111111111118011ull, "mov ah %llX", (unsigned long long)hi);
      uint16_t w;
      __asm__ volatile("movw (%1), %0" : "=r"(w) : "r"(p) : "memory");
      CHECK(w == 0x0180u, "16-bit load %X", w);
      uint64_t q = 0x0102030405060708ull;
      __asm__ volatile("movq %0, (%1)" :: "r"(q), "r"(p) : "memory");
      CHECK(g_last_st_val == 0x0807060504030201ull && g_last_st_n == 8, "64-bit store %llX", (unsigned long long)g_last_st_val); }
    /* local store halves stay plain memory */
    { volatile uint32_t* ls = (volatile uint32_t*)(vb + 0xE0100000u + 0x100u); *ls = 9; CHECK(*ls == 9, "LS plain"); }
    /* an instruction the guard cannot finish: a vector load from a register page opens the page */
    { uint64_t c0[3]; vm_guard_counts(c0);
      uint8_t* p = vb + 0xE0200000u + 0x50000u;
      uint32_t x;
      __asm__ volatile("vmovd (%1), %%xmm0\n\tvmovd %%xmm0, %0" : "=r"(x) : "r"(p) : "xmm0", "memory");
      uint64_t c1[3]; vm_guard_counts(c1);
      CHECK(c1[2] == c0[2] + 1 && x == 0, "a vector load opens its page (%llu -> %llu, %X)",
            (unsigned long long)c0[2], (unsigned long long)c1[2], x); }
    { uint64_t c[3]; vm_guard_counts(c);
      printf("null-page stores dropped %llu, raw-SPU accesses %llu, pages opened %llu\n",
             (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2]); }
    printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
