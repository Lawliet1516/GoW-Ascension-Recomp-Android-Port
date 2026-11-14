/* SPU guest-I/O journal hooks (the title's function recorder, spu_fnrec.c).
 *
 * Every way an SPU job reaches outside its context goes through four choke
 * points: a DMA transfer (mfc_do_transfer, list elements included), a lock-line
 * atomic (spu_mfc_atomic), a channel read or count (spu_rdch / spu_rchcnt) and
 * an interrupt being taken (spu_take_interrupt). While a thread's t_spu_iojrn
 * is set, each of them calls the hook instead of doing the work itself; the
 * hook gets the real operation as `real` and either runs it and logs what it
 * saw (recording) or serves the result from a journal (replay). NULL (the
 * default, every thread, always outside a recording): one TLS test per site.
 *
 * The variables are DEFINED by the title (host/spu/spu_fnrec.c), so a title
 * built against a runtime without these hooks still links; the runtime sets
 * g_spu_iojrn_rt in a constructor to say the hooks are there. Keep this layout
 * in step with the copy in spu_fnrec.c (SPU_IOJRN_VERSION).
 */
#ifndef SPU_IOJRN_H
#define SPU_IOJRN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

#define SPU_IOJRN_VERSION 1

typedef int      (*spu_iojrn_dma_fn)(struct spu_context*, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd);
typedef int      (*spu_iojrn_atomic_fn)(struct spu_context*, uint32_t cmd);
typedef uint32_t (*spu_iojrn_ch_fn)(struct spu_context*, uint32_t channel);

typedef struct spu_iojrn_ops {
    int      (*dma)(struct spu_context*, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd, spu_iojrn_dma_fn real);
    int      (*atomic)(struct spu_context*, uint32_t cmd, spu_iojrn_atomic_fn real);
    uint32_t (*rdch)(struct spu_context*, uint32_t channel, spu_iojrn_ch_fn real);
    uint32_t (*rchcnt)(struct spu_context*, uint32_t channel, spu_iojrn_ch_fn real);
    void     (*irq)(struct spu_context*, uint32_t srr0, uint32_t target);
    /* optional (NULL: none): every trampoline step of spu_drain_call's loop,
     * before it runs (a trace of the path taken) */
    void     (*step)(struct spu_context*, void (*fn)(struct spu_context*));
    /* optional: a SPURS kernel service the HLE answers from main memory
     * (selectWorkload, 0x9D0: LS 0x100-0x17F and r3); real does it */
    void     (*ksvc)(struct spu_context*, uint32_t pc, void (*real)(struct spu_context*));
} spu_iojrn_ops;

/* What the runtime offers the recorder (NULL: a runtime without the hooks). */
typedef struct spu_iojrn_rt {
    int version;
    uint32_t (*tag_completed_get)(struct spu_context*);
    void     (*tag_completed_set)(struct spu_context*, uint32_t);
} spu_iojrn_rt;

#if defined(_MSC_VER) || defined(_WIN32)
extern __declspec(thread) const spu_iojrn_ops* t_spu_iojrn;
#else
extern __thread const spu_iojrn_ops* t_spu_iojrn;
#endif
extern const spu_iojrn_rt* g_spu_iojrn_rt;

#ifdef __cplusplus
}
#endif

#endif
