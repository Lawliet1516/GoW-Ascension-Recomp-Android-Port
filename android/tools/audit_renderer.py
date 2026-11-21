#!/usr/bin/env python3
"""Reproduces the numbers in android/docs/RENDERER_AUDIT.md from the source tree (no game, no GPU needed).

  python3 android/tools/audit_renderer.py [--ledger android/docs/ld_switch_ledger.csv]

Everything here is a static scan: it counts and classifies text. The classification of a function as "D3D12-bound" means its body
mentions a D3D12/DXGI/D3DCompile symbol or the engine's device/command-list globals; the percentages are therefore an
upper bound on how much code must be rewritten (a D3D12-bound function usually also contains portable logic)."""
import argparse, collections, csv, os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
VID = os.path.join(ROOT, 'ps3kit', 'libs', 'video')

def read(p): return open(os.path.join(VID, p), errors='replace').read()

def sizes():
    print("== file sizes (lines / bytes)")
    for f in ['rsx_live_draw.c', 'rsx_d3d12_backend.c', 'rsx_draw_engine.c', 'rsx_vulkan_backend.c', 'rsx_metal_backend.m', 'rsx_fp_decompiler.c',
              'rsx_vp_decompiler.c', 'rsx_fp_uber.c', 'rsx_fsr3.h', 'rsx_dispatch.c', 'cellGcmSys.c']:
        p = os.path.join(VID, f)
        if os.path.exists(p): print(f"  {f:24} {sum(1 for _ in open(p, errors='replace')):7d} lines {os.path.getsize(p):9d} bytes")

FUNC = re.compile(r'^[a-zA-Z_][a-zA-Z0-9_ \*]* \**([a-zA-Z_][a-zA-Z0-9_]*)\(.*\)\s*(\{)?\s*$')
D3D = re.compile(r'ID3D12|D3D12_|IDXGI|DXGI_|D3DCompile|\bg\.dev\b|\bg\.cl\b|\bg\.q\b|->lpVtbl|CreateEvent|WaitForSingleObject')
WIN = re.compile(r'GetTickCount|QueryPerformance|CreateThread|\bHANDLE\b|Interlocked|_mm_|Sleep\(|GetModuleHandle|SetThreadPriority|GetCurrentThreadId')

def classify():
    lines = read('rsx_live_draw.c').split('\n')
    starts = []
    for i, l in enumerate(lines):
        if l and not l[0].isspace() and l[0] not in '#/*}' and not l.rstrip().endswith(';') and FUNC.match(l):
            starts.append((i, FUNC.match(l).group(1)))
    st = collections.Counter(); nf = collections.Counter()
    for k, (i, name) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else len(lines)
        body = '\n'.join(lines[i:end])
        c = 'D3D12-bound' if D3D.search(body) else ('Windows-API only' if WIN.search(body) else 'portable')
        st[c] += end - i; nf[c] += 1
    tot = sum(st.values())
    print(f"== rsx_live_draw.c: {len(starts)} functions, {tot} of {len(lines)} lines inside function bodies")
    for c in st: print(f"  {c:17} {nf[c]:4d} functions {st[c]:6d} lines ({st[c] * 100 / tot:.0f}%)")

def api():
    src = read('rsx_live_draw.c') + read('rsx_d3d12_backend.c')
    names = ('CreateCommittedResource CreatePlacedResource CreateHeap CreateGraphicsPipelineState CreateComputePipelineState CreateRootSignature '
             'CreatePipelineLibrary StorePipeline LoadGraphicsPipeline ResourceBarrier CopyTextureRegion CopyBufferRegion CopyResource ClearRenderTargetView '
             'ClearDepthStencilView OMSetRenderTargets DrawInstanced DrawIndexedInstanced ExecuteCommandLists Signal SetEventOnCompletion GetCompletedValue '
             'SetDescriptorHeaps SetGraphicsRootDescriptorTable SetGraphicsRootConstantBufferView SetGraphicsRoot32BitConstants CreateShaderResourceView '
             'CreateRenderTargetView CreateDepthStencilView CreateUnorderedAccessView CreateSampler BeginQuery EndQuery ResolveQueryData CreateQueryHeap '
             'OMSetDepthBounds OMSetStencilRef Present Map Unmap CreateSwapChainForHwnd D3DCompile CheckFeatureSupport').split()
    c = {n: len(re.findall(r'\b' + n + r'\(', src)) for n in names}
    print("== D3D12 entry points used (call sites in rsx_live_draw.c + rsx_d3d12_backend.c)")
    print('  ' + '  '.join(f"{n}={v}" for n, v in sorted(c.items(), key=lambda x: -x[1]) if v))
    fm = collections.Counter(re.findall(r'DXGI_FORMAT_[A-Z0-9_]+', src))
    print("== DXGI formats: " + '  '.join(f"{k[12:]}={v}" for k, v in fm.most_common(26)))
    print("== resource states referenced:", len(re.findall(r'D3D12_RESOURCE_STATE_', src)), "; heap types: DEFAULT",
          len(re.findall('HEAP_TYPE_DEFAULT', src)), "UPLOAD", len(re.findall('HEAP_TYPE_UPLOAD', src)), "READBACK", len(re.findall('HEAP_TYPE_READBACK', src)))

def ledger(path):
    src = read('rsx_live_draw.c').split('\n'); rows = {}
    for i, l in enumerate(src):
        for m in re.finditer(r'getenv\("([A-Z0-9_]+)"\)', l):
            n = m.group(1); ctx = ' '.join(src[i:i + 3]); d = '?'
            if re.search(r"!\(\s*\w+\s*&&\s*\w+\[0\]\s*==\s*'0'\s*\)", ctx) or re.search(r"\w+\s*=\s*!\(", ctx): d = 'ON (unless =0)'
            elif re.search(r"\w+\[0\]\s*==\s*'1'", ctx) or re.search(r"=\s*\w+\s*&&\s*\*\w+\s*==\s*'1'", ctx) or re.search(r"\w+\s*&&\s*\w+\[0\]\s*!=\s*'0'", ctx): d = 'off (needs =1)'
            elif re.search(r'atoi|strtol|atof|strtoul', ctx): d = 'numeric'
            elif re.search(r'\?\s*1\s*:\s*0|!=\s*NULL|!=\s*0', ctx): d = 'off (set)'
            com = ''
            for j in range(i, max(i - 40, 0), -1):
                if n in src[j] and ('/*' in src[j] or src[j].strip().startswith(('*', '//'))): com = src[j].strip()[:140]; break
            if n not in rows or (rows[n][1] == '?' and d != '?'): rows[n] = [i + 1, d, com]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['switch', 'line', 'default(heuristic)', 'nearest comment'])
        for n in sorted(rows): w.writerow([n] + rows[n])
    print(f"== {len(rows)} environment switches in rsx_live_draw.c ->", path, dict(collections.Counter(v[1] for v in rows.values())))

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('--ledger', default=os.path.join(ROOT, 'android', 'docs', 'ld_switch_ledger.csv'))
    a = ap.parse_args(); sizes(); classify(); api(); ledger(a.ledger)
