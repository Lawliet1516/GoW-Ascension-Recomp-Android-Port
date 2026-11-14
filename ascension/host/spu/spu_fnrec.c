/* Lifted SPU functions with native replacements: dispatch, recording and
 * replay. See spu_fnrec.h for the switches. */
#include "spu_fnrec.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <xmmintrin.h>
#include <intrin.h>

typedef void (*spu_fn)(spu_context*);
extern spu_fn spu_lookup(uint32_t addr, int image_id);
extern void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size);

/* ---- guest-I/O journal hooks (runtime spu_iojrn.h) -------------------------
 * The variables live here, so this file links against a runtime without the
 * hooks too; such a runtime never sets g_spu_iojrn_rt, and SPU_FN_RECORD_IO
 * then falls back to FNR1. */
#if defined(__has_include)
#  if __has_include("spu_iojrn.h")
#    include "spu_iojrn.h"
#    define FNR_HAVE_IOJRN_H 1
#  endif
#endif
#ifndef FNR_HAVE_IOJRN_H
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
    void     (*step)(struct spu_context*, void (*fn)(struct spu_context*));
    /* optional: a SPURS kernel service the HLE answers from main memory
     * (selectWorkload, 0x9D0: LS 0x100-0x17F and r3); real does it */
    void     (*ksvc)(struct spu_context*, uint32_t pc, void (*real)(struct spu_context*));
} spu_iojrn_ops;
typedef struct spu_iojrn_rt {
    int version;
    uint32_t (*tag_completed_get)(struct spu_context*);
    void     (*tag_completed_set)(struct spu_context*, uint32_t);
} spu_iojrn_rt;
#endif
SPU_THREAD_LOCAL const spu_iojrn_ops* t_spu_iojrn;
const spu_iojrn_rt* g_spu_iojrn_rt;

/* ---- which version runs ------------------------------------------------ */

/* "all", "0", or "218:73E0,218:9090" (image:hex address). */
static int list_has(const char* list, const spu_fn_desc* d)
{
    if (!list || !*list) return 0;
    if (!strcmp(list, "all") || !strcmp(list, "1")) return 1;
    const char* p = list;
    while (*p) {
        char* end;
        long img = strtol(p, &end, 10);
        if (*end == ':') {
            unsigned long a = strtoul(end + 1, &end, 16);
            if (img == d->image && a == d->addr) return 1;
        }
        while (*end && *end != ',') end++;
        p = *end == ',' ? end + 1 : end;
    }
    return 0;
}

/* Native versions register themselves (SPU_NATIVE_REGISTER in spu_fnrec.h),
 * so each lives in its own file and nothing else has to list it. */
static struct { int image; uint32_t addr; spu_fn_t fn; } s_natives[512];
static int s_nnatives;

void spu_native_register(int image, uint32_t addr, spu_fn_t fn)
{
    if (s_nnatives < 512) {
        s_natives[s_nnatives].image = image;
        s_natives[s_nnatives].addr = addr;
        s_natives[s_nnatives].fn = fn;
        s_nnatives++;
    }
}

static spu_fn_t native_find(int image, uint32_t addr)
{
    for (int i = 0; i < s_nnatives; i++)
        if (s_natives[i].image == image && s_natives[i].addr == addr) return s_natives[i].fn;
    return NULL;
}

/* The whole-EDGE-job entry, and the wrappers that exist only for the per-job
 * trace and the variant key (and fragments with no native yet): a default
 * "record everything" leaves these out, as before they were wrapped. */
static int fnw_is_job(const spu_fn_desc* d) { return d->image == 218 && d->addr == 0xAD98u; }
static int fnw_explicit_only(const spu_fn_desc* d)
{
    static const uint32_t s_218[] = { 0xAD98, 0x4030, 0x4110, 0x41A0, 0x44F0, 0x5110, 0x51D0, 0x5E30, 0x5E80,
                                      0x61B0, 0x67C0, 0x88B0, 0x9080, 0xAF38, 0xB700, 0xB9D0, 0xC340, 0xC490,
                                      0xC628, 0xC9A8, 0xCC98, 0xD618, 0xE4D0 };
    if (d->image == 218) {
        for (size_t i = 0; i < sizeof s_218 / sizeof s_218[0]; i++) if (s_218[i] == d->addr) return 1;
        return 0;
    }
    return d->image == 310 || d->image == 318 || d->image == 329 || d->image == 330;
}

static void fnw_init(void);
static int decide_mode(spu_fn_desc* d)
{
    fnw_init();
    if (!d->native) d->native = native_find(d->image, d->addr);
    const char* rec = getenv("SPU_FN_RECORD");
    if (rec && *rec) {
        const char* f = getenv("SPU_FN_RECORD_FUNCS");
        if (f ? list_has(f, d) : !fnw_explicit_only(d)) return 1;
    }
    if (d->native) {
        const char* n = getenv("SPU_NATIVE");
        if (!n || list_has(n, d)) return 2;
    }
    return 0;
}

/* Replay forces a version per call: -1 none, 0 lifted, 2 native. */
static SPU_THREAD_LOCAL int t_force = -1;
static SPU_THREAD_LOCAL spu_fn_desc* t_last_desc;

/* ---- image notes: build id and float modes ------------------------------- */

typedef struct { int image; int mode; uint64_t hash; } fnr_img;
static fnr_img s_imgs[64];
static int s_nimgs;

void spu_fnrec_note_image(int image, const char* src_hash, int float_mode)
{
    if (s_nimgs >= 64) return;
    for (int i = 0; i < s_nimgs; i++) if (s_imgs[i].image == image) return;
    s_imgs[s_nimgs].image = image;
    s_imgs[s_nimgs].mode = float_mode;
    s_imgs[s_nimgs].hash = src_hash ? strtoull(src_hash, 0, 16) : 0;
    s_nimgs++;
}

static uint64_t fnv64(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

/* Over the images in id order: two FNV-1a 64 streams with different seeds. */
static void fnr_build_id(uint8_t out[16])
{
    fnr_img v[64]; int n = s_nimgs;
    memcpy(v, s_imgs, sizeof(fnr_img) * (size_t)n);
    for (int i = 1; i < n; i++) for (int k = i; k > 0 && v[k - 1].image > v[k].image; k--) {
        fnr_img t = v[k]; v[k] = v[k - 1]; v[k - 1] = t; }
    uint64_t a = 0xCBF29CE484222325ull, b = 0x84222325CBF29CE4ull;
    for (int i = 0; i < n; i++) {
        a = fnv64(a, &v[i].image, 4); a = fnv64(a, &v[i].mode, 4); a = fnv64(a, &v[i].hash, 8);
        b = fnv64(b, &v[i].hash, 8); b = fnv64(b, &v[i].image, 4); b = fnv64(b, &v[i].mode, 4);
    }
    memcpy(out, &a, 8); memcpy(out + 8, &b, 8);
}

static int fnr_img_mode(int image)
{
    for (int i = 0; i < s_nimgs; i++) if (s_imgs[i].image == image) return s_imgs[i].mode;
    return -1;
}

/* ---- decrementer log / feed ---------------------------------------------- */

#define FNR_DEC_MAX 1024
typedef struct { uint32_t v[FNR_DEC_MAX]; uint32_t n; } fnr_declog;
static SPU_THREAD_LOCAL fnr_declog* t_logs[16];
static SPU_THREAD_LOCAL int t_nlogs;
static SPU_THREAD_LOCAL const uint32_t* t_feed;
static SPU_THREAD_LOCAL uint32_t t_feed_n, t_feed_i;

/* ---- the I/O journal (FNR2) ------------------------------------------------ */

enum { IO_GET = 1, IO_PUT = 2, IO_DMA_OTHER = 3, IO_GETLLAR = 4, IO_PUTLLC = 5, IO_PUTLLUC = 6,
       IO_RDCH = 7, IO_RCHCNT = 8, IO_IRQ = 9, IO_KSVC = 10, IO_EVLR = 11 };
/* IO_EVLR: the last entry of a recorded run. The lock-line-lost event bit
 * (SPU_EVENT_LR, 0x400) and the reservation are changed by OTHER processors
 * (spu_coherency.c) at any time; the run itself never sees that except through
 * journaled reads (RdEventStat, PUTLLC's result). So the journal carries their
 * value at the run's end: result = event_status & 0x400, IO_F_RESV = resv_valid,
 * ea = resv_ea. A replay applies it after the run, then compares the whole
 * event_status. Recordings made before it was added have none: their compare
 * masks the bit (and skips resv_valid). */
#define IO_F_XFER   0x01u   /* the DMA moved data (not rejected) */
#define IO_F_RESV   0x02u   /* GETLLAR left a valid reservation */
#define IO_NODATA   0xFFFFFFFFu

typedef struct {
    uint32_t seq;
    uint8_t  kind, flags;
    uint16_t tag;
    uint32_t cmd;           /* MFC command, or the channel */
    uint32_t lsa;
    uint64_t ea;            /* IRQ: the target */
    uint32_t size;
    uint32_t result;        /* DMA rc, atomic_stat after, channel value */
    uint32_t data_off;      /* into the blob, IO_NODATA for none */
    uint32_t lr;            /* r0 word 0 at the time: who issued it */
} fnr2_io;

typedef struct fnr_jrn {
    fnr2_io* io; uint32_t nio, capio;
    uint8_t* blob; uint64_t nblob, capblob;
    uint32_t* dec; uint32_t ndec, capdec;
    uint32_t flags, nirq;
    char* trace; size_t ntrace, captrace;    /* SPU_FN_TRACE_JOB */
    unsigned long long tsc0;
    int depth;
} fnr_jrn;
#define FNR2_F_ASYNC     0x1u   /* an interrupt was taken during the run */
#define FNR2_F_TRUNC     0x2u   /* the journal hit its size cap */
#define FNR2_BLOB_CAP    (96ull << 20)

static SPU_THREAD_LOCAL fnr_jrn* t_jrn[4];
static SPU_THREAD_LOCAL int t_njrn;
static SPU_THREAD_LOCAL fnr_jrn* t_trace_jrn;   /* the traced job's journal */

static uint32_t fnr_dec_hook(spu_context* ctx, uint32_t v)
{
    (void)ctx;
    if (t_feed) {
        if (t_feed_i < t_feed_n) v = t_feed[t_feed_i];
        t_feed_i++;
        return v;
    }
    for (int i = 0; i < t_nlogs; i++)
        if (t_logs[i]->n < FNR_DEC_MAX) t_logs[i]->v[t_logs[i]->n++] = v;
    for (int i = 0; i < t_njrn; i++) {
        fnr_jrn* j = t_jrn[i];
        if (j->ndec == j->capdec) {
            uint32_t c = j->capdec ? j->capdec * 2 : 256;
            uint32_t* p = (uint32_t*)realloc(j->dec, sizeof(uint32_t) * c);
            if (!p) { j->flags |= FNR2_F_TRUNC; continue; }
            j->dec = p; j->capdec = c;
        }
        j->dec[j->ndec++] = v;
    }
    return v;
}

static void jrn_trace(fnr_jrn* j, const char* fmt, ...)
{
    if (!j) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    if (j->ntrace + (size_t)n + 1 > j->captrace) {
        size_t c = j->captrace ? j->captrace * 2 : 65536;
        while (c < j->ntrace + (size_t)n + 1) c *= 2;
        char* p = (char*)realloc(j->trace, c);
        if (!p) return;
        j->trace = p; j->captrace = c;
    }
    memcpy(j->trace + j->ntrace, buf, (size_t)n);
    j->ntrace += (size_t)n;
}

static const char* io_kind_name(int k)
{
    static const char* nm[] = { "?", "GET", "PUT", "DMA", "GETLLAR", "PUTLLC", "PUTLLUC", "RDCH", "RCHCNT", "IRQ", "KSVC", "EVLR" };
    return k >= 0 && k <= 11 ? nm[k] : "?";
}

static void jrn_add(fnr_jrn* j, spu_context* ctx, int kind, uint32_t flags, uint32_t cmd, uint32_t lsa,
                    uint64_t ea, uint32_t size, uint32_t tag, uint32_t result, const uint8_t* data, uint32_t ndata)
{
    if (j->nio == j->capio) {
        uint32_t c = j->capio ? j->capio * 2 : 256;
        fnr2_io* p = (fnr2_io*)realloc(j->io, sizeof(fnr2_io) * c);
        if (!p) { j->flags |= FNR2_F_TRUNC; return; }
        j->io = p; j->capio = c;
    }
    fnr2_io* e = &j->io[j->nio];
    memset(e, 0, sizeof *e);
    e->seq = j->nio; e->kind = (uint8_t)kind; e->flags = (uint8_t)flags; e->tag = (uint16_t)tag;
    e->cmd = cmd; e->lsa = lsa; e->ea = ea; e->size = size; e->result = result;
    e->lr = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    e->data_off = IO_NODATA;
    if (data && ndata) {
        if (j->nblob + ndata > FNR2_BLOB_CAP) j->flags |= FNR2_F_TRUNC;
        else {
            if (j->nblob + ndata > j->capblob) {
                uint64_t c = j->capblob ? j->capblob * 2 : (256u << 10);
                while (c < j->nblob + ndata) c *= 2;
                uint8_t* p = (uint8_t*)realloc(j->blob, (size_t)c);
                if (!p) { j->flags |= FNR2_F_TRUNC; goto noblob; }
                j->blob = p; j->capblob = c;
            }
            memcpy(j->blob + j->nblob, data, ndata);
            e->data_off = (uint32_t)j->nblob;
            j->nblob += ndata;
        }
    }
noblob:
    j->nio++;
    if (j == t_trace_jrn)
        jrn_trace(j, "+%llu io#%u %s cmd=0x%X lsa=0x%05X ea=0x%08llX size=0x%X tag=%u res=0x%X lr=0x%05X%s\n",
                  __rdtsc() - j->tsc0, e->seq, io_kind_name(kind), cmd, lsa, (unsigned long long)ea, size, tag,
                  result, e->lr, (flags & IO_F_XFER) || kind > IO_PUT ? "" : " (rejected)");
}

/* The MFC's own malformed-transfer rule (spu_dma.h): such a transfer returns
 * 0 and moves nothing, so the replay must not deliver its bytes either. */
static int dma_malformed(uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd)
{
    static int s_lax = -1; if (s_lax < 0) s_lax = getenv("SPU_DMA_LAX") ? 1 : 0;
    if (s_lax) return 0;
    const int put = (cmd & 0x20) && !(cmd & 0x40);
    return size == 0 || size > 0x4000 || (size >= 16 && (size & 15)) || (((lsa ^ (uint32_t)ea) & 15) != 0) ||
           (put && (uint32_t)ea < 0x10000u);
}
static int dma_kind(uint32_t cmd)
{
    if ((cmd & 0xFFFFu) == MFC_SDCRZ_CMD) return IO_DMA_OTHER;
    if (cmd & 0x40) return IO_GET;
    if (cmd & 0x20) return IO_PUT;
    return IO_DMA_OTHER;
}

/* recording: run the real operation, log what it saw */
static int rec_dma(spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd, spu_iojrn_dma_fn real)
{
    const int rc = real(ctx, lsa, ea, size, cmd);
    const int kind = dma_kind(cmd);
    const uint32_t l = lsa & SPU_LS_MASK;
    const int xfer = rc == 0 && kind != IO_DMA_OTHER && !dma_malformed(lsa, ea, size, cmd) &&
                     size && size <= 0x4000 && (uint64_t)l + size <= SPU_LS_SIZE;
    for (int i = 0; i < t_njrn; i++)
        jrn_add(t_jrn[i], ctx, kind, xfer ? IO_F_XFER : 0, cmd, lsa, ea, size, ctx->mfc_tag & 31u, (uint32_t)rc,
                xfer ? ctx->ls + l : NULL, xfer ? size : 0);
    return rc;
}

static int atomic_kind(uint32_t cmd)
{
    return cmd == MFC_GETLLAR_CMD ? IO_GETLLAR : cmd == MFC_PUTLLC_CMD ? IO_PUTLLC : IO_PUTLLUC;
}

static int rec_atomic(spu_context* ctx, uint32_t cmd, spu_iojrn_atomic_fn real)
{
    const uint32_t ea = ctx->mfc_eal & ~127u, lsa = ctx->mfc_lsa & SPU_LS_MASK;
    /* PUTLLC/PUTLLUC: the line as the SPU stores it (before: the same bytes). */
    const int rc = real(ctx, cmd);
    if (rc && lsa + 128u <= SPU_LS_SIZE)
        for (int i = 0; i < t_njrn; i++)
            jrn_add(t_jrn[i], ctx, atomic_kind(cmd), ctx->resv_valid ? IO_F_RESV : 0, cmd, lsa, ea, 128,
                    ctx->mfc_tag & 31u, ctx->atomic_stat, ctx->ls + lsa, 128);
    return rc;
}

static uint32_t rec_rdch(spu_context* ctx, uint32_t ch, spu_iojrn_ch_fn real)
{
    const uint32_t v = real(ctx, ch);
    for (int i = 0; i < t_njrn; i++) jrn_add(t_jrn[i], ctx, IO_RDCH, 0, ch, 0, 0, 0, 0, v, NULL, 0);
    return v;
}

static uint32_t rec_rchcnt(spu_context* ctx, uint32_t ch, spu_iojrn_ch_fn real)
{
    const uint32_t v = real(ctx, ch);
    for (int i = 0; i < t_njrn; i++) jrn_add(t_jrn[i], ctx, IO_RCHCNT, 0, ch, 0, 0, 0, 0, v, NULL, 0);
    return v;
}

static void rec_irq(spu_context* ctx, uint32_t srr0, uint32_t target)
{
    for (int i = 0; i < t_njrn; i++) {
        t_jrn[i]->flags |= FNR2_F_ASYNC; t_jrn[i]->nirq++;
        jrn_add(t_jrn[i], ctx, IO_IRQ, 0, ctx->event_status & ctx->event_mask, srr0, target, 0, 0,
                ctx->event_status, NULL, 0);
    }
}

/* A traced run (SPU_FN_TRACE_JOB, SPU_FN_REPLAY_TRACE): each trampoline step. */
static void trace_step(spu_context* ctx, void (*fn)(spu_context*))
{
    fnr_jrn* j = t_trace_jrn;
    (void)fn;
    if (j) jrn_trace(j, "+%llu . %05X img=%d d=%u\n", __rdtsc() - j->tsc0, ctx->pc & SPU_LS_MASK, ctx->image_id, ctx->host_depth);
}

/* The kernel's selectWorkload answer: LS 0x100-0x17F (tempArea) and r3. */
static void rec_ksvc(spu_context* ctx, uint32_t pc, void (*real)(spu_context*))
{
    real(ctx);
    uint8_t d[144];
    memcpy(d, ctx->ls + 0x100, 128); memcpy(d + 128, &ctx->gpr[3], 16);
    for (int i = 0; i < t_njrn; i++) jrn_add(t_jrn[i], ctx, IO_KSVC, 0, pc, 0x100, 0, 144, 0, ctx->gpr[3]._u32[0], d, 144);
}

static const spu_iojrn_ops s_rec_ops = { rec_dma, rec_atomic, rec_rdch, rec_rchcnt, rec_irq, trace_step, rec_ksvc };

/* ---- SPU_EDGE_VARIANTS / SPU_FN_TRACE_JOB / the whole-job wrapper ---------- */

static int s_watch = -1;       /* -1 not decided; else any of the below */
static int s_variants, s_trace_max, s_job_drain, s_replaying;
static volatile long s_traced;

typedef struct {
    uint64_t fnmask;            /* desc->idx bits */
    uint64_t fragmask;          /* fragment images 300..363 (bit = image - 300) */
    uint32_t dcc0_bits;         /* index-unpack paths seen: 1 none, 2 <= 8 bits, 4 more */
    uint32_t lit;               /* 73E0: r13 & 0x10900 ORed, | 1 when r8 != 0 */
    uint16_t e550_types;        /* vertex types seen */
    uint8_t  e550_fmt;          /* palette formats seen (bit per format 0..7) */
    uint8_t  e550_streams;      /* direction-stream counts seen (bit per count) */
    uint8_t  d640_modes;        /* cull modes seen (bit per mode 0..7) */
    uint8_t  d640_flags;        /* p[0] & 0x90 ORed */
    uint8_t  d640_ms;           /* byte p+0x140 values seen (bit per value 0..7) */
    uint8_t  other_frag;        /* a fragment outside 300..363 */
} edge_key;

typedef struct { edge_key k; long long n; unsigned long long tsc; int used; } edge_slot;
#define EDGE_SLOTS 4096
static edge_slot s_eslots[EDGE_SLOTS];
static SRWLOCK s_elock = SRWLOCK_INIT;
static long long s_ejobs; static unsigned long long s_etsc;
static unsigned long long s_elast_ms, s_et0_ms; static long long s_elast_jobs;
static spu_fn_desc* s_idx_desc[65];
static volatile long s_nidx;

typedef struct { int active; edge_key k; } edge_job;
static SPU_THREAD_LOCAL edge_job t_job;

static unsigned long long fnr_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    return 0;
#endif
}

static void fnw_init(void)
{
    if (s_watch >= 0) return;
    const char* e = getenv("SPU_EDGE_VARIANTS");
    s_variants = e && *e && *e != '0' ? (atoi(e) > 1 ? atoi(e) : 1) : 0;
    e = getenv("SPU_FN_TRACE_JOB");
    s_trace_max = e ? atoi(e) : 0;
    e = getenv("SPU_FN_JOB_DRAIN");
    const int drain = e && *e && *e != '0';
    const char* rec = getenv("SPU_FN_RECORD");
    const char* f = getenv("SPU_FN_RECORD_FUNCS");
    spu_fn_desc job; memset(&job, 0, sizeof job); job.image = 218; job.addr = 0xAD98;
    const int rec_job = rec && *rec && f && list_has(f, &job);
    s_job_drain = s_variants || s_trace_max > 0 || drain || rec_job || s_replaying;
    if (s_variants) s_et0_ms = s_elast_ms = fnr_ms();
    s_watch = s_variants || s_trace_max > 0;
    if (s_job_drain)
        fprintf(stderr, "[fnrec] whole-job wrapper on (218:AD98)%s%s\n", s_variants ? ", SPU_EDGE_VARIANTS" : "",
                s_trace_max > 0 ? ", SPU_FN_TRACE_JOB" : "");
}

/* SPU_DIRECT (spu_context.h, SPU_XFER): a native, a recorded run and the
 * hooks below call lifted code and may look at (or rely on) the transfer it
 * leaves pending, or run more code after it at the same host depth; direct
 * transfers stay off while they run (each nested drain they start turns them
 * back on for its own steps). The plain lifted call runs as it would from
 * the drain loop, so it keeps them. */
#define FNW_NO_DIRECT(...) do {                                                           \
        if (spu_direct_on()) {                                                            \
        const spu_direct_state _fnw_ds = t_spu_direct; t_spu_direct.depth = ~0u;          \
        __VA_ARGS__;                                                                      \
        t_spu_direct = _fnw_ds;                                                           \
        } else { __VA_ARGS__; }                                                           \
        } while (0)

/* The lifted version, and for the whole-job entry its trampolines too, up to
 * the return to the policy module (the link register at entry). */
static void fn_lifted(spu_fn_desc* d, spu_context* ctx)
{
    if (s_job_drain && fnw_is_job(d)) {
        const uint32_t ret = ctx->gpr[0]._u32[0];
        FNW_NO_DIRECT(d->lifted(ctx));   /* its transfers belong to the drain below */
        spu_drain_call(ctx, ret);
        return;
    }
    d->lifted(ctx);
}

static int fnw_idx(spu_fn_desc* d)
{
    int i = d->idx;
    if (i) return i;
    AcquireSRWLockExclusive(&s_elock);
    if (!d->idx && s_nidx < 64) { s_nidx++; s_idx_desc[s_nidx] = d; d->idx = (int)s_nidx; }
    i = d->idx;
    ReleaseSRWLockExclusive(&s_elock);
    return i;
}

static uint32_t ls_u32(const spu_context* ctx, uint32_t a)
{
    const uint8_t* p = ctx->ls + (a & SPU_LS_MASK & ~3u);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint8_t ls_u8(const spu_context* ctx, uint32_t a) { return ctx->ls[a & SPU_LS_MASK]; }

static void frag_bit(edge_key* k, int image)
{
    if (image >= 300 && image < 364) k->fragmask |= 1ull << (image - 300);
    else if (image > 0) k->other_frag = 1;
}

/* The image whose resident code holds lsa (a streamed fragment). */
static int resident_image(const spu_context* ctx, uint32_t lsa)
{
    for (int s = 0; s < SPU_RESIDENT_CODE_SLOTS; s++) {
        const uint32_t a = ctx->resident_code[s].lsa, n = ctx->resident_code[s].size;
        if (n && lsa >= a && lsa < a + n) return ctx->resident_code[s].image_id;
    }
    return 0;
}

static void edge_note(spu_fn_desc* d, spu_context* ctx)
{
    edge_key* k = &t_job.k;
    const int i = fnw_idx(d);
    if (i > 0 && i <= 64) k->fnmask |= 1ull << (i - 1);
    if (d->image != 218) { frag_bit(k, d->image); return; }
    const uint32_t r3 = ctx->gpr[3]._u32[0];
    switch (d->addr) {
    case 0xE550: {
        const uint8_t b0 = ls_u8(ctx, r3);
        if ((b0 & 0x80) && !(b0 & 0x10)) {
            k->e550_types |= (uint16_t)(1u << (ls_u8(ctx, r3 + 6) & 15));
            k->e550_fmt |= (uint8_t)(1u << (ls_u8(ctx, r3 + 7) & 7));
            const int ns = (ls_u32(ctx, r3 + 0x60) != 0) + (ls_u32(ctx, r3 + 0x64) != 0) + (ls_u32(ctx, r3 + 0x68) != 0);
            k->e550_streams |= (uint8_t)(1u << ns);
        } else k->e550_types |= 0x8000u;   /* skipped (flags) */
        break; }
    case 0xD640:
        k->d640_modes |= (uint8_t)(1u << (ctx->gpr[4]._u32[0] > 7 ? 7 : ctx->gpr[4]._u32[0]));
        k->d640_flags |= (uint8_t)(ls_u8(ctx, r3) & 0x90);
        k->d640_ms |= (uint8_t)(1u << (ls_u8(ctx, r3 + 0x140) & 7));
        break;
    case 0x73E0:
        k->lit |= (ctx->gpr[13]._u32[0] & 0x10900u) | (ctx->gpr[8]._u32[0] != 0);
        break;
    case 0xDCC0: {
        /* bits per index (header byte 6): the unpack path, <= 8 (DE6C) or more
         * (DEF8/DFF0); 0 = no deltas (E0C8). SPU_EDGE_VARIANTS=2: the value. */
        const uint32_t bits = ls_u8(ctx, ctx->gpr[4]._u32[0] + 6) & 31;
        k->dcc0_bits |= s_variants >= 2 ? 1u << bits : bits == 0 ? 1u : bits <= 8 ? 2u : 4u;
        break; }
    case 0xD158:
        if (!(ls_u8(ctx, r3) & 0x10)) frag_bit(k, resident_image(ctx, ls_u32(ctx, r3 + 0x8C) & SPU_LS_MASK));
        break;
    default: break;
    }
}

static uint64_t edge_hash(const edge_key* k) { return fnv64(0xCBF29CE484222325ull, k, sizeof *k); }

static void edge_print(void)
{
    /* under s_elock: the 32 keys with the most jobs */
    int top[32]; int nt = 0;
    static unsigned char taken[EDGE_SLOTS];
    memset(taken, 0, sizeof taken);
    for (; nt < 32; nt++) {
        int b = -1;
        for (int s = 0; s < EDGE_SLOTS; s++)
            if (s_eslots[s].used && !taken[s] && (b < 0 || s_eslots[s].n > s_eslots[b].n)) b = s;
        if (b < 0) break;
        taken[b] = 1; top[nt] = b;
    }
    const unsigned long long now = fnr_ms();
    const double secs = (double)(now - s_elast_ms) / 1000.0;
    fprintf(stderr, "[edge-var] t=%.1fs jobs=%lld (%.0f/s over the last %.1f s) keys=",
            (double)(now - s_et0_ms) / 1000.0, s_ejobs, secs > 0 ? (double)(s_ejobs - s_elast_jobs) / secs : 0.0, secs);
    int nk = 0; for (int s = 0; s < EDGE_SLOTS; s++) nk += s_eslots[s].used;
    fprintf(stderr, "%d\n", nk);
    for (int q = 0; q < nt; q++) {
        const edge_slot* e = &s_eslots[top[q]];
        char fns[640]; int o = 0;
        for (int b = 0; b < 64 && o < (int)sizeof fns - 24; b++)
            if (e->k.fnmask & (1ull << b) && s_idx_desc[b + 1]) {
                const spu_fn_desc* d = s_idx_desc[b + 1];
                if (d->image == 218) o += snprintf(fns + o, sizeof fns - (size_t)o, "%s%X", o ? "," : "", d->addr);
            }
        fns[o] = 0;
        char frs[256]; int f = 0;
        for (int b = 0; b < 64 && f < (int)sizeof frs - 8; b++)
            if (e->k.fragmask & (1ull << b)) f += snprintf(frs + f, sizeof frs - (size_t)f, "%s%d", f ? "," : "", 300 + b);
        if (e->k.other_frag) f += snprintf(frs + f, sizeof frs - (size_t)f, "%sother", f ? "," : "");
        frs[f] = 0;
        fprintf(stderr, "[edge-var] #%d n=%lld share=%.2f%% tsc=%.2f%% avg=%.0fk key=frag{%s} skin{t=%X f=%X s=%X} "
                        "cull{m=%X fl=%X ms=%X} lit=%X dcc0b=%X fns{%s}\n",
                q + 1, e->n, s_ejobs ? 100.0 * (double)e->n / (double)s_ejobs : 0.0,
                s_etsc ? 100.0 * (double)e->tsc / (double)s_etsc : 0.0, e->n ? (double)e->tsc / (double)e->n / 1000.0 : 0.0,
                frs, e->k.e550_types, e->k.e550_fmt, e->k.e550_streams, e->k.d640_modes, e->k.d640_flags,
                e->k.d640_ms, e->k.lit, e->k.dcc0_bits, fns);
    }
    fflush(stderr);
    s_elast_ms = now; s_elast_jobs = s_ejobs;
}

static void edge_insert(const edge_key* k, unsigned long long tsc)
{
    const uint64_t h = edge_hash(k);
    AcquireSRWLockExclusive(&s_elock);
    s_ejobs++; s_etsc += tsc;
    for (uint32_t p = 0; p < EDGE_SLOTS; p++) {
        edge_slot* e = &s_eslots[(h + p) & (EDGE_SLOTS - 1)];
        if (!e->used) { e->used = 1; memcpy(&e->k, k, sizeof *k); e->n = 1; e->tsc = tsc; break; }
        if (!memcmp(&e->k, k, sizeof *k)) { e->n++; e->tsc += tsc; break; }
    }
    const unsigned long long now = fnr_ms();
    if (now - s_elast_ms >= 5000) edge_print();
    ReleaseSRWLockExclusive(&s_elock);
}

/* ---- recording -------------------------------------------------------------- */

typedef struct {
    char magic[4];            /* "FNR1" */
    uint32_t image, addr;
    uint32_t ctx_size;        /* sizeof(spu_context) when recorded */
    uint32_t ndec;            /* decrementer values that follow */
    uint32_t reserved;
    uint64_t ctx_addr;        /* where the recorded context lived (pointer fix-up) */
} fnr_header;

/* FNR2: the header, then
 *   fnr2_img[nimg]                       the images wrapped in this build
 *   u32 dec[ndec]
 *   ctx_in   the context but its local store (ctx_size - SPU_LS_SIZE bytes, in
 *            struct order), then the local store as runs of non-zero
 *            quadwords: {u32 lsa, u32 nquads, data}, ended by lsa 0xFFFFFFFF
 *   ctx_out  the same, the runs being the quadwords that differ from ctx_in
 *   fnr2_io[nio]
 *   blob (data_bytes): GET/PUT bytes and atomic lines, in journal order */
typedef struct {
    char magic[4];            /* "FNR2" */
    uint32_t image, addr;
    uint32_t ctx_size;
    uint32_t ndec, nio;
    uint32_t flags;           /* FNR2_F_ASYNC, FNR2_F_TRUNC */
    uint32_t float_mode;      /* of the recorded function's own image (SPU_FNREC_FLOAT_MODE) */
    uint8_t  build_id[16];
    uint32_t mxcsr;           /* at entry */
    uint32_t tag_completed;   /* the MFC engine's completed tags at entry */
    uint64_t ctx_addr;
    uint64_t data_bytes;
    uint32_t nimg, nirq;
    uint64_t tsc;             /* the run's TSC ticks (recording overhead included) */
    uint32_t io_size;         /* sizeof(fnr2_io) */
    uint32_t reserved[3];
} fnr2_header;
typedef struct { uint32_t image; uint32_t float_mode; uint64_t src_hash; } fnr2_img;

typedef struct { uint8_t* p; size_t n, cap; } fnr_buf;
static int buf_put(fnr_buf* b, const void* p, size_t n)
{
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : (1u << 20);
        while (c < b->n + n) c *= 2;
        uint8_t* q = (uint8_t*)realloc(b->p, c);
        if (!q) return 0;
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, p, n); b->n += n;
    return 1;
}

static void ctx_put(fnr_buf* b, const spu_context* c, const spu_context* base)
{
    const size_t off = offsetof(spu_context, ls_store);
    buf_put(b, c, off);
    buf_put(b, (const uint8_t*)c + off + SPU_LS_SIZE, sizeof *c - off - SPU_LS_SIZE);
    static const uint8_t z[16];
    uint32_t q = 0;
    while (q < SPU_LS_SIZE / 16) {
        const uint8_t* a = c->ls_store + q * 16;
        const int keep = base ? memcmp(a, base->ls_store + q * 16, 16) != 0 : memcmp(a, z, 16) != 0;
        if (!keep) { q++; continue; }
        uint32_t e = q + 1;
        while (e < SPU_LS_SIZE / 16) {
            const uint8_t* x = c->ls_store + e * 16;
            if (base ? !memcmp(x, base->ls_store + e * 16, 16) : !memcmp(x, z, 16)) break;
            e++;
        }
        uint32_t hd[2] = { q * 16, e - q };
        buf_put(b, hd, 8);
        buf_put(b, a, (size_t)(e - q) * 16);
        q = e;
    }
    uint32_t end[2] = { 0xFFFFFFFFu, 0 };
    buf_put(b, end, 8);
}

static void fnr_write2(spu_fn_desc* d, long slot, const spu_context* in, const spu_context* out,
                       const fnr_jrn* j, uint32_t mxcsr, uint32_t tagc, unsigned long long tsc)
{
    fnr_buf b = { 0, 0, 0 };
    fnr2_header h; memset(&h, 0, sizeof h);
    memcpy(h.magic, "FNR2", 4);
    h.image = (uint32_t)d->image; h.addr = d->addr; h.ctx_size = (uint32_t)sizeof *in;
    h.ndec = j->ndec; h.nio = j->nio; h.flags = j->flags; h.float_mode = (uint32_t)fnr_img_mode(d->image);
    fnr_build_id(h.build_id);
    h.mxcsr = mxcsr; h.tag_completed = tagc; h.ctx_addr = (uint64_t)(uintptr_t)out;
    h.data_bytes = j->nblob; h.nimg = (uint32_t)s_nimgs; h.nirq = j->nirq; h.tsc = tsc; h.io_size = sizeof(fnr2_io);
    buf_put(&b, &h, sizeof h);
    for (int i = 0; i < s_nimgs; i++) {
        fnr2_img im = { (uint32_t)s_imgs[i].image, (uint32_t)s_imgs[i].mode, s_imgs[i].hash };
        buf_put(&b, &im, sizeof im);
    }
    if (j->ndec) buf_put(&b, j->dec, sizeof(uint32_t) * j->ndec);
    ctx_put(&b, in, NULL);
    ctx_put(&b, out, in);
    if (j->nio) buf_put(&b, j->io, sizeof(fnr2_io) * j->nio);
    if (j->nblob) buf_put(&b, j->blob, (size_t)j->nblob);
    char path[1024];
    snprintf(path, sizeof path, "%s/i%d_%08X_%05ld.fnr", getenv("SPU_FN_RECORD"), d->image, d->addr, slot);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(b.p, 1, b.n, f); fclose(f); }
    if (j->trace && j->ntrace) {
        char tp[1100]; snprintf(tp, sizeof tp, "%s.trace.txt", path);
        FILE* tf = fopen(tp, "wb");
        if (tf) {
            fprintf(tf, "# %s: %u journal entries, %u dec reads, %u interrupts, %llu TSC ticks, flags 0x%X\n",
                    d->name, j->nio, j->ndec, j->nirq, tsc, j->flags);
            fwrite(j->trace, 1, j->ntrace, tf); fclose(tf);
        }
    }
    free(b.p);
}

static void fnr_record(spu_fn_desc* d, spu_context* ctx)
{
    static long s_max = -1, s_every = -1, s_io = -1;
    static unsigned long long s_after = (unsigned long long)-1, s_t0;
    if (s_max < 0) {
        const char* e = getenv("SPU_FN_RECORD_MAX");   s_max = e ? atol(e) : 40;
        e = getenv("SPU_FN_RECORD_EVERY");             s_every = e ? atol(e) : 97;
        if (s_every < 1) s_every = 1;
        e = getenv("SPU_FN_RECORD_AFTER_MS");          s_after = e ? strtoull(e, 0, 10) : 0;
        e = getenv("SPU_FN_RECORD_IO");
        s_io = e && *e && *e != '0' && g_spu_iojrn_rt && g_spu_iojrn_rt->version == SPU_IOJRN_VERSION;
        if (e && *e && *e != '0' && !s_io)
            fprintf(stderr, "[fnrec] SPU_FN_RECORD_IO: this runtime has no I/O journal hooks; recording FNR1\n");
        s_t0 = fnr_ms();
    }
    const long c = InterlockedIncrement(&d->calls);
    if (d->recorded >= s_max || (c % s_every) != 0 || fnr_ms() - s_t0 < s_after) { fn_lifted(d, ctx); return; }
    const long slot = InterlockedIncrement(&d->recorded);
    if (slot > s_max) { fn_lifted(d, ctx); return; }

    spu_context* in = (spu_context*)malloc(sizeof *ctx);
    if (s_io) {
        fnr_jrn* j = (fnr_jrn*)calloc(1, sizeof *j);
        if (!in || !j || t_njrn >= 4) { free(in); free(j); fn_lifted(d, ctx); return; }
        memcpy(in, ctx, sizeof *ctx);
        const uint32_t mx = _mm_getcsr();
        const uint32_t tagc = g_spu_iojrn_rt->tag_completed_get(ctx);
        const int trace = s_trace_max > 0 && fnw_is_job(d) && !t_trace_jrn &&
                          InterlockedIncrement(&s_traced) <= s_trace_max;
        uint32_t (*prev_hook)(spu_context*, uint32_t) = t_spu_dec_hook;
        const spu_iojrn_ops* prev_ops = t_spu_iojrn;
        t_jrn[t_njrn++] = j;
        t_spu_dec_hook = fnr_dec_hook;
        t_spu_iojrn = &s_rec_ops;
        j->tsc0 = __rdtsc();
        if (trace) {
            t_trace_jrn = j;
            jrn_trace(j, "+0 > %d:%X r3=%08X r4=%08X r0=%08X sp=%05X depth=%u int=%u evmask=0x%X\n", d->image, d->addr,
                      ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[0]._u32[0], ctx->gpr[1]._u32[0] & SPU_LS_MASK,
                      ctx->host_depth, ctx->int_enable, ctx->event_mask);
        }
        fn_lifted(d, ctx);
        const unsigned long long tsc = __rdtsc() - j->tsc0;
        jrn_add(j, ctx, IO_EVLR, ctx->resv_valid ? IO_F_RESV : 0, 0, 0, ctx->resv_ea, 0, 0,
                ctx->event_status & 0x400u, NULL, 0);
        if (trace) {
            jrn_trace(j, "+%llu < %d:%X pc=%05X r3=%08X int=%u\n", tsc, d->image, d->addr, ctx->pc & SPU_LS_MASK,
                      ctx->gpr[3]._u32[0], ctx->int_enable);
            t_trace_jrn = NULL;
        }
        t_njrn--;
        t_spu_iojrn = prev_ops;
        t_spu_dec_hook = (t_nlogs || t_njrn) ? fnr_dec_hook : prev_hook;
        fnr_write2(d, slot, in, ctx, j, mx, tagc, tsc);
        free(j->io); free(j->blob); free(j->dec); free(j->trace); free(j);
        free(in);
        return;
    }

    fnr_declog* lg = (fnr_declog*)calloc(1, sizeof *lg);
    if (!in || !lg || t_nlogs >= 16) { free(in); free(lg); fn_lifted(d, ctx); return; }
    memcpy(in, ctx, sizeof *ctx);
    uint32_t (*prev_hook)(spu_context*, uint32_t) = t_spu_dec_hook;
    t_logs[t_nlogs++] = lg;
    t_spu_dec_hook = fnr_dec_hook;
    fn_lifted(d, ctx);
    t_nlogs--;
    t_spu_dec_hook = (t_nlogs || t_njrn) ? fnr_dec_hook : prev_hook;

    char path[1024];
    snprintf(path, sizeof path, "%s/i%d_%08X_%03ld.fnr", getenv("SPU_FN_RECORD"), d->image, d->addr, slot);
    FILE* f = fopen(path, "wb");
    if (f) {
        fnr_header h;
        memset(&h, 0, sizeof h);
        memcpy(h.magic, "FNR1", 4);
        h.image = (uint32_t)d->image; h.addr = d->addr;
        h.ctx_size = (uint32_t)sizeof *ctx; h.ndec = lg->n;
        h.ctx_addr = (uint64_t)(uintptr_t)ctx;
        fwrite(&h, sizeof h, 1, f);
        fwrite(lg->v, sizeof(uint32_t), lg->n, f);
        fwrite(in, sizeof *ctx, 1, f);
        fwrite(ctx, sizeof *ctx, 1, f);
        fclose(f);
    }
    free(in); free(lg);
}

/* ---- dispatch -------------------------------------------------------------- */

/* In a replay (t_force) the forced native mode runs every native there is;
 * SPU_NATIVE=<list> narrows it, as in the game (to bisect a whole job). */
static int replay_native_ok(const spu_fn_desc* d)
{
    static const char* s_n = (const char*)1;
    if (s_n == (const char*)1) s_n = getenv("SPU_NATIVE");
    return !s_n || list_has(s_n, d);
}
static void fn_exec(spu_fn_desc* d, spu_context* ctx, int m)
{
    if (m == 2 && d->native && (t_force < 0 || replay_native_ok(d))) { FNW_NO_DIRECT(d->native(ctx)); return; }
    if (m == 1) { FNW_NO_DIRECT(fnr_record(d, ctx)); return; }
    fn_lifted(d, ctx);
}

/* Variants, trace and the whole-job entry: 1 when it ran the call. */
static __attribute__((noinline)) int fnw_slow(spu_fn_desc* d, spu_context* ctx, int m)
{
    fnw_init();
    if (!s_watch) return 0;
    if (fnw_is_job(d)) {
        if (!s_variants || t_job.active) return 0;
        memset(&t_job.k, 0, sizeof t_job.k);
        t_job.active = 1;
        const unsigned long long t0 = __rdtsc();
        fn_exec(d, ctx, m);
        const unsigned long long t = __rdtsc() - t0;
        t_job.active = 0;
        edge_insert(&t_job.k, t);
        return 1;
    }
    if (t_job.active) edge_note(d, ctx);
    fnr_jrn* tj = t_trace_jrn;
    if (!tj) return 0;
    tj->depth++;
    jrn_trace(tj, "+%llu %*s> %d:%X r3=%08X r4=%08X lr=%05X sp=%05X\n", __rdtsc() - tj->tsc0, tj->depth, "",
              d->image, d->addr, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[0]._u32[0] & SPU_LS_MASK,
              ctx->gpr[1]._u32[0] & SPU_LS_MASK);
    fn_exec(d, ctx, m);
    jrn_trace(tj, "+%llu %*s< %d:%X pc=%05X%s\n", __rdtsc() - tj->tsc0, tj->depth, "", d->image, d->addr,
              ctx->pc & SPU_LS_MASK, g_spu_trampoline_fn ? " (transfer pending)" : "");
    tj->depth--;
    return 1;
}

/* SPU_SKIP_AB=1 (a ceiling test for GPU-EDGE, not for play; see
 * GPU-EDGE-STEP2-DESIGN.md / EXPERIMENTS row 32): in every other [draws]
 * window (g_ld_spu_skip_on, toggled by the renderer: "[draws] per 5s: spuskip
 * window ON|OFF"), module 218's per-vertex lighting (73E0, with the 6AC8/7040
 * calls it makes) and skinning (E550, with its BBC8 calls) return at once, as
 * if the GPU did that work: the SPU time they cost is gone, their output
 * buffers keep stale data (wrong colours, unskinned poses in those windows).
 * The only state they leave for the rest of the job is kept: E550's B600 store
 * of min(r4, r6) to the descriptor's word +0x58 (when its flags let it run);
 * 73E0's B600 writes back the scratch pointer it read, unchanged. */
extern volatile int g_ld_spu_skip_on;   /* rsx_live_draw.c */

/* GPU-EDGE shadow mode (host/gpu_edge/gpu_shadow.cpp registers these when it
 * is in the build and GPU_SHADOW=1; NULL otherwise). Also called from the
 * lighting natives' loop hooks. */
void* (*g_gpu_shadow_begin)(int stage, const void* gpr, const unsigned char* ls);
void (*g_gpu_shadow_end)(void* tok, const void* gpr, const unsigned char* ls);
/* GPU-EDGE part 3, the CPU shell (host/gpu_edge/gpu_edge_shell.c registers it
 * when it is in the build and EDGE_GPU is set; NULL otherwise): every module
 * 218 call goes through it, with fn_exec to run the call itself. */
void (*g_gpu_edge_hook)(spu_fn_desc* d, spu_context* ctx, int m, void (*run)(spu_fn_desc*, spu_context*, int));
volatile long long g_spu_skip_calls;
static int spu_skip_ab(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_SKIP_AB"); on = e && *e == '1'; }
    return on;
}

void spu_fn_dispatch(spu_fn_desc* d, spu_context* ctx)
{
    if (__builtin_expect(g_ld_spu_skip_on, 0) && d->image == 218 && (d->addr == 0x73E0u || d->addr == 0xE550u) &&
        t_force < 0 && spu_skip_ab()) {
        if (d->addr == 0xE550u) {
            const uint32_t p = ctx->gpr[3]._u32[0] & (SPU_LS_MASK & ~15u);
            const uint8_t b0 = ctx->ls[p];
            if ((b0 & 0x80) && !(b0 & 0x10)) {
                const uint32_t a = ctx->gpr[4]._u32[0], b = ctx->gpr[6]._u32[0], v = a < b ? a : b;
                uint8_t* w = ctx->ls + ((p + 0x58) & SPU_LS_MASK);
                w[0] = (uint8_t)(v >> 24); w[1] = (uint8_t)(v >> 16); w[2] = (uint8_t)(v >> 8); w[3] = (uint8_t)v;
            }
        }
        _InterlockedIncrement64(&g_spu_skip_calls);
        SPU_RET(ctx);
    }
    int m = t_force;
    if (m < 0) {
        m = d->mode;
        if (m < 0) { m = decide_mode(d); d->mode = m; }
    } else {
        if (!t_last_desc) t_last_desc = d;    /* the replayed function: the outermost */
        if (!d->native) d->native = native_find(d->image, d->addr);
    }
    if (__builtin_expect(s_watch != 0, 0)) {
        int ran = 0;
        FNW_NO_DIRECT(ran = fnw_slow(d, ctx, m));
        if (ran) return;
    }
    if (__builtin_expect(g_gpu_edge_hook != 0, 0) && d->image == 218 && t_force < 0) {
        FNW_NO_DIRECT(g_gpu_edge_hook(d, ctx, m, fn_exec));
        return;
    }
    /* GPU_SHADOW=1 (host/gpu_edge/gpu_shadow.cpp, only in builds that add
     * it): a sample of the stage calls is copied for the GPU checker:
     * 3 skinning E550, 4 transform F538, 5 bitangents A880, 6 culling D640
     * (with its D250), 7 index decompression DCC0 (the copy after it lacks
     * the store of DCC0's tail call B600, still a pending trampoline here:
     * the checker adds it from the registers, as for the recordings),
     * 8 vertex output D158 (with its callback fragment), and 9 the vertex
     * decompression fragment DBC8 tail-branches to (5280 -> DBF8 -> DBC8;
     * only once 218:DBC8 is wrapped by tools/spu_wrap.py): for a sampled
     * call that fragment runs here, between the two copies, instead of in
     * the drain right after DBC8's call in DBF8 (which then has nothing to
     * do: same pc, same depth, same order of effects). */
    if (__builtin_expect(g_gpu_shadow_begin != 0, 0) && d->image == 218 && t_force < 0) {
        const uint32_t a = d->addr;
        const int st = a == 0xE550u ? 3 : a == 0xF538u ? 4 : a == 0xA880u ? 5 : a == 0xD640u ? 6 :
                       a == 0xDCC0u ? 7 : a == 0xD158u ? 8 : a == 0xDBC8u ? 9 : -1;
        if (st == 9) {
          FNW_NO_DIRECT({
            fn_exec(d, ctx, m);
            const uint32_t pc = ctx->pc & SPU_LS_MASK, lr = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            if (g_spu_trampoline_fn == spu_indirect_branch && pc != lr && pc == (ctx->gpr[8]._u32[0] & SPU_LS_MASK)) {
                void* tok = g_gpu_shadow_begin(9, ctx->gpr, ctx->ls);
                if (tok) {
                    spu_drain_call(ctx, lr);
                    g_gpu_shadow_end(tok, ctx->gpr, ctx->ls);
                }
            }
          });
            return;
        }
        if (st >= 0) {
          FNW_NO_DIRECT({
            void* tok = g_gpu_shadow_begin(st, ctx->gpr, ctx->ls);
            fn_exec(d, ctx, m);
            g_gpu_shadow_end(tok, ctx->gpr, ctx->ls);
          });
            return;
        }
    }
    fn_exec(d, ctx, m);
}

/* ---- replay ---------------------------------------------------------------- */

static void fnr_fixup(spu_context* c, uint64_t rec_addr)
{
    /* ls normally points at the context's own store. */
    const uint64_t rec_store = rec_addr + offsetof(spu_context, ls_store);
    if ((uint64_t)(uintptr_t)c->ls == rec_store || !c->ls) c->ls = c->ls_store;
    c->ch_wait_cv = NULL;
    c->ch_wait_lock = NULL;
    c->irq_frame = NULL;
}

/* First difference between two contexts in what a function can change:
 * registers, local store and the scalar state before the IRQ save area.
 * Returns 0 when they match; otherwise writes a description to msg. */
static int fnr_compare(const spu_context* a, const spu_context* b, char* msg, size_t msgsz)
{
    for (int r = 0; r < 128; r++)
        if (memcmp(&a->gpr[r], &b->gpr[r], 16)) {
            snprintf(msg, msgsz, "r%d: %08X %08X %08X %08X vs %08X %08X %08X %08X", r,
                     a->gpr[r]._u32[0], a->gpr[r]._u32[1], a->gpr[r]._u32[2], a->gpr[r]._u32[3],
                     b->gpr[r]._u32[0], b->gpr[r]._u32[1], b->gpr[r]._u32[2], b->gpr[r]._u32[3]);
            return 1;
        }
    for (uint32_t o = 0; o < SPU_LS_SIZE; o += 16)
        if (memcmp(a->ls_store + o, b->ls_store + o, 16)) {
            uint32_t n = 0;
            for (uint32_t k = o; k < SPU_LS_SIZE; k += 16) if (memcmp(a->ls_store + k, b->ls_store + k, 16)) n++;
            snprintf(msg, msgsz, "LS 0x%05X (%u quadwords differ)", o, n);
            return 2;
        }
    if (a->pc != b->pc) { snprintf(msg, msgsz, "pc 0x%X vs 0x%X", a->pc, b->pc); return 3; }
    if (a->status != b->status) { snprintf(msg, msgsz, "status 0x%X vs 0x%X", a->status, b->status); return 3; }
    return 0;
}

/* FNR2 (SPU_FN_REPLAY_STATE, default on for FNR2, =0 off): the scalar state a
 * whole job leaves for the policy module and for the next lifted job (design
 * O8/O10) -- fnr_compare looks only at registers, LS, pc and status. The
 * lock-line-lost event bit (0x400) and resv_valid are set by OTHER threads at
 * any time (spu_coherency.c): a recording journals their value at the run's
 * end (IO_EVLR, applied by the replay after the run), so with `evlr` they are
 * compared like the rest. A recording made before IO_EVLR existed has no such
 * entry; for it the bit is masked and resv_valid skipped, as the replay cannot
 * know what another processor did to the line. */
static int fnr_compare_state(const spu_context* a, const spu_context* b, int tramp, int evlr, char* msg, size_t msgsz)
{
#define FNR_CMP_U32(f) if (a->f != b->f) { snprintf(msg, msgsz, #f " 0x%X vs 0x%X", (unsigned)a->f, (unsigned)b->f); return 7; }
#define FNR_CMP_MEM(f) if (memcmp(&a->f, &b->f, sizeof a->f)) { snprintf(msg, msgsz, #f " differs"); return 7; }
    FNR_CMP_U32(int_enable) FNR_CMP_U32(srr0) FNR_CMP_U32(event_mask) FNR_CMP_U32(host_depth)
    FNR_CMP_U32(image_id) FNR_CMP_U32(drain_ret_pc) FNR_CMP_U32(pic_delta) FNR_CMP_U32(resident_ovl)
    FNR_CMP_U32(resident_task) FNR_CMP_U32(mfc_tag_mask) FNR_CMP_U32(atomic_stat) FNR_CMP_U32(irq_saved)
    FNR_CMP_U32(irq_resume_pc) FNR_CMP_U32(list_stall_mask) FNR_CMP_U32(list_stall_stat) FNR_CMP_U32(stop_code)
    { const uint32_t m = evlr ? ~0u : ~0x400u;
      if ((a->event_status & m) != (b->event_status & m)) {
          snprintf(msg, msgsz, "event_status 0x%X vs 0x%X%s", a->event_status, b->event_status, evlr ? "" : " (LR bit ignored)");
          return 7; } }
    if (evlr) { FNR_CMP_U32(resv_valid) }
    FNR_CMP_MEM(resident_code) FNR_CMP_MEM(list_stall_elem_lsa) FNR_CMP_MEM(list_stall_remaining)
    FNR_CMP_MEM(list_stall_dest_lsa) FNR_CMP_MEM(list_stall_ea_base) FNR_CMP_MEM(list_stall_cmd)
    if (a->ch_out_mbox.count != b->ch_out_mbox.count || a->ch_out_intr_mbox.count != b->ch_out_intr_mbox.count ||
        memcmp(a->ch_out_mbox.q, b->ch_out_mbox.q, sizeof a->ch_out_mbox.q) ||
        memcmp(a->ch_out_intr_mbox.q, b->ch_out_intr_mbox.q, sizeof a->ch_out_intr_mbox.q)) {
        snprintf(msg, msgsz, "outbound mailbox differs"); return 7; }
    if (tramp) { snprintf(msg, msgsz, "a trampoline is still pending at the job's return"); return 7; }
#undef FNR_CMP_U32
#undef FNR_CMP_MEM
    return 0;
}

static double fnr_qpc_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    return 0;
#endif
}

/* Serving an FNR2 run from its journal. */
typedef struct {
    const fnr2_io* io; uint32_t nio, i;
    const uint8_t* blob; uint64_t nblob;
    int bad;                  /* the first divergence, as a code (0 none) */
    char msg[256];
    uint32_t puts_bad;
} fnr_rep;
static SPU_THREAD_LOCAL fnr_rep* t_rep;
static SPU_THREAD_LOCAL spu_context* t_rep_ctx;   /* SPU_FN_REPLAY_TRACE: the run's context */
#define FNR_REP_ABORT 0xE0F2A001u

static void rep_fail(const char* fmt, ...)
{
    fnr_rep* r = t_rep;
    if (!r->bad) {
        r->bad = 1;
        va_list ap; va_start(ap, fmt);
        vsnprintf(r->msg, sizeof r->msg, fmt, ap);
        va_end(ap);
    }
    /* Out of the run: the guest went another way, and serving it would only
     * spin (a label wait) until the timeout. */
    RaiseException(FNR_REP_ABORT, 0, 0, NULL);
}

static const fnr2_io* rep_next(int kind, uint32_t cmd)
{
    fnr_rep* r = t_rep;
    if (r->i >= r->nio) rep_fail("io#%u: the run goes on past the journal's end (%s cmd=0x%X)", r->i, io_kind_name(kind), cmd);
    const fnr2_io* e = &r->io[r->i];
    if (t_trace_jrn)
        jrn_trace(t_trace_jrn, "+%llu io#%u %s cmd=0x%X lr=0x%05X (recorded %s cmd=0x%X lr=0x%05X)\n",
                  __rdtsc() - t_trace_jrn->tsc0, r->i, io_kind_name(kind), cmd,
                  t_rep_ctx ? t_rep_ctx->gpr[0]._u32[0] & SPU_LS_MASK : 0, io_kind_name(e->kind), e->cmd, e->lr);
    if (e->kind != kind || e->cmd != cmd)
        rep_fail("io#%u: %s cmd=0x%X where the recording has %s cmd=0x%X (lsa 0x%05X ea 0x%08llX)", r->i,
                 io_kind_name(kind), cmd, io_kind_name(e->kind), e->cmd, e->lsa, (unsigned long long)e->ea);
    r->i++;
    return e;
}
/* The replay's guest memory: a 4 GB reservation whose pages are committed
 * (zero) on first touch, holding the bytes each GET of the run delivered. The
 * runtime reads guest memory itself in one place a job reaches,
 * spu_overlay_note_get's content check of a code fragment against its
 * registered source (the bytes the same GET brought); nothing else in a
 * served run touches it. */
extern uint8_t* vm_base;
static uint8_t* s_rep_vm;
static volatile long s_rep_vm_faults;
static LONG CALLBACK rep_vm_veh(PEXCEPTION_POINTERS x)
{
    if (!s_rep_vm || x->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        x->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = (uintptr_t)x->ExceptionRecord->ExceptionInformation[1], b = (uintptr_t)s_rep_vm;
    if (a < b || a >= b + 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualAlloc((void*)(a & ~(uintptr_t)0xFFFF), 0x10000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&s_rep_vm_faults);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void rep_vm_init(void)
{
    if (s_rep_vm || vm_base) return;
    s_rep_vm = (uint8_t*)VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE);
    if (!s_rep_vm) return;
    AddVectoredExceptionHandler(1, rep_vm_veh);
    vm_base = s_rep_vm;
}
static void rep_vm_put(uint32_t ea, const uint8_t* p, uint32_t n)
{
    if (!s_rep_vm || (uint64_t)ea + n > 0x100000000ull) return;
    VirtualAlloc(s_rep_vm + (ea & ~0xFFFFu), (((uint64_t)ea + n + 0xFFFFu) & ~0xFFFFull) - (ea & ~0xFFFFu), MEM_COMMIT, PAGE_READWRITE);
    memcpy(s_rep_vm + ea, p, n);
}

static const uint8_t* rep_data(const fnr2_io* e, uint32_t n)
{
    if (e->data_off == IO_NODATA || (uint64_t)e->data_off + n > t_rep->nblob) return NULL;
    return t_rep->blob + e->data_off;
}

static int rep_dma(spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd, spu_iojrn_dma_fn real)
{
    (void)real;
    const int kind = dma_kind(cmd);
    const fnr2_io* e = rep_next(kind, cmd);
    if (e->lsa != lsa || e->ea != ea || e->size != size)
        rep_fail("io#%u: %s lsa=0x%05X ea=0x%08llX size=0x%X, recorded lsa=0x%05X ea=0x%08llX size=0x%X", e->seq,
                 io_kind_name(kind), lsa, (unsigned long long)ea, size, e->lsa, (unsigned long long)e->ea, e->size);
    if (e->flags & IO_F_XFER) {
        const uint32_t l = lsa & SPU_LS_MASK;
        const uint8_t* src = rep_data(e, size);
        if (!src) rep_fail("io#%u: no data in the journal", e->seq);
        if (kind == IO_GET) {
            memcpy(ctx->ls + l, src, size);
            rep_vm_put((uint32_t)ea, src, size);
            spu_overlay_note_get(ctx, (uint32_t)ea, ctx->ls + l, size);
        } else if (kind == IO_PUT && memcmp(ctx->ls + l, src, size)) {
            uint32_t k = 0; while (k < size && ctx->ls[l + k] == src[k]) k++;
            rep_fail("io#%u: PUT ea=0x%08llX size=0x%X: bytes differ from +0x%X (lr=0x%05X)", e->seq,
                     (unsigned long long)ea, size, k, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        }
    }
    return (int)e->result;
}

static int rep_atomic(spu_context* ctx, uint32_t cmd, spu_iojrn_atomic_fn real)
{
    (void)real;
    const int kind = atomic_kind(cmd);
    const fnr2_io* e = rep_next(kind, cmd);
    const uint32_t ea = ctx->mfc_eal & ~127u, lsa = ctx->mfc_lsa & SPU_LS_MASK;
    if (e->ea != ea || e->lsa != lsa)
        rep_fail("io#%u: %s ea=0x%08X lsa=0x%05X, recorded ea=0x%08llX lsa=0x%05X", e->seq, io_kind_name(kind), ea, lsa,
                 (unsigned long long)e->ea, e->lsa);
    const uint8_t* line = rep_data(e, 128);
    if (!line || lsa + 128u > SPU_LS_SIZE) rep_fail("io#%u: no line in the journal", e->seq);
    if (kind == IO_GETLLAR) {
        memcpy(ctx->ls + lsa, line, 128);
        memcpy(ctx->resv_line, line, 128);
        ctx->resv_ea = ea; ctx->resv_valid = (e->flags & IO_F_RESV) ? 1 : 0;
        ctx->dbg_getllar++;
    } else {
        if (memcmp(ctx->ls + lsa, line, 128)) {
            uint32_t k = 0; while (k < 128 && ctx->ls[lsa + k] == line[k]) k++;
            rep_fail("io#%u: %s ea=0x%08X: the stored line differs from +0x%X", e->seq, io_kind_name(kind), ea, k);
        }
        ctx->resv_valid = 0;
    }
    ctx->atomic_stat = e->result;
    return 1;
}

/* Channels another processor feeds come from the journal; the rest the run
 * computes itself, and they must agree with the recording. */
static int ch_external(uint32_t ch)
{
    return ch == SPU_RdInMbox || ch == SPU_RdSigNotify1 || ch == SPU_RdSigNotify2 || ch == SPU_RdEventStat;
}
static uint32_t rep_ch(spu_context* ctx, uint32_t ch, spu_iojrn_ch_fn real, int kind)
{
    const fnr2_io* e = rep_next(kind, ch);
    if (ch_external(ch)) {
        /* RdEventStat: the LR bit another processor raised (spu_resv_lost_poll
         * or a coherent store) lands in the context as it did when recorded. */
        if (ch == SPU_RdEventStat && kind == IO_RDCH && (e->result & 0x400u) && !(ctx->event_status & 0x400u)) {
            ctx->event_status |= 0x400u; ctx->resv_valid = 0; }
        return e->result;
    }
    const uint32_t v = real(ctx, ch);
    if (v != e->result)
        rep_fail("io#%u: %s ch%u = 0x%X, recorded 0x%X (lr=0x%05X)", e->seq, io_kind_name(kind), ch, v, e->result,
                 ctx->gpr[0]._u32[0] & SPU_LS_MASK);
    return v;
}
static uint32_t rep_rdch(spu_context* ctx, uint32_t ch, spu_iojrn_ch_fn real) { return rep_ch(ctx, ch, real, IO_RDCH); }
static uint32_t rep_rchcnt(spu_context* ctx, uint32_t ch, spu_iojrn_ch_fn real) { return rep_ch(ctx, ch, real, IO_RCHCNT); }

static void rep_irq(spu_context* ctx, uint32_t srr0, uint32_t target)
{
    const fnr2_io* e = rep_next(IO_IRQ, ctx->event_status & ctx->event_mask);
    if (e->lsa != srr0 || e->ea != target)
        rep_fail("io#%u: interrupt at srr0=0x%05X, recorded at 0x%05X", e->seq, srr0, e->lsa);
}

static void rep_ksvc(spu_context* ctx, uint32_t pc, void (*real)(spu_context*))
{
    (void)real;
    const fnr2_io* e = rep_next(IO_KSVC, pc);
    const uint8_t* d = rep_data(e, 144);
    if (!d) rep_fail("io#%u: no data in the journal", e->seq);
    memcpy(ctx->ls + 0x100, d, 128); memcpy(&ctx->gpr[3], d + 128, 16);
}

static const spu_iojrn_ops s_rep_ops = { rep_dma, rep_atomic, rep_rdch, rep_rchcnt, rep_irq, trace_step, rep_ksvc };
/* The same without the step hook (trace_step does nothing unless the run is
 * traced): an untraced replay's drains then run as in the game, where no
 * journal is set -- and with SPU_DIRECT their steps may go direct. */
static const spu_iojrn_ops s_rep_ops_nostep = { rep_dma, rep_atomic, rep_rdch, rep_rchcnt, rep_irq, NULL, rep_ksvc };

/* One replay run on its own thread: SPU float mode (round toward zero, FTZ,
 * DAZ, as every SPU worker runs), the recorded decrementer values, the forced
 * version. The caller gives up after a timeout: a function that calls into
 * the job manager spins on main memory the replay does not have (FNR1; an
 * FNR2 run is served from its journal). */
typedef struct {
    spu_fn fn; spu_context* ctx; const uint32_t* dec; uint32_t ndec; int mode;
    double us; int crashed; spu_fn_desc* desc;
    /* SPU_FN_REPLAY_WARM=<n>: after the checked run, n more runs on this
     * thread from copies of `src` into `warm` (caches warm, as in the game);
     * warm_us is the best of them. */
    const spu_context* src; spu_context* warm; uint64_t src_addr; int nwarm; double warm_us;
    /* FNR2 */
    fnr_rep* rep; uint32_t mxcsr, tagc; uint32_t dec_used; int tramp;
    int evlr;                 /* the journal ended with IO_EVLR (applied) */
    fnr_jrn* tr;              /* SPU_FN_REPLAY_TRACE: this run's trace */
} fnr_job;

static int fnr_run_one(fnr_job* j, spu_context* c)
{
    if (j->rep) {
        j->rep->i = 0; j->rep->bad = 0; j->rep->msg[0] = 0;
        t_rep = j->rep; t_spu_iojrn = j->tr ? &s_rep_ops : &s_rep_ops_nostep;
        if (g_spu_iojrn_rt) g_spu_iojrn_rt->tag_completed_set(c, j->tagc);
    }
    int crashed = 0;
    DWORD code = 0;
    if (j->tr) { t_trace_jrn = j->tr; t_rep_ctx = c; j->tr->tsc0 = __rdtsc(); s_watch = 1; }
    __try { j->fn(c); }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { crashed = code != FNR_REP_ABORT; }
    /* IO_EVLR: what other processors did to the LR bit and the reservation
     * by the end of the recorded run. */
    if (j->rep && !crashed && !j->rep->bad && j->rep->i < j->rep->nio && j->rep->io[j->rep->i].kind == IO_EVLR) {
        const fnr2_io* e = &j->rep->io[j->rep->i++];
        c->event_status = (c->event_status & ~0x400u) | (e->result & 0x400u);
        if (!(e->flags & IO_F_RESV)) c->resv_valid = 0;
        j->evlr = 1;
    }
    t_spu_iojrn = NULL;
    if (j->tr) { t_trace_jrn = NULL; t_rep_ctx = NULL; j->tr = NULL; }
    return crashed;
}

/* SPU_FN_REPLAY_PROF=<file>: a sampling profile of the replay runs. A
 * sampler thread suspends the run thread every ~20 us and logs its RIP; the
 * file holds the module base then the RIPs (u64 each), for a symbolizer with
 * the build's .map. SPU_FN_REPLAY_PROF_MODE=0|2: only the lifted or only the
 * native runs (default both). */
static volatile HANDLE s_prof_th;
static volatile LONG s_prof_on;
static uint64_t* s_prof; static size_t s_nprof, s_capprof;
static DWORD WINAPI fnr_prof_thread(LPVOID p)
{
    (void)p;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const long long gap = f.QuadPart / 50000;
    while (s_prof_on) {
        HANDLE t = s_prof_th;
        if (t && SuspendThread(t) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(t, &c)) {
                if (s_nprof == s_capprof) {
                    size_t n = s_capprof ? s_capprof * 2 : (1u << 20);
                    uint64_t* q = (uint64_t*)realloc(s_prof, n * 8);
                    if (q) { s_prof = q; s_capprof = n; }
                }
                if (s_nprof < s_capprof) s_prof[s_nprof++] = c.Rip;
            }
            ResumeThread(t);
        }
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        do { YieldProcessor(); QueryPerformanceCounter(&b); } while (b.QuadPart - a.QuadPart < gap);
    }
    return 0;
}

static DWORD WINAPI fnr_run_thread(LPVOID p)
{
    fnr_job* j = (fnr_job*)p;
    _mm_setcsr(j->rep ? j->mxcsr : (_mm_getcsr() | 0xE040u));
    t_feed = j->dec; t_feed_n = j->ndec; t_feed_i = 0;
    t_spu_dec_hook = fnr_dec_hook;
    t_force = j->mode; t_last_desc = NULL;
    g_spu_trampoline_fn = NULL;
    const double t0 = fnr_qpc_us();
    j->crashed = fnr_run_one(j, j->ctx);
    j->us = fnr_qpc_us() - t0;
    j->dec_used = t_feed_i;
    j->tramp = g_spu_trampoline_fn != NULL;
    j->desc = t_last_desc;
    j->warm_us = 0;
    if (j->nwarm > 0 && !j->crashed && j->warm && !(j->rep && j->rep->bad)) {
        double best = 1e30;
        fnr_rep keep; if (j->rep) keep = *j->rep;
        for (int k = 0; k < j->nwarm; k++) {
            memcpy(j->warm, j->src, sizeof *j->warm);
            fnr_fixup(j->warm, j->src_addr);
            j->warm->ls = j->warm->ls_store;
            t_feed_i = 0; g_spu_trampoline_fn = NULL;
            const unsigned long long w0 = __rdtsc();
            if (fnr_run_one(j, j->warm)) { j->crashed = 1; break; }
            const double w = (double)(__rdtsc() - w0) / 1000.0;   /* thousands of TSC ticks */
            if (w < best) best = w;
        }
        if (j->rep) *j->rep = keep;
        j->warm_us = best;
    }
    t_force = -1; t_feed = NULL; t_spu_dec_hook = NULL; g_spu_trampoline_fn = NULL; t_rep = NULL;
    return 0;
}

typedef struct {
    int image; uint32_t addr; const char* name;
    int files, lifted_bad, native_bad, has_native, timeouts;
    int async_files, async_lifted_bad, async_native_bad;
    double lifted_us, native_us, lifted_warm, native_warm;
    uint64_t bytes;
} fnr_stat;

/* An FNR2 file, decoded. */
typedef struct {
    fnr2_header h;
    uint32_t* dec;
    fnr2_io* io;
    uint8_t* blob;
    fnr2_img* imgs;
} fnr2_file;

static int ctx_get(const uint8_t** pp, const uint8_t* end, spu_context* c, const spu_context* base)
{
    const uint8_t* p = *pp;
    const size_t off = offsetof(spu_context, ls_store), rest = sizeof *c - off - SPU_LS_SIZE;
    if ((size_t)(end - p) < off + rest) return 0;
    memcpy(c, p, off); p += off;
    memcpy((uint8_t*)c + off + SPU_LS_SIZE, p, rest); p += rest;
    if (base) memcpy(c->ls_store, base->ls_store, SPU_LS_SIZE); else memset(c->ls_store, 0, SPU_LS_SIZE);
    for (;;) {
        uint32_t hd[2];
        if (end - p < 8) return 0;
        memcpy(hd, p, 8); p += 8;
        if (hd[0] == 0xFFFFFFFFu) break;
        const uint64_t n = (uint64_t)hd[1] * 16;
        if ((uint64_t)hd[0] + n > SPU_LS_SIZE || (uint64_t)(end - p) < n) return 0;
        memcpy(c->ls_store + hd[0], p, (size_t)n); p += n;
    }
    *pp = p;
    return 1;
}

static uint8_t* read_all(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END); const long long sz = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    uint8_t* b = sz > 0 ? (uint8_t*)malloc((size_t)sz) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = b ? (size_t)sz : 0;
    return b;
}

/* Parses the whole file into `in`/`out` and r (pointers into buf). */
static int fnr2_parse(uint8_t* buf, size_t n, fnr2_file* r, spu_context* in, spu_context* out)
{
    const uint8_t* p = buf, *end = buf + n;
    if (n < sizeof(fnr2_header)) return 0;
    memcpy(&r->h, p, sizeof r->h); p += sizeof r->h;
    if (memcmp(r->h.magic, "FNR2", 4) || r->h.ctx_size != sizeof(spu_context) || r->h.io_size != sizeof(fnr2_io)) return 0;
    if ((size_t)(end - p) < sizeof(fnr2_img) * r->h.nimg) return 0;
    r->imgs = (fnr2_img*)p; p += sizeof(fnr2_img) * r->h.nimg;
    if ((size_t)(end - p) < sizeof(uint32_t) * r->h.ndec) return 0;
    r->dec = (uint32_t*)p; p += sizeof(uint32_t) * r->h.ndec;
    if (!ctx_get(&p, end, in, NULL) || !ctx_get(&p, end, out, in)) return 0;
    if ((size_t)(end - p) < sizeof(fnr2_io) * r->h.nio) return 0;
    r->io = (fnr2_io*)p; p += sizeof(fnr2_io) * r->h.nio;
    if ((uint64_t)(end - p) < r->h.data_bytes) return 0;
    r->blob = (uint8_t*)p;
    return 1;
}

int spu_fnrec_replay_main(void)
{
    const char* dir = getenv("SPU_FN_REPLAY");
    if (!dir || !*dir) return -1;
    s_replaying = 1; s_watch = -1; fnw_init();
    const char* filt = getenv("SPU_FN_REPLAY_FUNCS");
    const char* re = getenv("SPU_FN_REPLAY_REPS");
    const int reps = re && atoi(re) > 0 ? atoi(re) : 1;
    const int verbose = getenv("SPU_FN_REPLAY_VERBOSE") != NULL;
    const char* te = getenv("SPU_FN_REPLAY_TIMEOUT_MS");
    const int timeout_ms = te && atoi(te) > 0 ? atoi(te) : 3000;
    const char* we = getenv("SPU_FN_REPLAY_WARM");
    const int nwarm = we && atoi(we) > 0 ? atoi(we) : 0;
    /* SPU_FN_REPLAY_NATIVE=0: lifted runs only (for a whole job, mode 2 is the
     * lifted job with the natives inside). SPU_FN_REPLAY_MAXMSG=<n>: mismatch
     * lines printed (default 200). */
    const char* ne = getenv("SPU_FN_REPLAY_NATIVE");
    const int want_native = !(ne && *ne == '0');
    const char* me = getenv("SPU_FN_REPLAY_MAXMSG");
    const int maxmsg = me ? atoi(me) : 200;
    int nmsg = 0;
    /* SPU_FN_REPLAY_DUMP=<dir>: write each run's final state (registers, local
     * store, pc, status and the other scalar SPU state) to <dir>/<file>.<lifted|
     * native>, so two builds can be compared byte for byte, including runs that
     * do not match the recording. */
    const char* dumpdir = getenv("SPU_FN_REPLAY_DUMP");
    /* SPU_FN_REPLAY_TRACE=<dir>: for FNR2 files, the first lifted run's trace
     * (wrapped functions and journal requests, as SPU_FN_TRACE_JOB) to
     * <dir>/<file>.replay.txt, to set beside the recording's .trace.txt. */
    const char* tracedir = getenv("SPU_FN_REPLAY_TRACE");
    const char* sce = getenv("SPU_FN_REPLAY_STATE");
    const int state_cmp = !(sce && *sce == '0');
    fprintf(stderr, "[fnrec] replaying %s (reps %d)\n", dir, reps);
    const char* prof = getenv("SPU_FN_REPLAY_PROF");
    const char* pme = getenv("SPU_FN_REPLAY_PROF_MODE");
    const int prof_mode = pme ? atoi(pme) : -1;
    HANDLE prof_th = NULL;
    if (prof && *prof) { s_prof_on = 1; prof_th = CreateThread(NULL, 0, fnr_prof_thread, NULL, 0, NULL); }

    static fnr_stat st[256]; int nst = 0;
    spu_context* in = (spu_context*)_aligned_malloc(sizeof(spu_context), 64);
    spu_context* out = (spu_context*)_aligned_malloc(sizeof(spu_context), 64);
    spu_context* run = (spu_context*)_aligned_malloc(sizeof(spu_context), 64);
    spu_context* warmctx = nwarm ? (spu_context*)_aligned_malloc(sizeof(spu_context), 64) : NULL;
    if (!in || !out || !run) return 3;
    uint8_t my_id[16]; fnr_build_id(my_id);
    int consec_timeouts = 0;
    int warned_build = 0;

    char pat[1024]; snprintf(pat, sizeof pat, "%s\\*.fnr", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "[fnrec] no .fnr files in %s\n", dir); return 2; }
    int bad = 0, nfiles2 = 0;
    do {
        char path[1024]; snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        size_t fsz = 0;
        uint8_t* buf = read_all(path, &fsz);
        if (!buf) continue;
        fnr_header hd; uint32_t* dec = NULL; uint32_t ndec = 0;
        fnr2_file f2; memset(&f2, 0, sizeof f2);
        fnr_rep rep; memset(&rep, 0, sizeof rep);
        int v2 = fsz >= 4 && !memcmp(buf, "FNR2", 4);
        int ok;
        uint64_t rec_addr;
        if (v2) {
            ok = fnr2_parse(buf, fsz, &f2, in, out);
            rep_vm_init();
            hd.image = f2.h.image; hd.addr = f2.h.addr;
            dec = f2.dec; ndec = f2.h.ndec; rec_addr = f2.h.ctx_addr;
            rep.io = f2.io; rep.nio = f2.h.nio; rep.blob = f2.blob; rep.nblob = f2.h.data_bytes;
            if (ok && memcmp(f2.h.build_id, my_id, 16) && !warned_build++)
                fprintf(stderr, "[fnrec] note: %s was recorded by a build with another build id (other lifted code or float modes)\n",
                        fd.cFileName);
            if (ok && !g_spu_iojrn_rt) { fprintf(stderr, "[fnrec] %s: FNR2 needs a runtime with the I/O journal hooks\n", fd.cFileName); ok = 0; }
            nfiles2++;
        } else {
            ok = fsz >= sizeof hd && (memcpy(&hd, buf, sizeof hd), !memcmp(hd.magic, "FNR1", 4)) && hd.ctx_size == sizeof(spu_context) &&
                 fsz >= sizeof hd + sizeof(uint32_t) * (size_t)hd.ndec + 2 * sizeof(spu_context);
            if (ok) {
                dec = (uint32_t*)(buf + sizeof hd); ndec = hd.ndec;
                memcpy(in, buf + sizeof hd + sizeof(uint32_t) * hd.ndec, sizeof *in);
                memcpy(out, buf + sizeof hd + sizeof(uint32_t) * hd.ndec + sizeof *in, sizeof *out);
            }
            rec_addr = ok ? hd.ctx_addr : 0;
        }
        if (!ok) { fprintf(stderr, "[fnrec] %s: unreadable or from another build (context size)\n", fd.cFileName); free(buf); continue; }
        spu_fn_desc probe;
        memset(&probe, 0, sizeof probe);
        probe.image = (int)hd.image; probe.addr = hd.addr;
        if (filt && *filt && !list_has(filt, &probe)) { free(buf); continue; }
        spu_fn fn = spu_lookup(hd.addr, (int)hd.image);
        if (!fn) { fprintf(stderr, "[fnrec] %s: no function at %d:0x%X\n", fd.cFileName, hd.image, hd.addr); free(buf); continue; }
        fnr_fixup(in, rec_addr);
        fnr_fixup(out, rec_addr);
        const int async = v2 && (f2.h.flags & FNR2_F_ASYNC);

        int si = -1;
        for (int i = 0; i < nst; i++) if (st[i].image == (int)hd.image && st[i].addr == hd.addr) si = i;
        if (si < 0 && nst < 256) { si = nst++; memset(&st[si], 0, sizeof st[si]); st[si].image = (int)hd.image; st[si].addr = hd.addr; }
        fnr_stat* s = &st[si];
        if (s->timeouts) { free(buf); continue; }   /* needs main memory: skip the rest */
        s->files++; s->bytes += fsz;
        if (async) s->async_files++;

        for (int mode = 0; mode <= 2; mode += 2) {
            if (mode == 2 && !want_native) break;
            double best = 1e30, wbest = 1e30; char msg[320] = "";
            int mism = 0;
            spu_fn_desc* seen = NULL;
            for (int r = 0; r < reps; r++) {
                memcpy(run, in, sizeof *run);
                fnr_fixup(run, (uint64_t)(uintptr_t)in);
                run->ls = run->ls_store;
                fnr_job job; memset(&job, 0, sizeof job);
                job.fn = fn; job.ctx = run; job.dec = dec; job.ndec = ndec; job.mode = mode;
                job.src = in; job.src_addr = (uint64_t)(uintptr_t)in; job.warm = warmctx; job.nwarm = r == 0 ? nwarm : 0;
                fnr_jrn trj; memset(&trj, 0, sizeof trj);
                if (v2) { job.rep = &rep; job.mxcsr = f2.h.mxcsr; job.tagc = f2.h.tag_completed;
                          if (tracedir && *tracedir && mode == 0 && r == 0) job.tr = &trj; }
                HANDLE th = CreateThread(NULL, 8u << 20, fnr_run_thread, &job, prof_th ? CREATE_SUSPENDED : 0, NULL);
                if (!th) { snprintf(msg, sizeof msg, "no thread"); mism = 9; break; }
                if (prof_th) { if (prof_mode < 0 || prof_mode == mode) s_prof_th = th; ResumeThread(th); }
                const DWORD wr = WaitForSingleObject(th, (DWORD)timeout_ms);
                s_prof_th = NULL;
                if (wr == WAIT_TIMEOUT) {
                    TerminateThread(th, 1); CloseHandle(th);
                    snprintf(msg, sizeof msg, v2 ? "TIMEOUT after %d ms" : "TIMEOUT after %d ms: it needs main memory (DMA/atomics), not replayable", timeout_ms);
                    mism = 8; if (!v2) s->timeouts++;
                    /* a run killed mid-way may hold a runtime lock: the next ones would only time out too */
                    if (v2 && ++consec_timeouts >= 3) { fprintf(stderr, "[fnrec] 3 timeouts in a row: stopping the replay\n"); fflush(stderr); ExitProcess(4); }
                    break;
                }
                CloseHandle(th);
                consec_timeouts = 0;
                if (trj.trace) {
                    char tp[1200]; snprintf(tp, sizeof tp, "%s/%s.replay.txt", tracedir, fd.cFileName);
                    FILE* tf = fopen(tp, "wb");
                    if (tf) { fwrite(trj.trace, 1, trj.ntrace, tf); fclose(tf); }
                    free(trj.trace);
                }
                seen = job.desc;
                if (job.crashed) { snprintf(msg, sizeof msg, "CRASHED (exception in the run)"); mism = 9; }
                if (job.us < best) best = job.us;
                if (job.nwarm && job.warm_us < wbest) wbest = job.warm_us;
                if (r == 0 && !mism && v2 && rep.bad) { snprintf(msg, sizeof msg, "%s", rep.msg); mism = 4; }
                if (r == 0 && !mism && v2 && rep.i != rep.nio) {
                    snprintf(msg, sizeof msg, "io: the run ended after %u of the %u journal entries (next %s cmd=0x%X lsa=0x%05X)",
                             rep.i, rep.nio, io_kind_name(rep.io[rep.i].kind), rep.io[rep.i].cmd, rep.io[rep.i].lsa);
                    mism = 5;
                }
                if (r == 0 && !mism && v2 && job.dec_used != ndec) {
                    snprintf(msg, sizeof msg, "dec: %u decrementer reads, recorded %u", job.dec_used, ndec); mism = 6; }
                if (r == 0 && !mism) mism = fnr_compare(out, run, msg, sizeof msg);
                if (r == 0 && !mism && v2 && state_cmp) mism = fnr_compare_state(out, run, job.tramp, job.evlr, msg, sizeof msg);
                if (r == 0 && dumpdir && *dumpdir) {
                    char dp[1200]; snprintf(dp, sizeof dp, "%s/%s.%s", dumpdir, fd.cFileName, mode ? "native" : "lifted");
                    FILE* df = fopen(dp, "wb");
                    if (df) {
                        uint32_t sc[16] = { run->pc, run->status, run->stop_code, run->srr0, run->int_enable,
                                            run->host_depth, (uint32_t)run->image_id, run->event_status, run->event_mask,
                                            run->mfc_lsa, run->mfc_eal, run->mfc_size, run->mfc_tag, run->drain_ret_pc,
                                            g_spu_trampoline_fn != NULL, (uint32_t)job.crashed };
                        fwrite(sc, sizeof sc, 1, df);
                        fwrite(run->gpr, sizeof run->gpr, 1, df);
                        fwrite(run->ls_store, SPU_LS_SIZE, 1, df);
                        fclose(df);
                    }
                }
                if (mism) break;
            }
            t_last_desc = seen;
            const int has_native = (t_last_desc && t_last_desc->native) || (v2 && fnw_is_job(&probe));
            if (t_last_desc && !s->name) s->name = t_last_desc->name;
            if (mode == 2 && !has_native) break;
            if (mode == 0) { s->lifted_us += best; s->lifted_warm += nwarm ? wbest : 0;
                             if (mism) { s->lifted_bad++; bad++; if (async) s->async_lifted_bad++; } }
            else           { s->has_native = 1; s->native_us += best; s->native_warm += nwarm ? wbest : 0;
                             if (mism) { s->native_bad++; bad++; if (async) s->async_native_bad++; } }
            if ((mism && nmsg++ < maxmsg) || verbose)
                fprintf(stderr, "[fnrec] %s %s: %s%s%s (%.2f us)\n", fd.cFileName, mode ? "native" : "lifted",
                        mism ? "MISMATCH " : "ok", async ? "[async] " : "", msg, best);
        }
        free(buf);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    fprintf(stderr, "[fnrec] %-48s %5s %8s %8s %10s %10s %7s\n", "function", "runs", "lift-bad", "nat-bad", "lifted us", "native us", "faster");
    for (int i = 0; i < nst; i++) {
        fnr_stat* s = &st[i];
        if (!s->files) continue;
        const double lu = s->lifted_us / s->files, nu = s->has_native ? s->native_us / s->files : 0;
        char name[64]; snprintf(name, sizeof name, "%d:%X %s", s->image, s->addr, s->name ? strrchr(s->name, '_') : "");
        if (s->has_native)
            fprintf(stderr, "[fnrec] %-48s %5d %8d %8d %10.2f %10.2f %6.2fx\n", name, s->files, s->lifted_bad,
                    s->native_bad, lu, nu, nu > 0 ? lu / nu : 0.0);
        else
            fprintf(stderr, "[fnrec] %-48s %5d %8d %8s %10.2f %10s %7s\n", name, s->files, s->lifted_bad, "-", lu, "-", "-");
        if (nfiles2)
            fprintf(stderr, "[fnrec]   %d:%X strict (no interrupt) runs %d: lifted-bad %d native-bad %d; async runs %d: "
                            "lifted-bad %d native-bad %d; %.1f KB per recording\n",
                    s->image, s->addr, s->files - s->async_files, s->lifted_bad - s->async_lifted_bad,
                    s->native_bad - s->async_native_bad, s->async_files, s->async_lifted_bad, s->async_native_bad,
                    (double)s->bytes / 1024.0 / s->files);
    }
    if (nwarm) {
        fprintf(stderr, "[fnrec] warm: best of %d runs on one thread, thousands of TSC ticks per call\n", nwarm);
        for (int i = 0; i < nst; i++) {
            fnr_stat* s = &st[i];
            if (!s->files) continue;
            fprintf(stderr, "[fnrec] warm %d:%X lifted %.3f\n", s->image, s->addr, s->lifted_warm / s->files);
            if (s->has_native) fprintf(stderr, "[fnrec] warmn %d:%X native %.3f\n", s->image, s->addr, s->native_warm / s->files);
        }
    }
    if (prof_th) {
        s_prof_on = 0; WaitForSingleObject(prof_th, INFINITE);
        FILE* pf = fopen(prof, "wb");
        if (pf) { const uint64_t base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL); fwrite(&base, 8, 1, pf);
                  fwrite(s_prof, 8, s_nprof, pf); fclose(pf); }
        fprintf(stderr, "[fnrec] profile: %zu samples -> %s\n", s_nprof, prof);
    }
    fprintf(stderr, "[fnrec] %s\n", bad ? "MISMATCHES FOUND" : "all runs match the recording");
    _aligned_free(in); _aligned_free(out); _aligned_free(run); if (warmctx) _aligned_free(warmctx);
    return bad ? 1 : 0;
}

/* ---- for the whole-job native (host/spu/natives/edgejob) ------------------- */

/* The native the dispatch would run for (image, addr): registered, and allowed
 * by SPU_NATIVE (game and replay alike), or NULL. */
spu_fn_t spu_fn_native_for(int image, uint32_t addr)
{
    const spu_fn_t f = native_find(image, addr);
    if (!f) return NULL;
    const char* n = getenv("SPU_NATIVE");
    if (n) {
        spu_fn_desc d; memset(&d, 0, sizeof d); d.image = image; d.addr = addr;
        if (!list_has(n, &d)) return NULL;
    }
    return f;
}

/* 1 when the whole-job wrapper drains the job itself (fn_lifted): what a
 * whole-job native must do when it hands the job to the lifted code. */
int spu_fnrec_job_drain(void) { fnw_init(); return s_job_drain; }

/* 1 when the job needs the dispatch at every wrapped call: the variant key, a
 * per-job trace, or a recording. A whole-job native then runs the lifted job. */
int spu_fnrec_job_watched(void)
{
    fnw_init();
    const char* rec = getenv("SPU_FN_RECORD");
    return s_watch || (rec && *rec);
}

/* 1 when this thread runs a replay in its forced-native mode. */
int spu_fnrec_replay_native(void) { return t_force == 2; }

/* The source hash an image's wrappers noted (spu_fnrec_note_image), 0 if none. */
uint64_t spu_fnrec_image_hash(int image)
{
    for (int i = 0; i < s_nimgs; i++) if (s_imgs[i].image == image) return s_imgs[i].hash;
    return 0;
}
