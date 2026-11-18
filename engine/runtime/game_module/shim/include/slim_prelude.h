/* slim prototype: force-included into every lifted TU instead of win32_posix_min.h.
   PPU_FENCE as the compiler builtin, so ppu_recomp.h needs no <atomic> (the C++ library). */
#pragma once
#define SLIM_MO_relaxed __ATOMIC_RELAXED
#define SLIM_MO_consume __ATOMIC_CONSUME
#define SLIM_MO_acquire __ATOMIC_ACQUIRE
#define SLIM_MO_release __ATOMIC_RELEASE
#define SLIM_MO_acq_rel __ATOMIC_ACQ_REL
#define SLIM_MO_seq_cst __ATOMIC_SEQ_CST
#define PPU_FENCE(o) __atomic_thread_fence(SLIM_MO_##o)
