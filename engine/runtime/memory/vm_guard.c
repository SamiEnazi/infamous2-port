/* PPU_LEAN_MEM (track A, 2026-11-16): the guest window's special pages and their fault handler.
 * See vm_guard.h. */
#include "vm_guard.h"
#include "../platform/win32_compat.h"   /* Win32 on Windows; VirtualProtect + the VEH dispatch on POSIX */

#include <stdio.h>
#include <string.h>

#define VMG_RAW_BASE   0xE0000000u   /* = runtime/spu/spu_raw.h SPU_RAW_BASE ... */
#define VMG_RAW_STRIDE 0x00100000u
#define VMG_RAW_COUNT  6u
#define VMG_RAW_PROB   0x00040000u   /* problem state from here to the end of each 1 MB */
#define VMG_NULL_END   0x00001000u   /* the page vm_null_store drops stores into */

static uint8_t* volatile s_vb;
static volatile unsigned s_armed;
static vmg_load_fn  s_raw_load;
static vmg_store_fn s_raw_store;
static volatile long long s_cnt[3];
static volatile long s_said[3];

/* ---- the decoder ------------------------------------------------------------------------------ */
/* ModRM (+ SIB + displacement) length at m; *mem = 0 for a register operand. */
static int modrm_len(const uint8_t* m, int* mem)
{
    const int mod = m[0] >> 6, rm = m[0] & 7;
    int n = 1;
    if (mod == 3) { *mem = 0; return 1; }
    *mem = 1;
    if (rm == 4) { n++; if (mod == 0 && (m[1] & 7) == 5) n += 4; }
    else if (mod == 0 && rm == 5) n += 4;          /* rip-relative */
    if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n;
}

static uint64_t rd_imm(const uint8_t* p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

int vm_guard_decode(const uint8_t* c, vmg_insn* o)
{
    memset(o, 0, sizeof *o);
    const uint8_t* p = c;
    int opsz = 0, rep = 0, rex = 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t b = *p;
        if (b == 0x66) { opsz = 1; p++; }
        else if (b == 0xF2 || b == 0xF3) { rep = b; p++; }
        else if (b == 0xF0 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) p++;
        else if (b == 0x64 || b == 0x65 || b == 0x67) return 0;   /* fs/gs (TLS) or 32-bit addressing: not a guest access */
        else break;
    }
    if ((*p & 0xF0) == 0x40) rex = *p++;
    const int W = (rex >> 3) & 1, R = (rex >> 2) & 1;
    const int gw = W ? 8 : (opsz ? 2 : 4);         /* the operand size of a 16/32/64-bit form */
    int mem = 0, ml, imm = 0, op = VMG_UNKNOWN, width = 0, reg = 0, dstw = 0, sx = 0, be = 0, b8 = 0;
    const uint8_t* m;
    if ((*p == 0xC5 || *p == 0xC4) && !rex && !opsz && !rep) {
        /* VEX: only the vector stores, and only as far as their length (a dropped store is skipped) */
        int map, pp;
        if (*p == 0xC5) { pp = p[1] & 3; map = 1; p += 2; }
        else { map = p[1] & 0x1F; pp = p[2] & 3; p += 3; }
        const uint8_t v = *p++;
        if (map != 1) return 0;
        if (!(v == 0x11 || v == 0x13 || v == 0x17 || v == 0x29 || v == 0x2B || v == 0xE7 || v == 0xD6 ||
              (v == 0x7F && pp) || (v == 0x7E && pp == 1)))
            return 0;
        ml = modrm_len(p, &mem);
        if (!mem) return 0;
        o->len = (uint8_t)(p + ml - c);
        o->op = VMG_WRITE_OTHER;
        return 1;
    }
    uint8_t b = *p++;
    if (b == 0x0F) {
        b = *p++;
        m = p;
        if (b == 0x38) {
            const uint8_t t = *p++; m = p;
            if (rep || (t != 0xF0 && t != 0xF1)) return 0;   /* (F2 0F 38 F0/F1 is crc32) */
            op = t == 0xF0 ? VMG_LOAD : VMG_STORE; width = gw; dstw = gw; be = 1;
        } else if (b == 0xB6 || b == 0xB7 || b == 0xBE || b == 0xBF) {
            op = VMG_LOAD; width = (b & 1) ? 2 : 1; dstw = gw; sx = b >= 0xBE;
        } else if (b == 0xC3) {
            op = VMG_STORE; width = W ? 8 : 4;                /* movnti */
        } else if (b == 0x11 || b == 0x13 || b == 0x17 || b == 0x29 || b == 0x2B || b == 0x7F || b == 0xE7 ||
                   b == 0xD6 || (b == 0x7E && opsz)) {
            op = VMG_WRITE_OTHER;                             /* SSE/MMX stores */
        } else if (b == 0x10 || b == 0x12 || b == 0x16 || b == 0x28 || b == 0x6F || b == 0x6E || b == 0x7E) {
            op = VMG_READ_OTHER;                              /* SSE/MMX loads */
        } else if (b == 0xB0 || b == 0xB1 || b == 0xC0 || b == 0xC1 || b == 0xAB || b == 0xB3 || b == 0xBB) {
            op = VMG_WRITE_OTHER;                             /* cmpxchg, xadd, bts/btr/btc */
        } else if (b == 0xA3) {
            op = VMG_READ_OTHER;                              /* bt */
        } else if (b == 0xBA) {
            op = ((p[0] >> 3) & 7) == 4 ? VMG_READ_OTHER : VMG_WRITE_OTHER; imm = 1;
        } else {
            return 0;
        }
    } else {
        m = p;
        const int sub = (p[0] >> 3) & 7;
        switch (b) {
        case 0x88: op = VMG_STORE; width = 1; b8 = 1; break;
        case 0x89: op = VMG_STORE; width = gw; break;
        case 0x8A: op = VMG_LOAD; width = 1; dstw = 1; b8 = 1; break;
        case 0x8B: op = VMG_LOAD; width = gw; dstw = gw; break;
        case 0x63: op = VMG_LOAD; width = 4; dstw = W ? 8 : 4; sx = W; break;
        case 0xC6: if (sub) return 0; op = VMG_STORE_IMM; width = 1; imm = 1; break;
        case 0xC7: if (sub) return 0; op = VMG_STORE_IMM; width = gw; imm = opsz ? 2 : 4; break;
        case 0x84: case 0x85: op = VMG_READ_OTHER; break;
        case 0x86: case 0x87: op = VMG_WRITE_OTHER; break;
        case 0x80: case 0x82: case 0x83: op = sub == 7 ? VMG_READ_OTHER : VMG_WRITE_OTHER; imm = 1; break;
        case 0x81: op = sub == 7 ? VMG_READ_OTHER : VMG_WRITE_OTHER; imm = opsz ? 2 : 4; break;
        case 0xC0: case 0xC1: op = VMG_WRITE_OTHER; imm = 1; break;
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: op = VMG_WRITE_OTHER; break;
        case 0xF6: op = sub <= 1 ? VMG_READ_OTHER : (sub <= 3 ? VMG_WRITE_OTHER : VMG_READ_OTHER); imm = sub <= 1 ? 1 : 0; break;
        case 0xF7: op = sub <= 1 ? VMG_READ_OTHER : (sub <= 3 ? VMG_WRITE_OTHER : VMG_READ_OTHER); imm = sub <= 1 ? (opsz ? 2 : 4) : 0; break;
        case 0xFE: case 0xFF: if (sub > 1) return 0; op = VMG_WRITE_OTHER; break;
        default:
            if (b < 0x40 && (b & 7) <= 3) {             /* the eight ALU groups: add or adc and sbb sub xor cmp */
                const int to_mem = (b & 7) <= 1;
                op = to_mem && (b & 0xF8) != 0x38 ? VMG_WRITE_OTHER : VMG_READ_OTHER;
                break;
            }
            return 0;
        }
    }
    ml = modrm_len(m, &mem);
    if (!mem) return 0;
    reg = ((m[0] >> 3) & 7) | (R << 3);
    o->len = (uint8_t)((m + ml + imm) - c);
    o->op = (uint8_t)op;
    o->width = (uint8_t)width;
    o->reg = (uint8_t)reg;
    o->high8 = (uint8_t)(b8 && !rex && reg >= 4 && reg < 8);
    if (o->high8) o->reg = (uint8_t)(reg - 4);
    o->dstw = (uint8_t)dstw;
    o->sx = (uint8_t)sx;
    o->be = (uint8_t)be;
    if (op == VMG_STORE_IMM) {
        uint64_t v = rd_imm(m + ml, imm);
        if (imm == 4 && W) v = (uint64_t)(int64_t)(int32_t)(uint32_t)v;   /* mov r/m64, imm32: sign-extended */
        o->imm = v;
    }
    return 1;
}

/* ---- the registers ------------------------------------------------------------------------- */
static ULONGLONG* gpr(CONTEXT* c, int r)
{
    switch (r) {
    case 0: return &c->Rax;  case 1: return &c->Rcx;  case 2: return &c->Rdx;  case 3: return &c->Rbx;
    case 4: return &c->Rsp;  case 5: return &c->Rbp;  case 6: return &c->Rsi;  case 7: return &c->Rdi;
    case 8: return &c->R8;   case 9: return &c->R9;   case 10: return &c->R10; case 11: return &c->R11;
    case 12: return &c->R12; case 13: return &c->R13; case 14: return &c->R14; default: return &c->R15;
    }
}

static uint64_t bswap_n(uint64_t v, unsigned n)
{
    switch (n) {
    case 2: return __builtin_bswap16((uint16_t)v);
    case 4: return __builtin_bswap32((uint32_t)v);
    case 8: return __builtin_bswap64(v);
    default: return v & 0xFFu;
    }
}

/* The memory bytes a store instruction writes, as a little-endian number. */
static uint64_t store_raw(CONTEXT* c, const vmg_insn* d)
{
    uint64_t v = d->op == VMG_STORE_IMM ? d->imm : *gpr(c, d->reg);
    if (d->high8) v >>= 8;
    if (d->width < 8) v &= (1ull << (8 * d->width)) - 1u;
    if (d->be) v = bswap_n(v, d->width);
    return v;
}

/* A load instruction's register result from the memory bytes (little-endian number). */
static void load_finish(CONTEXT* c, const vmg_insn* d, uint64_t raw)
{
    uint64_t v = raw;
    if (d->be) v = bswap_n(v, d->width);
    if (d->sx) {
        if (d->width == 1) v = (uint64_t)(int64_t)(int8_t)(uint8_t)v;
        else if (d->width == 2) v = (uint64_t)(int64_t)(int16_t)(uint16_t)v;
        else if (d->width == 4) v = (uint64_t)(int64_t)(int32_t)(uint32_t)v;
    }
    ULONGLONG* r = gpr(c, d->reg);
    if (d->high8) { *r = (*r & ~0xFF00ull) | ((v & 0xFFu) << 8); return; }
    switch (d->dstw) {
    case 1: *r = (*r & ~0xFFull) | (v & 0xFFu); break;
    case 2: *r = (*r & ~0xFFFFull) | (v & 0xFFFFu); break;
    case 4: *r = v & 0xFFFFFFFFull; break;            /* a 32-bit destination zero-extends */
    default: *r = v; break;
    }
}

/* ---- the handler ------------------------------------------------------------------------------ */
static void say(int k, const char* fmt, uint32_t ea, const void* rip, unsigned w)
{
    const long n = __atomic_fetch_add(&s_said[k], 1, __ATOMIC_RELAXED);
    if (n < 8 || (n & (n - 1)) == 0) {
        fprintf(stderr, fmt, ea, rip, w, (long long)(n + 1));
        fflush(stderr);
    }
}

static int raw_window(uint32_t ea)
{
    const uint32_t off = ea - VMG_RAW_BASE;
    return off < VMG_RAW_COUNT * VMG_RAW_STRIDE && (off & (VMG_RAW_STRIDE - 1)) >= VMG_RAW_PROB;
}

/* The page cannot be finished in place: open it (plain read-write memory) and let the access run. */
static LONG open_page(uint8_t* host, uint32_t ea, const void* rip)
{
    uint8_t* pg = (uint8_t*)((uintptr_t)host & ~(uintptr_t)0xFFF);
    DWORD old;
#ifdef _WIN32
    VirtualAlloc(pg, 0x1000, MEM_COMMIT, PAGE_READWRITE);
#endif
    VirtualProtect(pg, 0x1000, PAGE_READWRITE, &old);
    __atomic_fetch_add(&s_cnt[2], 1, __ATOMIC_RELAXED);
    say(2, "[lean-mem] an access to guest 0x%08X (host rip %p, %u) is not one the guard can finish: "
           "its page is plain memory from now on (#%lld)\n", ea, rip, 0);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG WINAPI vmg_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* const vb = s_vb;
    const uintptr_t fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    if (!vb || fault < (uintptr_t)vb || fault - (uintptr_t)vb >= 0x100000000ull)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t ea = (uint32_t)(fault - (uintptr_t)vb);
    const int write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
    const unsigned armed = s_armed;
    CONTEXT* c = ep->ContextRecord;
    const uint8_t* rip = (const uint8_t*)(uintptr_t)c->Rip;
    vmg_insn d;
    if ((armed & VMG_NULL_PAGE) && ea < VMG_NULL_END) {
        if (!write) return EXCEPTION_CONTINUE_SEARCH;   /* readable: not ours */
        if (vm_guard_decode(rip, &d) && (d.op == VMG_STORE || d.op == VMG_STORE_IMM || d.op == VMG_WRITE_OTHER)) {
            __atomic_fetch_add(&s_cnt[0], 1, __ATOMIC_RELAXED);
            say(0, "[null-write] guest store to 0x%08X (NULL page, host rip %p, %u bytes) -- DROPPED (#%lld)\n",
                ea, rip, d.width);
            c->Rip += d.len;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return open_page((uint8_t*)fault, ea, rip);
    }
    if ((armed & VMG_RAW_SPU) && raw_window(ea)) {
        if (vm_guard_decode(rip, &d) && d.width && (d.op == VMG_LOAD || d.op == VMG_STORE || d.op == VMG_STORE_IMM) &&
            raw_window(ea + d.width - 1u)) {
            int ok;
            if (d.op == VMG_LOAD) {
                uint64_t v = 0;
                ok = s_raw_load && s_raw_load(ea, d.width, &v);
                if (ok) load_finish(c, &d, bswap_n(v, d.width));   /* the guest value as memory bytes */
            } else {
                ok = s_raw_store && s_raw_store(ea, d.width, bswap_n(store_raw(c, &d), d.width));
            }
            if (ok) {
                __atomic_fetch_add(&s_cnt[1], 1, __ATOMIC_RELAXED);
                c->Rip += d.len;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        return open_page((uint8_t*)fault, ea, rip);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- arming ---------------------------------------------------------------------------------- */
static int protect(uint8_t* p, size_t n, DWORD prot)
{
    DWORD old;
#ifdef _WIN32
    if (!VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE)) return 0;   /* (the window is reserved, committed on touch) */
#endif
    return VirtualProtect(p, n, prot, &old) ? 1 : 0;
}

int vm_guard_install(uint8_t* vb, unsigned flags, vmg_load_fn raw_load, vmg_store_fn raw_store)
{
    static volatile long s_veh;
    if (!vb) return -1;
    if (s_vb && s_vb != vb) return -1;
    s_vb = vb;
    if (raw_load) s_raw_load = raw_load;
    if (raw_store) s_raw_store = raw_store;
    if (!__atomic_exchange_n(&s_veh, 1, __ATOMIC_SEQ_CST))
        AddVectoredExceptionHandler(1, vmg_veh);   /* ahead of the host's demand-commit handler */
    unsigned got = 0;
    if ((flags & VMG_NULL_PAGE) && !(s_armed & VMG_NULL_PAGE)) {
        /* the whole first 64 KB committed (the demand-commit granule), its first page read-only */
        if (protect(vb, 0x10000, PAGE_READWRITE) && protect(vb, VMG_NULL_END, PAGE_READONLY)) got |= VMG_NULL_PAGE;
        else fprintf(stderr, "[lean-mem] could not make the guest null page read-only (large pages?): "
                             "stores through a null pointer land, they are not dropped\n");
    }
    if ((flags & VMG_RAW_SPU) && !(s_armed & VMG_RAW_SPU)) {
        int ok = 1;
        for (uint32_t n = 0; n < VMG_RAW_COUNT; n++)
            ok &= protect(vb + VMG_RAW_BASE + n * VMG_RAW_STRIDE + VMG_RAW_PROB, VMG_RAW_STRIDE - VMG_RAW_PROB, PAGE_NOACCESS);
        if (ok) got |= VMG_RAW_SPU;
        else fprintf(stderr, "[lean-mem] could not guard the raw-SPU register windows\n");
    }
    __atomic_fetch_or(&s_armed, got, __ATOMIC_SEQ_CST);
    return (got & flags) == flags ? 0 : 1;
}

unsigned vm_guard_armed(void) { return s_armed; }

void vm_guard_counts(uint64_t out[3])
{
    for (int i = 0; i < 3; i++) out[i] = (uint64_t)__atomic_load_n(&s_cnt[i], __ATOMIC_RELAXED);
}
