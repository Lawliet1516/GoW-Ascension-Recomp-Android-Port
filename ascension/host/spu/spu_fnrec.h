/* Lifted SPU functions with a native replacement, recorded and replayed.
 *
 * tools/spu_wrap.py renames chosen lifted functions to <name>_lifted and puts
 * a wrapper under the original name that calls spu_fn_dispatch. The dispatch
 * runs the lifted code, the native version (when there is one and it is
 * enabled), or records the call.
 *
 * Switches (environment):
 *   SPU_NATIVE=0                 never run native code
 *   SPU_NATIVE=218:73E0,218:9090 only these (default: every native there is)
 *   SPU_FN_RECORD=<dir>          record calls: the whole spu_context on entry
 *                                and on return, plus the decrementer values
 *                                read in between, one .fnr file per call
 *   SPU_FN_RECORD_FUNCS=218:73E0,... which ones (default: all wrapped, except
 *                                the whole-job entry 218:AD98, the trace-only
 *                                wrappers and the fragments with no native,
 *                                which are recorded only when listed)
 *   SPU_FN_RECORD_MAX=<n>        calls per function (default 40)
 *   SPU_FN_RECORD_EVERY=<n>      every n-th call (default 97)
 *   SPU_FN_RECORD_AFTER_MS=<ms>  start this long after the first call
 *   SPU_FN_RECORD_IO=1           FNR2 files: also journal every guest-memory
 *                                access of the run (DMA GET/PUT with their
 *                                bytes, lock-line atomics, channel reads,
 *                                interrupts taken), plus the build id and the
 *                                float modes. Needs a runtime with the
 *                                spu_iojrn.h hooks; FNR1 otherwise.
 *   SPU_FN_REPLAY=<dir>          (at startup, instead of the game) run every
 *                                .fnr in <dir> through the lifted code and the
 *                                native code and compare with the recording;
 *                                SPU_FN_REPLAY_FUNCS filters, SPU_FN_REPLAY_REPS
 *                                repeats each run for timing. FNR2 files are
 *                                served from their journal (no main memory):
 *                                a GET gets the recorded bytes, a PUT and every
 *                                atomic store is compared, channel reads come
 *                                from the journal. A whole job (218:AD98) has
 *                                no native: its "native" run is the lifted job
 *                                with the existing natives inside it.
 *
 * Whole EDGE jobs (module 218, entry 0xAD98, one call per job from the WWS
 * policy module). Each of these turns on the whole-job wrapper (the job's
 * trampolines are drained inside the wrapper, so it sees the job end):
 *   SPU_EDGE_VARIANTS=1          per-job variant key (which wrapped functions
 *                                and fragments ran, and the parameters that
 *                                pick their paths); every 5 s the top keys by
 *                                job count with their share of jobs and time:
 *                                [edge-var] lines (=2: DCC0's exact bits
 *                                per index instead of its unpack path)
 *   SPU_FN_TRACE_JOB=<n>         with SPU_FN_RECORD on 218:AD98: for the first
 *                                n recorded jobs, a .trace.txt beside the .fnr
 *                                with every wrapped function's entry and exit
 *                                (TSC from the job start) and every journal
 *                                entry, in order
 *   SPU_FN_JOB_DRAIN=1           the whole-job wrapper alone
 */
#ifndef ASCENSION_SPU_FNREC_H
#define ASCENSION_SPU_FNREC_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_fn_t)(spu_context* ctx);

typedef struct spu_fn_desc {
    int image;
    uint32_t addr;
    const char* name;
    spu_fn_t lifted;
    spu_fn_t native;          /* NULL: no native version */
    volatile int mode;        /* -1 not yet decided, 0 lifted, 1 record, 2 native */
    volatile long calls;
    volatile long recorded;
    int idx;                  /* SPU_EDGE_VARIANTS bit (1-based, 0 = not yet); the
                               * generated initializers leave it zero */
} spu_fn_desc;

/* The wrapper's body. */
void spu_fn_dispatch(spu_fn_desc* d, spu_context* ctx);

/* A native version of the lifted function at (image, LS address). Use the
 * macro once per native, in the file that defines it:
 *   void spu_native_218_000073E0(spu_context* ctx) { ... }
 *   SPU_NATIVE_REGISTER(218, 0x73E0, spu_native_218_000073E0)
 * It must leave the context exactly as the lifted function does: every
 * register, the whole local store, pc and status (SPU_FN_REPLAY checks). */
void spu_native_register(int image, uint32_t addr, spu_fn_t fn);
#define SPU_NATIVE_REGISTER(image, addr, fn)                                  \
    __attribute__((constructor)) static void fn##_register(void)              \
    { spu_native_register((image), (addr), (fn)); }

/* Each wrapped image says how it was built (tools/spu_wrap.py emits the call):
 * src_hash = FNV-1a 64 of its lifted source as lifted (hex), float_mode =
 * SPU_FNREC_FLOAT_MODE as seen in that file. FNR2 headers carry both (the
 * build id is a hash over all of them). */
void spu_fnrec_note_image(int image, const char* src_hash, int float_mode);
#if defined(SPU_XFLOAT_APPROX)
#  define SPU_FNREC_FLOAT_BASE 2      /* approximate extended-range float ops */
#elif defined(SPU_XFLOAT_RESULT_CHECK)
#  define SPU_FNREC_FLOAT_BASE 1      /* exact, result-checked */
#else
#  define SPU_FNREC_FLOAT_BASE 0
#endif
#if defined(__FMA__)
#  define SPU_FNREC_FLOAT_MODE (SPU_FNREC_FLOAT_BASE | 0x10)   /* fma/fms as one vfmadd */
#else
#  define SPU_FNREC_FLOAT_MODE SPU_FNREC_FLOAT_BASE
#endif

/* For a whole-job native (host/spu/natives/edgejob/n218_edgejob.c): the native
 * the dispatch would run for (image, addr) or NULL; whether the job wrapper
 * drains the job itself; whether the job is watched (variants, trace,
 * recording: run it lifted); whether this thread runs a forced-native replay. */
spu_fn_t spu_fn_native_for(int image, uint32_t addr);
int spu_fnrec_job_drain(void);
int spu_fnrec_job_watched(void);
int spu_fnrec_replay_native(void);
uint64_t spu_fnrec_image_hash(int image);   /* as noted by spu_fnrec_note_image, 0: none */

/* Replay mode (SPU_FN_REPLAY): returns the process exit code, or -1 when the
 * variable is not set (boot the game normally). */
int spu_fnrec_replay_main(void);

#ifdef __cplusplus
}
#endif

#endif
