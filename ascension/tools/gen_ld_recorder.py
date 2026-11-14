#!/usr/bin/env python3
"""
gen_ld_recorder.py -- generate rsx_ld_recorder.h, the D3D12 recorder of
rsx_live_draw.c (my-changes\\recorder\\DESIGN.md), and its unit test.

One table below lists every command-list method rsx_live_draw.c calls through
LD_CL (tools/ld_cl_wrap.py), with each parameter's kind:
  V  by value (scalars, handles, COM pointers)
  A  const T* array whose length is another parameter (an expression)
  O  const T* that may be NULL (one element)
  F  const T* of a fixed number of elements, never NULL
OMSetDepthBounds is an ID3D12GraphicsCommandList1 method: it goes to the
list's ID3D12GraphicsCommandList1, queried once per list by the thread that
executes it (ldr_list1).

LD_RECORDER (default 0):
  0  ld_cl_<M>: the direct call (one predictable branch).
  1  each call encoded into a record, decoded and executed at once on the
     calling thread (the encoders under test).
  2  each call encoded into a 4 MB ring; a recorder thread decodes and
     executes them, so the D3D12 runtime and driver time leaves the RSX
     thread. ld_flush submits with ldr_submit (Close, ExecuteCommandLists and
     Signal run on the recorder, in order); ld_present waits for the ring to
     drain (ldr_drain) before Present; failures come back through
     ldr_failed; ldr_shutdown drains and stops the thread.
LD_RECORDER_TRACE=1: every call hashed from its arguments where it is made,
and again from the decoded arguments where it reaches D3D12 ([ld-rec] line
every 5 s with the mismatch total, the first eight named). The hash never
reads the record, so an encoder or decoder that loses or mangles anything
shows up.

Usage: python tools/gen_ld_recorder.py <out header> [<out unit test .c>]
"""
import sys

# name, return type, [(kind, C type of the parameter, name, length expr or element count)]
M = [
    ("ResourceBarrier", "void", [("V", "UINT", "NumBarriers", None),
                                 ("A", "D3D12_RESOURCE_BARRIER", "pBarriers", "NumBarriers")]),
    ("CopyTextureRegion", "void", [("F", "D3D12_TEXTURE_COPY_LOCATION", "pDst", "1"),
                                   ("V", "UINT", "DstX", None), ("V", "UINT", "DstY", None),
                                   ("V", "UINT", "DstZ", None),
                                   ("F", "D3D12_TEXTURE_COPY_LOCATION", "pSrc", "1"),
                                   ("O", "D3D12_BOX", "pSrcBox", None)]),
    ("SetComputeRootDescriptorTable", "void", [("V", "UINT", "RootParameterIndex", None),
                                               ("V", "D3D12_GPU_DESCRIPTOR_HANDLE", "BaseDescriptor", None)]),
    ("SetPipelineState", "void", [("V", "ID3D12PipelineState*", "pPipelineState", None)]),
    ("SetDescriptorHeaps", "void", [("V", "UINT", "NumDescriptorHeaps", None),
                                    ("A", "ID3D12DescriptorHeap*", "ppDescriptorHeaps", "NumDescriptorHeaps")]),
    ("SetGraphicsRootDescriptorTable", "void", [("V", "UINT", "RootParameterIndex", None),
                                                ("V", "D3D12_GPU_DESCRIPTOR_HANDLE", "BaseDescriptor", None)]),
    ("ClearDepthStencilView", "void", [("V", "D3D12_CPU_DESCRIPTOR_HANDLE", "DepthStencilView", None),
                                       ("V", "D3D12_CLEAR_FLAGS", "ClearFlags", None),
                                       ("V", "FLOAT", "Depth", None), ("V", "UINT8", "Stencil", None),
                                       ("V", "UINT", "NumRects", None),
                                       ("A", "D3D12_RECT", "pRects", "NumRects")]),
    ("SetComputeRootSignature", "void", [("V", "ID3D12RootSignature*", "pRootSignature", None)]),
    ("Dispatch", "void", [("V", "UINT", "X", None), ("V", "UINT", "Y", None), ("V", "UINT", "Z", None)]),
    ("ClearRenderTargetView", "void", [("V", "D3D12_CPU_DESCRIPTOR_HANDLE", "RenderTargetView", None),
                                       ("F", "FLOAT", "ColorRGBA", "4"),
                                       ("V", "UINT", "NumRects", None),
                                       ("A", "D3D12_RECT", "pRects", "NumRects")]),
    ("SetGraphicsRootSignature", "void", [("V", "ID3D12RootSignature*", "pRootSignature", None)]),
    ("SetGraphicsRootConstantBufferView", "void", [("V", "UINT", "RootParameterIndex", None),
                                                   ("V", "D3D12_GPU_VIRTUAL_ADDRESS", "BufferLocation", None)]),
    ("SetComputeRoot32BitConstant", "void", [("V", "UINT", "RootParameterIndex", None),
                                             ("V", "UINT", "SrcData", None),
                                             ("V", "UINT", "DestOffsetIn32BitValues", None)]),
    ("Reset", "HRESULT", [("V", "ID3D12CommandAllocator*", "pAllocator", None),
                          ("V", "ID3D12PipelineState*", "pInitialState", None)]),
    ("RSSetViewports", "void", [("V", "UINT", "NumViewports", None),
                                ("A", "D3D12_VIEWPORT", "pViewports", "NumViewports")]),
    ("RSSetScissorRects", "void", [("V", "UINT", "NumRects", None),
                                   ("A", "D3D12_RECT", "pRects", "NumRects")]),
    ("OMSetRenderTargets", "void", [("V", "UINT", "NumRenderTargetDescriptors", None),
                                    ("A", "D3D12_CPU_DESCRIPTOR_HANDLE", "pRenderTargetDescriptors",
                                     "(RTsSingleHandleToDescriptorRange ? (NumRenderTargetDescriptors ? 1u : 0u)"
                                     " : NumRenderTargetDescriptors)"),
                                    ("V", "BOOL", "RTsSingleHandleToDescriptorRange", None),
                                    ("O", "D3D12_CPU_DESCRIPTOR_HANDLE", "pDepthStencilDescriptor", None)]),
    ("IASetPrimitiveTopology", "void", [("V", "D3D12_PRIMITIVE_TOPOLOGY", "PrimitiveTopology", None)]),
    ("DrawInstanced", "void", [("V", "UINT", "VertexCountPerInstance", None), ("V", "UINT", "InstanceCount", None),
                               ("V", "UINT", "StartVertexLocation", None), ("V", "UINT", "StartInstanceLocation", None)]),
    ("CopyResource", "void", [("V", "ID3D12Resource*", "pDstResource", None),
                              ("V", "ID3D12Resource*", "pSrcResource", None)]),
    ("OMSetStencilRef", "void", [("V", "UINT", "StencilRef", None)]),
    ("OMSetBlendFactor", "void", [("F", "FLOAT", "BlendFactor", "4")]),
    ("IASetVertexBuffers", "void", [("V", "UINT", "StartSlot", None), ("V", "UINT", "NumViews", None),
                                    ("A", "D3D12_VERTEX_BUFFER_VIEW", "pViews", "NumViews")]),
    ("IASetIndexBuffer", "void", [("O", "D3D12_INDEX_BUFFER_VIEW", "pView", None)]),
    ("DrawIndexedInstanced", "void", [("V", "UINT", "IndexCountPerInstance", None), ("V", "UINT", "InstanceCount", None),
                                      ("V", "UINT", "StartIndexLocation", None), ("V", "INT", "BaseVertexLocation", None),
                                      ("V", "UINT", "StartInstanceLocation", None)]),
    ("Close", "HRESULT", []),
    ("OMSetDepthBounds", "void", [("V", "FLOAT", "Min", None), ("V", "FLOAT", "Max", None)]),
]
LIST1 = {"OMSetDepthBounds"}


def ptype(kind, t):
    if kind == "V":
        return t
    if t.endswith("*"):                      # array of COM pointers
        return t + " const*"
    return "const " + t + "*"


def plist(params):
    return ", ".join(f"{ptype(k, t)} {n}" for k, t, n, _ in params)


def alist(params):
    return ", ".join(n for _, _, n, _ in params)


def call_expr(name, params):
    args = alist(params)
    if name in LIST1:
        return f"{{ ID3D12GraphicsCommandList1* l1_ = ldr_list1(L); if (l1_) l1_->lpVtbl->{name}(l1_{', ' + args if args else ''}); }}"
    return f"L->lpVtbl->{name}(L{', ' + args if args else ''})"


RUNTIME = r'''
/* ---- shared state -------------------------------------------------------- */
static int g_ldr_flags = -1;   /* -1: not read yet; else bit 0 mode 1, bit 1 trace, bit 2 mode 2; 0 = direct */
static int g_ldr_mode, g_ldr_trace;
static unsigned long long g_ldr_calls, g_ldr_mismatch, g_ldr_seq;
static unsigned char g_ldr_tmp[1u << 16];   /* one record being built (RSX thread only) */
typedef struct { unsigned int size, op; unsigned long long hash; void* list; unsigned long long seq; } ldr_hdr_t;
#define LDR_HDR 32u
#define LDR_ALIGN(n) (((n) + 31u) & ~31u)
#define LDR_OP_SUBMIT 1000u
#define LDR_OP_WRAP   1001u
typedef struct { unsigned long long h; } ldr_hash_t;
static inline void ldr_h(ldr_hash_t* s, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) s->h = (s->h ^ b[i]) * 1099511628211ull;
}
typedef struct { unsigned char* p; } ldr_w_t;
static inline void ldr_put(ldr_w_t* w, const void* p, size_t n) { memcpy(w->p, p, n); w->p += n; }
typedef struct { const unsigned char* p; } ldr_r_t;
static inline const void* ldr_get(ldr_r_t* r, size_t n) { const void* q = r->p; r->p += n; return q; }

/* The list's ID3D12GraphicsCommandList1, per executing thread and list. */
static __declspec(thread) ID3D12GraphicsCommandList* t_ldr_l1_for;
static __declspec(thread) ID3D12GraphicsCommandList1* t_ldr_l1;
static ID3D12GraphicsCommandList1* ldr_list1(ID3D12GraphicsCommandList* L)
{
    if (t_ldr_l1_for != L) {
        if (t_ldr_l1) t_ldr_l1->lpVtbl->Release(t_ldr_l1);
        t_ldr_l1 = NULL;
        L->lpVtbl->QueryInterface(L, &IID_ID3D12GraphicsCommandList1, (void**)&t_ldr_l1);
        t_ldr_l1_for = L;
    }
    return t_ldr_l1;
}

/* ---- mode 2: the ring and the recorder thread ----------------------------- */
#define LDR_RING (4u << 20)
static unsigned char* g_ldr_ring;
static volatile long long g_ldr_w, g_ldr_r;              /* byte positions, never wrap */
static volatile long g_ldr_csleep, g_ldr_pwait, g_ldr_stop;
static HANDLE g_ldr_cev, g_ldr_pev, g_ldr_thread;
static volatile unsigned long long g_ldr_submitted;
static volatile long g_ldr_fail;
static HRESULT g_ldr_fail_hr;
static const char* g_ldr_fail_what;
static unsigned long long g_ldr_pwaits, g_ldr_pwait_us, g_ldr_hiwater;
static HRESULT ldr_exec(const unsigned char* rec);
static void ldr_exec_submit(const unsigned char* rec)
{
    const ldr_hdr_t* h = (const ldr_hdr_t*)rec;
    ID3D12GraphicsCommandList* L = (ID3D12GraphicsCommandList*)h->list;
    ID3D12CommandQueue* q; ID3D12Fence* f; unsigned long long v;
    memcpy(&q, rec + LDR_HDR, sizeof q); memcpy(&f, rec + LDR_HDR + 8, sizeof f); memcpy(&v, rec + LDR_HDR + 16, sizeof v);
    const HRESULT hr = L->lpVtbl->Close(L);
    if (FAILED(hr)) { if (!g_ldr_fail) { g_ldr_fail_hr = hr; g_ldr_fail_what = "Close"; __atomic_store_n(&g_ldr_fail, 1, __ATOMIC_RELEASE); } }
    else { ID3D12CommandList* lists[1] = { (ID3D12CommandList*)L }; q->lpVtbl->ExecuteCommandLists(q, 1, lists); }
    const HRESULT hs = q->lpVtbl->Signal(q, f, v);       /* always: a waiter on v must not hang */
    if (FAILED(hs) && !g_ldr_fail) { g_ldr_fail_hr = hs; g_ldr_fail_what = "Signal"; __atomic_store_n(&g_ldr_fail, 1, __ATOMIC_RELEASE); }
    __atomic_store_n(&g_ldr_submitted, v, __ATOMIC_SEQ_CST);
}
static DWORD WINAPI ldr_main(LPVOID unused)
{
    (void)unused;
    long long r = g_ldr_r;
    for (;;) {
        long long w = __atomic_load_n(&g_ldr_w, __ATOMIC_ACQUIRE);
        if (r == w) {
            for (int s = 0; s < 4000 && (w = __atomic_load_n(&g_ldr_w, __ATOMIC_ACQUIRE)) == r; s++) _mm_pause();
            if (w == r) {
                if (__atomic_load_n(&g_ldr_stop, __ATOMIC_ACQUIRE)) break;
                __atomic_store_n(&g_ldr_csleep, 1, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&g_ldr_w, __ATOMIC_SEQ_CST) == r) WaitForSingleObject(g_ldr_cev, 1);
                __atomic_store_n(&g_ldr_csleep, 0, __ATOMIC_SEQ_CST);
                continue;
            }
        }
        const unsigned char* rec = g_ldr_ring + (unsigned)(r % LDR_RING);
        const ldr_hdr_t* h = (const ldr_hdr_t*)rec;
        const unsigned size = h->size;
        if (h->op == LDR_OP_SUBMIT) ldr_exec_submit(rec);
        else if (h->op != LDR_OP_WRAP) (void)ldr_exec(rec);
        r += size;
        __atomic_store_n(&g_ldr_r, r, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_ldr_pwait, __ATOMIC_SEQ_CST)) SetEvent(g_ldr_pev);
    }
    if (t_ldr_l1) { t_ldr_l1->lpVtbl->Release(t_ldr_l1); t_ldr_l1 = NULL; t_ldr_l1_for = NULL; }
    return 0;
}
/* The RSX thread waits until cond holds: spin, then sleep on the event the
 * recorder sets after each record while someone waits (1 ms timeout). */
#define LDR_WAIT(cond) do {                                                         \
        extern unsigned long long ps3_qpc_us(void);                                 \
        const unsigned long long t0_ = ps3_qpc_us(); int s_ = 0;                    \
        while (!(cond)) {                                                           \
            if (++s_ < 2000) { _mm_pause(); continue; }                             \
            __atomic_store_n(&g_ldr_pwait, 1, __ATOMIC_SEQ_CST);                    \
            if (!(cond)) WaitForSingleObject(g_ldr_pev, 1);                         \
            __atomic_store_n(&g_ldr_pwait, 0, __ATOMIC_SEQ_CST);                    \
        }                                                                           \
        if (s_) { g_ldr_pwaits++; g_ldr_pwait_us += ps3_qpc_us() - t0_; }           \
    } while (0)
static void ldr_push(const unsigned char* rec, unsigned size)   /* size: a multiple of 32 */
{
    long long w = g_ldr_w;
    unsigned off = (unsigned)(w % LDR_RING);
    const unsigned need = size + (off + size > LDR_RING ? LDR_RING - off : 0u);
    if (w + need - __atomic_load_n(&g_ldr_r, __ATOMIC_ACQUIRE) > (long long)LDR_RING)
        LDR_WAIT(w + need - __atomic_load_n(&g_ldr_r, __ATOMIC_SEQ_CST) <= (long long)LDR_RING);
    if (off + size > LDR_RING) {                          /* no record wraps: pad to the end */
        ldr_hdr_t* m = (ldr_hdr_t*)(g_ldr_ring + off);
        m->size = LDR_RING - off; m->op = LDR_OP_WRAP;
        w += LDR_RING - off; off = 0;
    }
    memcpy(g_ldr_ring + off, rec, size);
    __atomic_store_n(&g_ldr_w, w + size, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_ldr_csleep, __ATOMIC_SEQ_CST)) SetEvent(g_ldr_cev);
    const unsigned long long used = (unsigned long long)(w + size - __atomic_load_n(&g_ldr_r, __ATOMIC_RELAXED));
    if (used > g_ldr_hiwater) g_ldr_hiwater = used;
}
static void ldr_start(void)
{
    if (g_ldr_thread) return;
    g_ldr_ring = (unsigned char*)_aligned_malloc(LDR_RING, 64);
    g_ldr_cev = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ldr_pev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_ldr_ring || !g_ldr_cev || !g_ldr_pev) { fprintf(stderr, "[ld-rec] mode 2 unavailable: ring/events\n"); g_ldr_mode = 0; g_ldr_flags = g_ldr_trace << 1; return; }
    g_ldr_thread = CreateThread(NULL, 0, ldr_main, NULL, 0, NULL);
    if (!g_ldr_thread) { fprintf(stderr, "[ld-rec] mode 2 unavailable: thread\n"); g_ldr_mode = 0; g_ldr_flags = g_ldr_trace << 1; }
}
static void ldr_set_mode(int mode, int trace)
{
    g_ldr_mode = mode; g_ldr_trace = trace;
    g_ldr_flags = (mode == 1) | (trace << 1) | ((mode == 2) << 2);
    if (mode == 2) ldr_start();
}
static void ldr_init(void)
{
    const char* m = getenv("LD_RECORDER"), *t = getenv("LD_RECORDER_TRACE");
    ldr_set_mode(m && m[0] == '1' ? 1 : m && m[0] == '2' ? 2 : 0, t && t[0] == '1');
    if (g_ldr_flags) fprintf(stderr, "[ld-rec] mode %d, trace %s\n", g_ldr_mode, g_ldr_trace ? "on" : "off");
}
/* rsx_live_draw.c's side of mode 2. */
static int ldr_mode2(void) { if (g_ldr_flags < 0) ldr_init(); return g_ldr_mode == 2; }
static void ldr_drain(void)
{
    if (g_ldr_mode != 2 || !g_ldr_thread) return;
    LDR_WAIT(__atomic_load_n(&g_ldr_r, __ATOMIC_SEQ_CST) == g_ldr_w);
}
/* Close, ExecuteCommandLists and Signal(v) of L, on the recorder, in order. */
static unsigned long long g_ldr_last_v;   /* the value of the last ldr_submit */
static void ldr_submit(ID3D12GraphicsCommandList* L, ID3D12CommandQueue* q, ID3D12Fence* f, unsigned long long v)
{
    unsigned char rec[64];
    memset(rec, 0, sizeof rec);
    ldr_hdr_t* h = (ldr_hdr_t*)rec;
    h->size = 64; h->op = LDR_OP_SUBMIT; h->list = L; h->seq = ++g_ldr_seq;
    memcpy(rec + LDR_HDR, &q, sizeof q); memcpy(rec + LDR_HDR + 8, &f, sizeof f); memcpy(rec + LDR_HDR + 16, &v, sizeof v);
    ldr_push(rec, 64);
    g_ldr_last_v = v;
}
/* Until Signal(v) is on the queue (a failed one counts): a fence wait on v
 * can end. ldr_wait_last_submit: the last submit's, before Present. */
static void ldr_wait_submitted(unsigned long long v)
{
    if (g_ldr_mode != 2 || !g_ldr_thread) return;
    LDR_WAIT(__atomic_load_n(&g_ldr_submitted, __ATOMIC_SEQ_CST) >= v);
}
static void ldr_wait_last_submit(void) { ldr_wait_submitted(g_ldr_last_v); }
static int ldr_failed(HRESULT* hr, const char** what)
{
    if (!__atomic_load_n(&g_ldr_fail, __ATOMIC_ACQUIRE)) return 0;
    *hr = g_ldr_fail_hr; *what = g_ldr_fail_what;
    return 1;
}
static void ldr_shutdown(void)
{
    if (!g_ldr_thread) return;
    ldr_drain();
    __atomic_store_n(&g_ldr_stop, 1, __ATOMIC_SEQ_CST);
    SetEvent(g_ldr_cev);
    WaitForSingleObject(g_ldr_thread, 2000);
    CloseHandle(g_ldr_thread); g_ldr_thread = NULL;
    g_ldr_mode = 0; g_ldr_flags = g_ldr_trace << 1;      /* anything later: direct */
}
static void ldr_tick(void)
{
    if ((++g_ldr_calls & 4095u) != 0) return;
    extern unsigned long long ps3_qpc_us(void);
    static unsigned long long t0, c0, w0, wu0;
    const unsigned long long now = ps3_qpc_us();
    if (!t0) { t0 = now; c0 = g_ldr_calls; w0 = g_ldr_pwaits; wu0 = g_ldr_pwait_us; }
    if (now - t0 < 5000000ull) return;
    fprintf(stderr, "[ld-rec] per 5s: %llu calls (mode %d, trace %s), hash mismatches %llu total, "
                    "ring high-water %llu KB, producer waits %llu (%.1f ms)\n",
            g_ldr_calls - c0, g_ldr_mode, g_ldr_trace ? "on" : "off", g_ldr_mismatch,
            g_ldr_hiwater >> 10, g_ldr_pwaits - w0, (double)(g_ldr_pwait_us - wu0) / 1000.0);
    t0 = now; c0 = g_ldr_calls; w0 = g_ldr_pwaits; wu0 = g_ldr_pwait_us; g_ldr_hiwater = 0;
}
static void ldr_compare(int op, const char* name, unsigned long long seq, unsigned long long caller, unsigned long long d3d)
{
    if (caller == d3d) return;
    if (++g_ldr_mismatch <= 8)
        fprintf(stderr, "[ld-rec] MISMATCH %llu: op %d %s call %llu: caller hash %016llx, D3D12-side %016llx\n",
                g_ldr_mismatch, op, name, seq, caller, d3d);
}
'''


def gen_header():
    o = []
    P = o.append
    P("/* rsx_ld_recorder.h -- GENERATED by ascension/tools/gen_ld_recorder.py; edit the")
    P(" * generator. The D3D12 recorder of rsx_live_draw.c (my-changes/recorder/DESIGN.md):")
    P(" * every command-list call, LD_CL(M, ...), is ld_cl_<M>(list, ...).")
    P(" *   LD_RECORDER unset or 0: the direct call (one predictable branch).")
    P(" *   LD_RECORDER=1: encoded, decoded and executed at once on the same thread.")
    P(" *   LD_RECORDER=2: encoded into a ring; a recorder thread executes it")
    P(" *     (ldr_submit / ldr_drain / ldr_failed / ldr_shutdown in rsx_live_draw.c).")
    P(" *   LD_RECORDER_TRACE=1: caller-side vs D3D12-side hash of every call; [ld-rec]")
    P(" *     line every 5 s with the mismatch total, the first eight named.")
    P(" * Include it once, after <windows.h> and <d3d12.h>. */")
    P("#ifndef RSX_LD_RECORDER_H")
    P("#define RSX_LD_RECORDER_H")
    P("#include <string.h>")
    P("#include <stdio.h>")
    P("#include <stdlib.h>")
    P("#include <malloc.h>")
    P("#include <immintrin.h>")
    o.append(RUNTIME)
    # per method: hash, encode
    for i, (name, ret, params) in enumerate(M):
        pl = plist(params)
        P(f"static inline unsigned long long ldr_hash_{name}({pl if pl else 'void'})")
        P("{")
        P(f"    ldr_hash_t s = {{ 1469598103934665603ull ^ {i}u }};")
        for k, t, n, x in params:
            if k == "V":
                P(f"    ldr_h(&s, &{n}, sizeof {n});")
            elif k == "A":
                P(f"    {{ const unsigned int c = {x}, nul = {n} == NULL; ldr_h(&s, &c, 4); ldr_h(&s, &nul, 4);")
                P(f"      if (!nul) ldr_h(&s, {n}, (size_t)c * sizeof *{n}); }}")
            elif k == "O":
                P(f"    {{ const unsigned int nul = {n} == NULL; ldr_h(&s, &nul, 4); if (!nul) ldr_h(&s, {n}, sizeof *{n}); }}")
            elif k == "F":
                P(f"    ldr_h(&s, {n}, (size_t)({x}) * sizeof *{n});")
        P("    return s.h;")
        P("}")
        P(f"static inline void ldr_enc_{name}(ldr_w_t* w{', ' + pl if pl else ''})")
        P("{")
        if not params:
            P("    (void)w;")
        for k, t, n, x in params:
            if k == "V":
                P(f"    ldr_put(w, &{n}, sizeof {n});")
            elif k == "A":
                P(f"    {{ const unsigned int c = {x}, nul = {n} == NULL; ldr_put(w, &c, 4); ldr_put(w, &nul, 4);")
                P(f"      if (!nul) ldr_put(w, {n}, (size_t)c * sizeof *{n}); }}")
            elif k == "O":
                P(f"    {{ const unsigned int nul = {n} == NULL; ldr_put(w, &nul, 4); if (!nul) ldr_put(w, {n}, sizeof *{n}); }}")
            elif k == "F":
                P(f"    ldr_put(w, {n}, (size_t)({x}) * sizeof *{n});")
        P("}")
    # exec
    P("/* Decode one record, hash what was decoded (trace), make the call. */")
    P("static HRESULT ldr_exec(const unsigned char* rec)")
    P("{")
    P("    const ldr_hdr_t* hd = (const ldr_hdr_t*)rec;")
    P("    ID3D12GraphicsCommandList* L = (ID3D12GraphicsCommandList*)hd->list;")
    P("    const unsigned long long caller = hd->hash, seq = hd->seq;")
    P("    ldr_r_t r = { rec + LDR_HDR };")
    P("    HRESULT hr = S_OK;")
    P("    (void)seq; (void)caller;")
    P("    switch (hd->op) {")
    for i, (name, ret, params) in enumerate(M):
        P(f"    case {i}: {{")
        for k, t, n, x in params:
            if k == "V":
                P(f"        {t} {n}; memcpy(&{n}, ldr_get(&r, sizeof {n}), sizeof {n});")
            elif k == "A":
                P(f"        unsigned int {n}_c, {n}_nul; memcpy(&{n}_c, ldr_get(&r, 4), 4); memcpy(&{n}_nul, ldr_get(&r, 4), 4);")
                P(f"        {ptype(k, t)} {n} = {n}_nul ? NULL : ({ptype(k, t)})ldr_get(&r, (size_t){n}_c * sizeof({t}));")
            elif k == "O":
                P(f"        unsigned int {n}_nul; memcpy(&{n}_nul, ldr_get(&r, 4), 4);")
                P(f"        {ptype(k, t)} {n} = {n}_nul ? NULL : ({ptype(k, t)})ldr_get(&r, sizeof({t}));")
            elif k == "F":
                P(f"        {ptype(k, t)} {n} = ({ptype(k, t)})ldr_get(&r, (size_t)({x}) * sizeof({t}));")
        args = alist(params)
        for k, t, n, x in params:
            if k == "A":
                P(f"        if ({n}_c != (unsigned int)({x})) ldr_compare({i}, \"{name} (length)\", seq, 0, 1);")
        P(f"        if (g_ldr_trace) ldr_compare({i}, \"{name}\", seq, caller, ldr_hash_{name}({args}));")
        c = call_expr(name, params)
        if ret == "HRESULT":
            P(f"        hr = {c};")
        else:
            P(f"        {c}{'' if c.endswith('}') else ';'}")
        P("        break; }")
    P("    default:")
    P("        fprintf(stderr, \"[ld-rec] bad record op %u\\n\", hd->op);")
    P("        break;")
    P("    }")
    P("    return hr;")
    P("}")
    P("")
    # slow paths and inline entry points
    for i, (name, ret, params) in enumerate(M):
        pl = plist(params)
        args = alist(params)
        sig = "ID3D12GraphicsCommandList* L" + (", " + pl if pl else "")
        direct = call_expr(name, params)
        is_block = direct.endswith("}")
        P(f"static {ret} ldr_slow_{name}({sig})")
        P("{")
        P("    if (g_ldr_flags < 0) ldr_init();")
        if ret == "HRESULT":
            P(f"    if (!g_ldr_flags) return {direct};")
        else:
            P(f"    if (!g_ldr_flags) {{ {direct}{'' if is_block else ';'} return; }}")
        P("    const unsigned long long seq = ++g_ldr_seq; ldr_tick();")
        P(f"    const unsigned long long h = g_ldr_trace ? ldr_hash_{name}({args}) : 0;")
        P("    if (!g_ldr_mode) {                       /* trace only */")
        if ret == "HRESULT":
            P(f"        const HRESULT hr = {direct};")
            P(f"        ldr_compare({i}, \"{name}\", seq, h, ldr_hash_{name}({args}));")
            P("        return hr;")
        else:
            P(f"        {direct}{'' if is_block else ';'}")
            P(f"        ldr_compare({i}, \"{name}\", seq, h, ldr_hash_{name}({args}));")
            P("        return;")
        P("    }")
        P("    ldr_w_t w = { g_ldr_tmp + LDR_HDR };")
        P(f"    ldr_enc_{name}(&w{', ' + args if args else ''});")
        P("    const unsigned size = LDR_ALIGN((unsigned)(w.p - g_ldr_tmp));")
        P("    if (size > sizeof g_ldr_tmp) { fprintf(stderr, \"[ld-rec] record overflow\\n\"); abort(); }")
        P(f"    ldr_hdr_t* hd = (ldr_hdr_t*)g_ldr_tmp; hd->size = size; hd->op = {i}; hd->hash = h; hd->list = L; hd->seq = seq;")
        if ret == "HRESULT":
            P("    if (g_ldr_mode == 2) { ldr_push(g_ldr_tmp, size); return S_OK; }")
            P("    return ldr_exec(g_ldr_tmp);")
        else:
            P("    if (g_ldr_mode == 2) { ldr_push(g_ldr_tmp, size); return; }")
            P("    (void)ldr_exec(g_ldr_tmp);")
        P("}")
        P(f"static inline {ret} ld_cl_{name}({sig})")
        P("{")
        if ret == "HRESULT":
            P(f"    if (__builtin_expect(g_ldr_flags == 0, 1)) return {direct};")
            P(f"    return ldr_slow_{name}(L{', ' + args if args else ''});")
        else:
            P(f"    if (__builtin_expect(g_ldr_flags == 0, 1)) {{ {direct}{'' if is_block else ';'} return; }}")
            P(f"    ldr_slow_{name}(L{', ' + args if args else ''});")
        P("}")
    P("")
    P("#define LD_CL(M, ...) ld_cl_##M(g.list, __VA_ARGS__)")
    P("#define LD_CL0(M) ld_cl_##M(g.list)")
    P("#endif /* RSX_LD_RECORDER_H */")
    return "\n".join(o) + "\n"


def gen_test():
    o = []
    P = o.append
    P("/* test_ld_recorder.c -- GENERATED by ascension/tools/gen_ld_recorder.py.")
    P(" * Every method of rsx_ld_recorder.h on a fake command list (and fake list1,")
    P(" * queue) whose methods hash what they receive. Random arguments (arrays of")
    P(" * 0-8, NULL where the API allows it):")
    P(" *   modes 0 and 1: each call checked as it happens;")
    P(" *   mode 2: 300,000 calls through the ring and the recorder thread (it wraps")
    P(" *     many times), with submits; the received sequence must equal the sent")
    P(" *     one, in order, and every submit must have signalled its value.")
    P(" * Trace on throughout: no mismatch may be reported. */")
    P("#define WIN32_LEAN_AND_MEAN")
    P("#include <windows.h>")
    P("#include <initguid.h>")
    P("#include <d3d12.h>")
    P("#include <stdio.h>")
    P("unsigned long long ps3_qpc_us(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (unsigned long long)(c.QuadPart * 1000000 / f.QuadPart); }")
    P("#include \"rsx_ld_recorder.h\"")
    P("static unsigned long long got;")
    P("static int got_op = -1;")
    P("static unsigned long long* recv; static volatile long recv_n;   /* mode 2: what the recorder delivered */")
    P("static int slow;   /* LDR_TEST_SLOW=<n>: each fake call spins n times, so the ring fills */")
    P("static void note(int op, unsigned long long h) { got_op = op; got = h; if (recv) recv[recv_n++] = h; for (volatile int i = 0; i < slow; i++) {} }")
    for i, (name, ret, params) in enumerate(M):
        pl = plist(params)
        args = alist(params)
        this = "ID3D12GraphicsCommandList1" if name in LIST1 else "ID3D12GraphicsCommandList"
        P(f"static {'HRESULT' if ret == 'HRESULT' else 'void'} STDMETHODCALLTYPE fake_{name}({this}* This{', ' + pl if pl else ''})")
        P(f"{{ (void)This; note({i}, ldr_hash_{name}({args})); {'return S_OK;' if ret == 'HRESULT' else ''} }}")
    P("static ID3D12GraphicsCommandList1Vtbl vt1;")
    P("static ID3D12GraphicsCommandList1 fake1 = { &vt1 };")
    P("static HRESULT STDMETHODCALLTYPE fake_QI(ID3D12GraphicsCommandList* This, REFIID riid, void** ppv) { (void)This; (void)riid; *ppv = &fake1; return S_OK; }")
    P("static ULONG STDMETHODCALLTYPE fake_Release1(ID3D12GraphicsCommandList1* This) { (void)This; return 1; }")
    P("static unsigned long long signalled[4096]; static volatile long n_signalled, n_executed;")
    P("static void STDMETHODCALLTYPE fake_Exec(ID3D12CommandQueue* This, UINT n, ID3D12CommandList* const* l) { (void)This; (void)n; (void)l; n_executed++; }")
    P("static unsigned long long signal_fail_at;   /* a Signal(v) that fails, for the failure path */")
    P("static HRESULT STDMETHODCALLTYPE fake_Signal(ID3D12CommandQueue* This, ID3D12Fence* f, UINT64 v) { (void)This; (void)f; signalled[n_signalled++ & 4095] = v; return v == signal_fail_at ? E_FAIL : S_OK; }")
    P("static unsigned long long rng = 0x2545F4914F6CDD1Dull;")
    P("static unsigned int rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned int)(rng >> 16); }")
    P("static void fill(void* p, size_t n) { unsigned char* b = (unsigned char*)p; for (size_t i = 0; i < n; i++) b[i] = (unsigned char)rnd(); }")
    P("static ID3D12GraphicsCommandList* L;")
    P("static int fails;")
    P("/* One random call of a random method; returns what the fake must receive. */")
    P("static unsigned long long one_call(int check_now, int mode)")
    P("{")
    P("    unsigned long long want = 0;")
    P(f"    switch (rnd() % {len(M)}) {{")
    for i, (name, ret, params) in enumerate(M):
        P(f"    case {i}: {{")
        for k, t, n, x in params:
            if k == "V":
                P(f"        {t} {n}; fill(&{n}, sizeof {n});")
        for k, t, n, x in params:
            if k == "V" and n.startswith("Num"):
                P(f"        {n} = rnd() % 9;")
            if k == "V" and n == "RTsSingleHandleToDescriptorRange":
                P(f"        {n} = rnd() & 1;")
        for k, t, n, x in params:
            if k == "A":
                P(f"        {t} {n}_buf[8]; fill({n}_buf, sizeof {n}_buf);")
                P(f"        {ptype(k, t)} {n} = ((unsigned int)({x}) == 0 && (rnd() & 1)) ? NULL : {n}_buf;")
            elif k == "O":
                P(f"        {t} {n}_buf; fill(&{n}_buf, sizeof {n}_buf);")
                P(f"        {ptype(k, t)} {n} = (rnd() & 1) ? NULL : &{n}_buf;")
            elif k == "F":
                P(f"        {t} {n}_buf[{x}]; fill({n}_buf, sizeof {n}_buf);")
                P(f"        {ptype(k, t)} {n} = {n}_buf;")
        args = alist(params)
        P(f"        want = ldr_hash_{name}({args});")
        if name == "Close":
            P("        if (mode == 2) return 0;   /* mode 2 closes in ldr_submit */")
        P("        got_op = -1;")
        P(f"        (void)ld_cl_{name}(L{', ' + args if args else ''});")
        P(f"        if (check_now && (got_op != {i} || got != want)) {{ fails++; if (fails < 10) printf(\"FAIL {name} mode %d\\n\", mode); }}")
        P("        break; }")
    P("    }")
    P("    return want;")
    P("}")
    P("int main(void)")
    P("{")
    P("    static ID3D12GraphicsCommandListVtbl vt;")
    for name, ret, params in M:
        if name in LIST1:
            P(f"    vt1.{name} = fake_{name};")
        else:
            P(f"    vt.{name} = fake_{name};")
    P("    vt.QueryInterface = fake_QI; vt1.Release = fake_Release1;")
    P("    static ID3D12GraphicsCommandList fake = { &vt };")
    P("    L = &fake;")
    P("    { const char* e = getenv(\"LDR_TEST_SLOW\"); slow = e ? atoi(e) : 0; }")
    P("    for (int round = 0; round < 20000; round++) {")
    P("        const int mode = round & 1;")
    P("        ldr_set_mode(mode, 1);")
    P("        (void)one_call(1, mode);")
    P("    }")
    P("    /* mode 2 */")
    P("    static ID3D12CommandQueueVtbl qvt; qvt.ExecuteCommandLists = fake_Exec; qvt.Signal = fake_Signal;")
    P("    static ID3D12CommandQueue fq = { &qvt };")
    P("    enum { N = 300000 };")
    P("    /* each submit closes the list on the recorder: the fake Close sees it too */")
    P("    static unsigned long long sent[N + N / 1024 + 2]; long n_sent = 0;")
    P("    recv = (unsigned long long*)malloc(sizeof(unsigned long long) * (N + N / 1024 + 2)); recv_n = 0;")
    P("    ldr_set_mode(2, 1);")
    P("    unsigned long long v = 0;")
    P("    for (long k = 0; k < N; k++) {")
    P("        const unsigned long long want = one_call(0, 2);")
    P("        if (want) sent[n_sent++] = want;")
    P("        if ((k & 1023) == 1023) { ldr_submit(L, &fq, (ID3D12Fence*)0, ++v); sent[n_sent++] = ldr_hash_Close(); }")
    P("    }")
    P("    ldr_submit(L, &fq, (ID3D12Fence*)0, ++v); sent[n_sent++] = ldr_hash_Close();")
    P("    ldr_wait_submitted(v);")
    P("    if (g_ldr_submitted < v) { printf(\"FAIL mode 2: ldr_wait_submitted(%llu) returned at %llu\\n\", v, g_ldr_submitted); fails++; }")
    P("    ldr_drain();")
    P("    long bad = 0;")
    P("    if (recv_n != n_sent) { printf(\"FAIL mode 2: sent %ld calls, recorder delivered %ld\\n\", n_sent, (long)recv_n); fails++; }")
    P("    for (long k = 0; k < n_sent && k < recv_n; k++) if (sent[k] != recv[k]) bad++;")
    P("    if (bad) { printf(\"FAIL mode 2: %ld calls out of order or changed\\n\", bad); fails++; }")
    P("    if (g_ldr_submitted != v || (unsigned long long)n_signalled != v) { printf(\"FAIL mode 2: submitted %llu signalled %ld want %llu\\n\", g_ldr_submitted, (long)n_signalled, v); fails++; }")
    P("    { HRESULT hr; const char* what;   /* no failure so far; then a failing Signal is reported */")
    P("      if (ldr_failed(&hr, &what)) { printf(\"FAIL mode 2: failure reported without one\\n\"); fails++; }")
    P("      signal_fail_at = ++v; ldr_submit(L, &fq, (ID3D12Fence*)0, v); sent[n_sent++] = ldr_hash_Close();")
    P("      ldr_wait_last_submit();")
    P("      if (!ldr_failed(&hr, &what) || hr != E_FAIL || strcmp(what, \"Signal\") != 0) { printf(\"FAIL mode 2: the failing Signal was not reported\\n\"); fails++; } }")
    P("    ldr_shutdown();")
    P("    if (g_ldr_mode != 0 || g_ldr_thread) { printf(\"FAIL mode 2: not direct after shutdown\\n\"); fails++; }")
    P("    printf(\"mode 2: %ld calls, %llu submits, ring high-water %llu KB, producer waits %llu\\n\", n_sent, v, g_ldr_hiwater >> 10, g_ldr_pwaits);")
    P("    printf(\"%s: %d failures, %llu trace mismatches\\n\", fails || g_ldr_mismatch ? \"FAIL\" : \"PASS\", fails, g_ldr_mismatch);")
    P("    return fails || g_ldr_mismatch;")
    P("}")
    return "\n".join(o) + "\n"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    open(sys.argv[1], "w", newline="\r\n").write(gen_header())
    if len(sys.argv) > 2:
        open(sys.argv[2], "w", newline="\r\n").write(gen_test())
    print("methods:", len(M))


if __name__ == "__main__":
    main()
