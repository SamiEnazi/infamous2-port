/* wws_native.h -- the WWS SPU job manager's per-job work done in host C (Track B1, the native job runner).
 *
 * A WWS job is an entry in a job manager's array: (len << 16 | type) : descriptor EA. The PS3 runs it with
 * the "Wws_Job" SPURS policy module (an SPU program, lifted at work/port/spu/wwsjob_pm_at_008E5B00): the
 * module claims the entry, DMAs the descriptor's command list into its local store, executes the commands
 * (reserve buffer sets, use buffers = load their input from main memory, request dependency decrements,
 * run job), calls the job module's entry and, when the job returns, frees its buffers, publishes the
 * completion and decrements the dependency cells. wws_native_run() does all of that for one job on a host
 * thread, in C, with the same local-store layout the job sees under the policy module (the module's own
 * tables at LS 0xDF0.., the job context and API table at LS 0x1330.., the job code at the page RunJob
 * names). The job module itself is still the lifted SPU code; the job's calls back into the job manager
 * (its API: get buffer, allocate / free a DMA tag, load next job) land on the C versions registered here
 * under image id WWS_NATIVE_IMAGE. No SPURS kernel and no lifted job manager code runs.
 *
 * Completion is NOT published here: the caller (wws_runner.c) owns the manager's completion words.
 */
#ifndef WWS_NATIVE_H
#define WWS_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

#define WWS_NATIVE_IMAGE 199          /* the image id the C job-manager API registers under */
#define WWS_PM_EA        0x008E5B00u  /* the policy module image in guest memory (LS 0xA00..0x3880) */
#define WWS_PM_SIZE      0x2E80u
#define WWS_JOB_RET_LS   0x3218u      /* the job's return address (the policy module's bisl) */

/* One job, as the claim would have returned it. */
typedef struct wws_job {
    uint32_t ctrl_ea;      /* the manager (its first 128-byte line is the "control" line) */
    uint32_t slot;         /* the manager's array 0/1 */
    uint32_t index;        /* entry index c in that array */
    uint32_t entry_hi;     /* (len << 16) | type */
    uint32_t desc_ea;      /* the descriptor (128-aligned) */
    uint32_t pair_hi, pair_lo;   /* the other 8 bytes of the entry's 16-byte quad (the claim's r3 words 2-3) */
    uint32_t wid;          /* the manager's SPURS workload id (u32[mgr+136]) */
    uint32_t spurs_ea;     /* the SPURS instance (u32[mgr+132]) */
} wws_job;

/* What a run left behind for the caller. */
typedef struct wws_result {
    int      ok;           /* 1 = the job ran and returned; 0 = refused (see why) */
    int      module;       /* the job module's image id (200..226), 0 if unknown */
    uint32_t n_deps;       /* dependency cells named by ReqDepDec (decremented by the caller) */
    uint32_t dep_ea[4];
    const char* why;       /* a refusal's reason */
} wws_result;

/* A host-side SPU: one per pool thread (and one per helping PPU thread). */
typedef struct wws_worker wws_worker;

wws_worker* wws_worker_create(uint32_t spu_num);   /* spu_num: the SPU number the job reads at API+0 (0..5) */
void        wws_worker_set_spu_num(wws_worker* w, uint32_t spu_num);
struct spu_context* wws_worker_ctx(wws_worker* w);

/* Stage the job (command list, buffers, code), run its module, retire its buffers. Synchronous. */
void wws_native_run(wws_worker* w, const wws_job* job, wws_result* out);

/* Atomically decrement the big-endian u16 at `cell` (a barrier cell) the way the policy module's
 * retire did (a lock-line update). Returns the value BEFORE the decrement. */
uint32_t wws_cell_dec(uint32_t cell);

/* A lock-line update of guest memory with the semantics of a successful PUTLLC: the 128-byte line
 * that holds [ea, ea+n) is locked, `fn` changes guest bytes in it, SPU reservations on the line are
 * broken and the line-commit hook runs. */
void wws_line_update(uint32_t ea, void (*fn)(uint8_t* line, void* arg), void* arg);

/* Register the C API (image WWS_NATIVE_IMAGE) in the SPU function registry. Called by the first worker and by
 * the job replay: AFTER the game module registered its images, so a wildcard (image 0) lookup of one of these
 * low addresses still finds the module's function first. */
void wws_native_api_register(void);

/* Statistics (PS3_NATIVE_JOBS_STATS). */
void wws_native_stats(uint64_t* jobs, uint64_t* code_reuse, uint64_t* code_loads);

#ifdef __cplusplus
}
#endif

#endif /* WWS_NATIVE_H */
