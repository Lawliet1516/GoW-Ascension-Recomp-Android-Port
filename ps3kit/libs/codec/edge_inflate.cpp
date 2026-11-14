/* Taken from the Twisted Metal port (sp00nznet/twistedmetal, src/tm_inflate.cpp,
 * MIT, Copyright (c) 2026 Ned Heller / sp00nznet), with its functions renamed
 * edge_inflate / edge_inflate_selftest so a port that links its own copy does
 * not collide. Used by _cellSpursLFQueuePushBody's Edge Zlib path
 * (libs/spurs/cellSpurs.c). */
/*
 * Minimal DEFLATE / zlib decoder, for HLE-ing the title's SPU decompressor.
 *
 * Twisted Metal inflates its packed assets with Sony's Edge Zlib running as a
 * SPURS task on the SPU. That task never receives work here, so nothing loads.
 * The requests themselves are plain zlib streams (verified: a captured 63-byte
 * request inflates to exactly the 132 bytes its descriptor asks for), so the
 * decompression can simply be done on the host instead — which is what
 * ps3recomp's own porting guide recommends for SPU decompression work.
 *
 * ps3recomp links no zlib and Windows exposes no system one, so this is a
 * self-contained inflater. It follows RFC 1951 directly and is written for
 * clarity over speed: assets are inflated once at load time. (edge_inflate_ref
 * now; EDGE_INFLATE_FAST below is the table-driven one that runs by default.)
 *
 * edge_inflate_selftest() checks it against a real request captured from the
 * running title.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct Bits {
    const uint8_t* src;
    uint32_t len, pos;
    uint32_t bitbuf, bitcnt;
    bool bad;

    int bit()
    {
        if (!bitcnt) {
            if (pos >= len) { bad = true; return 0; }
            bitbuf = src[pos++];
            bitcnt = 8;
        }
        const int b = bitbuf & 1;
        bitbuf >>= 1;
        bitcnt--;
        return b;
    }
    uint32_t bits(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v |= (uint32_t)bit() << i;
        return v;
    }
};

/* Canonical Huffman decoding table: counts per length, plus symbols in order. */
struct Huff {
    int16_t count[16];
    int16_t sym[288];
};

void huff_build(Huff* h, const uint8_t* lengths, int n)
{
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (int16_t)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->sym[offs[lengths[i]]++] = (int16_t)i;
}

int huff_decode(Bits& b, const Huff* h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= b.bit();
        const int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
        if (b.bad) break;
    }
    return -1;
}

const uint16_t LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
const uint8_t LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
const uint16_t DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
    4097,6145,8193,12289,16385,24577 };
const uint8_t DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

/* One compressed block into `out`; returns bytes written, -1 on a corrupt
 * stream, or FULL once `out` holds `cap` bytes and the block wants more. */
const int FULL = -2;

int inflate_block(Bits& b, uint8_t* out, uint32_t cap, uint32_t written,
                  const Huff* lit, const Huff* dist)
{
    for (;;) {
        const int sym = huff_decode(b, lit);
        if (sym < 0 || b.bad) return -1;
        if (sym < 256) {
            if (written >= cap) return FULL;
            out[written++] = (uint8_t)sym;
        } else if (sym == 256) {
            return (int)written;
        } else {
            const int s = sym - 257;
            if (s >= 29) return -1;
            const uint32_t length = LEN_BASE[s] + b.bits(LEN_EXTRA[s]);
            const int ds = huff_decode(b, dist);
            if (ds < 0 || ds >= 30) return -1;
            const uint32_t d = DIST_BASE[ds] + b.bits(DIST_EXTRA[ds]);
            if (d > written) return -1;
            for (uint32_t i = 0; i < length; i++, written++) {
                if (written >= cap) return FULL;
                out[written] = out[written - d];
            }
        }
    }
}

}  // namespace

/* Inflate a raw DEFLATE or zlib stream. Returns bytes written, or -1.
 * A leading 0x78 (CMF with CM=8) is treated as a zlib wrapper and skipped.
 *
 * A stream that decodes to more than `out_cap` bytes is not an error: the
 * first `out_cap` bytes are produced and `out_cap` returned, as the SPU
 * decompressor does. God of War: Ascension reads 64 bytes of a compressed
 * asset to look at its header, and an error there was retried forever. */
/* The reference (bit at a time); edge_inflate, the dispatcher, is below. */
extern "C" int edge_inflate_ref(uint8_t* out, uint32_t out_cap, const uint8_t* src, uint32_t src_len)
{
    if (!out || !src || src_len < 1) return -1;

    uint32_t start = 0;
    if (src_len >= 2 && (src[0] & 0x0F) == 8 && ((src[0] << 8) | src[1]) % 31 == 0)
        start = 2;   /* zlib header (RFC 1950) */

    Bits b{src, src_len, start, 0, 0, false};
    uint32_t written = 0;

    for (;;) {
        const int final = b.bit();
        const int type = (int)b.bits(2);
        if (b.bad) return -1;

        if (type == 0) {                        /* stored */
            b.bitcnt = 0;
            if (b.pos + 4 > b.len) return -1;
            const uint32_t n = (uint32_t)src[b.pos] | ((uint32_t)src[b.pos + 1] << 8);
            b.pos += 4;
            if (b.pos + n > b.len) return -1;
            if (written + n > out_cap) {
                memcpy(out + written, src + b.pos, out_cap - written);
                return (int)out_cap;
            }
            memcpy(out + written, src + b.pos, n);
            b.pos += n;
            written += n;
        } else if (type == 1 || type == 2) {
            Huff lit, dist;
            if (type == 1) {                    /* fixed Huffman */
                uint8_t l[288], d[30];
                for (int i = 0; i < 144; i++) l[i] = 8;
                for (int i = 144; i < 256; i++) l[i] = 9;
                for (int i = 256; i < 280; i++) l[i] = 7;
                for (int i = 280; i < 288; i++) l[i] = 8;
                for (int i = 0; i < 30; i++) d[i] = 5;
                huff_build(&lit, l, 288);
                huff_build(&dist, d, 30);
            } else {                            /* dynamic Huffman */
                static const uint8_t ORDER[19] = {
                    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                const int hlit  = (int)b.bits(5) + 257;
                const int hdist = (int)b.bits(5) + 1;
                const int hclen = (int)b.bits(4) + 4;
                if (hlit > 286 || hdist > 30) return -1;

                uint8_t cl[19] = {0};
                for (int i = 0; i < hclen; i++) cl[ORDER[i]] = (uint8_t)b.bits(3);
                Huff clh;
                huff_build(&clh, cl, 19);

                uint8_t lengths[288 + 30] = {0};
                int i = 0;
                while (i < hlit + hdist) {
                    const int sym = huff_decode(b, &clh);
                    if (sym < 0 || b.bad) return -1;
                    if (sym < 16) {
                        lengths[i++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (!i) return -1;
                        const uint8_t prev = lengths[i - 1];
                        for (int r = 3 + (int)b.bits(2); r && i < hlit + hdist; r--)
                            lengths[i++] = prev;
                    } else if (sym == 17) {
                        for (int r = 3 + (int)b.bits(3); r && i < hlit + hdist; r--)
                            lengths[i++] = 0;
                    } else {
                        for (int r = 11 + (int)b.bits(7); r && i < hlit + hdist; r--)
                            lengths[i++] = 0;
                    }
                }
                huff_build(&lit, lengths, hlit);
                huff_build(&dist, lengths + hlit, hdist);
            }
            const int n = inflate_block(b, out, out_cap, written, &lit, &dist);
            if (n == FULL) return (int)out_cap;
            if (n < 0) return -1;
            written = (uint32_t)n;
        } else {
            return -1;                          /* type 3 is reserved */
        }
        if (final) break;
        if (b.bad) return -1;
    }
    return (int)written;
}

/* ---------------------------------------------------------------------------
 * EDGE_INFLATE_FAST (default on; =0: the reference above for every request):
 * a table-driven inflater with a 64-bit bit buffer, ~10x the reference's speed.
 * God of War: Ascension streams every area through these requests on the PPU
 * thread that pushes them; at the reference's speed a 4-6 core PC loaded new
 * areas late (TEXTURE-LOADING-NOTES.md).
 *
 * Exactness: it follows the reference's control flow step for step (same
 * order of checks, same FULL / end-of-stream returns) and gives up -- REDO,
 * the whole request is then done by the reference -- on anything unusual: a
 * corrupt stream, an over-subscribed or incomplete code reaching an unused
 * pattern, or a read past the end of the input (where the reference's bit
 * reader goes on with zeros). So every result it returns itself is one the
 * reference returns, byte for byte. EDGE_INFLATE_VERIFY=1 runs the reference
 * too on every request and counts any difference ([inflate-verify]).
 * ------------------------------------------------------------------------- */
namespace {

const int REDO = -3;

enum { K_SYM = 0, K_LINK = 1, K_BAD = 2 };
inline uint32_t ent_make(uint32_t val, uint32_t kind, uint32_t nbits) { return (val << 16) | (kind << 8) | nbits; }
const uint32_t ENT_BAD = (uint32_t)K_BAD << 8;

/* Primary table of 2^pb entries indexed by the next pb bits (LSB first);
 * codes longer than pb go through a link to a subtable of 2^sb entries. */
struct FTable {
    int pb, sb;
    uint32_t e[(1u << 10) + 32768u];
};

inline uint32_t rev_bits(uint32_t c, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (c & 1u); c >>= 1; }
    return r;
}

/* false: over-subscribed (not a prefix code) -- the caller gives up (REDO). */
bool ftable_build(FTable* t, const uint8_t* lengths, int n, int pb)
{
    int count[16] = {0};
    int maxlen = 0;
    for (int i = 0; i < n; i++) { count[lengths[i]]++; if (lengths[i] > maxlen) maxlen = lengths[i]; }
    count[0] = 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) { left <<= 1; left -= count[len]; if (left < 0) return false; }
    uint32_t next[16];
    uint32_t code = 0;
    next[0] = 0;
    for (int len = 1; len <= 15; len++) { code = (code + (uint32_t)count[len - 1]) << 1; next[len] = code; }
    t->pb = pb;
    t->sb = maxlen > pb ? maxlen - pb : 0;
    const uint32_t psize = 1u << pb, ssize = 1u << t->sb;
    for (uint32_t i = 0; i < psize; i++) t->e[i] = ENT_BAD;
    uint32_t nsub = psize;
    for (int sym = 0; sym < n; sym++) {
        const int L = lengths[sym];
        if (!L) continue;
        const uint32_t r = rev_bits(next[L]++, L);
        if (L <= pb) {
            for (uint32_t k = r; k < psize; k += 1u << L) t->e[k] = ent_make((uint32_t)sym, K_SYM, (uint32_t)L);
        } else {
            const uint32_t pre = r & (psize - 1u);
            uint32_t base;
            if (((t->e[pre] >> 8) & 0xFFu) != K_LINK) {
                base = nsub;
                nsub += ssize;
                for (uint32_t k = 0; k < ssize; k++) t->e[base + k] = ENT_BAD;
                t->e[pre] = ent_make(base, K_LINK, (uint32_t)pb);
            } else {
                base = t->e[pre] >> 16;
            }
            const int Ls = L - pb;
            for (uint32_t k = r >> pb; k < ssize; k += 1u << Ls) t->e[base + k] = ent_make((uint32_t)sym, K_SYM, (uint32_t)Ls);
        }
    }
    return true;
}

struct FBits {
    const uint8_t* src;
    uint32_t len;     /* input bytes */
    uint32_t pos;     /* bytes loaded into the buffer, past the end counted too (zeros) */
    uint64_t buf;
    uint32_t cnt;     /* valid bits in buf */

    inline void refill()
    {
        if (pos + 8u <= len) {
            uint64_t v;
            memcpy(&v, src + pos, 8);   /* little-endian host */
            buf |= v << cnt;
            const uint32_t nb = (63u - cnt) >> 3;
            pos += nb;
            cnt += nb * 8u;
        } else {
            while (cnt <= 56u) {
                buf |= (uint64_t)(pos < len ? src[pos] : 0u) << cnt;
                pos++;
                cnt += 8u;
            }
        }
    }
    inline uint32_t take(uint32_t n)   /* n <= cnt */
    {
        const uint32_t v = (uint32_t)(buf & ((1ull << n) - 1u));
        buf >>= n;
        cnt -= n;
        return v;
    }
    /* Bits consumed so far beyond the input: the reference's reader would have
     * gone "bad" there. */
    inline bool overrun() const { return (uint64_t)pos * 8u - cnt > (uint64_t)len * 8u; }
    inline uint64_t consumed() const { return (uint64_t)pos * 8u - cnt; }
};

/* Next symbol, or -1 (an unused pattern of an incomplete code: REDO). Needs >= 15 bits in the buffer. */
inline int fdecode(FBits& b, const FTable* t)
{
    uint32_t e = t->e[b.buf & ((1u << t->pb) - 1u)];
    if (((e >> 8) & 0xFFu) == K_LINK) {
        b.buf >>= t->pb;
        b.cnt -= (uint32_t)t->pb;
        e = t->e[(e >> 16) + (uint32_t)(b.buf & ((1u << t->sb) - 1u))];
    }
    if (((e >> 8) & 0xFFu) != K_SYM) return -1;
    const uint32_t nb = e & 0xFFu;
    b.buf >>= nb;
    b.cnt -= nb;
    return (int)(e >> 16);
}

struct FTables { FTable lit, dist, cl; };

const FTables* fixed_tables()
{
    static FTables* ft = []() {
        FTables* t = (FTables*)malloc(sizeof(FTables));
        if (!t) return t;
        uint8_t l[288], d[30];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        for (int i = 0; i < 30; i++) d[i] = 5;
        ftable_build(&t->lit, l, 288, 10);
        ftable_build(&t->dist, d, 30, 8);
        return t;
    }();
    return ft;
}

/* The reference's inflate_block, table-driven. Returns bytes written, FULL,
 * or REDO. */
int finflate_block(FBits& b, uint8_t* out, uint32_t cap, uint32_t written, const FTable* lit, const FTable* dist)
{
    const uint32_t pmask = (1u << lit->pb) - 1u;
    for (;;) {
        if (b.cnt < 48u) b.refill();
        int sym = fdecode(b, lit);
        if (sym < 0) return REDO;
        if (sym < 256) {
            if (written >= cap) return FULL;
            out[written++] = (uint8_t)sym;
            /* More literals from the same buffer while their codes are in the
             * primary table and the bits are there (the next symbol of any
             * other kind goes round the loop as before). */
            while (b.cnt >= 15u) {
                const uint32_t e = lit->e[b.buf & pmask];
                if ((e & 0xFF00u) != 0u || (e >> 16) >= 256u) break;   /* not a primary-table literal */
                const uint32_t nb = e & 0xFFu;
                b.buf >>= nb;   /* consumed first, as the reference does: past the input end is then REDO */
                b.cnt -= nb;
                if (written >= cap) return FULL;
                out[written++] = (uint8_t)(e >> 16);
            }
        } else if (sym == 256) {
            return (int)written;
        } else {
            const int s = sym - 257;
            if (s >= 29) return REDO;
            const uint32_t length = LEN_BASE[s] + b.take(LEN_EXTRA[s]);
            const int ds = fdecode(b, dist);
            if (ds < 0 || ds >= 30) return REDO;
            const uint32_t d = DIST_BASE[ds] + b.take(DIST_EXTRA[ds]);
            if (d > written) return REDO;
            uint32_t n = length;
            const bool full = written + n > cap;
            if (full) n = cap - written;
            uint8_t* o = out + written;
            const uint8_t* q = o - d;
            if (d >= 8u && written + n + 8u <= cap) {
                /* 8 bytes at a time: the source is at least 8 behind, so every
                 * chunk read is already final; the last chunk may write up to 7
                 * bytes past the match, inside the buffer, which later output
                 * overwrites (nothing past the returned length is used). */
                uint8_t* const end = o + n;
                do { memcpy(o, q, 8); o += 8; q += 8; } while (o < end);
            }
            else if (d >= n) memcpy(o, q, n);
            else if (d == 1) memset(o, q[0], n);
            else for (uint32_t i = 0; i < n; i++) o[i] = q[i];
            written += n;
            if (full) return FULL;
        }
    }
}

const uint8_t CL_ORDER[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

FTables* thread_tables()
{
    static thread_local FTables* t = nullptr;
    if (!t) t = (FTables*)malloc(sizeof(FTables));
    return t;
}

}  // namespace

/* The fast path: a result the reference gives too, or REDO. */
extern "C" int edge_inflate_fast(uint8_t* out, uint32_t out_cap, const uint8_t* src, uint32_t src_len)
{
    if (!out || !src || src_len < 1) return -1;
    FTables* tt = thread_tables();
    const FTables* fx = fixed_tables();
    if (!tt || !fx) return REDO;

    uint32_t start = 0;
    if (src_len >= 2 && (src[0] & 0x0F) == 8 && ((src[0] << 8) | src[1]) % 31 == 0)
        start = 2;
    FBits b{src, src_len, start, 0, 0};
    uint32_t written = 0;

    for (;;) {
        if (b.cnt < 16u) b.refill();
        const uint32_t final = b.take(1);
        const uint32_t type = b.take(2);
        if (b.overrun()) return REDO;

        if (type == 0) {                        /* stored: the reference drops the partial byte */
            uint32_t p = (uint32_t)((b.consumed() + 7u) >> 3);
            if (p + 4 > src_len) return REDO;
            const uint32_t n = (uint32_t)src[p] | ((uint32_t)src[p + 1] << 8);
            p += 4;
            if (p + n > src_len) return REDO;
            if (written + n > out_cap) {
                memcpy(out + written, src + p, out_cap - written);
                return (int)out_cap;
            }
            memcpy(out + written, src + p, n);
            p += n;
            written += n;
            b.pos = p; b.buf = 0; b.cnt = 0;
        } else if (type == 1 || type == 2) {
            const FTable* lit;
            const FTable* dist;
            if (type == 1) {
                lit = &fx->lit; dist = &fx->dist;
            } else {
                b.refill();
                const int hlit  = (int)b.take(5) + 257;
                const int hdist = (int)b.take(5) + 1;
                const int hclen = (int)b.take(4) + 4;
                if (hlit > 286 || hdist > 30) return REDO;
                uint8_t cl[19] = {0};
                for (int i = 0; i < hclen; i++) {
                    if (b.cnt < 3u) b.refill();
                    cl[CL_ORDER[i]] = (uint8_t)b.take(3);
                }
                if (!ftable_build(&tt->cl, cl, 19, 7)) return REDO;
                uint8_t lengths[288 + 30] = {0};
                int i = 0;
                while (i < hlit + hdist) {
                    if (b.cnt < 16u) b.refill();
                    const int sym = fdecode(b, &tt->cl);
                    if (sym < 0) return REDO;
                    if (sym < 16) {
                        lengths[i++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (!i) return REDO;
                        const uint8_t prev = lengths[i - 1];
                        for (int r = 3 + (int)b.take(2); r && i < hlit + hdist; r--) lengths[i++] = prev;
                    } else if (sym == 17) {
                        for (int r = 3 + (int)b.take(3); r && i < hlit + hdist; r--) lengths[i++] = 0;
                    } else {
                        for (int r = 11 + (int)b.take(7); r && i < hlit + hdist; r--) lengths[i++] = 0;
                    }
                }
                if (b.overrun()) return REDO;
                if (!ftable_build(&tt->lit, lengths, hlit, 10)) return REDO;
                if (!ftable_build(&tt->dist, lengths + hlit, hdist, 8)) return REDO;
                lit = &tt->lit; dist = &tt->dist;
            }
            const int n = finflate_block(b, out, out_cap, written, lit, dist);
            if (n == REDO || b.overrun()) return REDO;
            if (n == FULL) return (int)out_cap;
            written = (uint32_t)n;
        } else {
            return REDO;                        /* type 3: the reference says -1 */
        }
        if (final) break;
    }
    return (int)written;
}

static int edge_env_flag(const char* name, int dflt)
{
    const char* e = getenv(name);
    return e && *e ? (e[0] != '0') : dflt;
}

extern "C" { volatile long long g_infl_redo, g_infl_verify_n, g_infl_verify_bad; }   /* EDGE_INFLATE_FAST / _VERIFY counters */

extern "C" int edge_inflate(uint8_t* out, uint32_t out_cap, const uint8_t* src, uint32_t src_len)
{
    static int fast = -1, verify = -1;
    if (fast < 0) fast = edge_env_flag("EDGE_INFLATE_FAST", 1);
    if (verify < 0) verify = edge_env_flag("EDGE_INFLATE_VERIFY", 0);
    if (!fast) return edge_inflate_ref(out, out_cap, src, src_len);
    int r = edge_inflate_fast(out, out_cap, src, src_len);
    if (r == REDO) {
        __atomic_fetch_add(&g_infl_redo, 1, __ATOMIC_RELAXED);
        r = edge_inflate_ref(out, out_cap, src, src_len);
    }
    if (verify && out) {
        uint8_t* chk = (uint8_t*)malloc(out_cap ? out_cap : 1);
        if (chk) {
            const int rr = edge_inflate_ref(chk, out_cap, src, src_len);
            const long long nv = __atomic_add_fetch(&g_infl_verify_n, 1, __ATOMIC_RELAXED);
            if (rr != r || (r > 0 && memcmp(chk, out, (size_t)r) != 0)) {
                const long long nb = __atomic_add_fetch(&g_infl_verify_bad, 1, __ATOMIC_RELAXED);
                if (nb <= 16)
                    fprintf(stderr, "[inflate-verify] MISMATCH #%lld: fast %d, reference %d (cap %u, in %u bytes)\n",
                            nb, r, rr, out_cap, src_len);
            }
            if ((nv & 4095) == 0)
                fprintf(stderr, "[inflate-verify] %lld requests checked against the reference, %lld differ, %lld redone\n",
                        nv, (long long)g_infl_verify_bad, (long long)g_infl_redo);
            free(chk);
        }
    }
    return r;
}

/* Checked against a real decompression request captured from the running
 * title: 63 bytes of zlib in, 132 bytes of asset manifest out, beginning
 * "ui\0". Returns 0 on success. */
extern "C" int edge_inflate_selftest(void)
{
    static const uint8_t SRC[63] = {
        0x78,0xDA,0x2B,0xCD,0x64,0xC0,0x00,0x79,0xE9,0x05,0x70,0xF6,0x8E,0x83,0xDD,0xCC,
        0xC7,0x4A,0xBA,0xA7,0x9D,0xDF,0x79,0x86,0xB1,0x14,0x8B,0xDA,0x82,0x92,0x22,0x38,
        0x5B,0xA2,0x8F,0x81,0xA1,0x4C,0xFD,0xCC,0x6C,0x5C,0x6A,0xCB,0x8A,0x12,0x73,0x61,
        0xEC,0x86,0xDA,0x27,0xEC,0x6A,0xDD,0xEB,0xC1,0x6A,0x01,0x8B,0xAC,0x18,0xDA };
    uint8_t out[256];
    const int n = edge_inflate(out, sizeof out, SRC, sizeof SRC);
    if (n != 132) { fprintf(stderr, "[inflate] selftest: got %d, want 132\n", n); return 1; }
    if (memcmp(out, "ui\0", 3) != 0) { fprintf(stderr, "[inflate] selftest: bad output\n"); return 1; }
    return 0;
}
