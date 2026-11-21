/* LD_AO (2026-11-15; default off, LD_AO=1: on): screen-space ambient occlusion, the soft contact shadows in
 * corners, under objects and where Kratos meets the floor that the PS3 game never had.
 *
 * Where: God of War: Ascension lays its scene's depth down first and then shades it with depth test EQUAL
 * (see LD_HOLE_FILL). When that colour pass on the scene-sized target ends (the next draw goes to another
 * target), the depth holds the finished opaque scene and nothing of the glow, the transparent effects or
 * the HUD has been drawn yet. There, once a frame:
 *   1. the depth (zdepth_snapshot, R32_FLOAT window z) is turned into view-space positions with the scene
 *      camera of the first EQUAL draw (clip = M * world in vertex constants 264..267, as FSR3 reads it) and
 *      an occlusion term is computed at half the host resolution (12 samples on a spiral, SAO-style);
 *   2. a depth-aware 3x3 blur at full resolution multiplies the scene target by it (blend: dst * src).
 * Switches: LD_AO_RADIUS=<world units> (default 0.6), LD_AO_STRENGTH=<0..300 %> (default 100),
 * LD_AO_SHOW=1 (test only: the scene target shows the occlusion alone, white = none). */

static struct {
    int on;                          /* -1: not read yet */
    float radius, strength;
    int show;
    u32 armed_target, armed_zslot;   /* the scene target whose EQUAL pass is running (0 = none) + 1 */
    u32 done_frame;
    float cam[8];                    /* 1/n, 1/f - 1/n, tan half fov x, y, radius, strength, show, 0 */
    ID3DBlob* vs_ps[2];              /* [0] occlusion, [1] blur+apply; NULL not made, (void*)1 failed */
    ID3D12PipelineState* occ_pso;
    DXGI_FORMAT fmt[LD_PP_FMTS];
    ID3D12PipelineState* apply_pso[LD_PP_FMTS];
    u32 n_apply;
    u64 passes, no_cam;
} s_ao = { -1 };

#define SRV_AO_DEPTH (SRV_PP_BASE + LD_PP_N + 2)
typedef char ld_ao_slot_fits[(LD_PP_N + 2 < 20) ? 1 : -1];   /* inside SRV_CPU_HEAP_SLOTS */

static int ld_ao_on(void)
{
    if (s_ao.on < 0) {
        const char* e = getenv("LD_AO");
        s_ao.on = e && e[0] == '1';
        /* defaults from the rooftop A/B (2026-11-15): 0.6 / 100 % was hardly visible at the game's camera distances */
        e = getenv("LD_AO_RADIUS");   s_ao.radius = e && atof(e) > 0.0 ? (float)atof(e) : 1.5f;
        e = getenv("LD_AO_STRENGTH"); s_ao.strength = e ? (float)atof(e) / 100.0f : 2.0f;
        if (s_ao.strength < 0.0f) s_ao.strength = 0.0f;
        if (s_ao.strength > 3.0f) s_ao.strength = 3.0f;
        e = getenv("LD_AO_SHOW");     s_ao.show = e && e[0] == '1';
        if (s_ao.on) fprintf(stderr, "[ao] ambient occlusion on: radius %.2f, strength %.0f%%%s%c", s_ao.radius,
                             s_ao.strength * 100.0f, s_ao.show ? ", SHOW (test)" : "", 10);
    }
    return s_ao.on;
}

static const char s_ao_src[] =
    "Texture2D depth : register(t0);\n"
    "Texture2D occ : register(t1);\n"
    "SamplerState point_clamp : register(s0);\n"
    "SamplerState linear_clamp : register(s1);\n"
    "cbuffer PP : register(b0) { float4 pp_uv; float4 cam; float4 opt; };\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "    V o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "    o.p = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    o.uv = pp_uv.xy + t * pp_uv.zw;\n"
    "    return o;\n"
    "}\n"
    "float lin(float z) { return 1.0 / max(cam.x + z * cam.y, 1e-7); }\n"
    "float2 su(float2 uv) { return uv / pp_uv.zw; }\n"   /* depth-copy uv -> scene uv */
    "float3 vpos(float2 uv, float z) { float l = lin(z); return float3((su(uv) * 2.0 - 1.0) * cam.zw * l, l); }\n"
    "float3 vat(float2 uv) { return vpos(uv, depth.SampleLevel(point_clamp, uv, 0.0).r); }\n"
    "float4 occlusion(V i) : SV_Target {\n"
    "    uint w, h; depth.GetDimensions(w, h); float2 px = 1.0 / float2(w, h);\n"
    "    float z = depth.SampleLevel(point_clamp, i.uv, 0.0).r;\n"
    "    if (z >= 0.99999) return 1.0;\n"
    "    float3 p = vpos(i.uv, z);\n"
    "    float3 dx1 = vat(i.uv + float2(px.x, 0)) - p, dx0 = p - vat(i.uv - float2(px.x, 0));\n"
    "    float3 dy1 = vat(i.uv + float2(0, px.y)) - p, dy0 = p - vat(i.uv - float2(0, px.y));\n"
    "    float3 ddxp = abs(dx1.z) < abs(dx0.z) ? dx1 : dx0, ddyp = abs(dy1.z) < abs(dy0.z) ? dy1 : dy0;\n"
    "    float3 n = normalize(cross(ddyp, ddxp)); if (dot(n, p) > 0) n = -n;\n"
    "    float r = opt.x;\n"
    "    float2 rs = r / (p.z * cam.zw) * 0.5 * pp_uv.zw;\n"     /* the radius in uv units at this depth */
    "    if (max(rs.x, rs.y) < 2.0 * px.x) return 1.0;\n"
    "    float a = frac(sin(dot(i.p.xy, float2(12.9898, 78.233))) * 43758.5453) * 6.2831853;\n"
    "    float sum = 0.0;\n"
    "    [unroll] for (int k = 0; k < 12; k++) {\n"
    "        float t = (k + 0.5) / 12.0; float ang = a + k * 2.3999632;\n"
    "        float2 o = float2(cos(ang), sin(ang)) * t * rs;\n"
    "        float3 q = vat(i.uv + o) - p;\n"
    "        float qq = dot(q, q), qn = dot(q, n);\n"
    "        float fall = saturate(1.0 - qq / (r * r));\n"
    "        sum += fall * max(qn - 0.01 * p.z, 0.0) / (qq + 0.001 * r * r);\n"
    "    }\n"
    "    float ao = saturate(1.0 - opt.y * 2.0 * sum / 12.0);\n"
    "    return ao;\n"
    "}\n"
    "float4 apply(V i) : SV_Target {\n"
    "    uint w, h; occ.GetDimensions(w, h); float2 ps = pp_uv.zw / float2(w, h);\n"
    "    float lc = lin(depth.SampleLevel(point_clamp, i.uv, 0.0).r);\n"
    "    float s = 0.0, ws = 0.0;\n"
    "    [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {\n"
    "        float2 u = i.uv + float2(x, y) * ps;\n"
    "        float l = lin(depth.SampleLevel(point_clamp, u, 0.0).r);\n"
    "        float wt = exp(-abs(l - lc) / (0.03 * lc + 1e-4));\n"
    "        s += occ.SampleLevel(linear_clamp, su(u), 0.0).r * wt; ws += wt;\n"
    "    }\n"
    "    float ao = ws > 0 ? s / ws : 1.0;\n"
    "    if (opt.z > 0.5) return float4(ao.xxx, 0.0);\n"   /* SHOW: with the blend below, replaces rgb */
    "    return float4(ao.xxx, 1.0);\n"
    "}\n";

/* blend: rgb = dst * src (src ZERO, dst SRC_COLOR); SHOW: rgb = src (ONE, ZERO); alpha kept. */
static ID3D12PipelineState* ld_ao_pipeline(ID3DBlob* ps, DXGI_FORMAT fmt, int mode)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_pp.rs;
    pd.VS.pShaderBytecode = s_ao.vs_ps[0]->lpVtbl->GetBufferPointer(s_ao.vs_ps[0]);
    pd.VS.BytecodeLength = s_ao.vs_ps[0]->lpVtbl->GetBufferSize(s_ao.vs_ps[0]);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (mode) {
        D3D12_RENDER_TARGET_BLEND_DESC* b = &pd.BlendState.RenderTarget[0];
        b->BlendEnable = TRUE;
        b->SrcBlend = mode == 2 ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
        b->DestBlend = mode == 2 ? D3D12_BLEND_ZERO : D3D12_BLEND_SRC_COLOR;
        b->BlendOp = D3D12_BLEND_OP_ADD;
        b->SrcBlendAlpha = D3D12_BLEND_ZERO; b->DestBlendAlpha = D3D12_BLEND_ONE; b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b->LogicOp = D3D12_LOGIC_OP_NOOP;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED |
            D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    }
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    if (FAILED(g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso)))
        pso = NULL;
    return pso;
}

static ID3DBlob* s_ao_vsblob;
static int ld_ao_shaders(void)
{
    if (s_ao.vs_ps[1] == (ID3DBlob*)1) return 0;
    if (s_ao.vs_ps[1]) return 1;
    s_ao.vs_ps[1] = (ID3DBlob*)1;
    ID3DBlob* vs = ld_pp_compile(s_ao_src, sizeof s_ao_src - 1u, "pp_ao", "vs", "vs_5_0");
    ID3DBlob* o = vs ? ld_pp_compile(s_ao_src, sizeof s_ao_src - 1u, "pp_ao", "occlusion", "ps_5_0") : NULL;
    ID3DBlob* a = o ? ld_pp_compile(s_ao_src, sizeof s_ao_src - 1u, "pp_ao", "apply", "ps_5_0") : NULL;
    if (!a) { fprintf(stderr, "[ao] shaders failed: off%c", 10); s_ao.on = 0; return 0; }
    s_ao.vs_ps[0] = vs;   /* the vertex shader (ld_ao_pipeline reads it from here) */
    s_ao_vsblob = o;      /* the occlusion pixel shader */
    s_ao.vs_ps[1] = a;    /* the apply pixel shader */
    return 1;
}

/* The scene camera of this draw: 0 when the constants are not the scene's. */
static int ld_ao_camera(const rsx_dsp_viewport* vp)
{
    const ld_vp_memo_t* m = g_ld_cur_vp_memo;
    if (m && ((m->cb_mask[264u >> 5] >> (264u & 31u)) & 0xFu) != 0xFu) return 0;
    if (vp->scale[2] == 0.0f) return 0;
    double M[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) { float f; memcpy(&f, &g.rsx.constants[264 + c][r], 4); M[r * 4 + c] = f; }
    const double *r0 = M, *r1 = M + 4, *r2 = M + 8, *r3 = M + 12;
    double c[3];   /* the eye: r0, r1, r3 . (x, 1) = 0 */
    { const double a[3] = {r0[0], r0[1], r0[2]}, b[3] = {r1[0], r1[1], r1[2]}, d[3] = {r3[0], r3[1], r3[2]};
      const double bxd[3] = {b[1]*d[2]-b[2]*d[1], b[2]*d[0]-b[0]*d[2], b[0]*d[1]-b[1]*d[0]};
      const double dxa[3] = {d[1]*a[2]-d[2]*a[1], d[2]*a[0]-d[0]*a[2], d[0]*a[1]-d[1]*a[0]};
      const double axb[3] = {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
      const double det = a[0]*bxd[0] + a[1]*bxd[1] + a[2]*bxd[2];
      if (det == 0.0) return 0;
      for (int k = 0; k < 3; k++) c[k] = -(r0[3]*bxd[k] + r1[3]*dxa[k] + r3[3]*axb[k]) / det; }
    const double f[3] = {r3[0], r3[1], r3[2]};
    const double fl = sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (!(fl > 0.0)) return 0;
    double ul = 0, rl = 0;
    { double u[3], ri[3], ku = 0, kr = 0;
      for (int k = 0; k < 3; k++) { ku += r1[k] * f[k] / fl; kr += r0[k] * f[k] / fl; }
      for (int k = 0; k < 3; k++) { u[k] = r1[k] - ku * f[k] / fl; ri[k] = r0[k] - kr * f[k] / fl; }
      ul = sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]); rl = sqrt(ri[0]*ri[0] + ri[1]*ri[1] + ri[2]*ri[2]); }
    if (!(ul > 0.0 && rl > 0.0)) return 0;
    double nf[2];
    for (int k = 0; k < 2; k++) {
        const double v = (double)k, s2 = vp->scale[2], t2 = vp->translate[2];
        const double a = r2[0]*c[0] + r2[1]*c[1] + r2[2]*c[2] + r2[3], b = (r2[0]*f[0] + r2[1]*f[1] + r2[2]*f[2]) / fl;
        const double cc = r3[0]*c[0] + r3[1]*c[1] + r3[2]*c[2] + r3[3];
        const double den = s2 * b - (v - t2) * fl;
        nf[k] = den != 0.0 ? ((v - t2) * cc - s2 * a) / den : 0.0;
    }
    if (!(nf[0] > 1e-5 && nf[1] > nf[0] && nf[1] < 1e8)) return 0;
    s_ao.cam[0] = (float)(1.0 / nf[0]);
    s_ao.cam[1] = (float)(1.0 / nf[1] - 1.0 / nf[0]);
    s_ao.cam[2] = (float)(fl / rl);   /* tan of half the horizontal field of view */
    s_ao.cam[3] = (float)(fl / ul);
    { static int logs; if (logs++ < 3) fprintf(stderr, "[ao] scene camera: near %.4g far %.4g, tan half fov %.3f x %.3f%c",
                                               nf[0], nf[1], s_ao.cam[2], s_ao.cam[3], 10); }
    return 1;
}

/* The armed scene pass is over: occlusion, then the scene target times it. */
static void ld_ao_apply(void)
{
    const u32 target = s_ao.armed_target - 1u, zslot = s_ao.armed_zslot;
    s_ao.armed_target = 0;
    if (target >= g.n_surfaces || !zslot || !ld_pp_init() || !ld_ao_shaders()) return;
    surface_t* s = &g.surfaces[target];
    zdepth_t* z = &g.zdepths[zslot - 1];
    if (!s->tex || !zdepth_snapshot(zslot) || !z->snapshot) return;
    D3D12_RESOURCE_DESC sd, zd;
    s->tex->lpVtbl->GetDesc(s->tex, &sd);
    z->snapshot->lpVtbl->GetDesc(z->snapshot, &zd);
    const u32 W = (u32)sd.Width, H = sd.Height;
    if (zd.Format != DXGI_FORMAT_R32_FLOAT || (u32)zd.Width < W || zd.Height < H) return;
    const u32 hw = (W + 1u) / 2u, hh = (H + 1u) / 2u;
    if (!ld_pp_target(LD_PP_AO, hw, hh, DXGI_FORMAT_R8_UNORM)) return;
    if (!s_ao.occ_pso) {
        s_ao.occ_pso = ld_ao_pipeline(s_ao_vsblob, DXGI_FORMAT_R8_UNORM, 0);
        if (!s_ao.occ_pso) { s_ao.on = 0; fprintf(stderr, "[ao] pipeline failed: off%c", 10); return; }
    }
    ID3D12PipelineState* ap = NULL;
    for (u32 k = 0; k < s_ao.n_apply; k++) if (s_ao.fmt[k] == sd.Format) ap = s_ao.apply_pso[k];
    if (!ap) {
        if (s_ao.n_apply >= LD_PP_FMTS) return;
        ap = ld_ao_pipeline(s_ao.vs_ps[1], sd.Format, s_ao.show ? 2 : 1);
        if (!ap) return;
        s_ao.fmt[s_ao.n_apply] = sd.Format; s_ao.apply_pso[s_ao.n_apply++] = ap;
    }
    /* the scene covers W x H of the (possibly larger) depth copy */
    const float c[12] = { 0.0f, 0.0f, (float)W / (float)zd.Width, (float)H / (float)zd.Height,
                          s_ao.cam[0], s_ao.cam[1], s_ao.cam[2], s_ao.cam[3],
                          s_ao.radius, s_ao.strength, (float)s_ao.show, 0.0f };
    D3D12_GPU_VIRTUAL_ADDRESS va;
    if (!ld_pp_room(c, 3, 2, 0, &va)) return;
    srv_write(SRV_AO_DEPTH, z->snapshot);
    const D3D12_GPU_DESCRIPTOR_HANDLE t1 = ld_pp_table(SRV_AO_DEPTH, SRV_WHITE, SRV_WHITE);
    const D3D12_GPU_DESCRIPTOR_HANDLE t2 = ld_pp_table(SRV_AO_DEPTH, SRV_PP_BASE + LD_PP_AO, SRV_WHITE);
    if (!t1.ptr || !t2.ptr) return;
    const D3D12_RESOURCE_STATES zs = z->snapshot_state;
    ID3D12Resource* ao = s_pp.t[LD_PP_AO].res;
    ld_pp_barrier(z->snapshot, zs, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ld_pp_barrier(ao, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ld_pp_draw(s_ao.occ_pso, t1, va, rtv_handle(LD_RTV_PP + LD_PP_AO), 0, 0, hw, hh);
    ld_pp_barrier(ao, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ld_pp_draw(ap, t2, va, rtv_handle(LD_SWAP_BUFFERS + target), 0, 0, W, H);
    ld_pp_barrier(z->snapshot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, zs);
    s_ao.passes++;
    { static int logs; if (logs++ < 2) fprintf(stderr, "[ao] applied to 0x%X (%ux%u, occlusion %ux%u) at frame %u%c",
                                               s->offset, W, H, hw, hh, g_ld_frames, 10); }
}

/* Each draw, before anything of it is recorded (beside ld_hole_draw). */
static void ld_ao_draw(u32 target, u32 zslot, const rsx_dsp_viewport* vp)
{
    if (!ld_ao_on()) return;
    if (s_ao.armed_target && target != s_ao.armed_target - 1u) ld_ao_apply();
    if (s_ao.armed_target || s_ao.done_frame == g_ld_frames + 1u || !zslot || target >= g.n_surfaces) return;
    if (!(rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u) || (rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1u) ||
        rsx_dsp_reg(&g.rsx, M_DEPTH_FUNC) != 0x0202u || !rsx_dsp_reg(&g.rsx, M_COLOR_MASK))
        return;
    const surface_t* t = &g.surfaces[target];
    if (t->w != g.width || t->h != g.height) return;            /* the scene's own size */
    if (!ld_ao_camera(vp)) { s_ao.no_cam++; return; }
    s_ao.armed_target = target + 1u;
    s_ao.armed_zslot = zslot;
    s_ao.done_frame = g_ld_frames + 1u;                         /* once a frame */
}
