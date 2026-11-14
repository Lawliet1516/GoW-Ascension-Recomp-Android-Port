/*
 * SPU_SMC_STUB_FAST (spu_smc_stub.h) against the SMC microstep it short-cuts.
 * The reference below is spu_smc_microstep's step loop copied verbatim from
 * spu_channels.c, up to the point where it hands an unknown instruction to the
 * interpreter (here: return 0). Every case runs both on the same random local
 * store and register file and compares all 128 registers, the whole local
 * store, pc, interrupt enable and the trampoline:
 *   - the game's stubs (lqa / stqa $0..$75 to 0x3F9C0.., bi $79 at 0x3FEC0),
 *   - random blocks: 8..256 lqa/stqa in any order, any registers, any
 *     addresses, bi with/without the E/D bits, any target register, anywhere
 *     in local store,
 *   - shapes the fast path must decline, leaving everything untouched:
 *     another terminator (bisl, iret, biz, binz, br), fewer than 8 transfers,
 *     a block that stores over its own words, one that runs off local store.
 *
 * From ps3kit\runtime\spu:
 *   clang-cl /nologo /O2 -msse4.1 -I. -I..\..\include tests\test_spu_smc_stub.c
 */
#include "spu_helpers.h"
#include "spu_smc_stub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the runtime defines for the inline LS helpers and the trampoline. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_pcwatch(spu_context* ctx, uint32_t pc) { (void)ctx; (void)pc; }
int g_spu_ls_watch_n;   /* 0: no LS watch armed */
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; }

static int ref_microstep(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK & ~3u;
    extern void spu_pcwatch(spu_context*, uint32_t);
    for (int steps = 0; steps < 4096; steps++) {
        spu_pcwatch(ctx, pc);
        uint32_t w = ((uint32_t)ctx->ls[pc] << 24) | ((uint32_t)ctx->ls[pc+1] << 16) |
                     ((uint32_t)ctx->ls[pc+2] << 8) | ctx->ls[pc+3];
        uint32_t op11 = w >> 21, op9 = w >> 23, op8 = w >> 24, op7 = w >> 25;
        uint32_t rt = w & 0x7F, ra = (w >> 7) & 0x7F, rb = (w >> 14) & 0x7F;
        int32_t  i10 = (int32_t)(w << 8) >> 22;               /* bits 14-23 sext */
        int32_t  i16 = (int32_t)(int16_t)((w >> 7) & 0xFFFF);
        (void)rb;

        if (op11 == 0x1A8 || op11 == 0x1A9 || op11 == 0x1AA || op11 == 0x1AB) {
            /* bi / bisl / iret / bisled (+E/D interrupt bits) */
            if (op11 == 0x1AB &&                       /* bisled: only on event */
                (ctx->event_status & ctx->event_mask) == 0) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            uint32_t tgt = (op11 == 0x1AA) ? ctx->srr0
                                           : ctx->gpr[ra]._u32[0];
            if (op11 == 0x1A9 || op11 == 0x1AB)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op11 >= 0x128 && op11 <= 0x12B) {
            /* biz / binz / bihz / bihnz rt,ra (+E/D bits, taken-only) */
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op11) {
            case 0x128: taken = (cv == 0); break;                     /* biz  */
            case 0x129: taken = (cv != 0); break;                     /* binz */
            case 0x12A: taken = ((cv & 0xFFFF) == 0); break;          /* bihz */
            default:    taken = ((cv & 0xFFFF) != 0); break;          /* bihnz*/
            }
            if (!taken) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            ctx->pc = ctx->gpr[ra]._u32[0] & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x064 || op9 == 0x060 || op9 == 0x066 || op9 == 0x062) {
            /* br / bra / brsl / brasl */
            uint32_t tgt = (op9 == 0x060 || op9 == 0x062)
                         ? ((uint32_t)i16 << 2)
                         : (pc + ((uint32_t)i16 << 2));
            if (op9 == 0x066 || op9 == 0x062)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x040 || op9 == 0x042 || op9 == 0x044 || op9 == 0x046) {
            /* brz / brnz / brhz / brhnz rt,label (relative) */
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op9) {
            case 0x040: taken = (cv == 0); break;
            case 0x042: taken = (cv != 0); break;
            case 0x044: taken = ((cv & 0xFFFF) == 0); break;
            default:    taken = ((cv & 0xFFFF) != 0); break;
            }
            if (!taken) { pc += 4; continue; }
            ctx->pc = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op8 == 0x24) {                                    /* stqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            spu_ls_write128(ctx, a, ctx->gpr[rt]);
            pc += 4; continue;
        }
        if (op8 == 0x34) {                                    /* lqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x041 || op9 == 0x061) {                   /* stqa / lqa (abs) */
            /* LSA = I16 << 2 (SPU ISA, RI16 form), as in spu_interp.c and
             * the lifter. */
            uint32_t a = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK & ~15u;
            if (op9 == 0x041) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x047 || op9 == 0x067) {                   /* stqr / lqr (rel) */
            uint32_t a = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~15u;
            if (op9 == 0x047) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op8 == 0x44) {                                    /* xori rt,ra,i10 */
            /* Runtime-generated job stubs toggle their own instruction words.
             * Use the signed RI10 immediate, just like statically lifted xori. */
            ctx->gpr[rt] = spu_xori(ctx->gpr[ra], i10);
            pc += 4; continue;
        }
        if (op8 == 0x1C) {                                    /* ai rt,ra,i10 */
            u128 r = ctx->gpr[ra];
            for (int k = 0; k < 4; k++) r._u32[k] += (uint32_t)i10;
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op11 == 0x040 || op11 == 0x0C0) {                 /* sf / a (word) */
            u128 x = ctx->gpr[ra], y = ctx->gpr[rb], r;
            for (int k = 0; k < 4; k++)
                r._u32[k] = (op11 == 0x040) ? y._u32[k] - x._u32[k]
                                            : x._u32[k] + y._u32[k];
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op9 == 0x081) { ctx->gpr[rt] = spu_splat_u32((uint32_t)i16); pc += 4; continue; }  /* il  */
        if (op9 == 0x082) { ctx->gpr[rt] = spu_splat_u32(((w >> 7) & 0xFFFF) << 16); pc += 4; continue; } /* ilhu */
        if (op9 == 0x0C1) { u128 r = ctx->gpr[rt];            /* iohl */
            for (int k = 0; k < 4; k++) r._u32[k] |= (w >> 7) & 0xFFFF;
            ctx->gpr[rt] = r; pc += 4; continue; }
        if (op7 == 0x21)  { ctx->gpr[rt] = spu_splat_u32((w >> 7) & 0x3FFFF); pc += 4; continue; } /* ila */
        if (op11 == 0x201 || op11 == 0x001) { pc += 4; continue; }  /* nop/lnop */
        if (op11 == 0x002 || op11 == 0x003) { pc += 4; continue; }  /* sync/dsync */
        /* Branch hints, all no-ops for execution: hbr is the 11-bit-opcode RR
         * form (0x1AC), hbra and hbrr are the 7-bit-opcode RI18 form (0x08,
         * 0x09) -- the same op7 group as ila (0x21) above, NOT op9. Testing
         * op9 here never matched either (hbrr 0x12033296 has op9 0x24), so a
         * runtime-generated stub carrying a branch hint decoded to UNKNOWN,
         * the microstep bailed, and the SPU fell into branch-to-0. */
        /* sagemono reached the same fix independently in #166, from a different
         * witness (hbrr 0x1200048C rather than 0x12033296). Same line, same
         * diagnosis, arrived at separately -- which is about as good as
         * corroboration gets for a decode bug. */
        if (op11 == 0x1AC || op7 == 0x08 || op7 == 0x09) { pc += 4; continue; } /* hbr/hbra/hbrr */

        return 0;   /* spu_smc_microstep hands this instruction to the interpreter */
    }
    return 0;
}

static unsigned long long rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }

static spu_context a, b;
static uint8_t ls_a[SPU_LS_SIZE], ls_b[SPU_LS_SIZE];
static void word(spu_context* c, uint32_t pc, uint32_t w)
{
    c->ls[pc] = (uint8_t)(w >> 24); c->ls[pc + 1] = (uint8_t)(w >> 16);
    c->ls[pc + 2] = (uint8_t)(w >> 8); c->ls[pc + 3] = (uint8_t)w;
}
static void randomize(spu_context* c)
{
    for (uint32_t i = 0; i < SPU_LS_SIZE; i += 4) word(c, i, rnd());
    for (int r = 0; r < 128; r++) for (int k = 0; k < 4; k++) c->gpr[r]._u32[k] = rnd();
    c->int_enable = rnd() & 1;
}
static uint32_t ldst(int store, uint32_t rt, uint32_t lsa) { return ((store ? 0x041u : 0x061u) << 23) | (((lsa >> 2) & 0xFFFFu) << 7) | (rt & 0x7Fu); }
static uint32_t bi(uint32_t ra, uint32_t ed) { return (0x1A8u << 21) | ed | ((ra & 0x7Fu) << 7); }

static int fails, ran, declined;
static void compare(const char* what, int expect_fast)
{
    b = a; b.ls = ls_b; memcpy(ls_b, ls_a, SPU_LS_SIZE);   /* same start for both */
    g_spu_trampoline_fn = 0;
    const int rf = spu_smc_stub_fast(&a);
    void (*tf)(spu_context*) = g_spu_trampoline_fn;
    if (!expect_fast) {
        declined++;
        if (rf || tf || memcmp(a.gpr, b.gpr, sizeof a.gpr) != 0 || memcmp(ls_a, ls_b, SPU_LS_SIZE) != 0 ||
            a.pc != b.pc || a.int_enable != b.int_enable) { fails++; printf("FAIL %s: not declined cleanly\n", what); }
        return;
    }
    g_spu_trampoline_fn = 0;
    const int rr = ref_microstep(&b);
    void (*tr)(spu_context*) = g_spu_trampoline_fn;
    ran++;
    if (rf != 1 || rr != 1) { fails++; printf("FAIL %s: fast=%d ref=%d\n", what, rf, rr); return; }
    if (memcmp(a.gpr, b.gpr, sizeof a.gpr) != 0) { fails++; printf("FAIL %s: registers\n", what); }
    if (memcmp(a.ls, b.ls, SPU_LS_SIZE) != 0) { fails++; printf("FAIL %s: local store\n", what); }
    if (a.pc != b.pc || a.int_enable != b.int_enable || tf != tr) {
        fails++; printf("FAIL %s: pc %05X/%05X int %d/%d\n", what, a.pc, b.pc, a.int_enable, b.int_enable);
    }
}

/* The game's stub: 76 transfers at 0x3FEC0, bi $79, bi $79 padding to the top. */
static void game_stub(int store)
{
    randomize(&a);
    for (uint32_t r = 0; r < 76; r++) word(&a, 0x3FEC0 + 4 * r, ldst(store, r, 0x3F9C0 + 16 * r));
    for (uint32_t pc = 0x3FEC0 + 4 * 76; pc < SPU_LS_SIZE; pc += 4) word(&a, pc, 0x35002780u);
    a.pc = 0x3FEC0;
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ns(void) { static LARGE_INTEGER f; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f);
                             QueryPerformanceCounter(&c); return (double)c.QuadPart * 1e9 / (double)f.QuadPart; }

int main(void)
{
    a.ls = ls_a; b.ls = ls_b;
    for (int i = 0; i < 200; i++) { game_stub(i & 1); compare(i & 1 ? "game save stub" : "game restore stub", 1); }

    for (int i = 0; i < 20000; i++) {
        randomize(&a);
        const uint32_t n = 8 + rnd() % 249, len = (n + 1) * 4;
        const uint32_t pc = (rnd() % ((SPU_LS_SIZE - len) / 4)) * 4;
        const int stores = rnd() % 3;     /* 0 loads, 1 stores, 2 mixed */
        for (uint32_t k = 0; k < n; k++) {
            uint32_t lsa;
            do lsa = rnd() & (SPU_LS_SIZE - 1); while ((lsa & ~15u) < pc + len && pc < (lsa & ~15u) + 16);
            const int st = stores == 2 ? (int)(rnd() & 1) : stores;
            word(&a, pc + 4 * k, ldst(st, rnd(), lsa));
        }
        static const uint32_t eds[3] = { 0, 0x40000u, 0x80000u };
        word(&a, pc + 4 * n, bi(rnd(), eds[rnd() % 3]));
        a.pc = pc | (rnd() & 3);
        compare("random block", 1);
    }
    /* Loads and stores to the same few lines, in any order: the order matters. */
    for (int i = 0; i < 5000; i++) {
        randomize(&a);
        const uint32_t n = 8 + rnd() % 40, pc = 0x8000;
        for (uint32_t k = 0; k < n; k++) word(&a, pc + 4 * k, ldst(rnd() & 1, rnd() % 8, 0x20000 + 16 * (rnd() % 4)));
        word(&a, pc + 4 * n, bi(rnd() % 8, 0));
        a.pc = pc;
        compare("aliasing block", 1);
    }

    for (int i = 0; i < 3000; i++) {
        game_stub(i & 1);
        const int kind = i % 7;
        const char* what = "";
        switch (kind) {
        case 0: word(&a, 0x3FEC0 + 4 * 76, (0x1A9u << 21) | (79u << 7) | 5u); what = "bisl end"; break;
        case 1: word(&a, 0x3FEC0 + 4 * 76, (0x1AAu << 21)); what = "iret end"; break;
        case 2: word(&a, 0x3FEC0 + 4 * 76, (0x128u << 21) | (79u << 7) | 3u); what = "biz end"; break;
        case 3: word(&a, 0x3FEC0 + 4 * 76, (0x129u << 21) | (79u << 7) | 3u); what = "binz end"; break;
        case 4: word(&a, 0x3FEC0 + 4 * 76, (0x064u << 23) | (16u << 7)); what = "br end"; break;
        case 5: a.pc = 0x3FEC0 + 4 * 69; what = "7 transfers"; break;     /* 7 transfers then bi */
        case 6: word(&a, 0x3FEC0 + 4 * (rnd() % 76), ldst(1, rnd(), 0x3FEC0 + 16 * (rnd() % 20)));
                what = "stores over its own words"; break;
        }
        compare(what, 0);
    }
    /* Off the end of local store: transfers up to the last word, no terminator. */
    for (int i = 0; i < 50; i++) {
        randomize(&a);
        for (uint32_t pc = SPU_LS_SIZE - 64; pc < SPU_LS_SIZE; pc += 4) word(&a, pc, ldst(0, rnd(), 0x1000 + 16 * (rnd() % 64)));
        a.pc = SPU_LS_SIZE - 64;
        compare("runs off local store", 0);
    }

    /* Speed: the game's restore stub. */
    game_stub(0);
    static spu_context base;
    base = a;
    memcpy(ls_b, ls_a, SPU_LS_SIZE);
    const int N = 200000;
    double t0 = now_ns();
    for (int i = 0; i < N; i++) { b.pc = 0x3FEC0; memcpy(b.gpr, base.gpr, 80 * 16); ref_microstep(&b); }
    double t1 = now_ns();
    for (int i = 0; i < N; i++) { a.pc = 0x3FEC0; memcpy(a.gpr, base.gpr, 80 * 16); spu_smc_stub_fast(&a); }
    double t2 = now_ns();
    printf("restore stub: microstep %.0f ns, fast %.0f ns a call\n", (t1 - t0) / N, (t2 - t1) / N);

    printf("%s: %d blocks compared, %d declined, %d failures\n", fails ? "FAIL" : "PASS", ran, declined, fails);
    return fails != 0;
}
