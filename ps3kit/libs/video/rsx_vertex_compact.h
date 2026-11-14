/*
 * ps3recomp - compact CPU vertex-fetch planning for the live RSX renderer
 *
 * The legacy renderer expands every guest attribute into a 16 x float4
 * vertex.  This helper preserves the same conversion rules while allowing a
 * caller to fetch only the attributes statically read by the active vertex
 * program.  It is D3D12-independent so format/bounds behavior can be tested
 * without creating a device.
 */
#ifndef PS3RECOMP_RSX_VERTEX_COMPACT_H
#define PS3RECOMP_RSX_VERTEX_COMPACT_H

#include "rsx_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef const u8* (*rsx_vertex_guest_ptr_fn)(
    void* user, u32 location, u32 offset, u32 min_bytes);

typedef struct rsx_vertex_layout_plan {
    u32 mask;
    u32 stride;
    u8 count;
    u8 attrs[RSX_DSP_NUM_VERTEX_ATTR];
    s8 slot_by_attr[RSX_DSP_NUM_VERTEX_ATTR];
    /* LD_VTX_RAW (rsx_live_draw.c): the attributes arrive as the guest's own
     * bytes and the vertex shader decodes them. Per slot: raw_kind 0 = a
     * float4 (an attribute with no array: its constant value), else
     * type * 8 + size; raw_in = input slot, raw_off = byte offset in it,
     * raw_words = 32-bit words read. All zero when raw is 0. */
    u8 raw;
    u8 raw_kind[RSX_DSP_NUM_VERTEX_ATTR];
    u8 raw_in[RSX_DSP_NUM_VERTEX_ATTR];
    u8 raw_words[RSX_DSP_NUM_VERTEX_ATTR];
    u16 raw_off[RSX_DSP_NUM_VERTEX_ATTR];
} rsx_vertex_layout_plan;

typedef struct rsx_vertex_ref {
    u32 vertex_id;
    u32 base_index;
} rsx_vertex_ref;

typedef struct rsx_vertex_remap_slot {
    u64 key;
    u32 value;
    u32 generation;
} rsx_vertex_remap_slot;

typedef struct rsx_vertex_remap {
    u32* occurrence_to_unique;
    u32 occurrence_capacity;
    rsx_vertex_remap_slot* slots;
    u32 slot_capacity;
    u32 generation;
    /* Direct table for a draw whose references share one base index and
     * span a small vertex-id range: index = vertex_id - min, no hashing. */
    u32* dir_stamp;
    u32* dir_value;
    u32 dir_capacity;
    u32 dir_generation;
} rsx_vertex_remap;

typedef struct rsx_vertex_fetch_attr {
    rsx_dsp_vertex_attr desc;
    u32 elem_size;
    u32 stride;
    float default_value[4];
    const u8* stable_base;
    u32 stable_first;
    u32 stable_last;
} rsx_vertex_fetch_attr;

typedef struct rsx_vertex_fetch_plan {
    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_attr attr[RSX_DSP_NUM_VERTEX_ATTR];
    u32 base_offset;
    u32 divider_mask;
    rsx_vertex_guest_ptr_fn guest_ptr;
    void* guest_user;
    /* INLINE_ARRAY source: when set, the vertices came down the FIFO and the
     * array offsets/locations mean nothing -- every attribute is read out of
     * this stream instead. See rsx_vertex_inline_layout. */
    const u8* inline_data;
    u32 inline_bytes;
    u32 inline_off[RSX_DSP_NUM_VERTEX_ATTR]; /* byte offset within a vertex */
    u32 inline_stride;                       /* decoded once, at _init      */
} rsx_vertex_fetch_plan;

/* Deterministic ascending-register layout; each selected register remains a
 * float4 and retains its original ATTR semantic index. */
void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask);

/* Byte offset of each attribute within one INLINE_ARRAY vertex, and the
 * vertex size as the return value.
 *
 * An inline stream carries no per-array offset registers -- the only thing
 * that says where an attribute sits is the VTXFMT declaration, and the
 * hardware rule is that the ENABLED attributes pack in ascending register
 * order. The declared stride wins over the packed size when the two differ
 * (a format may pad); the packed size is the fallback when no enabled array
 * declares a stride. Shared so the hoisted, legacy and reference fetch paths
 * cannot drift on it. */
u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR]);

/* Point a plan at an INLINE_ARRAY stream instead of guest memory. Call
 * between _init and _prepare; a NULL `data` leaves the plan reading guest
 * memory, so an ordinary draw can call it unconditionally and pay nothing. */
void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes);

/* Decode all 16 descriptors/defaults once for a draw. */
void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user);

/* Attempt one bounds-checked contiguous mapping per used attribute.  Any
 * attribute whose complete span is not contiguous safely falls back to the
 * legacy per-element resolver in rsx_vertex_fetch_one. */
void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count);

/* Fetch one compact vertex in layout order.  Disabled attributes use their
 * RSX current/default value.  Enabled non-position attributes retain the
 * legacy fallback-to-default behavior on an unsupported/invalid fetch; an
 * invalid enabled ATTR0 remains fatal. */
int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out);

/* rsx_vertex_fetch_one for refs[0..count), into out + i * layout.stride,
 * attribute by attribute: each attribute's format and span are settled once
 * instead of once per vertex. Same bytes, same failure rule. */
int rsx_vertex_fetch_many(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs,
    u32 count, u8* out);

/* The batch fetch's SSE4.1 loops (LD_VTX_SIMD, default on): -1 until the
 * first batch reads the environment; set 0/1 to override (tests). */
extern int rsx_vertex_fetch_simd;

/* LD_VTX_FAST (default on): the AVX2 + FMA batch fetch, prepare span and
 * index decode, on CPUs that have both (checked once). -1 until the first
 * use reads the environment; set 0/1 to override (tests). Same bytes. */
extern int rsx_vertex_fetch_fast;

/* refs[i] = { first_vertex + i, base_index } for i < count. */
void rsx_vertex_fill_run_refs(
    rsx_vertex_ref* refs, u32 first_vertex, u32 base_index, u32 count);

/* One triangle-strip segment (begin, count references) as triangle-list
 * indices: for i + 2 < count, triangle i is (i + (i & 1), i + 1 - (i & 1),
 * i + 2) from begin, each through map (map[x]; NULL: x itself). Returns the
 * number of indices written, 3 * (count - 2). */
u32 rsx_vertex_strip_indices(const u32* map, u32 begin, u32 count, u32* out);

/* out[i] = first + i for i < count. */
void rsx_vertex_fill_iota(u32* out, u32 first, u32 count);

/* LD_VTX_FAST's switch alone, without the CPU check (the live renderer's
 * plain-C shortcuts that come with it). */
int rsx_vertex_fast_switch(void);

/* Collapse repeated (vertex_id, base_index) references in first-use order.
 * refs is rewritten in place with the unique prefix.  The occurrence map
 * remains valid until the next call using the same remap object. */
int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count);

/* Room for `count` occurrences in remap->occurrence_to_unique. */
int rsx_vertex_remap_reserve(rsx_vertex_remap* remap, u32 count);

/* Decode big-endian indices (u16, or u32 when is_u32) from `src` into `out`,
 * stopping before the first one equal to `stop` when stop_enabled (a primitive
 * restart). Returns how many were decoded: `count` when no stop value came.
 * *lo and *hi are lowered/raised to the smallest and largest decoded index. */
u32 rsx_vertex_decode_index_run(
    const u8* src, u32 count, int is_u32, int stop_enabled, u32 stop,
    u32* out, u32* lo, u32* hi);

/* Inline: it runs once per index of every indexed draw. */
static inline u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap);

/* Select the live renderer's triangle topology path.  A remapped triangle
 * list must use an index buffer to reconstruct duplicate occurrences, just
 * like converted strips, fans, and quads.  Returns zero for unsupported
 * primitive classes. */
int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed);

/* Public for focused format/endian tests. */
int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4]);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_VERTEX_COMPACT_H */
