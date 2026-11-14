// God of War: Ascension's VRAM manager: CopyLocalMemory (func_00374A70) and
// MoveLocalMemory (func_00374EBC) check their arguments against the local
// memory size (the word at 0x9BDF88) and, on a bad one, print
// "invalid size" / "... offset error" and execute `tw 31,r1,r1` -- an
// unconditional trap that ends the game on a PS3. The lifter emits tw as a
// comment, so the lifted copy carried on with the bad extent: a negative size
// (a texture block shrunk below zero by func_00370E38, timing-dependent) made
// CopyLocalMemory walk 4 GB in 16 MB steps, ~500 error lines per call, every
// frame, until the game froze (fps ~12 or no flips).
//
// PPU_VRAM_GUARD=1: tools/vram_guard_patch.py makes each of the two functions
// ask ppu_vram_guard() first; when the arguments are ones the guest itself
// rejects (the same checks, with the 32-bit sums done without wrapping, so a
// negative size cannot pass by overflow), the call returns without copying.
// The patch also turns each of their `tw` sites into a jump to the function's
// epilogue (ppu_vram_trap), for checks that fail further in. Either way it
// only changes behaviour where real hardware would already have trapped. The
// bad block descriptor stays; its texture shows stale data until it is next
// loaded. Unset (0), the lifted functions run as before.
//
// Seen 2026-10-24: the negative size comes from a texture shrunk twice --
// func_00370E38 called again (from func_00372B60 at 0x3734AC) on a block
// already shrunk (size 0x5560 = 0x55560 - 0x50000, block+0x20 cleared) with
// the same cut 0x50000. Why the texture is picked again is still open.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include "ppu_recomp.h"   /* the generated header: ppu_context and the guest loads */

extern "C" void ppu_dump_guest_stack(ppu_context* ctx, const char* tag);
extern "C" {

void ppu_vram_trace_bad(const char* why);

int ppu_vram_guard(ppu_context* ctx, int kind)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PPU_VRAM_GUARD"); on = e && e[0] == '1'; }
    if (!on) return 0;
    const uint64_t dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4], size = (uint32_t)ctx->gpr[5];
    const uint64_t lim = vm_read32(0x009BDF88u);   /* the bound both functions compare against */
    const int bad = size >= lim || src >= lim || dst >= lim || src + size > lim || dst + size > lim;
    if (!bad) return 0;
    static long n = 0;
    if (n++ < 8) {
        fprintf(stderr, "[vram-guard] skipped %s(dst=0x%08X src=0x%08X size=0x%08X) lr=0x%08X (local size 0x%08X; #%ld)\n",
                kind ? "MoveLocalMemory" : "CopyLocalMemory", (uint32_t)dst, (uint32_t)src, (uint32_t)size,
                (uint32_t)ctx->lr, (uint32_t)lim, n);
        if (n <= 2) ppu_dump_guest_stack(ctx, "vram-guard");
    } else if ((n % 1000) == 0)
        fprintf(stderr, "[vram-guard] %ld calls skipped so far\n", n);
    return 1;
}

/* One of the guest's own checks inside the copy/move failed and it reached its
 * `tw 31,r1,r1` (the error line is already printed). Under PPU_VRAM_GUARD the
 * lifted code then jumps to the function's epilogue: the call is abandoned, as
 * the trap would have abandoned the game. Returns whether to do that. */
int ppu_vram_trap(ppu_context* ctx, int kind)
{
    ppu_vram_trace_bad(kind ? "MoveLocalMemory trap" : "CopyLocalMemory trap");
    static int on = -1;
    if (on < 0) { const char* e = getenv("PPU_VRAM_GUARD"); on = e && e[0] == '1'; }
    if (!on) return 0;
    static long n = 0;
    if (n++ < 8)
        fprintf(stderr, "[vram-guard] trap in %s: call abandoned (lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X; #%ld)\n",
                kind ? "MoveLocalMemory" : "CopyLocalMemory", (uint32_t)ctx->lr, (uint32_t)ctx->gpr[3],
                (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], n);
    else if ((n % 1000) == 0)
        fprintf(stderr, "[vram-guard] %ld traps so far\n", n);
    return 1;
}

/* PPU_VRAM_SHRINK_LOG=1 (diagnostic): func_00370E38 shrinks a texture's VRAM
 * block -- block = texture+0x44, its size at block+0xC drops by the amount at
 * texture+0x1C and its address at block+0x8 moves up by it. A shrink larger
 * than the block makes the size negative, which is where the bad moves come
 * from. Says so, with the call chain, the first few times. Never changes
 * anything. */
/* At the top of func_00370E38, which cuts texture+0x1C bytes (its top mip
 * levels) off the texture's VRAM block (block+0xC = size): a cut larger than
 * the block is a texture picked again after it was already cut -- its size
 * would go negative, the compaction behind it then copied 4 GB in 16 MB steps
 * and every texture after it in VRAM was scrambled, then the game froze (seen
 * at area loads). PPU_VRAM_SHRINK_SAFE (default on; =0 off): such a shrink is
 * skipped (returns 1; the texture keeps its mip levels), which is what the
 * first cut already did for it. */
int ppu_vram_shrink_check(ppu_context* ctx)
{
    static int on = -1, safe = -1;
    /* PPU_VRAM_FAULT=<n> (test only): the n-th shrink is made larger than its
     * block (texture+0x1C = size + 1 MB), to reproduce the negative block size
     * on demand and see what PPU_VRAM_GUARD does with the moves that follow. */
    static long fault = -1, calls = 0;
    if (on < 0) {
        const char* e = getenv("PPU_VRAM_SHRINK_LOG"); on = e && e[0] == '1';
        const char* f = getenv("PPU_VRAM_FAULT"); fault = f ? atol(f) : 0;
        if (fault > 0) on = 1;
        { const char* t = getenv("PPU_VRAM_TRACE"); if (t && t[0] >= '1') on = 1; }
        const char* s = getenv("PPU_VRAM_SHRINK_SAFE"); safe = !(s && s[0] == '0');
    }
    const uint32_t tex = (uint32_t)ctx->gpr[4];
    const uint32_t blk = vm_read32(tex + 0x44);
    if (!blk) return 0;
    if (fault > 0 && ++calls == fault) {
        const uint32_t sz = vm_read32(blk + 0xC);
        vm_write32(tex + 0x1C, sz + 0x100000u);
        fprintf(stderr, "[vram-fault] shrink #%ld: texture 0x%08X block 0x%08X size 0x%08X -> cut 0x%08X (test)\n",
                calls, tex, blk, sz, sz + 0x100000u);
    }
    const uint32_t size = vm_read32(blk + 0xC), cut = vm_read32(tex + 0x1C);
    /* PPU_VRAM_REPAIR (default on; =0: only the skip below, as before). A
     * streamable texture (texture+0x12 bit 0) whose block already lacks its
     * top mips (block size + cut <= the full size, texture+0x20) is the real
     * case behind "picked again after its cut": the block was made at the
     * reduced size (func_0036DD1C) but its entry in the per-block table
     * ([manager] + ((block - [manager+4]) / 128) * 16, halfword +2) has lost
     * 0x8000 "streamed, low-res". Without it the picker keeps taking the block
     * for a full-size one, and func_0036EEF8 describes the texture to the RSX
     * at full size over a block that holds only the low mips -- it showed
     * garbage until the next area load (a tester, and ~1 in 4 heavy-fight
     * runs here). The repair puts the block where its cut would have left it:
     * 0x8000 set, 0x2000 (top mips present) and 0x4000 (wanted) clear, and
     * rebuilds the descriptor of every instance as func_00370E38 does after a
     * cut (func_0036F2D0(instance, texture) over block+0x18 -> +0x14), so the
     * texture shows its low mips and streams back in the normal way. Which
     * write drops 0x8000 is still open. */
    static int repair = -1;
    if (repair < 0) { const char* r = getenv("PPU_VRAM_REPAIR"); repair = !(r && r[0] == '0'); }
    if (repair && cut && (vm_read16(tex + 0x12) & 1u) && (uint64_t)size + cut <= vm_read32(tex + 0x20)) {
        const uint32_t base = vm_read32((uint32_t)ctx->gpr[5]), start = vm_read32((uint32_t)ctx->gpr[5] + 4);
        const uint32_t ent = base + (uint32_t)(((int32_t)(blk - start) / 128) * 16);
        const uint16_t was = vm_read16(ent + 2);
        vm_write16(ent + 2, (uint16_t)((was | 0x8000u) & ~0x6000u));
        /* The game's own call, registers as its caller left them afterwards. */
        const uint64_t lr = ctx->lr, cr = ctx->cr, ctr = ctx->ctr, xer = ctx->xer;
        const uint64_t r2 = ctx->gpr[2], r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
        ctx->gpr[2] = 0x0081EF60ull;   /* the TOC func_00370E38 uses */
        int rebuilt = 0;
        for (uint32_t inst = vm_read32(blk + 0x18); inst && rebuilt < 64; inst = vm_read32(inst + 0x14), rebuilt++) {
            ctx->gpr[3] = inst; ctx->gpr[4] = tex;
            ctx->lr = 0x00370FC0;
            func_0036F2D0(ctx);
            while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf((void*)ctx); }
        }
        ctx->lr = lr; ctx->cr = cr; ctx->ctr = ctr; ctx->xer = xer;
        ctx->gpr[2] = r2; ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5;
        static long k = 0;
        if (k++ < 16)
            fprintf(stderr, "[vram-shrink] repaired: texture 0x%08X block 0x%08X (0x%08X of 0x%08X, top mips not "
                    "there) had lost its streamed flag: flags 0x%04X -> 0x%04X, %d descriptor(s) rebuilt, "
                    "it streams back in normally (#%ld)\n", tex, blk, size, vm_read32(tex + 0x20), was,
                    vm_read16(ent + 2), rebuilt, k);
        return 1;
    }
    if (cut <= size) return 0;
    if (safe) {
        static long k = 0;
        if (k++ < 16)
            fprintf(stderr, "[vram-shrink] skipped: texture 0x%08X block 0x%08X has 0x%08X left, cut 0x%08X "
                    "(its block lost the streamed flag; this texture can look garbled until the next area "
                    "load) lr=0x%08X (#%ld)\n", tex, blk, size, cut, (uint32_t)ctx->lr, k);
    }
    if (!on) return safe;
    ppu_vram_trace_bad("shrink larger than its block");
    static long n = 0;
    if (n++ < 6) {
        fprintf(stderr, "[vram-shrink] texture 0x%08X block 0x%08X: size 0x%08X < cut 0x%08X (addr 0x%08X, +0x20 0x%08X) lr=0x%08X r3=0x%08X r5=0x%08X\n",
                tex, blk, size, cut, vm_read32(blk + 0x8), vm_read32(blk + 0x20), (uint32_t)ctx->lr,
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[5]);
        fprintf(stderr, "[vram-shrink]   texture:");
        for (uint32_t o = 0; o < 0x50; o += 4) fprintf(stderr, " %08X", vm_read32(tex + o));
        fprintf(stderr, "\n[vram-shrink]   block:  ");
        for (uint32_t o = 0; o < 0x30; o += 4) fprintf(stderr, " %08X", vm_read32(blk + o));
        fprintf(stderr, "\n");
        ppu_dump_guest_stack(ctx, "vram-shrink");
    }
    return safe;
}
/* PPU_VRAM_REPAIR, also at the top of func_0036EEF8 (writes a texture's RSX
 * descriptor; r3 = the descriptor, r4 = the texture). The check above runs only
 * when the picker tries to cut the block again, which can be long after the
 * entry lost 0x8000 or never; every descriptor written in between described the
 * full size over the low mips. That is the user's purple burst (2026-11-02, the
 * Grapple hall): big see-through quads of striped pieces of the textures behind
 * the block in VRAM, with a repair logged seconds later. Here the entry gets the
 * same flags first, so this descriptor comes out at the low size as after a cut,
 * and the texture streams back in the normal way. Descriptors written before the
 * flags went wrong were written with the right ones. The same goes for a low
 * block whose entry kept 0x8000 but has 0x2000 (top mips present): flags 0xAC00
 * over a 0xAAB0 block of 0xAAAB0, the user's save, 6 min of play (user49), put
 * right by the cut-time repair above. Both look like another block's flags
 * copied in (the moves copy all eight bits, func_00370D7C). PPU_VRAM_DESC=0:
 * only reported here (the cut-time repair stays). */
void ppu_vram_desc_check(ppu_context* ctx)
{
    static int repair = -1, change = -1;
    if (repair < 0) {
        const char* r = getenv("PPU_VRAM_REPAIR"); repair = !(r && r[0] == '0');
        const char* d = getenv("PPU_VRAM_DESC"); change = !(d && d[0] == '0');
    }
    if (!repair) return;
    const uint32_t tex = (uint32_t)ctx->gpr[4];
    const uint32_t blk = vm_read32(tex + 0x44);
    if (!blk) return;
    const uint32_t size = vm_read32(blk + 0xC), cut = vm_read32(tex + 0x1C);
    if (!cut || !(vm_read16(tex + 0x12) & 1u) || (uint64_t)size + cut > vm_read32(tex + 0x20)) return;
    /* The per-block table func_0036EEF8 reads: [0x96E174] + ((block - [0x96E178]) / 128) * 16. */
    const uint32_t ent = vm_read32(0x0096E174u) + (uint32_t)(((int32_t)(blk - vm_read32(0x0096E178u)) / 128) * 16);
    const uint16_t was = vm_read16(ent + 2);
    if ((was & 0x8000u) && !(was & 0x2000u)) return;   /* the low-size descriptor, as it should be */
    if (change) vm_write16(ent + 2, (uint16_t)((was | 0x8000u) & ~0x6000u));
    static long k = 0;
    if (k++ < (change ? 16 : 64))
        fprintf(stderr, "[vram-desc] %s: texture 0x%08X block 0x%08X (0x%08X of 0x%08X, top mips not there) "
                "had flags 0x%04X (%s) -> 0x%04X before its descriptor was written (#%ld)\n",
                change ? "repaired" : "seen (PPU_VRAM_DESC=0, left)", tex, blk, size, vm_read32(tex + 0x20), was,
                (was & 0x8000u) ? "top mips present" : "streamed flag lost", vm_read16(ent + 2), k);
}
/* PPU_VRAM_TRACE=1 (diagnostic; tools/vram_trace_patch.py puts a
 * ppu_vram_scope_t at the top of the VRAM manager's functions): every entry
 * goes into a ring with the thread, the caller and the manager's state; two
 * threads inside the manager at once is reported as it happens (with both
 * guest stacks), and the ring is printed at the first bad copy/move, shrink
 * or block. Never changes anything. */
extern "C" unsigned long long ps3_qpc_us(void);
struct vram_ev { unsigned long long t; uint32_t tid, fn, lr, a3, a4, a5, x0, x1, x2, x3, depth; };
static vram_ev s_ring[16384];
static std::atomic<uint32_t> s_ridx{0};
static std::atomic<long long> s_owner{-1};
static std::atomic<uint32_t> s_owner_fn{0};
static thread_local int t_vdepth = 0;
static int vram_trace_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PPU_VRAM_TRACE"); on = e && e[0] >= '1' ? e[0] - '0' : 0; }
    return on;
}
static uint32_t vram_label4e(void)
{
    const uint32_t base = vm_read32(0x009BDF94u), stride = vm_read32(0x009BDF98u);
    return base ? vm_read32(base + 78u * stride) : 0xFFFFFFFFu;
}
static void vram_ring_dump(const char* why)
{
    static std::atomic<int> dumped{0};
    if (dumped.fetch_add(1) != 0) return;
    const uint32_t n = s_ridx.load(), cnt = n < 16384u ? n : 16384u;
    fprintf(stderr, "[vram-trace] === ring dump (%s): last %u of %u events, now t=%llu ===\n", why, cnt, n, ps3_qpc_us());
    for (uint32_t i = n - cnt; i != n; i++) {
        const vram_ev& e = s_ring[i & 16383u];
        fprintf(stderr, "[vram-ev] t=%llu tid=%u d=%u fn=%08X lr=%08X r3=%08X r4=%08X r5=%08X | %08X %08X %08X %08X\n",
                e.t, e.tid, e.depth, e.fn, e.lr, e.a3, e.a4, e.a5, e.x0, e.x1, e.x2, e.x3);
    }
    fprintf(stderr, "[vram-trace] === end ring dump ===\n");
}
/* The slot array the manager keeps its blocks in, in order (0x96B51C, count
 * at 0x96DDBC; a 0 entry is a gap; block+6 is the block's own index). */
static int vram_check_blocks(const char* where)
{
    const uint32_t lim = vm_read32(0x009BDF88u), count = vm_read32(0x0096DDBCu);
    if (count > 4096u) { fprintf(stderr, "[vram-check] %s: slot count %u\n", where, count); return 1; }
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t b = vm_read32(0x0096B51Cu + i * 4u);
        if (!b) continue;
        const uint32_t off = vm_read32(b + 0x8), size = vm_read32(b + 0xC);
        const uint32_t idx = vm_read16(b + 0x6);
        if (size >= lim || off >= lim || (uint64_t)off + size > lim || idx != i) {
            static int n = 0;
            if (n++ < 4)
                fprintf(stderr, "[vram-check] %s: slot %u block 0x%08X off 0x%08X size 0x%08X idx %u (count %u)\n",
                        where, i, b, off, size, idx, count);
            return 1;
        }
    }
    return 0;
}
/* The two move lists: the plan (0x96E58C, count at 0x96E70C) and the moves
 * queued for the frame's flush (0x96E714, count at 0x96E710), 12-byte
 * entries {from, to, size} in offsets from the texture heap's base. An entry
 * the heap (size at 0x96DDC4) cannot hold is the bad move before it runs. */
static int vram_check_moves(const char* where)
{
    static int reported = 0;
    if (reported >= 3) return 0;
    const uint32_t hs = vm_read32(0x0096DDC4u);
    struct { uint32_t base, cnt_ea; const char* name; } lists[2] = {
        { 0x0096E58Cu, 0x0096E70Cu, "plan" }, { 0x0096E714u, 0x0096E710u, "queued" } };
    for (int k = 0; k < 2; k++) {
        const uint32_t n = vm_read32(lists[k].cnt_ea);
        int bad = n > 512u;
        for (uint32_t i = 0; !bad && i < n; i++) {
            const uint32_t e = lists[k].base + i * 12u;
            const uint64_t a = vm_read32(e), b = vm_read32(e + 4), z = vm_read32(e + 8);
            bad = z > hs || a + z > hs || b + z > hs;
        }
        if (!bad) continue;
        reported++;
        fprintf(stderr, "[vram-moves] %s: bad %s list, %u entries (heap base 0x%08X size 0x%08X, seq %u, label 0x%08X) t=%llu\n",
                where, lists[k].name, n, vm_read32(0x0081C6E0u), hs, vm_read32(0x0096E158u), vram_label4e(), ps3_qpc_us());
        for (uint32_t i = 0; i < n && i < 64u; i++) {
            const uint32_t e = lists[k].base + i * 12u;
            fprintf(stderr, "[vram-moves]   #%u from 0x%08X to 0x%08X size 0x%08X\n", i, vm_read32(e), vm_read32(e + 4), vm_read32(e + 8));
        }
        return 1;
    }
    return 0;
}
void ppu_vram_enter(ppu_context* ctx, uint32_t fn)
{
    if (!vram_trace_on()) return;
    const long long tid = (long long)ctx->thread_id;
    if (t_vdepth++ == 0) {
        long long exp = -1;
        if (!s_owner.compare_exchange_strong(exp, tid) && exp != tid) {
            static std::atomic<int> nc{0};
            const int k = nc.fetch_add(1);
            if (k < 12)
                fprintf(stderr, "[vram-trace] CONCURRENT: tid %lld enters %08X (lr %08X) while tid %lld is in %08X, t=%llu\n",
                        tid, fn, (uint32_t)ctx->lr, exp, s_owner_fn.load(), ps3_qpc_us());
            if (k < 3) ppu_dump_guest_stack(ctx, "vram-concurrent");
        } else
            s_owner_fn.store(fn);
    }
    /* the stream loader's per-chunk steps: tens of thousands a run, they
     * would push the manager's own history out of the ring */
    if (fn == 0x003C98CCu || fn == 0x003C8F54u) return;
    vram_ev e;
    e.t = ps3_qpc_us(); e.tid = (uint32_t)tid; e.fn = fn; e.lr = (uint32_t)ctx->lr; e.depth = (uint32_t)t_vdepth;
    e.a3 = (uint32_t)ctx->gpr[3]; e.a4 = (uint32_t)ctx->gpr[4]; e.a5 = (uint32_t)ctx->gpr[5];
    e.x0 = e.x1 = e.x2 = e.x3 = 0;
    if (fn == 0x00372B60u || fn == 0x00373680u || fn == 0x00366E18u) {
        e.x0 = vm_read32(0x0096E158u); e.x1 = vram_label4e(); e.x2 = vm_read32(0x0096E710u); e.x3 = vm_read32(0x0096E70Cu);
    } else if (fn == 0x00370E38u) {
        const uint32_t tex = (uint32_t)ctx->gpr[4], blk = vm_read32(tex + 0x44);
        if (blk) {
            e.x0 = vm_read32(blk + 0x8); e.x1 = vm_read32(blk + 0xC); e.x2 = vm_read32(tex + 0x1C);
            const uint32_t base = vm_read32((uint32_t)ctx->gpr[5]), start = vm_read32((uint32_t)ctx->gpr[5] + 4);
            const uint32_t ent = base + (uint32_t)(((int32_t)(blk - start) / 128) * 16);
            e.x3 = ((uint32_t)vm_read16(tex + 0x12) << 16) | vm_read16(ent + 2);
        }
    } else if (fn == 0x003763C8u) {
        /* the VRAM reconfiguration state machine, every frame: requested and
         * current configuration, its state, the RSX get and the get it waits for */
        const uint32_t ctrl = vm_read32(0x009BE004u);
        e.x0 = (vm_read32(0x0082A448u) << 16) | (vm_read32(0x0082A44Cu) & 0xFFFFu);
        e.x1 = vm_read32(0x0081C6C8u); e.x2 = ctrl ? vm_read32(ctrl + 4) : 0; e.x3 = vm_read32(0x0081C718u);
    } else {
        e.x0 = vm_read32(0x0096E158u); e.x1 = vm_read32(0x0096DDBCu); e.x2 = vm_read32(0x0096E710u);
        e.x3 = vm_read32(0x0081C6E0u);   /* the texture heap's base */
    }
    s_ring[s_ridx.fetch_add(1) & 16383u] = e;
    /* PPU_VRAM_TRACE=2: also print, as they happen, the calls that change
     * the layout -- moves, shrinks, flushes and plans with work queued, the
     * other entry points, and anything another thread calls. */
    if (vram_trace_on() >= 2) {
        static uint32_t main_tid = 0;
        if (!main_tid && fn == 0x00372B60u) main_tid = (uint32_t)tid;
        const int inner_copy = fn == 0x00374A70u && e.lr >= 0x00374EBCu && e.lr < 0x003751C0u;
        static uint32_t last_cfg = 0xFFFFFFFFu, last_state = 0xFFFFFFFFu;
        int quiet = (fn == 0x00372B60u || fn == 0x00373680u || fn == 0x00366E18u) && !e.x2 && !e.x3;
        if (fn == 0x003763C8u) {
            quiet = e.x0 == last_cfg && e.x1 == last_state;
            last_cfg = e.x0; last_state = e.x1;
        }
        const int noisy = fn == 0x0036DD0Cu || fn == 0x0036E7F4u || fn == 0x00370D7Cu || fn == 0x0037122Cu;
        if (!inner_copy && ((!quiet && !noisy) || (main_tid && e.tid != main_tid))) {
            static std::atomic<long> nl{0};
            if (nl.fetch_add(1) < 200000)
                fprintf(stderr, "[vram-ev] t=%llu tid=%u d=%u fn=%08X lr=%08X r3=%08X r4=%08X r5=%08X | %08X %08X %08X %08X\n",
                        e.t, e.tid, e.depth, e.fn, e.lr, e.a3, e.a4, e.a5, e.x0, e.x1, e.x2, e.x3);
        }
    }
    if (fn == 0x00373680u && vram_check_blocks("flush")) vram_ring_dump("bad block at flush");
    if (fn == 0x00373680u && vram_check_moves("flush")) vram_ring_dump("bad move queued at flush");
}
void ppu_vram_leave(ppu_context* ctx, uint32_t fn)
{
    if (!vram_trace_on()) return;
    /* The plan list holds 32 entries (0x96E58C..0x96E70C, its count right
     * after); func_0036ECC0 appends a copy for each extra instance of a
     * texture without a bound. */
    if (fn == 0x0036ECC0u) {
        static uint32_t maxn = 0;
        const uint32_t n = vm_read32(0x0096E70Cu);
        if (n > maxn && n <= 4096u) {
            maxn = n;
            fprintf(stderr, "[vram-plan] plan list now %u entries (queued %u, seq %u, label 0x%08X, loading %u/%u) t=%llu\n",
                    n, vm_read32(0x0096E710u), vm_read32(0x0096E158u), vram_label4e(),
                    vm_read32(0x00997578u), vm_read32(0x00999898u), ps3_qpc_us());
        }
        if (n > 32u) {
            static int k = 0;
            if (k++ < 4) {
                fprintf(stderr, "[vram-plan] OVERFLOW: plan list count 0x%08X, queued count 0x%08X\n", n, vm_read32(0x0096E710u));
                ppu_dump_guest_stack(ctx, "vram-plan");
            }
        }
    }
    if ((fn == 0x003717A4u || fn == 0x00372B60u || fn == 0x0037130Cu || fn == 0x0037122Cu) && vram_check_moves(
            fn == 0x003717A4u ? "after planner 3717A4" : fn == 0x00372B60u ? "after 372B60" : "after heap resize"))
        vram_ring_dump("bad move list");
    if (--t_vdepth == 0) {
        long long exp = (long long)ctx->thread_id;
        s_owner.compare_exchange_strong(exp, -1);
        if (fn == 0x00372B60u && vram_check_blocks("after 372B60")) vram_ring_dump("bad block after 372B60");
    }
}
void ppu_vram_trace_bad(const char* why)
{
    if (vram_trace_on()) vram_ring_dump(why);
}

/* PPU_VRAM_PLAN (tools/vram_plan_patch.py): the instance-copy plan at
 * 0x96E58C holds 32 entries of {dst, src, size}; its count (0x96E70C) and the
 * queued-move count (0x96E710) follow it, and nothing in the game stops the
 * 33rd entry from landing on them. Entries past 32 wait here instead, and the
 * drain takes 32 at a time; the count word keeps the true total. */
static const uint32_t kPlanBase = 0x0096E58Cu, kPlanSlots = 32u, kPlanExt = 4096u;
static uint32_t s_plan_ext[kPlanExt][3];
static uint32_t s_plan_total, s_plan_batch;
static std::atomic_flag s_plan_lock = ATOMIC_FLAG_INIT;
static void plan_lock(void) { while (s_plan_lock.test_and_set(std::memory_order_acquire)) {} }
static void plan_unlock(void) { s_plan_lock.clear(std::memory_order_release); }

void ppu_vram_plan_put(uint32_t base, uint32_t off, uint32_t w0, uint32_t w1, uint32_t w2)
{
    const uint32_t i = off / 12u;
    if (base != kPlanBase || i < kPlanSlots) {
        vm_write32(base + off, w0); vm_write32(base + off + 4u, w1); vm_write32(base + off + 8u, w2);
        return;
    }
    plan_lock();
    const uint32_t j = i - kPlanSlots;
    if (j < kPlanExt) { s_plan_ext[j][0] = w0; s_plan_ext[j][1] = w1; s_plan_ext[j][2] = w2; }
    plan_unlock();
    static long n = 0;
    if (j >= kPlanExt) {
        if (n++ < 8) fprintf(stderr, "[vram-plan] copy plan entry %u dropped (more than %u)\n", i, kPlanSlots + kPlanExt);
    } else if (i == kPlanSlots && n++ < 16)
        fprintf(stderr, "[vram-plan] copy plan passed the game's %u entries; the rest wait on the host\n", kPlanSlots);
}

uint32_t ppu_vram_plan_batch(uint32_t count)
{
    plan_lock();
    if (count > kPlanSlots + kPlanExt) count = kPlanSlots + kPlanExt;
    s_plan_total = count;
    s_plan_batch = count < kPlanSlots ? count : kPlanSlots;
    const uint32_t b = s_plan_batch;
    plan_unlock();
    static uint32_t peak = 15;                   /* the high-water mark, from 16 entries up */
    if (count > peak) {
        peak = count;
        fprintf(stderr, "[vram-plan] copy plan peak %u entries (the game's list holds %u)\n", count, kPlanSlots);
    }
    return b;
}

uint32_t ppu_vram_plan_consume(void)
{
    plan_lock();
    const uint32_t total = s_plan_total, done = s_plan_batch;
    const uint32_t rem = total > done ? total - done : 0u;
    for (uint32_t n = 0; n < rem; n++) {        /* entry done + n (>= 32: on the host) to slot n */
        const uint32_t* e = s_plan_ext[done + n - kPlanSlots];
        if (n < kPlanSlots) {
            vm_write32(kPlanBase + n * 12u, e[0]); vm_write32(kPlanBase + n * 12u + 4u, e[1]);
            vm_write32(kPlanBase + n * 12u + 8u, e[2]);
        } else {
            uint32_t* d = s_plan_ext[n - kPlanSlots];
            d[0] = e[0]; d[1] = e[1]; d[2] = e[2];
        }
    }
    s_plan_total = s_plan_batch = 0;
    plan_unlock();
    if (rem) {
        static long k = 0;
        if (k++ < 16) fprintf(stderr, "[vram-plan] drained %u copies, %u wait for the next drain\n", done, rem);
    }
    return rem;
}
}

// Z pre-pass skip (experiment, default off). God of War: Ascension draws the
// main scene twice: a depth-only pre-pass (fxZPrePass, vtable 0x6684B8; its
// per-frame builder is func_00411F2C) and then the colour pass with depth func
// EQUAL. On the PS3 the pre-pass saved RSX fill rate; here it costs a third of
// the draws (and their SPU EDGE jobs). tools/zprepass_patch.py puts
//     if (ppu_zprepass_skip(ctx)) return;
// at the top of func_00411F2C; on a skip r3 = 0, the builder's own "nothing to
// draw" result (0x411F68). The colour pass then needs the renderer to test
// LESS_EQUAL and write depth itself (LD_SKIP_PREPASS) or the scene is lost.
//   PPU_ZPREPASS_SKIP=1     skip every frame
//   PPU_ZPREPASS_SKIP_AB=1  skip on/off every PPU_ZPREPASS_SKIP_AB_MS (default 15000) ("[zpp-ab] on|off")
extern "C" int ppu_zprepass_skip(ppu_context* ctx)
{
    static int mode = -1;   /* 0 off, 1 always, 2 A/B */
    static unsigned long long t_next = 0;
    static int ab_on = 0;
    static long skipped = 0, ran = 0;
    if (mode < 0) {
        const char* a = getenv("PPU_ZPREPASS_SKIP_AB");
        const char* e = getenv("PPU_ZPREPASS_SKIP");
        mode = (a && a[0] == '1') ? 2 : (e && e[0] == '1') ? 1 : 0;
        if (mode) fprintf(stderr, "[zprepass] skip mode %s\n", mode == 2 ? "A/B" : "always");
    }
    if (!mode) return 0;
    int skip = 1;
    if (mode == 2) {
        unsigned long long now = ps3_qpc_us();
        if (now >= t_next) {
            if (t_next) { fprintf(stderr, "[zprepass] last period: skipped %ld ran %ld\n", skipped, ran); skipped = ran = 0; }
            ab_on = !ab_on;
            static unsigned long long period = 0;
            if (!period) { const char* m = getenv("PPU_ZPREPASS_SKIP_AB_MS"); period = 1000ull * (m ? atol(m) : 15000); }
            t_next = now + period;
            fprintf(stderr, "[zpp-ab] %s t=%lluus\n", ab_on ? "on" : "off", now);
        }
        skip = ab_on;
    }
    if (!skip) { ran++; return 0; }
    skipped++;
    ctx->gpr[3] = 0;
    return 1;
}

// Texture streaming budget (experiment, default off). func_00366C24 builds the
// 0x30-byte parameter block of the per-frame "UpdateTexStreaming" SPU job
// (+0x00 = 0xA28 entries of the VRAM flag table, +0x08 = the table, +0x20 = 7)
// and the game streams about that many textures in per frame (6-9 measured), so
// at 25 fps an area's textures take 2.4x as long to arrive as at 60.
// tools/texstream_patch.py calls this right after the +0x20 store (r29 = the
// block); nothing else is touched.
//   TEX_STREAM_BUDGET=<n>    store n (1..64) instead of 7
//   TEX_STREAM_BUDGET=auto   7 x (frame time / 16.7 ms), 7..21: per second as at 60 fps
extern "C" void ppu_texstream_budget(ppu_context* ctx)
{
    static int mode = -2;   /* -1 auto, 0 off, n fixed */
    if (mode == -2) {
        const char* e = getenv("TEX_STREAM_BUDGET");
        mode = !e || !*e ? 0 : !strcmp(e, "auto") ? -1 : atoi(e);
        if (mode > 64) mode = 64;
        if (mode < -1) mode = 0;
        if (mode) fprintf(stderr, "[tex-stream] budget %s\n", mode < 0 ? "auto (7 per 16.7 ms)" : e);
    }
    if (!mode) return;
    const uint32_t blk = (uint32_t)ctx->gpr[29];
    const uint32_t orig = vm_read16(blk + 0x20);
    if (orig != 7) {
        static long odd = 0;
        if (odd++ < 4) fprintf(stderr, "[tex-stream] +0x20 is %u, not 7: left alone\n", orig);
        return;
    }
    static unsigned long long t_last = 0, t_rep = 0;
    static double frame_ms = 16.7;
    static long n = 0, sum = 0;
    const unsigned long long now = ps3_qpc_us();
    if (t_last && now > t_last) {
        double ms = (double)(now - t_last) / 1000.0;
        if (ms > 100.0) ms = 100.0;   /* a loading screen or a pause: not a frame */
        frame_ms += (ms - frame_ms) * 0.1;
    }
    t_last = now;
    int b = mode;
    if (mode < 0) {
        b = (int)(7.0 * frame_ms / 16.7 + 0.5);
        if (b < 7) b = 7;
        if (b > 21) b = 21;
    }
    vm_write16(blk + 0x20, (uint16_t)b);
    n++; sum += b;
    if (now - t_rep >= 5000000ull) {
        if (t_rep) fprintf(stderr, "[tex-stream] per 5s: %ld jobs, budget %.1f avg, frame %.1f ms\n", n, n ? (double)sum / n : 0.0, frame_ms);
        t_rep = now; n = 0; sum = 0;
    }
}

/* The game's own debug print, func_001B16E4(level, fmt, ...): empty in the retail executable (it only spills
 * r5..r10 to the caller's parameter save area), so the developers' log lines ("starting local multiplayer",
 * "enter lobby: ...", "start local game", ...) go nowhere. tools/gamelog_patch.py calls this before its return.
 * PS3_GAME_LOG=1 prints them as "[game] ..."; unset, it returns at once. The variadic arguments are read from
 * the parameter save area (ELF v1: argument k at r1 + 0x30 + 8k; the stub has just stored r5..r10 there), so
 * a float argument is its double in that 8-byte slot. */
static void game_log_str(uint32_t ea, char* out, size_t cap)
{
    size_t n = 0;
    if (!ea) { snprintf(out, cap, "(null)"); return; }
    while (n + 1 < cap) { const char c = (char)vm_read8(ea + (uint32_t)n); if (!c) break; out[n++] = c; }
    out[n] = 0;
}

extern "C" void ppu_game_log(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3_GAME_LOG"); on = (e && *e && strcmp(e, "0")) ? 1 : 0; }
    if (!on) return;
    const uint32_t fmt = (uint32_t)ctx->gpr[4];
    const uint32_t ap0 = (uint32_t)ctx->gpr[1] + 0x40u;   /* argument 2 = the first variadic one (r5) */
    if (!fmt) return;
    char f[512], out[1024];
    game_log_str(fmt, f, sizeof f);
    size_t o = 0; int argi = 0;
    for (const char* s = f; *s && o + 1 < sizeof out; ) {
        if (*s != '%') { out[o++] = *s++; continue; }
        if (s[1] == '%') { out[o++] = '%'; s += 2; continue; }
        char spec[32]; size_t k = 0; spec[k++] = *s++;
        while (*s && strchr("-+ #0123456789.", *s) && k < 20) spec[k++] = *s++;
        int lng = 0;
        while (*s && strchr("hlLqjzt", *s)) { if (*s == 'l' || *s == 'q' || *s == 'j' || *s == 'z' || *s == 't') lng++; s++; }
        const char conv = *s ? *s++ : 0;
        if (!conv) break;
        const uint64_t v = vm_read64(ap0 + 8u * (uint32_t)argi++);
        char piece[600]; piece[0] = 0;
        switch (conv) {
        case 'd': case 'i':
            spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, lng >= 1 ? (long long)(int64_t)v : (long long)(int32_t)(uint32_t)v); break;
        case 'u': case 'x': case 'X': case 'o':
            spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0;
            snprintf(piece, sizeof piece, spec, lng >= 1 ? (unsigned long long)v : (unsigned long long)(uint32_t)v); break;
        case 'c':
            spec[k++] = 'c'; spec[k] = 0; snprintf(piece, sizeof piece, spec, (int)(v & 0xFF)); break;
        case 'p':
            snprintf(piece, sizeof piece, "0x%08X", (uint32_t)v); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d; memcpy(&d, &v, sizeof d);
            spec[k++] = conv; spec[k] = 0; snprintf(piece, sizeof piece, spec, d); break; }
        case 's': {
            char str[512]; game_log_str((uint32_t)v, str, sizeof str);
            spec[k++] = 's'; spec[k] = 0; snprintf(piece, sizeof piece, spec, str); break; }
        default:
            snprintf(piece, sizeof piece, "%%%c", conv); break;
        }
        for (const char* q = piece; *q && o + 1 < sizeof out; ) out[o++] = *q++;
    }
    while (o && (out[o - 1] == '\n' || out[o - 1] == '\r')) o--;
    out[o] = 0;
    printf("[game] %s\n", out);
}

