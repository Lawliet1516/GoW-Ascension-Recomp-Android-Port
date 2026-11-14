/*
 * ps3recomp - write tracer for one guest address range. See spu_wtrace.h.
 */
#include "spu_wtrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

uint32_t g_wtrace_lo, g_wtrace_span;
uint32_t g_wtrace_lo2, g_wtrace_span2;
/* The Wws job-list header line the hdr dump and check read (SPU_WTRACE_HDR=<hex>;
 * default the second range's first line). */
static uint32_t s_hdr_line;

typedef struct {
    uint64_t seq;       /* 0 = empty */
    uint64_t t_us;
    uint64_t ra;
    uint32_t ea, n;
    uint32_t a, b, c, d, e;
    uint32_t who;
    uint32_t zero;      /* data is a zero fill */
    uint8_t  data[128]; /* the first 128 bytes written (a whole lock line) */
} wtrace_ev;

#define WTRACE_N (1u << 20)          /* 1 Mi events, ~190 MB */
static wtrace_ev* s_ev;
static volatile long long s_seq;

static uint64_t wtrace_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / (f.QuadPart / 1000000));
#else
    return 0;
#endif
}

static uint32_t (*s_resolve)(void* ra);

void spu_wtrace_init(uint32_t (*resolve)(void* ra))
{
    s_resolve = resolve;
    const char* e = getenv("SPU_WTRACE");
    if (!e || !*e || e[0] == '0') return;
    char* d = NULL;
    const uint32_t lo = (uint32_t)strtoul(e, &d, 16);
    const uint32_t hi = (d && *d == '-') ? (uint32_t)strtoul(d + 1, NULL, 16) : 0;
    if (hi <= lo) {
        fprintf(stderr, "[wtrace] SPU_WTRACE=%s: expected <lo>-<hi> in hex; off\n", e);
        return;
    }
    s_ev = (wtrace_ev*)calloc(WTRACE_N, sizeof(wtrace_ev));
    if (!s_ev) { fprintf(stderr, "[wtrace] no memory for the ring; off\n"); return; }
    g_wtrace_lo = lo;
    g_wtrace_span = hi - lo;
    fprintf(stderr, "[wtrace] recording every write to 0x%08X-0x%08X (%u events kept)\n", lo, hi, WTRACE_N);
    /* ",<lo2>-<hi2>": the second range (spu_wtrace.h). */
    const char* c = strchr(e, ',');
    if (c) {
        char* d2 = NULL;
        const uint32_t lo2 = (uint32_t)strtoul(c + 1, &d2, 16);
        const uint32_t hi2 = (d2 && *d2 == '-') ? (uint32_t)strtoul(d2 + 1, NULL, 16) : 0;
        if (hi2 > lo2) {
            g_wtrace_lo2 = lo2;
            g_wtrace_span2 = hi2 - lo2;
            fprintf(stderr, "[wtrace] and 0x%08X-0x%08X (PPU stores there only out of line)\n", lo2, hi2);
        }
    }
    { const char* h = getenv("SPU_WTRACE_HDR");
      s_hdr_line = (h ? (uint32_t)strtoul(h, NULL, 16) : g_wtrace_lo2) & ~127u; }
}

void spu_wtrace_rec(char who, uint32_t ea, uint32_t n, const void* data,
                    uint32_t a, uint32_t b, uint32_t c, uint32_t d, void* ra)
{
    spu_wtrace_rec2(who, ea, n, data, a, b, c, d, 0u, ra);
}

void spu_wtrace_rec2(char who, uint32_t ea, uint32_t n, const void* data,
                     uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, void* ra)
{
    if (!s_ev) return;
    const uint64_t seq = (uint64_t)__atomic_add_fetch(&s_seq, 1, __ATOMIC_RELAXED);
    wtrace_ev* ev = &s_ev[seq & (WTRACE_N - 1u)];
    ev->seq = 0;                       /* torn while being written */
    ev->t_us = wtrace_now_us();
    ev->ra = (uint64_t)(uintptr_t)ra;
    ev->ea = ea; ev->n = n;
    ev->a = a; ev->b = b; ev->c = c; ev->d = d; ev->e = e;
    ev->who = (uint32_t)(unsigned char)who;
    ev->zero = data == NULL;
    if (data) memcpy(ev->data, data, n < sizeof ev->data ? n : sizeof ev->data);
    __atomic_store_n(&ev->seq, seq, __ATOMIC_RELEASE);
    /* SPU_WTRACE_TIDS=1: report when a PPU write into the traced ranges comes
     * from a different host thread than the previous one (first 24), with both
     * writers: is a buffer shared by two PPU threads? */
    static int s_tids = -1; static volatile uint32_t s_last_tid, s_last_lr, s_last_ea;
    if (s_tids < 0) s_tids = getenv("SPU_WTRACE_TIDS") ? 24 : 0;
    if (s_tids > 0 && who != 'S' && who != 'A' && who != 'G') {
        const uint32_t pt = s_last_tid;
        if (pt && pt != a && __atomic_sub_fetch(&s_tids, 1, __ATOMIC_RELAXED) >= 0)
            fprintf(stderr, "[wtrace] tid change: %c 0x%08X+0x%X by tid=%u lr=0x%08X r30=%08X; previous write "
                            "0x%08X by tid=%u lr=0x%08X\n", who, ea, n, a, b, e, s_last_ea, pt, s_last_lr);
        s_last_tid = a; s_last_lr = b; s_last_ea = ea;
    }
    /* SPU_WTRACE_BUILDER=<hex>: God of War: Ascension's batch-descriptor
     * builder object (func_0046368C's r30). It keeps three staging buffers
     * that rotate at each descriptor finalize (func_0046368C 0x463A1C..):
     *   A  [+0x30, write ptr +0x24, cap +0x34]  bytecode two descriptors ahead
     *   B  [+0x60, write ptr +0x54, cap +0x64]  bytecode of the next descriptor
     *      (func_00439658 appends through +0x54)
     *   D  [+0x58, cap +0x5C]  the descriptor being finished: B's bytecode, a
     *      header at +0x84, then the DMA list up to +0x50; copied out to the
     *      batch buffer by the finalize, after which D -> A, A -> B, B -> D.
     * Prints (a) the first PPU write of every (function, lr, region) pair --
     * a map of who writes where -- and (b) any moment the three regions
     * overlap; up to 96 lines. A bytecode writer showing up in D's list, or
     * anything but the finalize writing D below its header, is the bug. */
    static int s_bld = -1; static uint32_t s_bobj; static volatile long s_bld_n;
    static uint32_t s_seen[512];
    if (s_bld < 0) { const char* x = getenv("SPU_WTRACE_BUILDER");
                     s_bobj = x ? (uint32_t)strtoul(x, NULL, 16) : 0; s_bld = s_bobj ? 1 : 0; }
    if (s_bld > 0 && who != 'S' && who != 'A' && who != 'G') {
        extern uint8_t* vm_base;
        const uint8_t* o = vm_base + s_bobj;
        #define WT_BE32(off) ((uint32_t)o[off] << 24 | (uint32_t)o[(off) + 1] << 16 | (uint32_t)o[(off) + 2] << 8 | o[(off) + 3])
        const uint32_t act = WT_BE32(0x30), actp = WT_BE32(0x24), actc = WT_BE32(0x34);
        const uint32_t nxt = WT_BE32(0x60), nxtp = WT_BE32(0x54), nxtc = WT_BE32(0x64);
        const uint32_t dsc = WT_BE32(0x58), dsch = WT_BE32(0x84), dscl = WT_BE32(0x50), dscc = WT_BE32(0x5C);
        const uint32_t cry = WT_BE32(0x28), cryl = WT_BE32(0x20), cryc = WT_BE32(0x2C);
        #undef WT_BE32
        int cls = 0;   /* 0 other, 1 A, 2 B, 3 D bytecode, 4 D header, 5 D list */
        if (act && ea >= act && ea < act + actc) cls = 1;
        else if (nxt && ea >= nxt && ea < nxt + nxtc) cls = 2;
        else if (dsc && ea >= dsc && ea < dsc + dscc)
            cls = ea < dsc + dsch ? 3 : ea < dsc + dsch + 16u ? 4 : 5;
        else if (cry && ea >= cry && ea < cry + cryc) cls = 6;
        /* A buffer moved or resized outside the finalize's rotation (a grow:
         * the start or capacity fields +0x30/+0x34/+0x58/+0x5C/+0x60/+0x64). */
        const uint32_t fo = ea - s_bobj;
        if (fo == 0x30u || fo == 0x34u || fo == 0x58u || fo == 0x5Cu || fo == 0x60u || fo == 0x64u ||
            fo == 0x28u || fo == 0x2Cu) {
            static volatile long s_gn;
            const uint32_t fn0 = s_resolve ? s_resolve(ra) : 0u;
            /* grows (A 0x00461510, B 0x00461368, D 0x004611D4, carry 0x004616B8) up to 48; anything
             * else but the rotation, the constructor and the boot memset, the first 16 */
            static volatile long s_on;
            const int grow = fn0 == 0x00461510u || fn0 == 0x00461368u || fn0 == 0x004611D4u || fn0 == 0x004616B8u;
            if (fn0 != 0x0046368Cu && fn0 != 0x004F64F8u && fn0 != 0x00460DA4u &&
                (grow ? __atomic_add_fetch(&s_gn, 1, __ATOMIC_RELAXED) <= 48
                      : __atomic_add_fetch(&s_on, 1, __ATOMIC_RELAXED) <= 16))
                fprintf(stderr, "[wtrace] builder field +0x%02X <- %02X%02X%02X%02X by fn=0x%08X lr=0x%08X; "
                                "A %08X cap %X, B %08X cap %X, D %08X cap %X, carry %08X cap %X\n",
                        ea - s_bobj, data ? ((const uint8_t*)data)[0] : 0u, data ? ((const uint8_t*)data)[1] : 0u,
                        data ? ((const uint8_t*)data)[2] : 0u, data ? ((const uint8_t*)data)[3] : 0u,
                        fn0, b, act, actc, nxt, nxtc, dsc, dscc, cry, cryc);
        }
        if (!cls || !act || !nxt || !dsc || s_bld_n >= 96) goto bld_done;   /* the map: the three buffers only */
        static const char* const cn[7] = { "other", "A(bytecode+2)", "B(bytecode+1)", "D-bytecode", "D-header", "D-list",
                                           "carry-list" };
        const uint32_t fn = s_resolve ? s_resolve(ra) : 0u;
        const uint32_t key = (fn * 2654435761u) ^ (b * 40503u) ^ (uint32_t)cls ^ ((uint32_t)(unsigned char)who << 24);
        uint32_t slot = key & 511u; int fresh = 1;
        for (int k = 0; k < 8; k++, slot = (slot + 1u) & 511u) {
            if (s_seen[slot] == key) { fresh = 0; break; }
            if (!s_seen[slot]) { s_seen[slot] = key; break; }
        }
        #define WT_OV(p, pc, q, qc) ((p) && (q) && (p) < (q) + (qc) && (q) < (p) + (pc))
        /* (the rotation, the set-up and the teardown move the fields one at a
         * time: their transient overlaps are not the bug) */
        const int ov = fn != 0x0046368Cu && fn != 0x00461C5Cu && fn != 0x00462FC4u &&
                       (WT_OV(act, actc, nxt, nxtc) || WT_OV(act, actc, dsc, dscc) || WT_OV(nxt, nxtc, dsc, dscc) ||
                        WT_OV(cry, cryc, act, actc) || WT_OV(cry, cryc, nxt, nxtc) || WT_OV(cry, cryc, dsc, dscc));
        #undef WT_OV
        if ((fresh || ov) && __atomic_add_fetch(&s_bld_n, 1, __ATOMIC_RELAXED) <= 96) {
            char bytes[40]; int p = 0;
            for (uint32_t i = 0; i < n && i < 16u; i++)
                p += snprintf(bytes + p, sizeof bytes - (size_t)p, "%02X", data ? ((const uint8_t*)data)[i] : 0u);
            fprintf(stderr, "[wtrace] builder%s: %c 0x%08X+0x%X in %s by fn=0x%08X lr=0x%08X tid=%u r29=%08X r30=%08X "
                            "[%s]; A %08X ptr %08X cap %X, B %08X ptr %08X cap %X, D %08X hdr +%X list end +%X cap %X, "
                            "carry %08X used %X cap %X\n",
                    ov ? " OVERLAP" : "", who, ea, n, cn[cls], fn, b, a, d, e, bytes, act, actp, actc, nxt, nxtp, nxtc,
                    dsc, dsch, dscl, dscc, cry, cryl, cryc);
        }
    bld_done:;
    }
    /* SPU_WTRACE_SHOWLR=<hex guest lr>: print the first 8 PPU writes recorded
     * with that lr as they happen, with r29/r30 (who and from where). */
    static int s_show = -1; static uint32_t s_lr;
    if (s_show < 0) { const char* x = getenv("SPU_WTRACE_SHOWLR");
                      s_lr = x ? (uint32_t)strtoul(x, NULL, 16) : 0; s_show = s_lr ? 8 : 0; }
    if (s_show > 0 && b == s_lr && who != 'S' && who != 'A' && who != 'G' &&
        __atomic_sub_fetch(&s_show, 1, __ATOMIC_RELAXED) >= 0)
    {
        fprintf(stderr, "[wtrace] lr 0x%08X: %c 0x%08X+0x%X r29=%08X r30=%08X sp=%08X\n",
                b, who, ea, n, d, e, c);
        extern uint8_t* vm_base;
        if (e >= 0x10000u && e < 0x3FFFFF00u)   /* r30 as a guest object: its first 0xA0 bytes */
            for (uint32_t o = 0; o < 0xA0u; o += 16u) {
                const uint8_t* q = vm_base + e + o;
                fprintf(stderr, "[wtrace] lr 0x%08X:   r30+0x%02X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                        b, o, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11],
                        q[12], q[13], q[14], q[15]);
            }
    }
}

void spu_wtrace_dump(uint32_t ea, uint32_t n, unsigned max, const char* tag)
{
    if (!s_ev) {
        fprintf(stderr, "[wtrace] %s: tracer off (SPU_WTRACE unset)\n", tag);
        return;
    }
    if (!spu_wtrace_hit(ea, n)) {
        fprintf(stderr, "[wtrace] %s: 0x%08X+0x%X is outside the traced range 0x%08X-0x%08X\n",
                tag, ea, n, g_wtrace_lo, g_wtrace_lo + g_wtrace_span);
        return;
    }
    const uint64_t now = wtrace_now_us();
    const uint64_t top = (uint64_t)__atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
    const uint64_t oldest = top > WTRACE_N ? top - WTRACE_N + 1u : 1u;
    unsigned shown = 0;
    uint64_t first_t = 0;
    for (uint64_t s = top; s >= oldest && shown < max; s--) {
        const wtrace_ev* ev = &s_ev[s & (WTRACE_N - 1u)];
        if (__atomic_load_n(&ev->seq, __ATOMIC_ACQUIRE) != s) continue;
        if (!(ev->ea < ea + n && ea < ev->ea + ev->n)) continue;
        if (!shown) first_t = ev->t_us;
        char who = (char)ev->who;
        char src[128];
        if (who == 'S' || who == 'A' || who == 'G')
            snprintf(src, sizeof src, "SPU img=%u ovl=%u pc=0x%05X lr=0x%05X", ev->a, ev->b, ev->c, ev->d);
        else
            snprintf(src, sizeof src, "PPU fn=0x%08X lr=0x%08X sp=0x%08X r29=%08X r30=%08X tid=%u",
                     s_resolve ? s_resolve((void*)(uintptr_t)ev->ra) : 0u, ev->b, ev->c, ev->d, ev->e, ev->a);
        /* The bytes this write left at [ea, ea + n), where it covered them.
         * A span wider than a quadword (a whole job descriptor) shows each
         * event's own first 16 bytes instead. */
        char bytes[64]; int p = 0;
        if (n > 16u)
            for (uint32_t i = 0; i < ev->n && i < 16u && p < (int)sizeof bytes - 3; i++) {
                if (ev->zero) p += snprintf(bytes + p, sizeof bytes - p, "00");
                else p += snprintf(bytes + p, sizeof bytes - p, "%02X", ev->data[i]);
                if ((i & 3u) == 3u && p < (int)sizeof bytes - 2) bytes[p++] = ' ', bytes[p] = 0;
            }
        else
        for (uint32_t i = 0; i < n && i < 16u && p < (int)sizeof bytes - 3; i++) {
            const uint32_t off = ea + i - ev->ea;
            if (ea + i < ev->ea || off >= ev->n) p += snprintf(bytes + p, sizeof bytes - p, "..");
            else if (ev->zero) p += snprintf(bytes + p, sizeof bytes - p, "00");
            else if (off < sizeof ev->data) p += snprintf(bytes + p, sizeof bytes - p, "%02X", ev->data[off]);
            else p += snprintf(bytes + p, sizeof bytes - p, "??");
            if ((i & 3u) == 3u && p < (int)sizeof bytes - 2) bytes[p++] = ' ', bytes[p] = 0;
        }
        fprintf(stderr, "[wtrace] %s: #%llu %c %llu us ago: 0x%08X+0x%X by %s -> [%s]\n",
                tag, (unsigned long long)s, who, (unsigned long long)(now - ev->t_us),
                ev->ea, ev->n, src, bytes);
        shown++;
    }
    if (!shown)
        fprintf(stderr, "[wtrace] %s: no write to 0x%08X+0x%X among the last %llu recorded\n",
                tag, ea, n, (unsigned long long)(top - oldest + 1u));
    else if (first_t)
        fprintf(stderr, "[wtrace] %s: %u writes shown (ring holds %llu events, oldest %llu us ago)\n",
                tag, shown, (unsigned long long)(top - oldest + 1u),
                (unsigned long long)(now - s_ev[oldest & (WTRACE_N - 1u)].t_us));
    fflush(stderr);
}

static uint32_t wt_be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }

void spu_wtrace_dump_hdr(unsigned max, const char* tag)
{
    if (!s_ev || !s_hdr_line) return;
    const uint32_t line = s_hdr_line;
    const uint64_t now = wtrace_now_us();
    const uint64_t top = (uint64_t)__atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
    const uint64_t oldest = top > WTRACE_N ? top - WTRACE_N + 1u : 1u;
    unsigned shown = 0;
    for (uint64_t s = top; s >= oldest && shown < max; s--) {
        const wtrace_ev* ev = &s_ev[s & (WTRACE_N - 1u)];
        if (__atomic_load_n(&ev->seq, __ATOMIC_ACQUIRE) != s) continue;
        if (!(ev->ea < line + 128u && line < ev->ea + ev->n) || ev->who == 'G') continue;
        char src[96];
        if (ev->who == 'S' || ev->who == 'A')
            snprintf(src, sizeof src, "SPU img=%u ovl=%u pc=0x%05X lr=0x%05X", ev->a, ev->b, ev->c, ev->d);
        else
            snprintf(src, sizeof src, "PPU fn=0x%08X lr=0x%08X tid=%u",
                     s_resolve ? s_resolve((void*)(uintptr_t)ev->ra) : 0u, ev->b, ev->a);
        char body[200]; int p = 0;
        if (ev->ea == line && ev->n >= 128u && !ev->zero) {
            /* a whole-line commit: the four segment headers */
            for (int g = 0; g < 4; g++) {
                const uint8_t* h = ev->data + 0x40 + 16 * g;
                p += snprintf(body + p, sizeof body - (size_t)p, "%s%u/%u:", g ? " | " : "", wt_be16(h), wt_be16(h + 2));
                for (int k = 0; k < 6; k++) {
                    const uint32_t v = wt_be16(h + 4 + 2 * k);
                    p += v == 0xFFFFu ? snprintf(body + p, sizeof body - (size_t)p, " -")
                                      : snprintf(body + p, sizeof body - (size_t)p, " %u", v);
                }
            }
        } else {
            p += snprintf(body + p, sizeof body - (size_t)p, "+0x%02X:", ev->ea - line);
            for (uint32_t i = 0; i < ev->n && i < 16u; i++)
                p += snprintf(body + p, sizeof body - (size_t)p, "%02X", ev->zero ? 0u : ev->data[i]);
        }
        fprintf(stderr, "[wtrace] %s hdr: #%llu %c %llu us ago by %s: %s\n", tag, (unsigned long long)s,
                (char)ev->who, (unsigned long long)(now - ev->t_us), src, body);
        shown++;
    }
    fflush(stderr);
}

void spu_wtrace_hdr_commit(uint32_t ea, const uint8_t* o, const uint8_t* nw,
                           uint32_t spu_num, uint32_t img, uint32_t pc, uint32_t lr)
{
    if (!s_hdr_line || (ea & ~127u) != s_hdr_line) return;
    for (int g = 0; g < 4; g++) {
        const uint8_t* a = o + 0x40 + 16 * g;
        const uint8_t* b = nw + 0x40 + 16 * g;
        for (uint32_t k = 0; k < 6; k++) {
            if (k == spu_num || wt_be16(a + 4 + 2 * k) == wt_be16(b + 4 + 2 * k)) continue;
            static volatile long s_n;
            const long n = __atomic_add_fetch(&s_n, 1, __ATOMIC_RELAXED);
            if (n <= 32 || !(n & (n - 1)))
                fprintf(stderr, "[wtrace] hdr #%ld: SPU spuNum=%u (img=%u pc=0x%05X lr=0x%05X) committed seg %d "
                                "minActive[%u] %u -> %u (avail/taken %u/%u -> %u/%u)\n",
                        n, spu_num, img, pc, lr, g, k, wt_be16(a + 4 + 2 * k), wt_be16(b + 4 + 2 * k),
                        wt_be16(a), wt_be16(a + 2), wt_be16(b), wt_be16(b + 2));
        }
    }
}

uint32_t spu_wtrace_last_ppu_e(uint32_t ea, uint32_t n)
{
    if (!s_ev) return 0;
    const uint64_t top = (uint64_t)__atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
    const uint64_t oldest = top > WTRACE_N ? top - WTRACE_N + 1u : 1u;
    for (uint64_t s = top; s >= oldest; s--) {
        const wtrace_ev* ev = &s_ev[s & (WTRACE_N - 1u)];
        if (__atomic_load_n(&ev->seq, __ATOMIC_ACQUIRE) != s) continue;
        if (!(ev->ea < ea + n && ea < ev->ea + ev->n)) continue;
        if (ev->who == 'P' || ev->who == 'V' || ev->who == 'C' || ev->who == 'Z') return ev->e;
    }
    return 0;
}

uint32_t spu_wtrace_last_ppu_d(uint32_t ea, uint32_t n)
{
    if (!s_ev) return 0;
    const uint64_t top = (uint64_t)__atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
    const uint64_t oldest = top > WTRACE_N ? top - WTRACE_N + 1u : 1u;
    for (uint64_t s = top; s >= oldest; s--) {
        const wtrace_ev* ev = &s_ev[s & (WTRACE_N - 1u)];
        if (__atomic_load_n(&ev->seq, __ATOMIC_ACQUIRE) != s) continue;
        if (!(ev->ea < ea + n && ea < ev->ea + ev->n)) continue;
        if (ev->who == 'P' || ev->who == 'V' || ev->who == 'C' || ev->who == 'Z') return ev->d;
    }
    return 0;
}
