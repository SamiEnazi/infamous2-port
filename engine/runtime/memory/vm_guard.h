/* PPU_LEAN_MEM (track A, 2026-11-16): the guest window's special pages, off the access fast path.
 *
 * A lean lift (tools/ppu_fastpath.py PPU_LEAN_MEM) loads and stores guest memory as one host access
 * at vm_base + ea: no range check, no gcm poll-word compare. What those checks used to send to the
 * out-of-line accessors becomes a page the host's MMU refuses, and this module's exception handler
 * (a Win32 vectored handler; on POSIX the same handler through runtime/platform/win32_compat.c's
 * signal dispatch) finishes the access the way the out-of-line accessor would have and resumes
 * after the instruction:
 *
 *   VMG_NULL_PAGE  guest 0x0000..0x0FFF is read-only (it reads as the zeros it holds: nothing can
 *                  store there). A store is DROPPED, as vm_write* drops one (vm_null_store: the
 *                  hardware faults it), and reported.
 *   VMG_RAW_SPU    the raw-SPU problem-state halves of the six windows at 0xE0000000 + n MB
 *                  (+0x40000 .. +0x100000) are no-access; each load or store is handed to the
 *                  callbacks (runtime/spu/spu_raw.c: the register side effects + the registers'
 *                  own memory, kept beside the window while it is guarded).
 *
 * The handler decodes the faulting instruction (the forms clang emits for a guest access: mov,
 * movzx/movsx/movsxd, movbe, mov-immediate; vector and read-modify-write forms only as far as
 * their length, so a dropped store can be skipped). An instruction it cannot finish is never
 * guessed at: the page is opened (read-write, plain memory) and the access re-runs, with a report.
 */
#ifndef VM_GUARD_H
#define VM_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMG_NULL_PAGE 1u
#define VMG_RAW_SPU   2u

/* The raw-SPU window callbacks: n = 1, 2, 4 or 8 bytes; *val / val = the value as the guest sees
 * it (big-endian memory read as a number). Return 1 when done. */
typedef int (*vmg_load_fn)(uint32_t ea, unsigned n, uint64_t* val);
typedef int (*vmg_store_fn)(uint32_t ea, unsigned n, uint64_t val);

/* Arms the pages of `flags` on the guest window at vb (once; later calls add flags). 0 = done. */
int vm_guard_install(uint8_t* vb, unsigned flags, vmg_load_fn raw_load, vmg_store_fn raw_store);
/* Which of VMG_* are armed. */
unsigned vm_guard_armed(void);
/* Faults the handler finished: [0] null-page stores dropped, [1] raw-SPU accesses emulated,
 * [2] pages opened because an instruction could not be finished. */
void vm_guard_counts(uint64_t out[3]);

/* The decoder, exposed for runtime/memory/tests/test_vm_guard.c. */
enum { VMG_UNKNOWN = 0, VMG_LOAD, VMG_STORE, VMG_STORE_IMM, VMG_WRITE_OTHER, VMG_READ_OTHER };
typedef struct vmg_insn {
    uint8_t  len;     /* bytes; 0 = not decoded */
    uint8_t  op;      /* VMG_* above */
    uint8_t  width;   /* bytes of memory accessed (LOAD / STORE / STORE_IMM) */
    uint8_t  reg;     /* the general register loaded or stored (0 = rax .. 15 = r15) */
    uint8_t  high8;   /* reg is ah/ch/dh/bh (an 8-bit access without REX) */
    uint8_t  dstw;    /* LOAD: the destination's width in bytes */
    uint8_t  sx;      /* LOAD: sign-extended (movsx, movsxd) */
    uint8_t  be;      /* movbe: the value is byte-swapped on its way */
    uint64_t imm;     /* STORE_IMM: the immediate, as stored (raw little-endian bytes) */
} vmg_insn;
int vm_guard_decode(const uint8_t* code, vmg_insn* out);

#ifdef __cplusplus
}
#endif

#endif /* VM_GUARD_H */
