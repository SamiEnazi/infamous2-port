"""Lift-time fast paths for the PPU codegen (ppu2 agent, 2026-11-08).

Each rewrite here has its own switch, read in every lifting worker (the
workers inherit the environment); all of them off = the lift as before, byte
for byte. Every replacement computes exactly what the text it replaces
computes -- only the order and number of tests change.

  PPU_FP_ONE_NAN=1  scalar FP helpers test the RESULT for NaN once, and only
                    on that (rare) path look at the operands: every IEEE
                    add/sub/mul/div/fma with a NaN operand gives a NaN, so a
                    non-NaN result proves no operand was one. Same values,
                    bit for bit, including which NaN is propagated. stfs of a
                    value a float holds exactly stores the float's bits.
                    (tools/test_fp_fast.py: old vs new on 4M operand sets.)
  PPU_VMX_SSE=1     the hot AltiVec ops on SSE/AVX2 (runtime/ppu/ppu_vmx_sse.h):
                    float arithmetic, compares, vperm, vsldoi, vspltw, the logic
                    ops and lvx/stvx as single 16-byte accesses; each computes
                    what the scalar form computes (tools/test_vmx_sse.c).
  PPU_VMX_FAST=1    (ppu3; needs PPU_VMX_SSE) the float ops' plain SSE result when no
                    operand or result lane is a denormal and no result lane a NaN,
                    else the exact form (ppu_vmx_sse.h; tools/test_vmx_sse.c).
  PPU_ST_LEAN=1     (ppu3; needs PPU_MM) the store fast path's reservation test reads
                    the bitmap only (not the armed flag first), as one aligned word:
                    the same bit (see header_preamble).
  PPU_VM_RUNS_LOADS=1  ppu_nv_dirty's access runs for loads (see _RUNS_C);
                    PPU_VM_RUNS_VERIFY=1 = the check build of it.
  PPU_ST_PMAP=1     (ppu-mem; needs PPU_MM) a store takes the fast path on one byte of a
                    page map (g_ppu_st_pmap: in range, no line of the page ever reserved),
                    else the PPU_MM store path as before (see _mm_block).
  PPU_MOVBE=1       (ppu-mem; needs PPU_MM) the PPU_MM fast paths' guest accesses as one
                    volatile access of their width, no alignment split (MOVBE on x86).
  PPU_TOC_FOLD=1    (ppu-mem; needs PPU_ST_PMAP) loads of the TOC entries (r2 + d) are the
                    entry's value from the ELF, while the runtime vouches for it per entry
                    (g_ppu_toc_dirty: checked against guest memory at start, set by any
                    store into the TOC); else the load (see toc_setup).
  PPU_LEAN_MEM=1    (track A; needs PPU_MOVBE and PPU_ST_PMAP) a guest load is one host load at
                    vm_base + ea, nothing else: no range test, no gcm poll-word compare. A store
                    keeps the page-map test and its re-check; one the map does not clear calls
                    ppu_lean_st<N> (the careful path, out of line). The runtime's guard pages and
                    live gcm ref word replace the tests (runtime/ppu/ppu_loader.cpp, see _LEAN_NOTE).
  PPU_ST_CMPB=1     (cg1, 2026-11-20; needs PPU_ST_PMAP) the page-map test (and re-check) of a store
                    as one `cmpb $1, (map, page)` instead of a load, a compare and the branch: two
                    instructions less per store, the same byte read at the same point (see _ST_CMPB).
  PPU_ST_RUNS=1     (cg1, 2026-11-20; needs PPU_ST_PMAP and PPU_MOVBE) three or more stores off one
                    register in straight-line code (a prologue's register saves, a structure's
                    fields) take one page test and one re-check for the run instead of one each;
                    the stretch is emitted twice, the careful copy for a run whose pages are not
                    fast (see store_runs). PPU_ST_RUN_MIN=<n>: the shortest run (default 3);
                    PPU_ST_RUNS_VERIFY=1: the check lift (each grouped store compares its own
                    address with the run's and its page with the map: ppu_run_bad reports).
  PPU_DRAIN_NONE=1  (track A; needs PPU_XTAIL) no trampoline drain after any call: no function of an
                    XTAIL lift returns with the trampoline set, and the runtime drains what an
                    import (ps3_hle_call) or an indirect call leaves before it returns.
  PPU_ICALL_INLINE=1 (track A) an indirect call tests the lift's own target table first
                    (g_ppu_icall_tbl, filled by ps3_indirect_call): a hit is one direct host call.
  PPU_VMX_CLEAN, PPU_VMX_INT, PPU_FP_SINGLE, PPU_FP_CVT (ppu-vmx, 2026-11-10): the
                    dataflow fast paths of tools/ppu_vmx_flow.py, applied after these.

Two run-time switches for a title's own code (cheats agent, 2026-11-09), each read from a
file of title knowledge kept with the title, not here (both unset = the lift as before):
  PPU_HOOK_FILE=<file>   a hook slot at chosen function entries (see _hooks)
  PPU_PATCH_FILE=<file>  run-time alternatives for single instructions (see patch_line)
"""
import os
import re


def _on(name: str) -> bool:
    return os.environ.get(name, "0") not in ("0", "")


_FP_BIN_OLD = """\
static inline double ppu_fp_bin(double a, double b, double r)
{
    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    if (r != r) return ppu_fp_default_qnan();
    return r;
}
"""
_FP_BIN_NEW = """\
static inline double ppu_fp_nan2(double a, double b)
{
    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    return ppu_fp_default_qnan();
}
/* PPU_FP_ONE_NAN: a NaN operand always makes r a NaN, so one test of r
 * decides; the operands are only looked at on that path (same result). */
static inline double ppu_fp_bin(double a, double b, double r)
{
    if (__builtin_expect(r != r, 0)) return ppu_fp_nan2(a, b);
    return r;
}
"""
_FMA_OLD = """\
static inline double ppu_fmadd_core(double a, double c, double b, int neg_b, int neg_res)
{
    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    if (c != c) return ppu_fp_quiet(c);
    double r = fma(a, c, neg_b ? -b : b);
    if (r != r) return ppu_fp_default_qnan();
    return neg_res ? -r : r;
}
"""
_FMA_NEW = """\
static inline double ppu_fma_nan3(double a, double c, double b)
{
    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    if (c != c) return ppu_fp_quiet(c);
    return ppu_fp_default_qnan();
}
static inline double ppu_fmadd_core(double a, double c, double b, int neg_b, int neg_res)
{
    double r = fma(a, c, neg_b ? -b : b);
    if (__builtin_expect(r != r, 0)) return ppu_fma_nan3(a, c, b);
    return neg_res ? -r : r;
}
"""


_VMX_INC = '#include "ppu_vmx.h"   /* exact VMX semantics (runtime/ppu/ppu_vmx.h) */\n'

# PPU_VM_RUNS_LOADS=1 (needs PPU_MM and PPU_NV_LOCALS): ppu_nv_dirty's access runs
# (soyjxck/ps3recomp, MIT) for LOADS only -- four or more loads at constant offsets
# from one unchanged register, with no label, call or return between, take one range
# test (PPU_RUN) instead of one each. The test passes only if every load of the run
# would have passed its own (inside the snapshot's span, the gcm ref-poll word
# outside the run), and then each load reads the very address its own fast path
# would; otherwise each load takes its own path, test and all. Stores keep their
# per-store coherence protocol (not grouped). PPU_VM_RUNS_VERIFY=1 adds, at every
# grouped load, a check of exactly that claim (ppu_run_bad reports a violation).
_RUNS_C = r"""
/* PPU_VM_RUNS_LOADS (ppu2): one range test per run of loads off one register */
static inline uint16_t ppu_run_ld16(const uint8_t* p) { uint16_t v;
    if (!((uintptr_t)p & 1u)) v = __atomic_load_n((const uint16_t*)p, __ATOMIC_RELAXED); else memcpy(&v, p, 2);
    return __builtin_bswap16(v); }
static inline uint32_t ppu_run_ld32(const uint8_t* p) { uint32_t v;
    if (!((uintptr_t)p & 3u)) v = __atomic_load_n((const uint32_t*)p, __ATOMIC_RELAXED); else memcpy(&v, p, 4);
    return __builtin_bswap32(v); }
static inline uint64_t ppu_run_ld64(const uint8_t* p) { uint64_t v;
    if (!((uintptr_t)p & 7u)) v = __atomic_load_n((const uint64_t*)p, __ATOMIC_RELAXED); else memcpy(&v, p, 8);
    return __builtin_bswap64(v); }
#define PPU_RUN_VARS(r) const uint8_t* _rp##r = 0; int _rk##r = 0; (void)_rp##r; (void)_rk##r;
#define PPU_RUN(r, base, lo, len, bits) do { const uint32_t _re = (uint32_t)((uint64_t)(base) + (int64_t)(lo)); \
    _rk##r = vm_mm_in(&_mm, _re, (uint32_t)(len)) && (uint32_t)(_mm.poll - _re) >= (uint32_t)(len); \
    _rp##r = _mm.vb + _re; } while (0)
#ifdef PPU_VM_RUNS_VERIFY
#ifdef __cplusplus
extern "C"
#endif
void ppu_run_bad(uint32_t ea, uint32_t n);
static inline const uint8_t* ppu_run_chk(const ppu_mm* m, const uint8_t* p, uint64_t a, uint32_t n) {
    const uint32_t ea = (uint32_t)a;
    if (p != m->vb + ea || !vm_mm_in(m, ea, n) || (n == 4 && ea == m->poll)) ppu_run_bad(ea, n);
    return p; }
#define PPU_RUN_P(r, o, a, n) ppu_run_chk(&_mm, _rp##r + (o), (a), (n))
#else
#define PPU_RUN_P(r, o, a, n) (_rp##r + (o))
#endif
#define PPU_RR8(r, o, a)  (_rk##r ? __atomic_load_n(PPU_RUN_P(r, o, a, 1), __ATOMIC_RELAXED) : vm_read8(a))
#define PPU_RR16(r, o, a) (_rk##r ? ppu_run_ld16(PPU_RUN_P(r, o, a, 2)) : vm_read16(a))
#define PPU_RR32(r, o, a) (_rk##r ? ppu_run_ld32(PPU_RUN_P(r, o, a, 4)) : vm_read32(a))
#define PPU_RR64(r, o, a) (_rk##r ? ppu_run_ld64(PPU_RUN_P(r, o, a, 8)) : vm_read64(a))
"""
_RE_LOAD_ONLY = r"vm_(read)(8|16|32|64)\((?=ctx->gpr\[(\d+)\](?: \+ (-?0x[0-9A-Fa-f]+|-?\d+))?[,)])"


def tune_nv(mod):
    """ppu_nv_dirty after import: with PPU_VM_RUNS_LOADS, its access runs on, loads only."""
    if _on("PPU_VM_RUNS_LOADS"):
        if not (_on("PPU_MM") and _on("PPU_NV_LOCALS")):
            raise RuntimeError("PPU_VM_RUNS_LOADS needs PPU_MM=1 and PPU_NV_LOCALS=1")
        if mod.RE_RUN_ACC.pattern.replace("(read|write)", "(read)") != _RE_LOAD_ONLY:
            raise RuntimeError("PPU_VM_RUNS_LOADS: ppu_nv_dirty's access pattern changed; update ppu_fastpath.py")
        mod.RE_RUN_ACC = re.compile(_RE_LOAD_ONLY)
        mod.RUNS_ON = True
    return mod


# stfs: a double that a float holds exactly (the usual case: a single-precision result)
# stores the float's own bits, which is what the bit selection gives for it; anything
# else (rounding needed, out of range, NaN: the compare is false) takes the full path.
_STFS_OLD = "static inline uint32_t ppu_stfs_bits(double v)\n{"
_STFS_NEW = "static inline uint32_t ppu_stfs_bits_full(double v)\n{"
_STFS_AT = "/* lfs/lfsx/lfsu/lfsux:"
_STFS_FAST = """\
static inline uint32_t ppu_stfs_bits(double v)
{
#ifndef PPU_FP_AUDIT
    const float f = (float)v;
    if (__builtin_expect((double)f == v, 1)) { uint32_t w; memcpy(&w, &f, 4); return w; }
#endif
    return ppu_stfs_bits_full(v);
}
"""


# PPU_ST_LEAN (ppu3): the PPU_MM store fast path's reservation test. Bits of the
# reservation bitmap are only ever SET (runtime/spu/spu_coherency.c coh_mark_line,
# never cleared), and the armed flag is set right after the first one, so with the
# flag still 0 every bit is 0 as well: reading the bit alone sends exactly the stores
# the flag-then-bit test sends to the out-of-line path, plus any that land in the
# moment between a line's first bit and the flag (which then take the careful path).
# On x86 the acquire load of the flag orders nothing a plain load does not. The bit
# is read as part of an aligned 32-bit word (the bitmap is a 4 MiB static array,
# 16-byte aligned by the x86-64 large-array rule): line = ea >> 7, word = ea >> 12,
# bit = line & 31 -- the same bit of the same byte (little-endian), in one shift
# fewer, and the compiler makes the test a single bt.
_RESV_OLD = """\
static inline int vm_mm_resv(const ppu_mm* m, uint32_t ea)
{
    if (!__atomic_load_n(m->armed, __ATOMIC_ACQUIRE)) return 0;
    const uint32_t line = ea >> 7;
    return (__atomic_load_n(&m->bits[line >> 3], __ATOMIC_RELAXED) >> (line & 7)) & 1u;
}
"""
_RESV_NEW = """\
static inline int vm_mm_resv(const ppu_mm* m, uint32_t ea)
{   /* PPU_ST_LEAN: the bit alone (bits are only ever set, the armed flag after the first one) */
    const uint32_t w = __atomic_load_n((const uint32_t*)m->bits + (ea >> 12), __ATOMIC_RELAXED);
    return (w >> ((ea >> 7) & 31u)) & 1u;
}
"""


# PPU_ST_PMAP / PPU_MOVBE / PPU_TOC_FOLD (ppu-mem, 2026-11-10): the PPU_MM accessors.
#
# PPU_ST_PMAP. A guest store's fast path used to be a range compare, the reservation bit
# of its line (shift, load, bt), the store, the compiler fence and the bit again: ~20
# instructions, five loads. Now one byte per 4 KB guest page (g_ppu_st_pmap, 1 MiB,
# runtime/spu/spu_coherency.c) says "fast" (== 1) only for a page inside the inline range
# (0x10000 .. the end of the mapping, minus one page so no fast store can run past it, all
# of it 0 while a memory diagnostic is armed) none of whose lines was ever reserved. The
# store reads that byte, stores, fences, reads it again: if it is no longer 1 the page got
# its first reserved line meanwhile and the out-of-line ppu_store_pm_recheck re-checks the
# line's bit and notifies, as vm_mm_after would have. Any other byte value takes the old
# PPU_MM path (range, bit, store, fence, bit), unchanged.
# Why it is the same protocol: coh_mark_line sets the page's "reserved" bit (2) BEFORE the
# line's bitmap bit and both before its process-wide barrier (FlushProcessWriteBuffers), and
# neither is ever cleared. So a page byte of 1 means no line of the page was reserved when
# it was read (bit set => page byte set), and the re-read after the store has the same
# property the bitmap re-check had: if it still reads 1, it ran before the barrier reached
# this thread, so the store did too and the marking SPU/PPU sees it in its snapshot; if it
# reads != 1, the re-check of the line's bit (set before the barrier as well) decides the
# notify exactly as before. The page map is filled once (runtime ppu_run, before any guest
# code) by compare-and-swap from 0, so a mark that came first is never overwritten.
#
# PPU_MOVBE. The fast paths read and write guest memory with one volatile access of the
# access width through an alignment-1 type: x86 performs it as a single instruction
# (MOVBE for the byte-swapped ones) -- single-copy atomic when aligned, as on Cell, which is
# all the aligned __atomic form gave -- and the aligned/unaligned split (a test per access)
# goes away. Volatile also keeps the compiler from merging, splitting, eliding or hoisting
# the access out of a loop, the reason they were atomics.
_MM_ACC_START = "static inline uint8_t vm_read8_mm(const ppu_mm* m, uint64_t a)\n{\n"
_MM_ACC_END = "    (vm_write64)(a, v);\n}\n"
_MM_ACC_NEED = ("vm_read16_mm(", "vm_read32_mm(", "vm_read64_mm(", "#define VM_MM_ST(", "vm_write8_mm(",
                "vm_write16_mm(", "vm_write32_mm(", "vm_write64_mm(", "ea != m->poll", "vm_mm_st_ok(m, ea, ",
                "vm_mm_after(m, ea, ")
_TOC: dict = {}     # toc_setup: base, lo, hi, image (bytes of [lo, hi)), fold switch


# PPU_LEAN_MEM (track A, 2026-11-16). _LEAN_NOTE: what the runtime does instead of the tests the
# accessors no longer make. The whole 4 GB guest window is reserved (an ea of any value is a host
# address inside it; one crossing 4 GB runs into the 64 KB guard behind it), so the range test only
# ever sent four kinds of address out of line, each now off the fast path:
#  - the null page: read-only, a store there dropped by the runtime's fault handler, as vm_write*
#    dropped it (runtime/memory/vm_guard.c VMG_NULL_PAGE);
#  - the raw-SPU register windows (0xE0000000 + n MB + 0x40000..): no-access pages whose accesses the
#    fault handler emulates through the raw-SPU register file (VMG_RAW_SPU; plain memory without a
#    raw SPU, as the out-of-line path served them);
#  - the gcm ref word (the 32-bit poll compare): plain memory the RSX side publishes into as it
#    drains fences, the poll's kick moved to the guest's sleeps (libs/video/cellGcmSys.c
#    PS3_GCM_REF_LIVE);
#  - an armed memory diagnostic (vm_inline_ok 0): it no longer sees lifted loads (the runtime says so).
# The loads are the same single volatile access PPU_MOVBE made after its tests, so every value read
# is the one the old path read. Stores: the page-map protocol of PPU_ST_PMAP unchanged; the careful
# path behind it (range, reservation bit, TOC watch, the out-of-line accessor) is ppu_lean_st<N>.
_LEAN_DECL = """/* PPU_LEAN_MEM (track A): the careful store, out of line (runtime/ppu/ppu_loader.cpp) */
#ifdef __cplusplus
extern "C" {
#endif
void ppu_lean_st8(uint32_t ea, uint8_t v);
void ppu_lean_st16(uint32_t ea, uint16_t v);
void ppu_lean_st32(uint32_t ea, uint32_t v);
void ppu_lean_st64(uint32_t ea, uint64_t v);
int  ppu_lean_mem_register(int flags);
#ifdef __cplusplus
}
#endif
"""
_TAKE_OLD = """static inline ppu_mm ppu_mm_take(void)
{
    const uint32_t hi = (ppu_vm_size != 0 && ppu_vm_size < 0xE0000000u) ? ppu_vm_size : 0xE0000000u;
    ppu_mm m;
    m.vb = vm_base;
    m.span = vm_inline_ok ? (int64_t)(hi - 0x10000u) : 0;
    m.poll = ppu_hle_inject_base + 0x2008u;
    m.armed = g_spu_coh_armed;
    m.bits = g_spu_coh_bitmap;
    return m;
}
"""
_TAKE_LEAN = """static inline ppu_mm ppu_mm_take(void)
{   /* PPU_LEAN_MEM: the base alone -- the accessors test nothing else (see ppu_fastpath.py) */
    ppu_mm m;
    m.vb = vm_base;
    m.span = 0; m.poll = 0; m.armed = 0; m.bits = 0;
    return m;
}
"""


# PPU_ST_CMPB=1 (agent cg1, 2026-11-20; needs PPU_ST_PMAP): the page-map test of a store (and of its
# re-check) as ONE instruction, `cmpb $1, (map, page)` + the branch, instead of a byte load, a compare
# and the branch. Clang does not fold an atomic (or volatile) load into a compare, so each of the
# ~400,000 store sites of the lift carried two extra instructions (and ~4 bytes); on the main thread
# the page-map tests are ~11 % of the lifted code's instructions. The asm reads the same byte at the
# same point as the relaxed atomic load did (an asm goto is volatile: never merged, hoisted or
# dropped; the "m" operand orders it like a load of the map against the store and the signal fence
# around it), so the store protocol is unchanged. Other targets / compilers: the old form.
_ST_CMPB = r"""
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__clang__) || defined(__GNUC__))
/* PPU_ST_CMPB (cg1): the page byte compared in memory, one instruction (see ppu_fastpath.py) */
static inline __attribute__((always_inline)) int vm_pm_fast(uint32_t ea)
{
    __asm__ goto("cmpb $1, (%0,%q1)\n\tjne %l[no]" : : "r"(g_ppu_st_pmap), "r"((uint64_t)(ea >> 12)),
                 "m"(*(const unsigned char (*)[1u << 20])g_ppu_st_pmap) : "cc" : no);
    return 1;
no:
    return 0;
}
#else
static inline int vm_pm_fast(uint32_t ea)
{
    return __atomic_load_n(&g_ppu_st_pmap[ea >> 12], __ATOMIC_RELAXED) == 1u;
}
#endif
"""


# --- PPU_ST_RUNS (agent cg1, 2026-11-20) ------------------------------------------------------------
# One page test for a run of stores off one register. Where three or more lifted stores off one guest
# register (r1 in a prologue's register saves, a structure's fields) follow one another in straight-line
# code -- no label, branch, call or return between them, the register unchanged or moved by constants --
# the stretch of lines from the first to the last is emitted twice:
#     { const uint32_t _sre = (uint32_t)(rB + lo); uint8_t* const _srp = _mm.vb + _sre;
#       if (both end pages of [_sre, _sre + span) are fast pages) { the lines, each run store a plain
#           byte-swapped store at _srp + offset; then the fence and ONE re-check of the two pages: if
#           either went slow meanwhile, ppu_store_pm_recheck() for every store of the run, in order }
#       else { the lines as they were } }
# span <= 4096, so the two end pages are all the pages the run touches; a fast page (map byte 1) is in the
# inline range, so no address wraps. The protocol is the per-store one stretched over the run: every store
# of the run follows the page test and precedes the re-check, and a map byte only ever changes from 1 to
# "careful" (a line's first reservation), so a store either ran before the reserver's process barrier
# (visible to its snapshot) or the re-check after it sees the page's bit and the line's own bit decides
# (ppu_store_pm_recheck, as before). As with the per-store form, a store that raced a page's first
# reservation may break that reservation needlessly (allowed: a reservation may always be lost); the
# window is the run's length instead of one store's. The careful copy runs whenever a page of the run is
# not fast at its start. PPU_ST_RUN_MIN=<n>: the shortest run (default 3).
import os as _os
import re as _re

_SR_MACROS = r"""
/* PPU_ST_RUNS (cg1): one page test for a run of stores off one register (see ppu_fastpath.py) */
#define PPU_SRUN_OK(e, span) (vm_pm_fast(e) && vm_pm_fast((uint32_t)((e) + (span) - 1u)))
#define PPU_SRW8(p, o, v)  (*(volatile uint8_t*)((p) + (o)) = (uint8_t)(v))
#define PPU_SRW16(p, o, v) (*(volatile ppu_u16u*)((p) + (o)) = __builtin_bswap16((uint16_t)(v)))
#define PPU_SRW32(p, o, v) (*(volatile ppu_u32u*)((p) + (o)) = __builtin_bswap32((uint32_t)(v)))
#define PPU_SRW64(p, o, v) (*(volatile ppu_u64u*)((p) + (o)) = __builtin_bswap64((uint64_t)(v)))
#define PPU_SRC(e, o, n) ppu_store_pm_recheck((uint32_t)((e) + (o)), (n));
#ifdef PPU_ST_RUNS_VERIFY   /* the check lift: each grouped store's own address against the run's */
#ifdef __cplusplus
extern "C"
#endif
void ppu_run_bad(uint32_t ea, uint32_t n);
#define PPU_SRV(e, o, a, n) do { if ((uint32_t)(a) != (uint32_t)((e) + (o)) || !vm_pm_fast((uint32_t)(a))) \
    ppu_run_bad((uint32_t)(a), (n)); } while (0)
#else
#define PPU_SRV(e, o, a, n) ((void)0)
#endif
#define PPU_SRUN_END(e, span, RC) do { __atomic_signal_fence(__ATOMIC_SEQ_CST); \
    if (__builtin_expect(!PPU_SRUN_OK(e, span), 0)) { RC } } while (0)
"""

_SR_KW = _re.compile(r"\b(if|else|goto|return|switch|case|default|for|while|do|break|continue)\b")
# calls that leave the straight line or touch reservations / ordering: never inside a run
_SR_BAD = _re.compile(r"\b(\w*func_[0-9A-Fa-f]+|ps3_\w+|lv2_\w+|ppu_icall\w*|DRAIN_TRAMPOLINE|PPU_TAIL\w*|"
                      r"g_trampoline_fn|ppu_hle\w*|\w*(?:lwarx|ldarx|stwcx|stdcx|resv|reserve)\w*|ppu_hook\w*|"
                      r"ppu_native\w*|g_ppu_patch\w*|PPU_FENCE|\w*thread_fence|\w*sync\w*|\w*eieio\w*|"
                      r"setjmp|longjmp|abort|exit)\b")
_SR_DECL0 = _re.compile(r"^(const\s+)?(u?int(8|16|32|64)_t|double|float|ppu_vr|__m128\w*|__m256\w*|auto|"
                        r"PPU_RUN_VARS)\b")
_SR_STORE = _re.compile(r"^vm_write(8|16|32|64)\(")
_SR_BASE = _re.compile(r"^(r\d+)(?:\s*\+\s*(-?0x[0-9A-Fa-f]+|-?\d+))?$")
_SR_ADJ = _re.compile(r"^(r\d+)\s*(\+=|-=)\s*\(?(?:\(int64_t\))?\(?(-?0x[0-9A-Fa-f]+|-?\d+)\)?\)?$")
_SR_GSET = _re.compile(r"(?<![\w>.\]])(r\d+)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|(?:\+\+|--)\s*(r\d+)\b|\b(r\d+)\s*(?:\+\+|--)|&\s*(r\d+)\b")


def _sr_split(line):
    """The top-level statements of one line, or None for something this pass does not take apart."""
    out, cur, dp, db = [], [], 0, 0
    i, n = 0, len(line)
    while i < n:
        ch = line[i]
        if ch == '/' and line.startswith('/*', i):
            j = line.find('*/', i + 2)
            if j < 0: return None
            i = j + 2; continue
        if ch in '"\'': return None
        cur.append(ch)
        if ch == '(': dp += 1
        elif ch == ')': dp -= 1
        elif ch == '{': db += 1
        elif ch == '}':
            db -= 1
            if db == 0 and dp == 0:
                out.append(''.join(cur).strip()); cur = []
        elif ch == ';' and dp == 0 and db == 0:
            out.append(''.join(cur).strip()); cur = []
        if dp < 0 or db < 0: return None
        i += 1
    if dp or db: return None
    rest = ''.join(cur).strip()
    if rest: return None              # a label, or text this pass does not know
    return [s for s in out if s and s != ';']


def _sr_args(s, i):
    """(args, end): the top-level comma-separated arguments of the call whose '(' is at s[i]."""
    dp, cur, args = 0, [], []
    for j in range(i, len(s)):
        ch = s[j]
        if ch == '(':
            dp += 1
            if dp == 1: continue
        elif ch == ')':
            dp -= 1
            if dp == 0:
                args.append(''.join(cur).strip()); return args, j
        elif ch == ',' and dp == 1:
            args.append(''.join(cur).strip()); cur = []; continue
        cur.append(ch)
    return None, -1


def _sr_gsets(t):
    """The GPR locals a statement may assign (or take the address of)."""
    out = set()
    for m in _SR_GSET.finditer(t):
        out.add(m.group(1) or m.group(2) or m.group(3) or m.group(4))
    return out


def _sr_stmt(st, depth=0):
    """('store', width, base, off, value) | ('adj', reg, delta) | ('set', regs) | None (a barrier)."""
    t = st.strip()
    if t.endswith(';'): t = t[:-1].strip()
    if not t or t == '/* nop */': return ('set', set())
    if _SR_KW.search(t) or _SR_BAD.search(t): return None
    if t.startswith('{') and t.endswith('}'):
        inner = _sr_split(t[1:-1])
        if inner is None: return None
        regs = set()
        for x in inner:
            k = _sr_stmt(x, depth + 1)
            if k is None: return None
            if k[0] == 'set': regs |= k[1]
            elif k[0] == 'adj': regs.add(k[1])
        return ('set', regs | _sr_gsets(t))
    if _SR_DECL0.match(t):
        return ('set', _sr_gsets(t)) if depth else None     # a declaration: only inside a block
    m = _SR_STORE.match(t)
    if m and depth == 0:
        args, end = _sr_args(t, t.index('('))
        if args is not None and len(args) == 2 and not t[end + 1:].strip():
            b = _SR_BASE.match(args[0])
            if b and not _sr_gsets(t):
                return ('store', int(m.group(1)), b.group(1), int(b.group(2) or '0', 0), args[1])
    m = _SR_ADJ.match(t)
    if m:
        return ('adj', m.group(1), int(m.group(3), 0) * (1 if m.group(2) == '+=' else -1))
    return ('set', _sr_gsets(t))


_SR_VERIFY = _os.environ.get("PPU_ST_RUNS_VERIFY", "0") not in ("0", "")


def store_runs(lines, stats=None):
    """A chunk's function lines with PPU_ST_RUNS applied."""
    run_min = int(_os.environ.get("PPU_ST_RUN_MIN", "3"))
    out = []
    i, n = 0, len(lines)
    in_fn = False
    while i < n:
        ln = lines[i]
        if not in_fn:
            out.append(ln)
            if ln.startswith("void ") and ln.rstrip().endswith("{") and "(ppu_context* ctx)" in ln:
                in_fn = True
            i += 1; continue
        if ln.startswith("}"):
            in_fn = False; out.append(ln); i += 1; continue
        # scan a stretch of whitelisted lines from i
        info = []
        j = i
        while j < n and not lines[j].startswith("}"):
            sts = _sr_split(lines[j])
            ks = [(_sr_stmt(s)) for s in sts] if sts is not None else None
            if ks is None or any(k is None for k in ks): break
            info.append(ks); j += 1
        if not info:
            out.append(ln); i += 1; continue
        # runs inside lines[i:j]: per base register, members with offsets relative to the base's value at the
        # start of the run's first line
        runs = []                      # (first line, last line, base, [(line, stmt index, width, rel off)])
        open_ = {}                     # base -> dict(first, members, delta, line_delta_ok)
        def close(b):
            r = open_.pop(b, None)
            if r and len(r['m']) >= run_min:
                lo = min(o for _l, _s, w, o in r['m']); hi = max(o + w // 8 for _l, _s, w, o in r['m'])
                if hi - lo <= 4096:
                    runs.append((r['first'], r['m'][-1][0], b, r['m'], lo, hi))
        for li, ks in enumerate(info):
            for si, k in enumerate(ks):
                if k[0] == 'store':
                    _, w, b, off, _v = k
                    r = open_.get(b)
                    if r is None:
                        # start: only where nothing earlier on this line moves b
                        if any((kk[0] == 'set' and b in kk[1]) or (kk[0] == 'adj' and kk[1] == b) for kk in ks[:si]):
                            continue
                        r = open_[b] = {'first': li, 'm': [], 'd': 0}
                    r['m'].append((li, si, w, off + r['d']))
                elif k[0] == 'adj':
                    if k[1] in open_: open_[k[1]]['d'] += k[2]
                elif k[0] == 'set':
                    for reg in k[1]:
                        if reg in open_: close(reg)
        for b in list(open_): close(b)
        if not runs:
            out.extend(lines[i:j]); i = j; continue
        # runs may overlap in lines (two bases): take them first come, non-overlapping
        runs.sort(key=lambda r: (r[0], r[1]))
        chosen, last = [], -1
        for r in runs:
            if r[0] > last:
                chosen.append(r); last = r[1]
        pos = 0
        for (fl, ll, b, mem, lo, hi) in chosen:
            out.extend(lines[i + pos:i + fl])
            span = hi - lo
            memset = {(l, s): (w, o) for l, s, w, o in mem}
            fast = []
            for l in range(fl, ll + 1):
                sts = _sr_split(lines[i + l]); parts = []
                for si, s in enumerate(sts):
                    if (l, si) in memset:
                        w, o = memset[(l, si)]
                        k = info[l][si]
                        if _SR_VERIFY:
                            parts.append("PPU_SRV(_sre, 0x%X, %s, %d);" % (o - lo, _sr_args(lines[i + l].strip() and s, s.index('('))[0][0], w // 8))
                        parts.append("PPU_SRW%d(_srp, 0x%X, %s);" % (w, o - lo, k[4]))
                    else:
                        parts.append(s if s.endswith('}') or s.endswith(';') else s + ';')
                fast.append("        " + " ".join(parts))
            rc = " ".join("PPU_SRC(_sre, 0x%X, %d)" % (o - lo, w // 8) for _l, _s, w, o in mem)
            ea = "%s + %d" % (b, lo) if lo >= 0 else "%s - %d" % (b, -lo)
            out.append("        { const uint32_t _sre = (uint32_t)(%s); uint8_t* const _srp = _mm.vb + _sre; (void)_srp;" % ea)
            out.append("        if (__builtin_expect(PPU_SRUN_OK(_sre, %du), 1)) {   /* PPU_ST_RUNS: %d stores off %s */" % (span, len(mem), b))
            out.extend(fast)
            out.append("        PPU_SRUN_END(_sre, %du, %s);" % (span, rc))
            out.append("        } else {")
            out.extend(lines[i + fl:i + ll + 1])
            out.append("        } }")
            pos = ll + 1
            if stats is not None:
                stats["st_runs"] = stats.get("st_runs", 0) + 1
                stats["st_run_stores"] = stats.get("st_run_stores", 0) + len(mem)
        out.extend(lines[i + pos:j])
        i = j
    return out


def _sr_inl() -> str:
    """PPU_ST_RUNS: the accessors always inline. A run's careful copy is cold (the run's test is expected to
    pass), and clang does not inline into cold code: an accessor called there takes &_mm, which then lives in
    memory for the whole function (four stores at every entry)."""
    return " __attribute__((always_inline))" if _on("PPU_ST_RUNS") else ""


def _lean_on() -> bool:
    return _on("PPU_LEAN_MEM")


def _mm_block(movbe: bool, pmap: bool, toc: bool, lean: bool = False) -> str:
    L = []
    W = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t"}
    if lean:
        L.append(_LEAN_DECL.rstrip("\n"))
    if movbe:
        L.append("/* PPU_MOVBE (ppu-mem): one volatile access of the width, alignment 1 (see ppu_fastpath.py) */")
        for w in (16, 32, 64):
            L.append("typedef uint%d_t __attribute__((aligned(1))) ppu_u%du;" % (w, w))
        L.append("#define VM_MM_ST(T, m, ea, val) do { typedef T __attribute__((aligned(1))) _ppu_stu; "
                 "*(volatile _ppu_stu*)((m)->vb + (uint32_t)(ea)) = (val); } while (0)")
    else:
        L.append("#define VM_MM_ST(T, m, ea, val) do { uint8_t* _p = (m)->vb + (uint32_t)(ea); T _v = (val); \\\n"
                 "    if (!((uintptr_t)_p & (sizeof(T) - 1))) __atomic_store_n((T*)_p, _v, __ATOMIC_RELAXED); \\\n"
                 "    else memcpy(_p, &_v, sizeof(T)); } while (0)")
    for w in (8, 16, 32, 64):
        T, n = W[w], w // 8
        poll = " && __builtin_expect(ea != m->poll, 1)" if w == 32 else ""
        L.append("static inline%s %s vm_read%d_mm(const ppu_mm* m, uint64_t a)\n{\n    const uint32_t ea = (uint32_t)a;" % (_sr_inl(), T, w))
        if lean:   # PPU_LEAN_MEM: the access alone
            acc = ("*(volatile const uint8_t*)(m->vb + ea)" if w == 8 else
                   "__builtin_bswap%d(*(volatile const ppu_u%du*)(m->vb + ea))" % (w, w))
            L.append("    return %s;\n}" % acc)
            continue
        if movbe:
            acc = ("*(volatile const uint8_t*)(m->vb + ea)" if w == 8 else
                   "__builtin_bswap%d(*(volatile const ppu_u%du*)(m->vb + ea))" % (w, w))
            L.append("    if (__builtin_expect(vm_mm_in(m, ea, %d), 1)%s) return %s;" % (n, poll, acc))
        elif w == 8:
            L.append("    if (__builtin_expect(vm_mm_in(m, ea, 1), 1)) return __atomic_load_n(m->vb + ea, __ATOMIC_RELAXED);")
        else:
            L.append("    if (__builtin_expect(vm_mm_in(m, ea, %d), 1)%s) {   /* aligned: single-copy atomic, as on Cell */" % (n, poll))
            L.append("        %s v;" % T)
            L.append("        if (!(ea & %du)) v = __atomic_load_n((const %s*)(m->vb + ea), __ATOMIC_RELAXED);" % (n - 1, T))
            L.append("        else memcpy(&v, m->vb + ea, %d);" % n)
            L.append("        return __builtin_bswap%d(v); }" % w)
        L.append("    return (vm_read%d)(a);   /* the out-of-line accessor, not the macro */\n}" % w)
    if pmap:
        L.append("/* PPU_ST_PMAP (ppu-mem): one byte per 4 KB guest page, 1 = the store fast path (see ppu_fastpath.py) */")
        L.append("#ifdef __cplusplus\nextern \"C\" {\n#endif")
        L.append("extern unsigned char g_ppu_st_pmap[1u << 20];")
        L.append("void ppu_store_pm_recheck(uint32_t ea, uint32_t n);")
        if toc:
            L.append("void ppu_toc_write_hit(uint32_t ea, uint32_t n);")
        L.append("#ifdef __cplusplus\n}\n#endif")
        if _on("PPU_ST_CMPB"):   # (cg1) the byte compared in memory: see _ST_CMPB
            L.append(_ST_CMPB.rstrip("\n"))
        else:
            L.append("static inline int vm_pm_fast(uint32_t ea)\n{\n"
                     "    return __atomic_load_n(&g_ppu_st_pmap[ea >> 12], __ATOMIC_RELAXED) == 1u;\n}")
        if toc:   # a store that may touch a folded TOC entry says so first (its page is never fast)
            L.append("#define PPU_PM_WATCH(ea, n) do { if (__builtin_expect((uint32_t)(ea) - (PPU_TOC_LO - (n) + 1u) < "
                     "(PPU_TOC_HI - PPU_TOC_LO + (n) - 1u), 0)) ppu_toc_write_hit((uint32_t)(ea), (n)); } while (0)")
        else:
            L.append("#define PPU_PM_WATCH(ea, n) ((void)0)")
    for w in (8, 16, 32, 64):
        T, n = W[w], w // 8
        v = "v" if w == 8 else "__builtin_bswap%d(v)" % w
        st = ("__atomic_store_n(m->vb + ea, v, __ATOMIC_RELAXED)" if (w == 8 and not movbe) else
              "VM_MM_ST(%s, m, ea, %s)" % (T, v))
        L.append("static inline%s void vm_write%d_mm(const ppu_mm* m, uint64_t a, %s v)\n{\n    const uint32_t ea = (uint32_t)a;" % (_sr_inl(), w, T))
        if pmap:
            L.append("    if (__builtin_expect(vm_pm_fast(ea), 1)) {")
            L.append("        %s;" % st)
            L.append("        __atomic_signal_fence(__ATOMIC_SEQ_CST);   /* store, then re-check: see spu_coh_reserve */")
            L.append("        if (__builtin_expect(!vm_pm_fast(ea), 0)) ppu_store_pm_recheck(ea, %d);" % n)
            L.append("        return; }")
            if lean:   # PPU_LEAN_MEM: the careful path out of line (it watches the TOC itself)
                L.append("    ppu_lean_st%d(ea, v);\n}" % w)
                continue
            L.append("    PPU_PM_WATCH(ea, %du);" % n)
        L.append("    if (vm_mm_st_ok(m, ea, %d)) { %s; vm_mm_after(m, ea, %d); return; }" % (n, st, n))
        L.append("    (vm_write%d)(a, v);   /* the out-of-line accessor, not the macro */\n}" % w)
    if pmap and movbe and _on("PPU_ST_RUNS"):   # (cg1) see store_runs
        if _on("PPU_ST_RUNS_VERIFY"):
            L.append("#define PPU_ST_RUNS_VERIFY 1")
        L.append(_SR_MACROS.strip("\n"))
    return "\n".join(L) + "\n"


def _toc_header() -> str:
    t = _TOC
    return ("/* PPU_TOC_FOLD (ppu-mem): the TOC entries' values from the ELF, while g_ppu_toc_ok[k] (one byte per\n"
            " * 4-byte entry: set at start where memory matches the lift, cleared by any store into the entry)\n"
            " * says so -- see ppu_fastpath.py */\n"
            "#define PPU_TOC_BASE 0x%08Xull\n#define PPU_TOC_LO 0x%08Xu\n#define PPU_TOC_HI 0x%08Xu\n"
            "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
            "extern unsigned char g_ppu_toc_ok[];\n"
            "int ppu_toc_fold_register(uint32_t lo, uint32_t n, const unsigned char* image, uint64_t toc);\n"
            "#ifdef __cplusplus\n}\n#endif\n"
            "#define PPU_TOC_OK(k) (__atomic_load_n(&g_ppu_toc_ok[k], __ATOMIC_RELAXED) != 0)\n"
            "#define PPU_TOC_LD32(r, d, k, val) (((r) == PPU_TOC_BASE && PPU_TOC_OK(k)) ? (uint32_t)(val) : vm_read32((r) + (d)))\n"
            "#define PPU_TOC_LD64(r, d, k, val) (((r) == PPU_TOC_BASE && PPU_TOC_OK(k) && PPU_TOC_OK((k) + 1)) ? "
            "(uint64_t)(val) : vm_read64((r) + (d)))\n" % (t["base"], t["lo"], t["hi"]))


def header_preamble(text: str) -> str:
    """The lifted code's header (ppu_recomp.h) with the switched-on rewrites."""
    if _on("PPU_ST_LEAN"):
        if _RESV_OLD not in text:
            raise RuntimeError("PPU_ST_LEAN needs PPU_MM=1 (or the lifter's vm_mm_resv changed; update ppu_fastpath.py)")
        text = text.replace(_RESV_OLD, _RESV_NEW)
    movbe, pmap, toc, lean = _on("PPU_MOVBE"), _on("PPU_ST_PMAP"), _toc_on(), _lean_on()
    if lean:
        if not (movbe and pmap):
            raise RuntimeError("PPU_LEAN_MEM needs PPU_MOVBE=1 and PPU_ST_PMAP=1")
        if _TAKE_OLD not in text:
            raise RuntimeError("PPU_LEAN_MEM: the lifter's ppu_mm_take changed; update ppu_fastpath.py")
        text = text.replace(_TAKE_OLD, _TAKE_LEAN)
    if movbe or pmap:
        i = text.find(_MM_ACC_START)
        j = text.find(_MM_ACC_END, i) if i >= 0 else -1
        if i < 0 or j < 0 or not all(s in text[i:j] for s in _MM_ACC_NEED):
            raise RuntimeError("PPU_ST_PMAP / PPU_MOVBE need PPU_MM=1 (or the lifter's PPU_MM accessors changed; "
                               "update ppu_fastpath.py)")
        text = text[:i] + (_toc_header() if toc else "") + _mm_block(movbe, pmap, toc, lean) + text[j + len(_MM_ACC_END):]
    return text


# PPU_TOC_FOLD: what is proven at lift time (toc_setup) and what the runtime guards.
#  - The module has one TOC (the lifter's toc_base) and the ELF's .toc section sits at
#    toc_base - 0x8000 (the ABI's convention); only 4/8-byte loads of r2 + d that land inside
#    that section, aligned, are folded, and only while r2 == toc_base (compared per load).
#  - No instruction of the EBOOT can write the TOC or hand its address on: no store has r2
#    as its base or index, no instruction other than a load computes a register other than r2
#    from r2 (an `addi rX, r2, d` would be the address of an entry), r2's value is stored only
#    to the stack (`std r2, N(r1)`, the ABI's TOC save), and no word of the loaded image (code,
#    data, the TOC itself) holds an address inside the TOC. Any exception found turns the
#    folding off for the lift (and says which). inFAMOUS 2 v1.04: none in 1.89M lifted
#    instructions; 87,309 32-bit and 131 64-bit TOC loads folded.
#  - At run time every entry starts "dirty" (fold off) until ppu_run compared guest memory
#    with the lifted image (patches applied to the ELF image, e.g. from RPCS3's patch.yml,
#    keep their entries dirty); the TOC's pages are never on the store fast path, and every
#    lifted store into [lo, hi) and the runtime's out-of-line stores mark the entries they
#    touch dirty before they store. A memory diagnostic armed (vm_inline_ok 0) keeps them all
#    dirty, so the diagnostics see every TOC read.
_RE_TOC_LD = re.compile(r"\bvm_read(32|64)\((r2|ctx->gpr\[2\]) \+ (-?(?:0x[0-9A-Fa-f]+|\d+))\)")
_TOC_LOADS = {"lbz", "lhz", "lha", "lwz", "lwa", "ld", "lfs", "lfd", "lbzx", "lhzx", "lhax", "lwzx", "lwax", "ldx",
              "lfsx", "lfdx", "lwarx", "ldarx", "lvx", "lvxl", "lvlx", "lvrx", "lwbrx", "lhbrx", "ldbrx"}
_GPR_FIRST_STORES = {"stb", "sth", "stw", "std", "stbu", "sthu", "stwu", "stdu", "stbx", "sthx", "stwx", "stdx",
                     "stbux", "sthux", "stwux", "stdux", "stmw", "stswi", "stswx", "stwcx.", "stdcx.", "stwbrx",
                     "sthbrx", "stdbrx"}


def _toc_on() -> bool:
    return _on("PPU_TOC_FOLD") and bool(_TOC.get("fold"))


def toc_setup(toc_base: int, elf_path: str, insns, bounds=None) -> None:
    """PPU_TOC_FOLD: find the TOC section, prove the EBOOT never writes it (see above), keep its bytes.
    `insns`: the decoded executable segments; `bounds`: the lifted functions' [start, end) -- only
    their instructions run as lifted code (literal pools and data in the segment are skipped)."""
    _TOC.clear()
    if not _on("PPU_TOC_FOLD"):
        return
    if not (_on("PPU_ST_PMAP") and _on("PPU_MM")):
        raise RuntimeError("PPU_TOC_FOLD needs PPU_ST_PMAP=1 and PPU_MM=1 (its stores' watch is the page map)")
    import struct
    if not toc_base:
        print("  PPU_TOC_FOLD: no single TOC -> off")
        return
    b = open(elf_path, "rb").read()
    if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 2:
        print("  PPU_TOC_FOLD: not a big-endian ELF64 -> off")
        return
    shoff, = struct.unpack_from(">Q", b, 0x28)
    shentsize, shnum = struct.unpack_from(">HH", b, 0x3A)
    lo = hi = None
    for i in range(shnum):
        _nm, ty, fl, addr, off, sz = struct.unpack_from(">IIQQQQ", b, shoff + i * shentsize)
        if ty == 1 and (fl & 2) and addr == toc_base - 0x8000 and 0 < sz <= 0x10000:   # PROGBITS, ALLOC
            lo, hi, data = addr, addr + sz, b[off:off + sz]
            break
    if lo is None:
        print(f"  PPU_TOC_FOLD: no section at TOC-0x8000 (0x{toc_base - 0x8000:X}) -> off")
        return
    bad = []
    if bounds:
        import bisect
        bs = []
        for s, e in sorted(bounds):          # merged: ranges nest (tail entries) and touch
            if bs and s <= bs[-1][1]:
                bs[-1][1] = max(bs[-1][1], e)
            else:
                bs.append([s, e])
        starts = [s for s, _e in bs]

        def lifted(a):
            k = bisect.bisect_right(starts, a) - 1
            return k >= 0 and a < bs[k][1]
        insns = [i for i in insns if lifted(i.addr)]
    for ins in insns:
        ops = [t.strip() for t in re.split(r"[,()]", ins.operands or "") if t.strip()]
        if "r2" not in ops:
            continue
        mn = ins.mnemonic
        if mn.startswith("lf") or mn.startswith("stf") or mn.startswith("lv") or mn.startswith("stv"):
            ops = ops[1:]          # the first operand is an FPR / VR (printed as rN): not r2
            if "r2" not in ops:
                continue
            if mn.startswith("l"):
                continue           # r2 as a load's base or index
            bad.append(ins)        # an FP/vector store with r2 as base or index
            continue
        if mn in _TOC_LOADS:
            continue               # r2 as base/index of a load, or as its target
        if mn in _GPR_FIRST_STORES or mn.startswith("st"):
            if ops[0] == "r2" and "r2" not in ops[1:] and ops[-1] == "r1" and mn in ("std", "stw"):
                continue           # the TOC save on the stack
            bad.append(ins)
            continue
        if mn.startswith("mt"):
            bad.append(ins)        # r2 into CTR / LR / an SPR
            continue
        if mn in ("dcbz", "dcbt", "dcbtst", "dcbf", "dcbst", "icbi", "dcba"):
            if mn == "dcbz":
                bad.append(ins)
            continue
        if mn.startswith("cmp") or mn in ("tw", "td", "twi", "tdi"):
            continue               # compares and traps read r2 and write no GPR
        if ops[0] == "r2":
            continue               # an instruction computing r2 itself (crt0's li/oris/ori r2)
        bad.append(ins)            # r2 flows into another register (or CTR/LR, or memory)
    # No word of the loaded image (code, read-only data, data, the TOC itself) holds an address
    # inside the TOC: no static pointer to an entry exists for code to store through.
    phoff, = struct.unpack_from(">Q", b, 0x20)
    phentsize, phnum = struct.unpack_from(">HH", b, 0x36)
    into = []
    for i in range(phnum):
        pt, _pf, off, va, _pa, fsz, _msz, _al = struct.unpack_from(">IIQQQQQQ", b, phoff + i * phentsize)
        if pt != 1 or not fsz:
            continue
        seg = b[off:off + fsz]
        for k in range((-va) & 3, len(seg) - 3, 4):
            w = int.from_bytes(seg[k:k + 4], "big")
            if lo <= w < hi:
                into.append(va + k)
    if bad or into:
        print(f"  PPU_TOC_FOLD: off -- {len(bad)} instruction(s) could write or leak the TOC "
              f"({', '.join('%08X %s %s' % (i.addr, i.mnemonic, i.operands) for i in bad[:6])}), "
              f"{len(into)} word(s) of the image point into it ({', '.join('%08X' % a for a in into[:6])})")
        return
    _TOC.update(base=toc_base, lo=lo, hi=hi, image=data, fold=True)
    print(f"  PPU_TOC_FOLD: TOC 0x{toc_base:08X}, entries 0x{lo:08X}..0x{hi:08X} ({len(data)} bytes); "
          f"no store or address computation off r2 in {len(insns)} instructions")


def _toc_sub(m) -> str:
    t = _TOC
    w, reg, d = int(m.group(1)), m.group(2), int(m.group(3), 0)
    a = (t["base"] + d) & 0xFFFFFFFF
    n = w // 8
    if a % n or a < t["lo"] or a + n > t["hi"]:
        return m.group(0)
    k = (a - t["lo"]) >> 2
    v = int.from_bytes(t["image"][a - t["lo"]:a - t["lo"] + n], "big")
    STATS["toc%d" % w] = STATS.get("toc%d" % w, 0) + 1
    return ("PPU_TOC_LD32(%s, %s, %d, 0x%08Xu)" if w == 32 else "PPU_TOC_LD64(%s, %s, %d, 0x%016Xull)") % (reg, m.group(3), k, v)


def _track_a_flags() -> int:
    """What a track-A lift tells the runtime it was built with (ppu_lean_mem_register)."""
    return (1 if _lean_on() else 0) | (2 if _drain_none() else 0) | (4 if _icall_inl() else 0)


def trailer_lines() -> list:
    """Lines for the end of the last chunk: the TOC image PPU_TOC_FOLD lifted against, and what a
    track-A lift registers."""
    out = []
    f = _track_a_flags()
    if f:
        out += ["", "/* track A: what this lift was built with (1 lean memory, 2 no drains, 4 inline indirect calls) */",
                "static const int s_ppu_lean_reg = ppu_lean_mem_register(%d);" % f]
    if not _toc_on():
        return out
    t, img = _TOC, _TOC["image"]
    out += ["", "/* PPU_TOC_FOLD: the TOC bytes the folded loads assume; ppu_run compares them with guest memory */",
           "static const unsigned char s_ppu_toc_image[%d] = {" % len(img)]
    for k in range(0, len(img), 32):
        out.append("    " + ",".join("0x%02X" % c for c in img[k:k + 32]) + ",")
    out.append("};")
    out.append("static const int s_ppu_toc_reg = ppu_toc_fold_register(0x%08Xu, %du, s_ppu_toc_image, PPU_TOC_BASE);"
               % (t["lo"], len(img)))
    return out


def _drain_none() -> bool:
    return _on("PPU_DRAIN_NONE")


def _icall_inl() -> bool:
    return _on("PPU_ICALL_INLINE")


_DRAIN_NONE_C = """
/* PPU_DRAIN_NONE (track A): no function of this lift returns with the trampoline set (PPU_XTAIL), and
 * the runtime drains what an import or an indirect call leaves before it returns (ps3_hle_call,
 * ps3_indirect_call): the drain after a call has nothing to do. */
#undef DRAIN_TRAMPOLINE
#define DRAIN_TRAMPOLINE(ctx) ((void)0)
"""
# The lift's own copy of the indirect-call target cache (runtime/ppu/ppu_loader.cpp g_ppu_icall_tbl):
# 8192 words, guest address in the high half, the function's offset from function_table in the low
# half, ~0 = empty; only EBOOT functions are entered (a lifted firmware function may return with a
# trampoline pending). A hit makes the host call ps3_indirect_call's fast path made, without its
# bookkeeping (the active context -- already this one --, the depth cap and the virtual-call stack,
# both diagnostics); a miss is ps3_indirect_call, which fills the slot.
_ICALL_C = """
/* PPU_ICALL_INLINE (track A): an indirect call through the lift's target table first */
#ifdef __cplusplus
extern "C" uint64_t g_ppu_icall_tbl[8192];
#else
extern uint64_t g_ppu_icall_tbl[8192];
#endif
static inline void ppu_icall_inl(ppu_context* ctx)
{
    const uint32_t a = (uint32_t)ctx->ctr;
    const uint64_t e = __atomic_load_n(&g_ppu_icall_tbl[(a >> 2) & 8191u], __ATOMIC_RELAXED);
    if (__builtin_expect((uint32_t)(e >> 32) == a, 1)) {
        ((void (*)(ppu_context*))(void*)((const char*)(const void*)function_table + (int32_t)(uint32_t)e))(ctx);
        return;
    }
    ps3_indirect_call(ctx);
}
"""


def source_preamble(text: str) -> str:
    """The per-chunk source preamble with the switched-on helper rewrites."""
    if _drain_none():
        if not _on("PPU_XTAIL"):
            raise RuntimeError("PPU_DRAIN_NONE needs PPU_XTAIL=1 (a lift without it sets the trampoline itself)")
        if "#define DRAIN_TRAMPOLINE(ctx)" not in text:
            raise RuntimeError("PPU_DRAIN_NONE: the lifter's DRAIN_TRAMPOLINE changed; update ppu_fastpath.py")
        text += _DRAIN_NONE_C
    if _icall_inl():
        if 'extern "C" void ps3_indirect_call(ppu_context* ctx);' not in text:
            raise RuntimeError("PPU_ICALL_INLINE: the lifter's ps3_indirect_call declaration changed; update ppu_fastpath.py")
        text += _ICALL_C
    if _on("PPU_FP_ONE_NAN"):
        for old, new in ((_FP_BIN_OLD, _FP_BIN_NEW), (_FMA_OLD, _FMA_NEW), (_STFS_OLD, _STFS_NEW),
                         (_STFS_AT, _STFS_FAST + _STFS_AT)):
            if old not in text:
                raise RuntimeError("PPU_FP_ONE_NAN: the lifter's FP helper text changed; update ppu_fastpath.py")
            text = text.replace(old, new)
    if _on("PPU_VMX_SSE"):
        if _VMX_INC not in text:
            raise RuntimeError("PPU_VMX_SSE: the lifter's ppu_vmx.h include changed; update ppu_fastpath.py")
        fast = ('#define PPU_VMX_FAST 1   /* PPU_VMX_FAST (ppu3): the plain SSE result when no lane is special */\n'
                if _on("PPU_VMX_FAST") else "")
        text = text.replace(_VMX_INC, _VMX_INC + fast + '#include "ppu_vmx_sse.h"   /* PPU_VMX_SSE: same results, 4 lanes at once */\n')
    elif _on("PPU_VMX_FAST"):
        raise RuntimeError("PPU_VMX_FAST needs PPU_VMX_SSE=1")
    if _on("PPU_VM_RUNS_LOADS"):
        text += ("\n#define PPU_VM_RUNS_VERIFY 1\n" if _on("PPU_VM_RUNS_VERIFY") else "") + _RUNS_C
    if _natives():
        text += ("\n/* PPU_NATIVE_FILE (ppu3): native libc bodies, runtime/ppu/ppu_loader.cpp */\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
                 + "".join("int ppu_native_%s(ppu_context* ctx);\n" % k for k in _NATIVE_KINDS)
                 + "#ifdef __cplusplus\n}\n#endif\n")
    if _hooks():
        text += ("\n/* PPU_HOOK_FILE: run-time hook slots at function entries, runtime/ppu/ppu_loader.cpp */\n"
                 "typedef void (*ppu_hook_body)(ppu_context*);\n"
                 "typedef void (*ppu_hook_fn)(ppu_context*, ppu_hook_body);\n"
                 "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
                 "int ppu_hook_register(uint32_t addr, ppu_hook_fn volatile* slot);\n"
                 "#ifdef __cplusplus\n}\n#endif\n")
    if _patches():
        text += ("\n/* PPU_PATCH_FILE: run-time instruction alternatives, runtime/ppu/ppu_loader.cpp */\n"
                 "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
                 "extern volatile uint8_t g_ppu_patch[256];\n"
                 "int ppu_patch_register(int slot);\n"
                 "#ifdef __cplusplus\n}\n#endif\n")
    return _flow().source_preamble(text)


def _flow():
    import ppu_vmx_flow   # ppu-vmx's dataflow fast paths (all switches off = unchanged text)
    return ppu_vmx_flow


# PPU_VMX_SSE: the lifter's inline vperm (a 16-step byte loop) -> ppu_vperm_sse (two pshufb + a blend).
_OPND = r"(&(?:v\d+|ctx->vr\[\d+\]))"
_RE_VPERM = re.compile(r"\{ uint8_t\* a = \(uint8_t\*\)" + _OPND + r"; uint8_t\* b = \(uint8_t\*\)" + _OPND +
                       r"; uint8_t\* c = \(uint8_t\*\)" + _OPND + r"; uint8_t\* d = \(uint8_t\*\)" + _OPND +
                       r"; uint8_t tmp\[16\]; for \(int i = 0; i < 16; i\+\+\) \{ uint8_t sel = c\[i\] & 0x1F; "
                       r"tmp\[i\] = \(sel < 16\) \? a\[sel\] : b\[sel - 16\]; \} memcpy\(d, tmp, 16\); \}")
# vsldoi: a 32-byte stack temp -> palignr; vspltw: four word stores -> pshufd (both would
# otherwise make the next 16-byte read of the register a failed store-to-load forward).
_RE_VSLDOI = re.compile(r"\{ uint8_t tmp\[32\]; memcpy\(tmp, " + _OPND + r", 16\); memcpy\(tmp\+16, " + _OPND +
                        r", 16\); memcpy\(" + _OPND + r", tmp \+ (\d+), 16\); \}")
_RE_VSPLTW = re.compile(r"\{ uint32_t\* d = \(uint32_t\*\)" + _OPND + r"; uint32_t val = \(\(uint32_t\*\)" + _OPND +
                        r"\)\[([0-3])\]; d\[0\] = d\[1\] = d\[2\] = d\[3\] = val; \}")
# vxor / vor / vand / vandc / vsel as two 64-bit halves -> one 16-byte op (same bits).
_U = r"uint64_t\* ?{0} ?= ?\(uint64_t\*\)" + _OPND + "; "
_LOGIC = [
    (re.compile(r"\{ " + _U.format("d") + _U.format("a") + _U.format("b") +
                r"d\[0\] = a\[0\] \^ b\[0\]; d\[1\] = a\[1\] \^ b\[1\]; \}"), r"PPU_VLOGIC_SSE(\1, _mm_xor_si128, \2, \3);", "vxor"),
    (re.compile(r"\{ " + _U.format("d") + _U.format("a") + _U.format("b") +
                r"d\[0\] = a\[0\] \| b\[0\]; d\[1\] = a\[1\] \| b\[1\]; \}"), r"PPU_VLOGIC_SSE(\1, _mm_or_si128, \2, \3);", "vor"),
    (re.compile(r"\{ " + _U.format("d") + _U.format("a") + _U.format("b") +
                r"for\(int i=0;i<2;i\+\+\) d\[i\]=a\[i\]&b\[i\]; \}"), r"PPU_VLOGIC_SSE(\1, _mm_and_si128, \2, \3);", "vand"),
    (re.compile(r"\{ " + _U.format("d") + _U.format("a") + _U.format("b") +
                r"d\[0\]=a\[0\]&~b\[0\]; d\[1\]=a\[1\]&~b\[1\]; \}"), r"PPU_VLOGIC_SSE(\1, ppu_vandc_sse, \2, \3);", "vandc"),
    (re.compile(r"\{ " + _U.format("d") + _U.format("a") + _U.format("b") + _U.format("c") +
                r"d\[0\]=\(a\[0\]&~c\[0\]\)\|\(b\[0\]&c\[0\]\); d\[1\]=\(a\[1\]&~c\[1\]\)\|\(b\[1\]&c\[1\]\); \}"),
     r"PPU_VSEL_SSE(\1, \2, \3, \4);", "vsel"),
]
# vmrgh/vmrgl (words, halfwords): t = {a[s], b[s], a[s+1], b[s+1], ...} -> unpacklo/hi
_RE_VMRG = re.compile(r"\{ uint(32|16)_t\* d=\(uint\1_t\*\)" + _OPND + r"; uint\1_t\* a=\(uint\1_t\*\)" + _OPND +
                      r"; uint\1_t\* b=\(uint\1_t\*\)" + _OPND + r"; uint\1_t t\[(\d+)\]; ((?:t\[\d+\]=[ab]\[\d+\]; )+)"
                      r"memcpy\(d, t, 16\); \}")


def _vmrg(m):
    bits, d, a, b, k, body = m.group(1), m.group(2), m.group(3), m.group(4), int(m.group(5)), m.group(6)
    got = re.findall(r"t\[(\d+)\]=([ab])\[(\d+)\]; ", body)
    for s, op in ((0, "unpacklo"), (k // 2, "unpackhi")):
        want = [(str(i), "ab"[i & 1], str(s + i // 2)) for i in range(k)]
        if k == 128 // int(bits) and got == want:
            STATS["vmrg"] = STATS.get("vmrg", 0) + 1
            return "PPU_VMRG_SSE(%s, %s, %s, _mm_%s_epi%s);" % (d, a, b, op, bits)
    return m.group(0)


# lvsl / lvsr: { uint8_t b = (uint8_t)(X); for (...) ((uint8_t*)D)[i] = b + i; }
_RE_LVSL = re.compile(r"\{ uint8_t b = (\(uint8_t\)\((?:[^;{}]*)\)); for \(int i = 0; i < 16; i\+\+\) "
                      r"\(\(uint8_t\*\)" + _OPND + r"\)\[i\] = b \+ i; \}")
# lvewx: zero the register, the aligned word at ea into its lane
_RE_LVEWX = re.compile(r"\{ uint64_t ea = ([^;{}]+); ea &= ~3ULL; memset\(" + _OPND + r", 0, 16\); "
                       r"\{ uint32_t _e = VM_RAW_LD\(uint32_t, ea\); memcpy\(\(uint8_t\*\)\2 \+ \(ea & 15\), &_e, 4\); \} \}")
# lvlx / lvrx: the bytes of ea's aligned block from / up to ea, zeros elsewhere
_LVX_HEAD = (r"\{ uint64_t ea = ([^;{}]+); uint32_t sh = \(uint32_t\)\(ea & 0xF\); "
             r"uint8_t\* m = vm_base \+ \(uint32_t\)\(ea & ~0xFULL\); uint8_t\* d = \(uint8_t\*\)" + _OPND + "; ")
_RE_LVLX = re.compile(_LVX_HEAD + r"for \(int i = 0; i < 16; i\+\+\) d\[i\] = \(sh \+ \(uint32_t\)i < 16\) \? m\[sh \+ i\] : 0; \}")
_RE_LVRX = re.compile(_LVX_HEAD + r"for \(int i = 0; i < 16; i\+\+\) d\[i\] = \(\(uint32_t\)i >= 16u - sh\) "
                      r"\? m\[i - \(int\)\(16u - sh\)\] : 0; \}")
STATS: dict = {}


def _sub(ln, rx, rep, key):
    ln, k = rx.subn(rep, ln)
    if k:
        STATS[key] = STATS.get(key, 0) + k
    return ln


# PPU_NATIVE_FILE=<file> (ppu3): lifted functions that are a libc routine get a native
# body in front (runtime/ppu/ppu_loader.cpp ppu_native_*; it declines -> the lifted
# body runs). The file -- title knowledge, kept with the title -- has one
# "<hex guest address> <kind>" per line (# comments); kinds: memcpy, memset.
_NATIVE_KINDS = ("memcpy", "memset")
_native_map = None


def _natives() -> dict:
    global _native_map
    if _native_map is None:
        _native_map = {}
        path = os.environ.get("PPU_NATIVE_FILE", "")
        if path:
            for ln in open(path, encoding="utf-8"):
                ln = ln.split("#")[0].strip()
                if not ln:
                    continue
                a, k = ln.split()
                if k not in _NATIVE_KINDS:
                    raise RuntimeError(f"PPU_NATIVE_FILE: unknown kind {k!r} (have {', '.join(_NATIVE_KINDS)})")
                _native_map["func_%08X" % int(a, 16)] = k
    return _native_map


_RE_FUNC_HEAD = re.compile(r"^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{$")


# PPU_HOOK_FILE=<file> (cheats agent, 2026-11-09): a hook slot at chosen function entries,
# for a title's own run-time code (cheats, fixes). The file -- title knowledge, kept with the
# title -- has one "<hex guest address>" per line (# comments; words after the address are
# ignored). Each listed func_X becomes func_X_lifted and an entry
#     void func_X(ctx) { h = slot; if (h) { h(ctx, func_X_lifted); return; } func_X_lifted(ctx); }
# whose slot registers itself with the runtime at start (runtime/ppu/ppu_loader.cpp
# ppu_hook_register). An empty slot costs one load and a never-taken branch per call; the
# title's code fills one with ppu_hook_set(addr, fn) while the game runs (include/ps3emu/
# ppu_hooks.h). The hook sees in ctx every argument register as the call made it (r3-r10, f1-f13,
# v2-v13: the callers write those back for a hooked function, ppu_lifter's solve) and runs the body itself (ppu_hook_call: the body and its trampoline drain);
# after that ctx holds what the callers read back (r3/f1 returns).
_hook_set = None


def _hooks() -> set:
    global _hook_set
    if _hook_set is None:
        _hook_set = set()
        path = os.environ.get("PPU_HOOK_FILE", "")
        if path:
            for ln in open(path, encoding="utf-8"):
                ln = ln.split("#")[0].strip()
                if ln:
                    _hook_set.add("func_%08X" % int(ln.split()[0], 16))
    return _hook_set


def _hook_entry(name: str) -> list:
    a = name[5:]
    slot = "s_ppu_hook_" + a
    return ["static ppu_hook_fn volatile %s;" % slot,
            "static const int %s_reg = ppu_hook_register(0x%su, &%s);" % (slot, a, slot),
            "void %s(ppu_context* ctx) { ppu_hook_fn _h = %s; if (__builtin_expect(_h != 0, 0)) "
            "{ _h(ctx, %s_lifted); return; } %s_lifted(ctx); }" % (name, slot, name, name)]


def _native_wrap(lines: list) -> list:
    nat, hk = _natives(), _hooks()
    out, cur = [], None
    for ln in lines:
        m = _RE_FUNC_HEAD.match(ln) if ln.startswith("void func_") else None
        if m and (m.group(1) in nat or m.group(1) in hk):
            cur = m.group(1)
            if cur in nat and cur in hk:
                raise RuntimeError(f"{cur} is in both PPU_NATIVE_FILE and PPU_HOOK_FILE")
            out.append("void %s_lifted(ppu_context* ctx) {" % cur)
            STATS["native" if cur in nat else "hook"] = STATS.get("native" if cur in nat else "hook", 0) + 1
            continue
        out.append(ln)
        if cur and ln == "}":
            if cur in nat:
                out.append("void %s(ppu_context* ctx) { if (ppu_native_%s(ctx)) return; %s_lifted(ctx); }"
                           % (cur, nat[cur], cur))
            else:
                out += _hook_entry(cur)
            cur = None
    return out


# PPU_PATCH_FILE=<file> (cheats agent, 2026-11-09): run-time alternatives for single
# instructions. The file -- title knowledge, kept with the title -- has lines
#     <hex address> <slot 0-255> <hex original word> <hex alternative>[,<hex alternative>...]
# (# comments). The instruction is lifted as
#     if (g_ppu_patch[slot] == 1) { <alternative 1> } else if (... == 2) { ... } else { <original> }
# each side the lifter's own translation of that word, so slot value 0 (the default) runs the
# original and the title's code picks an alternative while the game runs (runtime/ppu/
# ppu_loader.cpp g_ppu_patch). Both sides must be straight-line (no branch, call, return or
# trap); the original word is checked against the EBOOT, and the lift stops on a mismatch.
_patch_map = None
_RE_XFER = re.compile(r"\bgoto\b|\breturn\b|func_[0-9A-Fa-f]{8}\(|ps3_(?:hle|indirect)_call|lv2_syscall|"
                      r"g_trampoline_fn|DRAIN_TRAMPOLINE|\bloc_[0-9A-Fa-f]{8}")


def _patches() -> dict:
    global _patch_map
    if _patch_map is None:
        _patch_map = {}
        path = os.environ.get("PPU_PATCH_FILE", "")
        if path:
            for ln in open(path, encoding="utf-8"):
                ln = ln.split("#")[0].strip()
                if not ln:
                    continue
                f = ln.split()
                if len(f) != 4:
                    raise RuntimeError(f"PPU_PATCH_FILE: '{ln}': want <addr> <slot> <orig> <alt>[,<alt>...]")
                a, slot, orig = int(f[0], 16), int(f[1], 0), int(f[2], 16)
                alts = [int(w, 16) for w in f[3].split(",") if w]
                if not 0 <= slot < 256 or not alts or a in _patch_map:
                    raise RuntimeError(f"PPU_PATCH_FILE: bad or repeated line '{ln}'")
                _patch_map[a] = (slot, orig, alts)
    return _patch_map


def patch_line(lifter, insn, func, c_line: str) -> str:
    """The lifted statement of `insn`, with its PPU_PATCH_FILE alternatives if it has any."""
    p = _patches().get(insn.addr)
    if p is None:
        return c_line
    slot, orig, alts = p
    if insn.raw != orig:
        raise RuntimeError(f"PPU_PATCH_FILE: 0x{insn.addr:08X} holds {insn.raw:08X}, not {orig:08X} "
                           f"(another EBOOT?)")
    from ppu_disasm import decode
    sides = []
    for w in alts + [None]:
        code = c_line if w is None else lifter._translate(decode(w, insn.addr), func)
        code = (code or "").strip() or ";"
        if _RE_XFER.search(code):
            raise RuntimeError(f"PPU_PATCH_FILE: 0x{insn.addr:08X}: {'the original' if w is None else '%08X' % w} "
                               f"is not straight-line code: {code[:80]}")
        sides.append(code)
    STATS["patch"] = STATS.get("patch", 0) + 1
    out = " else ".join("if (__builtin_expect(g_ppu_patch[%d] == %d, 0)) { %s }" % (slot, k + 1, s)
                        for k, s in enumerate(sides[:-1]))
    return out + " else { %s }" % sides[-1]


_RE_PATCH_SLOT = re.compile(r"g_ppu_patch\[(\d+)\] == 1\b")


def rewrite_body(lines: list) -> list:
    """A chunk's function lines with the switched-on statement rewrites."""
    lines = _rewrite_body0(lines)
    if _on("PPU_ST_RUNS"):   # (cg1) last: on the final text of the stores
        if not (_on("PPU_ST_PMAP") and _on("PPU_MOVBE")):
            raise RuntimeError("PPU_ST_RUNS needs PPU_ST_PMAP=1 and PPU_MOVBE=1")
        lines = store_runs(lines, STATS)
    return lines


def _rewrite_body0(lines: list) -> list:
    if _drain_none() and any("g_trampoline_fn = (void" in ln for ln in lines):
        raise RuntimeError("PPU_DRAIN_NONE: a function of this lift sets the trampoline (a tail jump PPU_XTAIL "
                           "did not make a tail call); the drains cannot be left out")
    if _icall_inl():
        k = 0
        for i, ln in enumerate(lines):
            if "ps3_indirect_call(ctx);" in ln:
                lines[i] = ln.replace("ps3_indirect_call(ctx);", "ppu_icall_inl(ctx);")
                k += 1
        STATS["icall_inl"] = STATS.get("icall_inl", 0) + k
    if _natives() or _hooks():
        lines = _native_wrap(lines)
    if _patches():   # the instruction slots this chunk lifted: the runtime can tell they exist
        slots = sorted({int(s) for ln in lines if "g_ppu_patch[" in ln for s in _RE_PATCH_SLOT.findall(ln)})
        lines = lines + ["static const int s_ppu_patch_reg_%d = ppu_patch_register(%d);" % (s, s) for s in slots]
    if _toc_on():   # PPU_TOC_FOLD: TOC entry loads -> the entry's value while the runtime vouches for it
        lines = [_RE_TOC_LD.sub(_toc_sub, ln) if "vm_read" in ln and "2" in ln else ln for ln in lines]
    if not _on("PPU_VMX_SSE"):
        return _flow().rewrite(lines)
    out = []
    for ln in lines:
        if "sel < 16" in ln:
            ln = _sub(ln, _RE_VPERM, r"ppu_vperm_sse(\4, \1, \2, \3);", "vperm")
        if "tmp[32]" in ln:
            ln = _sub(ln, _RE_VSLDOI, r"PPU_VSLDOI_SSE(\3, \1, \2, \4);", "vsldoi")
        if "d[0] = d[1] = d[2] = d[3] = val" in ln:
            ln = _sub(ln, _RE_VSPLTW, r"PPU_VSPLTW_SSE(\1, \2, \3);", "vspltw")
        if "(uint64_t*)&" in ln:
            for rx, rep, key in _LOGIC:
                ln = _sub(ln, rx, rep, key)
        if "memcpy(d, t, 16)" in ln:
            ln = _RE_VMRG.sub(_vmrg, ln)
        if "] = b + i; }" in ln:
            ln = _sub(ln, _RE_LVSL, r"{ uint8_t b = \1; PPU_LVSL_SSE(\2, b); }", "lvsl")
        if "ea &= ~3ULL; memset(" in ln:
            ln = _sub(ln, _RE_LVEWX, r"{ uint64_t ea = \1; ea &= ~3ULL; ppu_lvewx_sse(\2, ea); }", "lvewx")
        if "uint8_t* m = vm_base" in ln:
            ln = _sub(ln, _RE_LVLX, r"ppu_lvlx_sse(\2, \1);", "lvlx")
            ln = _sub(ln, _RE_LVRX, r"ppu_lvrx_sse(\2, \1);", "lvrx")
        if "vm_raw_" in ln:
            ln = _sub(ln, re.compile(r"\bvm_raw_ld16\("), "ppu_vld16_sse(", "lvx")
            ln = _sub(ln, re.compile(r"\bvm_raw_st16\("), "ppu_vst16_sse(", "stvx")
        out.append(ln)
    return _flow().rewrite(out)
