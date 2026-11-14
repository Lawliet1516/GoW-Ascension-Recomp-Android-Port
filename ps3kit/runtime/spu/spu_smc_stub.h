/* spu_smc_stub.h -- SPU_SMC_STUB_FAST (default on; SPU_SMC_STUB_FAST=0: off).
 *
 * The WWS job manager builds a register save/restore stub at run time near
 * the top of local store (God of War: Ascension: LS 0x3FEC0, `lqa $0..$75`
 * or `stqa $0..$75` to 0x3F9C0.., then `bi $79`) and calls it around every
 * job switch. No lift can exist for bytes made at run time, so the SMC
 * microstep runs it -- one decode, one spu_pcwatch call and one LS helper per
 * instruction, ~310 ns a call and 15% of the SMC path's time in combat
 * (11.7 M calls a minute).
 *
 * spu_smc_stub_fast takes a block of that shape -- at least 8 lqa/stqa (the
 * absolute forms, any registers, any addresses, in any order) ending in a
 * plain `bi` -- in one go, through the same LS helpers in the same order and
 * with the same branch the microstep's bi takes, so the registers, local
 * store, pc and interrupt enable come out the same. It declines (returns 0,
 * nothing done; the microstep runs as before) for anything else: another
 * terminator (bisl, iret, biz...), a block that writes its own words (the
 * step loop would read the new ones), one that reaches the end of local
 * store, more than 256 transfers, or when spu_pcwatch's traces are on
 * (SPU_ITRACE_MOD / SPU_PCWATCH see every step). */
#ifndef SPU_SMC_STUB_H
#define SPU_SMC_STUB_H

#include "spu_context.h"
#include <stdlib.h>
#include <string.h>
#if defined(__SSSE3__)
#include <tmmintrin.h>
#endif

static int spu_smc_stub_fast_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SPU_SMC_STUB_FAST");
        on = !(e && e[0] == '0') && !getenv("SPU_ITRACE_MOD") && !getenv("SPU_PCWATCH");
    }
    return on;
}

static inline uint32_t spu_smc_stub_word(const spu_context* ctx, uint32_t a)
{
    uint32_t w;                              /* a is word aligned and inside local store */
    memcpy(&w, ctx->ls + a, 4);
    return SPU_BSWAP32(w);
}

/* 1: the block ran and ended in its branch (ctx->pc, g_spu_trampoline_fn set
 * as the microstep's bi sets them); 0: not this shape, nothing changed. */
static int spu_smc_stub_fast(spu_context* ctx)
{
    const uint32_t pc0 = ctx->pc & SPU_LS_MASK & ~3u;
    uint32_t ins[256];
    uint32_t p = pc0, n = 0, w, stores = 0;
    for (;;) {
        if (p > SPU_LS_SIZE - 4u) return 0;
        w = spu_smc_stub_word(ctx, p);
        const uint32_t op9 = w >> 23;
        if (op9 != 0x041u && op9 != 0x061u) break;            /* stqa / lqa */
        if (n == 256u) return 0;
        ins[n++] = w;
        stores |= op9 == 0x041u;
        p += 4u;
    }
    if ((w >> 21) != 0x1A8u || n < 8u) return 0;              /* bi */
    const uint32_t end = p + 4u;                              /* the block: [pc0, end) */
    if (stores)
        for (uint32_t k = 0; k < n; k++) {
            if ((ins[k] >> 23) != 0x041u) continue;
            const uint32_t a = (((ins[k] >> 7) & 0xFFFFu) << 2) & SPU_LS_MASK & ~15u;
            if (a < end && pc0 < a + 16u) return 0;          /* it writes its own words */
        }
    /* No store lands in the block, so these are the words the step loop
     * would read, in its order. */
#if defined(__SSSE3__) && !defined(SPU_LS_NO_HOOKS)
    /* With no LS probe or watch armed (all three gates read and 0) the LS
     * helpers come down to a 16-byte load or store and a byte shuffle, plus
     * the WWS probe on a store to 0x1320 (kept: that store takes the helper).
     * The gates are read once here: in the helpers every transfer reloaded
     * them, since the byte stores may alias them. */
    if (g_spu_ls_probe == 0 && g_spu_ls_watch_n == 0 && g_spu_smc_watch == 0) {
        const __m128i bswap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        uint8_t* const ls = ctx->ls;
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t x = ins[k];
            const uint32_t a = (((x >> 7) & 0xFFFFu) << 2) & SPU_LS_MASK & ~15u;
            u128* const r = &ctx->gpr[x & 0x7Fu];
            if ((x >> 23) == 0x041u) {
                if (a == 0x1320u) { spu_ls_write128(ctx, a, *r); continue; }
                _mm_storeu_si128((__m128i*)(ls + a), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)r), bswap));
            } else {
                _mm_storeu_si128((__m128i*)r, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + a)), bswap));
            }
        }
    } else
#endif
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t x = ins[k];
        const uint32_t a = (((x >> 7) & 0xFFFFu) << 2) & SPU_LS_MASK & ~15u;
        if ((x >> 23) == 0x041u) spu_ls_write128(ctx, a, ctx->gpr[x & 0x7Fu]);
        else                     ctx->gpr[x & 0x7Fu] = spu_ls_read128(ctx, a);
    }
    if (w & 0x40000u) ctx->int_enable = 1;
    else if (w & 0x80000u) ctx->int_enable = 0;
    ctx->pc = ctx->gpr[(w >> 7) & 0x7Fu]._u32[0] & SPU_LS_MASK & ~3u;
    g_spu_trampoline_fn = spu_indirect_branch;
    return 1;
}

#endif /* SPU_SMC_STUB_H */
