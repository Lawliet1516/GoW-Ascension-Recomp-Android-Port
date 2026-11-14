/*
 * ps3recomp - write tracer for one guest address range (SPU_WTRACE)
 *
 * SPU_WTRACE=<lo>-<hi> (hex guest addresses): every write that lands in
 * [lo, hi) is kept in a ring with who made it:
 *   P  a PPU store (lifted code; the inline store gate sends the range out of
 *      line, see ppu_recomp.h vm_st_inl_ok / vm_raw_st_ok),
 *   V  a PPU raw store (vector stvx/stvewx, byte-reversed, dcbz),
 *   C  a PPU store-conditional (stwcx./stdcx.),
 *   S  an SPU DMA PUT,
 *   A  an SPU atomic commit (PUTLLC/PUTLLUC),
 *   G  an SPU DMA GET (a read, kept so a history shows where a fetch fell).
 * spu_wtrace_dump prints the writes that touched a span, newest first.
 *
 * Built for God of War: Ascension's module-216 freeze: a 16-byte batch-job
 * command lands on a DMA list still in use in the batch buffer at 0x1232xxxx,
 * and the question is which agent wrote it, and when.
 */
#ifndef SPU_WTRACE_H
#define SPU_WTRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0/0 when off. Read by the lifted code's inline store gate. */
extern uint32_t g_wtrace_lo, g_wtrace_span;
/* An optional second range: SPU_WTRACE=<lo>-<hi>,<lo2>-<hi2>. Every recorder
 * sees it except the lifted code's inline store gate, which knows only the
 * first; a PPU store into the second is recorded when it goes out of line
 * anyway, i.e. on a line an SPU has reserved (the Wws job-list header line,
 * which every job manager polls, always is). */
extern uint32_t g_wtrace_lo2, g_wtrace_span2;

static inline int spu_wtrace_overlap(uint32_t lo, uint32_t span, uint32_t ea, uint32_t n)
{
    return span && (uint64_t)ea < (uint64_t)lo + span && (uint64_t)lo < (uint64_t)ea + (n ? n : 1u);
}
static inline int spu_wtrace_hit(uint32_t ea, uint32_t n)
{
    return spu_wtrace_overlap(g_wtrace_lo, g_wtrace_span, ea, n) ||
           spu_wtrace_overlap(g_wtrace_lo2, g_wtrace_span2, ea, n);
}

/* Parse SPU_WTRACE; call once at boot, after the title's defaults are set.
 * resolve (may be NULL) names the lifted function a host return address is
 * in, for PPU writes. */
void spu_wtrace_init(uint32_t (*resolve)(void* ra));

/* Record one write. data may be NULL (a zero fill: dcbz). a..d by source:
 * P/V/C: host thread id, guest lr, guest r1, 0 (ra = host return address,
 * resolved to the guest function when printed); S/A: image id, resident
 * overlay, pc, lr. */
void spu_wtrace_rec(char who, uint32_t ea, uint32_t n, const void* data,
                    uint32_t a, uint32_t b, uint32_t c, uint32_t d, void* ra);
/* The same with a fifth word; PPU writes pass guest r29 and r30 in d and e
 * (in the descriptor copy loop of God of War: Ascension's batch builder,
 * func_0046368C, r29 is the source pointer and r30 the builder). */
void spu_wtrace_rec2(char who, uint32_t ea, uint32_t n, const void* data,
                     uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, void* ra);

/* Print the recorded writes that touched [ea, ea + n), newest first (up to
 * max), under the given tag. */
void spu_wtrace_dump(uint32_t ea, uint32_t n, unsigned max, const char* tag);

/* Guest r30 of the newest PPU write that touched [ea, ea + n), or 0. */
uint32_t spu_wtrace_last_ppu_e(uint32_t ea, uint32_t n);
/* Its guest r29 (in the batch builder's descriptor copy: the source + 16). */
uint32_t spu_wtrace_last_ppu_d(uint32_t ea, uint32_t n);

/* The second range's 128-byte line read as a Wws job-list header (four 16-byte
 * segment headers at +0x40: avail, taken, minActive[6]), one line per recorded
 * write, newest first: who changed which counters. */
void spu_wtrace_dump_hdr(unsigned max, const char* tag);

/* A committed SPU atomic on the second range: report a commit that changed
 * another SPU's minActive slot (old = the reserved snapshot, new = the line
 * written; spu_num = the committer's SpursKernelContext spuNum). */
void spu_wtrace_hdr_commit(uint32_t ea, const uint8_t* old_line, const uint8_t* new_line,
                           uint32_t spu_num, uint32_t img, uint32_t pc, uint32_t lr);

#ifdef __cplusplus
}
#endif

#endif /* SPU_WTRACE_H */
