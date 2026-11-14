/*
 * rrc_replay: draw an RPCS3 RSX frame capture (.rrc, Alt+C in RPCS3) with the
 * live-draw engine, so one frame can be compared against RPCS3 with exactly
 * the inputs RPCS3 had -- same memory, same commands, same animation phase.
 *
 *   rrc_replay <capture.rrc> <outdir> [options]
 *     --after-draw N    also dump the surfaces right after the N-th draw
 *                       (1-based, counted at SET_BEGIN_END != 0); repeatable
 *     --surf L:OFF      a surface to dump (hex offset, location 0/1); repeatable.
 *                       Default: the colour targets the capture used.
 *
 * The .rrc must be uncompressed (RPCS3 writes .rrc.gz; gunzip it first).
 * The live-draw debug switches (LD_HLSL_DUMP, LD_FP_PROBE, RSX_DRAW_CSV, ...)
 * work as in the game. Windows only (D3D12).
 *
 * Capture layout (RPCS3 utils::serial, little-endian, sizes as VLE):
 *   magic, version, LE                      3 x u32
 *   tile_map          n x (432-byte state + u64)
 *   memory_map        n x (u32 offset, u32 location, u64 data_state, u64 index)
 *   memory_data_map   n x (VLE length, bytes, u64 index)
 *   display_buffers   n x (132-byte state + u64)
 *   replay_commands   n x (u32 header, u32 arg, VLE k + k x u64 memory
 *                          blocks to apply first, u64 tile, u64 display)
 *   reg_state         transform program (544 x 4 u32) + 0x4000 registers
 * A command with header 0 continues the previous method (+4 unless the
 * header was non-incrementing). memory_map offsets of location 1 are RSX IO
 * offsets, which is what the engine's location 1 means as well.
 */
#include "ps3emu/ps3types.h"
#include "rsx_live_draw.h"
#include "rsx_null_backend.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>

/* The runtime library references these; a lifted title provides them. */
uint8_t* vm_base = NULL;
struct rr_func_entry { uint64_t addr; void* func; const char* name; };
const struct rr_func_entry function_table[1] = { { 0, NULL, NULL } };
const uint64_t function_table_count = 0;

#define RR_LOCAL_SIZE (256u << 20)

typedef struct { u32 offset, location; u64 state, index; } rr_block;
typedef struct { u64 index; const u8* data; u32 len; } rr_data;
typedef struct { u32 header, arg; const u64* mem; u32 nmem; } rr_cmd;

static u8* s_local;
static u8* s_main;
static u32 s_main_size;

static const u8* rr_guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    const u32 size = location == 0 ? RR_LOCAL_SIZE : s_main_size;
    if ((u64)offset + min_bytes > size) return NULL;
    return (location == 0 ? s_local : s_main) + offset;
}

static const u8* s_p; static const u8* s_end;
static int rr_need(size_t n) { return (size_t)(s_end - s_p) >= n; }
static u32 rr_u32(void) { u32 v = 0; if (rr_need(4)) { memcpy(&v, s_p, 4); s_p += 4; } return v; }
static u64 rr_u64(void) { u64 v = 0; if (rr_need(8)) { memcpy(&v, s_p, 8); s_p += 8; } return v; }
static u64 rr_vle(void)
{
    u64 v = 0; int sh = 0;
    while (s_p < s_end) { const u8 b = *s_p++; v |= (u64)(b & 0x7F) << sh; sh += 7; if (!(b & 0x80)) break; }
    return v;
}

static int cmp_block(const void* a, const void* b)
{
    const u64 x = ((const rr_block*)a)->index, y = ((const rr_block*)b)->index;
    return x < y ? -1 : x > y;
}
static int cmp_data(const void* a, const void* b)
{
    const u64 x = ((const rr_data*)a)->index, y = ((const rr_data*)b)->index;
    return x < y ? -1 : x > y;
}

static rr_block* s_blocks; static u32 s_nblocks;
static rr_data* s_datas; static u32 s_ndatas;

static void rr_apply_block(u64 index)
{
    rr_block key = { 0, 0, 0, index };
    const rr_block* b = bsearch(&key, s_blocks, s_nblocks, sizeof *b, cmp_block);
    if (!b) return;
    rr_data dk = { b->state, NULL, 0 };
    const rr_data* d = bsearch(&dk, s_datas, s_ndatas, sizeof *d, cmp_data);
    if (!d) return;
    u8* base = b->location == 0 ? s_local : s_main;
    const u32 size = b->location == 0 ? RR_LOCAL_SIZE : s_main_size;
    if ((u64)b->offset + d->len > size) {
        fprintf(stderr, "[rrc] block %u:%X+%u past the arena\n", b->location, b->offset, d->len);
        return;
    }
    memcpy(base + b->offset, d->data, d->len);
}

/* The 2D engine, as cellGcmSys.c drives it: NV3062 (destination surface) on
 * subchannel 2/3, NV3089 (scaled image) on 6/7, the copy triggered by
 * IMAGE_IN_POINT. The game runs these in cellGcmSys, not in the live engine,
 * so a replay has to do them itself: on the GPU (live-draw blit / resolve) for
 * surfaces it drew, and in the arenas for everything else. DMA 0xFEED0001 is
 * main memory, anything else local. */
static struct { u32 dst_dma, pitch, dst_offset; } s_2d;
static struct { u32 src_dma, fmt, out_pt, out_sz, ds_dx, dt_dy, in_sz, in_fmt, in_off; } s_89;

static u8* rr_arena(u32 loc, u32 off, u32 bytes)
{
    const u32 size = loc == 0 ? RR_LOCAL_SIZE : s_main_size;
    if ((u64)off + bytes > size) return NULL;
    return (loc == 0 ? s_local : s_main) + off;
}

static void rr_nv3089_blit(u32 in_uv)
{
    const u32 out_w = s_89.out_sz & 0xFFFF, out_h = s_89.out_sz >> 16;
    const u32 out_x = s_89.out_pt & 0xFFFF, out_y = s_89.out_pt >> 16;
    const u32 in_pitch = s_89.in_fmt & 0xFFFF, dst_pitch = s_2d.pitch >> 16;
    if (!out_w || !out_h || !in_pitch || !dst_pitch) return;
    const u32 sl = s_89.src_dma != 0xFEED0001u ? 0u : 1u;
    const u32 dl = s_2d.dst_dma != 0xFEED0001u ? 0u : 1u;
    const u32 u0 = (in_uv & 0xFFFF) >> 4, v0 = (in_uv >> 16) >> 4;
    const u32 src = s_89.in_off + v0 * in_pitch + u0 * 4u;
    const u32 dst = s_2d.dst_offset + out_y * dst_pitch + out_x * 4u;
    printf("[rrc] nv3089 %u:%X/%u -> %u:%X/%u %ux%u ds=%X dt=%X fmt=%X\n", sl, src, in_pitch,
           dl, dst, dst_pitch, out_w, out_h, s_89.ds_dx, s_89.dt_dy, s_89.fmt & 0xFF);
    if (s_89.ds_dx == 0x100000u && s_89.dt_dy == 0x100000u) {
        if (!getenv("RR_NO_GPU_BLIT"))
            rsx_live_draw_blit(sl, src, in_pitch, dl, dst, dst_pitch, out_w, out_h);
        const u32 f = s_89.fmt & 0xFF;
        if (f == 0x7 || f == 0x8 || f == 0xD)          /* 32-bit colour: copy the rows */
            for (u32 y = 0; y < out_h; y++) {
                const u8* s = rr_arena(sl, src + y * in_pitch, out_w * 4u);
                u8* d = rr_arena(dl, dst + y * dst_pitch, out_w * 4u);
                if (s && d) memmove(d, s, out_w * 4u);
            }
    } else {
        rsx_live_draw_resolve_blit(sl, src, in_pitch, s_89.ds_dx, s_89.dt_dy, dl,
                                   s_2d.dst_offset, dst_pitch, out_x, out_y, out_w, out_h);
    }
}

/* Returns 1 when the method belonged to the 2D engine. */
static int rr_2d_method(u32 subch, u32 m, u32 data)
{
    if (subch == 2 || subch == 3) {
        switch (m) {
        case 0x0184: case 0x0188: s_2d.dst_dma = data; break;
        case 0x0304: s_2d.pitch = data; break;
        case 0x030C: s_2d.dst_offset = data; break;
        }
        return 1;
    }
    if (subch == 6 || subch == 7) {
        switch (m) {
        case 0x0184: s_89.src_dma = data; break;
        case 0x0300: s_89.fmt = data; break;
        case 0x0310: s_89.out_pt = data; break;
        case 0x0314: s_89.out_sz = data; break;
        case 0x0318: s_89.ds_dx = data; break;
        case 0x031C: s_89.dt_dy = data; break;
        case 0x0400: s_89.in_sz = data; break;
        case 0x0404: s_89.in_fmt = data; break;
        case 0x0408: s_89.in_off = data; break;
        case 0x040C: rr_nv3089_blit(data); break;
        }
        return 1;
    }
    return 0;
}

static u32 s_surf_loc[32], s_surf_off[32], s_nsurf;

static void rr_add_surf(u32 loc, u32 off)
{
    for (u32 i = 0; i < s_nsurf; i++) if (s_surf_loc[i] == loc && s_surf_off[i] == off) return;
    if (s_nsurf < 32) { s_surf_loc[s_nsurf] = loc; s_surf_off[s_nsurf] = off; s_nsurf++; }
}

static void rr_dump(const char* dir, const char* tag)
{
    rsx_live_draw_flush();
    for (u32 i = 0; i < s_nsurf; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s_%u_%08X.ppm", dir, tag, s_surf_loc[i], s_surf_off[i]);
        if (rsx_live_draw_debug_dump_surface(s_surf_loc[i], s_surf_off[i], path) == 0)
            printf("[rrc] wrote %s\n", path);
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: rrc_replay <capture.rrc> <outdir> [--after-draw N]... [--surf L:OFF]...\n");
        return 2;
    }
    const char* outdir = argv[2];
    u32 after[64], nafter = 0;
    const char* vpconst = NULL;
    /* --guest-surf L:OFF@N: right before draw N, load that surface's GPU image
     * from the capture's memory -- what RPCS3 had there -- so the draw sees
     * RPCS3's inputs even where an earlier pass of ours came out different. */
    u32 gs_loc[16], gs_off[16], gs_draw[16], ngs = 0;
    const char* gs_file[16];
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--after-draw") && i + 1 < argc && nafter < 64) after[nafter++] = (u32)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--vpconst") && i + 1 < argc) vpconst = argv[++i];
        else if (!strcmp(argv[i], "--guest-surf") && i + 1 < argc && ngs < 16) {
            const char* s = argv[++i];
            const char* c = strchr(s, ':'); const char* at = strchr(s, '@');
            if (c && at) { gs_loc[ngs] = (u32)strtoul(s, 0, 0); gs_off[ngs] = (u32)strtoul(c + 1, 0, 16);
                           gs_draw[ngs] = (u32)strtoul(at + 1, 0, 0);
                           /* ...=file: raw guest bytes to put at that offset first */
                           const char* eq = strchr(at, '=');
                           gs_file[ngs] = eq ? eq + 1 : NULL; ngs++; }
        }
        else if (!strcmp(argv[i], "--surf") && i + 1 < argc) {
            const char* s = argv[++i];
            const char* c = strchr(s, ':');
            if (c) rr_add_surf((u32)strtoul(s, 0, 0), (u32)strtoul(c + 1, 0, 16));
        }
    }
    _mkdir(outdir);

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); const long len = ftell(f); fseek(f, 0, SEEK_SET);
    u8* file = (u8*)malloc((size_t)len);
    if (!file || fread(file, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    s_p = file; s_end = file + len;

    const u32 magic = rr_u32(), version = rr_u32(), le = rr_u32();
    printf("[rrc] magic %08X version %u le %u, %ld bytes\n", magic, version, le, len);
    u64 n = rr_vle(); s_p += n * (432 + 8);                       /* tile_map */
    n = rr_vle();
    s_blocks = (rr_block*)calloc((size_t)n, sizeof *s_blocks); s_nblocks = (u32)n;
    u32 main_end = 0;
    for (u32 i = 0; i < s_nblocks; i++) {
        s_blocks[i].offset = rr_u32(); s_blocks[i].location = rr_u32();
        s_blocks[i].state = rr_u64(); s_blocks[i].index = rr_u64();
    }
    n = rr_vle();
    s_datas = (rr_data*)calloc((size_t)n, sizeof *s_datas); s_ndatas = (u32)n;
    for (u32 i = 0; i < s_ndatas; i++) {
        const u64 l = rr_vle();
        s_datas[i].data = s_p; s_datas[i].len = (u32)l; s_p += l;
        s_datas[i].index = rr_u64();
    }
    qsort(s_blocks, s_nblocks, sizeof *s_blocks, cmp_block);
    qsort(s_datas, s_ndatas, sizeof *s_datas, cmp_data);
    for (u32 i = 0; i < s_nblocks; i++) {
        if (s_blocks[i].location != 1) continue;
        rr_data dk = { s_blocks[i].state, NULL, 0 };
        const rr_data* d = bsearch(&dk, s_datas, s_ndatas, sizeof *d, cmp_data);
        if (d && s_blocks[i].offset + d->len > main_end) main_end = s_blocks[i].offset + d->len;
    }
    s_main_size = (main_end + 0xFFFFFu) & ~0xFFFFFu;
    n = rr_vle(); s_p += n * (132 + 8);                           /* display buffers */
    n = rr_vle();
    rr_cmd* cmds = (rr_cmd*)calloc((size_t)n, sizeof *cmds); const u32 ncmds = (u32)n;
    for (u32 i = 0; i < ncmds; i++) {
        cmds[i].header = rr_u32(); cmds[i].arg = rr_u32();
        const u64 k = rr_vle();
        cmds[i].mem = (const u64*)s_p; cmds[i].nmem = (u32)k; s_p += k * 8;
        rr_u64(); rr_u64();
    }
    if (!rr_need(8704 + 0x10000)) { fprintf(stderr, "[rrc] no register state at the end\n"); return 1; }
    const u32* transform = (const u32*)s_p;
    const u32* regs = (const u32*)(s_p + 8704);
    printf("[rrc] %u blocks, %u data, %u commands, main arena %u MB\n",
           s_nblocks, s_ndatas, ncmds, s_main_size >> 20);

    s_local = (u8*)calloc(1, RR_LOCAL_SIZE);
    s_main = (u8*)calloc(1, s_main_size ? s_main_size : 1);
    if (!s_local || !s_main) { fprintf(stderr, "arena alloc failed\n"); return 1; }

    _putenv("RSX_LIVE_DRAW=1");
    if (rsx_null_backend_init(1280, 720, "rrc_replay") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 1280, 720, rr_guest_ptr, NULL) != 0) {
        fprintf(stderr, "[rrc] live-draw init failed\n");
        return 1;
    }
    rsx_null_backend_suppress_present(1);
    rsx_live_draw_seed_registers(regs, 0x4000);
    rsx_live_draw_seed_transform_program(transform, 544 * 4);
    /* RPCS3 leaves the transform constants out of a capture's register
     * state, so constants the game uploaded before the captured frame read
     * as zero. --vpconst seeds them from our own F12 capture
     * (captures\cap_*\vp_constants.bin); the frame's uploads still win. */
    if (vpconst) {
        FILE* cf = fopen(vpconst, "rb");
        static u32 words[512 * 4];
        const size_t got = cf ? fread(words, 4, 512 * 4, cf) : 0;
        if (cf) fclose(cf);
        printf("[rrc] %zu transform-constant words from %s\n", got, vpconst);
        if (got) rsx_live_draw_seed_transform_constants(words, (u32)got);
    }

    /* Two passes over the frame. A single frame uploads every texture at once
     * and overflows the engine's 64 MB upload ring (decode failed, sampled
     * as nothing), which the game never sees after its first frames since its
     * textures stay cached. The first pass warms that cache; the dumps come
     * from the second. --passes 1 for one. */
    u32 passes = 2;
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--passes") && i + 1 < argc) passes = (u32)strtoul(argv[i + 1], 0, 0);
    u32 method = 0, nonincr = 0, draws = 0, n2d = 0;
    for (u32 pass = 0; pass < passes; pass++) {
    const int last = pass + 1 == passes;
    method = nonincr = draws = n2d = 0;
    if (pass) rsx_live_draw_flush();
    for (u32 i = 0; i < ncmds; i++) {
        for (u32 k = 0; k < cmds[i].nmem; k++) {
            u64 idx; memcpy(&idx, &cmds[i].mem[k], 8);
            rr_apply_block(idx);
        }
        if (cmds[i].header) { method = cmds[i].header & 0xFFFCu; nonincr = cmds[i].header & 0x40000000u; }
        else if (!nonincr) method += 4;
        const u32 arg = cmds[i].arg;
        if (method < 0x100) continue;                  /* FIFO object/NOP traffic */
        /* The game hands every method to the live engine as well, with its
         * subchannel bits; do the same, after the 2D engine had its look. */
        if (rr_2d_method((method >> 13) & 7u, method & 0x1FFCu, arg)) n2d++;
        if (method == 0x0210 || method == 0x0218 || method == 0x0288 || method == 0x028C)
            rr_add_surf(0, arg);                      /* colour target offsets (local) */
        if ((method & 0x1FFCu) == 0x1808 && arg)
            for (u32 k = 0; k < ngs; k++)
                if (gs_draw[k] == draws + 1 && last) {
                    if (gs_file[k]) {
                        FILE* gf = fopen(gs_file[k], "rb");
                        u8* d = gf ? rr_arena(gs_loc[k], gs_off[k], 1) : NULL;
                        if (gf && d) {
                            const size_t room = (gs_loc[k] ? s_main_size : RR_LOCAL_SIZE) - gs_off[k];
                            printf("[rrc] %zu bytes from %s\n", fread(d, 1, room, gf), gs_file[k]);
                        }
                        if (gf) fclose(gf);
                    }
                    printf("[rrc] draw %u: %u:%X from guest memory: %d\n", draws + 1, gs_loc[k], gs_off[k],
                           rsx_live_draw_debug_surface_from_guest(gs_loc[k], gs_off[k]));
                }
        rsx_live_draw_method(method, arg);
        if (method == 0x1808 && arg) draws++;
        if (method == 0x1808 && !arg && last)
            for (u32 a = 0; a < nafter; a++)
                if (after[a] == draws) {
                    char tag[32]; snprintf(tag, sizeof tag, "draw%04u", draws);
                    rr_dump(outdir, tag);
                }
    }
    printf("[rrc] pass %u: %u draws replayed, %u 2D-engine methods\n", pass + 1, draws, n2d);
    }
    rr_dump(outdir, "end");
    rsx_live_draw_shutdown();
    return 0;
}
