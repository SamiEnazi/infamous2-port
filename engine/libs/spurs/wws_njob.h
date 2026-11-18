/* wws_njob.h -- native job modules (Track B2): a C function on guest memory in place of a lifted SPU job.
 *
 * The native job runner (wws_runner.c, Track B1) claims a job, stages it into a host SPU context and runs the
 * lifted job module there (wws_native_run). A NATIVE job module skips all of that: the runner decodes the
 * job's descriptor (its UseBuffer commands name the inputs in main memory, ReqDepDec its dependency cells,
 * RunJob its parameters) and calls the module's C function, which reads its inputs straight from guest memory
 * (big-endian), computes, and writes its outputs straight to guest memory through the services below (the
 * same PUT / atomic semantics the MFC gives the lifted job). No local store, no DMA, no SPU channels.
 *
 * Contract of run(): return 1 = the job is done, every output written; 0 = declined, NOTHING written (the
 * runner then runs the lifted job, which is always correct). A module therefore computes first and writes last.
 *
 * The same run() is checked bit for bit against the job recordings (spu_jobrec.c, PS3_JOBREPLAY: the "native
 * pass" builds the wws_njob from a recording's start state and serves read/put/atomic_add from the recording),
 * and live against the lifted job (PS3_NATIVE_CULL=2: capture, run lifted, verify()).
 *
 * Switches (wws_njob.c): PS3_NATIVE_CULL=0|1|2|<modules>  (default 1 = every registered native module;
 *   "210,208" = only those; 2 = verify mode: the native runs in capture mode, then the lifted job runs and its
 *   outputs are compared with the capture; every job's outputs are the lifted ones). Only with the runner
 *   (PS3_NATIVE_JOBS=1): SPURS-run jobs are never replaced.
 *   PS3_NATIVE_CULL_STATS=0|1 (default 1): a line per 10 s per module: jobs/s, us per job, fallbacks by reason.
 */
#ifndef WWS_NJOB_H
#define WWS_NJOB_H

#include <stdint.h>
#include "wws_native.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One UseBuffer of the descriptor, by buffer set (the job's getbuf(set) names the same buffer). */
typedef struct wws_nbuf {
    uint32_t ea;           /* the command's EA: the data (plain load) or the DMA list (list load); 0 = none */
    uint32_t size;         /* bytes (the command's size16 << 4): the load or the list */
    uint8_t  used, load, list, share;
    uint8_t  pages, nbufs;     /* the set's ReserveBufSet: pages (1 KB) per buffer, buffers */
} wws_nbuf;

#define WWS_NJOB_SETS 16

typedef struct wws_njob wws_njob;
struct wws_njob {
    int module;
    const wws_job* job;                  /* the entry (NULL in the replay) */
    wws_nbuf buf[WWS_NJOB_SETS];
    const uint8_t* params;               /* the RunJob parameters (host pointer, big-endian), params_len bytes */
    uint32_t params_len;
    uint32_t n_deps, dep_ea[4];          /* the descriptor's ReqDepDec cells (the runner decrements them) */
    /* services: guest memory in the game, the recording in the replay */
    const uint8_t* (*read)(wws_njob* nj, uint32_t ea, uint32_t size);   /* host pointer of [ea, ea+size), NULL = not readable */
    /* a PUT as the MFC does it (rules, coherence, put hook). dontcare: NULL, or `size` bytes, nonzero = a byte the
     * lifted job leaves as stale local store (undefined on the PS3): the checks do not compare it */
    void (*put)(wws_njob* nj, uint32_t ea, const uint8_t* src, uint32_t size, const uint8_t* dontcare);
    uint32_t (*atomic_add)(wws_njob* nj, uint32_t ea, uint32_t v);     /* big-endian u32 at ea += v (a lock-line update); old */
    void* svc;
    int capture;                         /* verify mode: put / atomic_add only note (nothing is written) */
    /* why a run declined (set by the module) */
    uint32_t fallback;                   /* the module's reason bits */
    uint32_t bad_ea, bad_size, bad_op;
};

/* A native job module. */
typedef struct wws_nmod {
    int module;                          /* 200..226 */
    const char* name;
    int (*run)(wws_njob* nj);
    /* verify mode, after the lifted job ran on `ls` (the worker's local store; buf_ls[set] = each set's buffer
     * address there): 0 = the lifted job left what the native captured, else a module code (>0) */
    int (*verify)(wws_njob* nj, const uint8_t* ls, const uint32_t buf_ls[WWS_NJOB_SETS]);
    const char* const* reasons;          /* names of the fallback bits, NULL-terminated */
    uint32_t list_sets;                  /* bit s = buffer set s is a DMA-list load (the replay rebuilds its inputs by it) */
} wws_nmod;

const wws_nmod* wws_nmod_find(int module);          /* a registered module, NULL = none */
int  wws_nmod_enabled(int module);                   /* PS3_NATIVE_CULL lets this module run natively */
int  wws_nmod_mode(void);                            /* 0 off, 1 on, 2 verify */

/* Decode a descriptor in guest memory into nj (buffers, parameters). 0 = a form the natives do not take. */
int  wws_njob_decode(wws_njob* nj, const wws_job* j);

/* wws_runner.c's run_one: try the native module of `module` for this job. 1 = done natively (out->n_deps /
 * dep_ea filled from the descriptor, out->ok = 1); 0 = run the lifted job (in verify mode: the lifted job was
 * run here already when the return is 2: out is its result). */
int  wws_njob_try(wws_worker* w, const wws_job* j, int module, wws_result* out);

/* The stats line (wws_runner.c calls it from its pool; it prints itself every 10 s). */
void wws_njob_stats_tick(void);

/* wws_native.c: the local-store address of set `set`'s buffer after the last run on this worker (0 = none) */
uint32_t wws_worker_buffer_ls(wws_worker* w, uint32_t set);

#ifdef __cplusplus
}
#endif

#endif /* WWS_NJOB_H */
