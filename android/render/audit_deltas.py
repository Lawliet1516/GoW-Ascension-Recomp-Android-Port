#!/usr/bin/env python3
"""Reproducible measurements behind android/render/ASCENSION_DELTAS_AUDIT.md.  Read-only: it only runs `git show` / `git diff --no-index` and writes under
android/render/results/deltas_audit/.

For each of the 12 files the fork changed relative to ps3recomp v0.15.0 (the base) it takes
  * the fork's production copy         HEAD:ps3kit/libs/video/<file>          (untouched by the migration)
  * the base                           refs/upstream/ps3recomp/v0.15.0:libs/video/<file>
  * the migration target               render/libs/video/<file>               (pure upstream v0.20.0, as committed in step 5 of MERGE_LOG)
and reports, per hunk of (base -> fork), how much of what the fork ADDED is already present, line for line, in upstream v0.20.0.

Verbatim presence is a LOWER bound for "incorporated": upstream may have implemented the same behaviour with other code. The report therefore treats
this table as a pointer, and decides each feature by reading the code (see the report's evidence column).

usage: audit_deltas.py            (from the repository root)
"""
import os, re, subprocess, sys, collections

ROOT = subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], text=True).strip()
OUT = os.path.join(ROOT, 'android/render/results/deltas_audit')
UP = os.path.join(ROOT, 'render/libs/video')
BASE_REF = 'refs/upstream/ps3recomp/v0.15.0'
FILES = ['cellGcmSys.c', 'rsx_commands.c', 'rsx_dispatch.c', 'rsx_dispatch.h', 'rsx_draw_engine.c', 'rsx_fp_decompiler.c', 'rsx_fp_decompiler.h',
         'rsx_vertex_compact.c', 'rsx_vertex_compact.h', 'rsx_vp_decompiler.c', 'rsx_vp_decompiler.h', 'rsx_vulkan_backend.c']
os.makedirs(OUT, exist_ok=True)
tmp = os.path.join(OUT, '.tmp'); os.makedirs(tmp, exist_ok=True)


def git_show(spec):
    return subprocess.run(['git', '-C', ROOT, 'show', spec], capture_output=True).stdout


def norm(s):
    return re.sub(r'\s+', ' ', s.strip())


rows = []
func_rows = []
tot = [0, 0, 0, 0]   # changed, sig, present, hunks
for f in FILES:
    b = os.path.join(tmp, 'base_' + f); k = os.path.join(tmp, 'fork_' + f)
    open(b, 'wb').write(git_show(f'{BASE_REF}:libs/video/{f}'))
    open(k, 'wb').write(git_show(f'HEAD:ps3kit/libs/video/{f}'))
    d = subprocess.run(['git', 'diff', '--no-index', '-U0', '-p', b, k], capture_output=True, text=True, errors='replace').stdout
    U = set(norm(l) for l in open(os.path.join(UP, f), errors='replace'))
    hdr = None; cur = []; rem = 0; per_func = collections.OrderedDict()

    def flush():
        global rem
        if hdr is None:
            return
        sig = [x for x in cur if len(x) >= 20]
        pres = sum(1 for x in sig if x in U)
        m = re.match(r'@@ [^@]+@@ ?(.*)', hdr)
        ctx = m.group(1).strip() if m else ''
        mm = re.search(r'([A-Za-z_]\w*)\s*\(', ctx)
        fn = mm.group(1) if mm else (ctx[:40] or '(file scope)')
        rows.append((f, hdr.split('@@')[1].strip(), fn, len(cur), len(sig), pres, rem))
        a = per_func.setdefault(fn, [0, 0, 0, 0]); a[0] += len(cur); a[1] += rem; a[2] += len(sig); a[3] += pres
    for l in d.split('\n'):
        if l.startswith('@@'):
            flush(); hdr = l; cur = []; rem = 0
        elif l.startswith('+') and not l.startswith('+++'):
            cur.append(norm(l[1:]))
        elif l.startswith('-') and not l.startswith('---'):
            rem += 1
    flush()
    for fn, (a, r, s, p) in per_func.items():
        func_rows.append((f, fn, a, r, s, p))

with open(os.path.join(OUT, 'overlap_by_hunk.tsv'), 'w') as o:
    o.write('file\thunk(-base,+fork)\tnearest_function\tadded\tsignificant(>=20ch)\tpresent_verbatim_in_v0.20.0\tremoved\n')
    for r in rows:
        o.write('\t'.join(map(str, r)) + '\n')
with open(os.path.join(OUT, 'by_function.tsv'), 'w') as o:
    o.write('file\tnearest_function(git context, approximate)\tadded\tremoved\tsignificant\tpresent_verbatim\n')
    for r in sorted(func_rows, key=lambda x: (x[0], -(x[2] + x[3]))):
        o.write('\t'.join(map(str, r)) + '\n')
with open(os.path.join(OUT, 'overlap_summary.txt'), 'w') as o:
    o.write('file                      added  removed  hunks  significant  present_verbatim  pct\n')
    T = [0, 0, 0, 0, 0]
    for f in FILES:
        rs = [r for r in rows if r[0] == f]
        a = sum(r[3] for r in rs); rm = sum(r[6] for r in rs); s = sum(r[4] for r in rs); p = sum(r[5] for r in rs)
        o.write(f'{f:24} {a:6} {rm:8} {len(rs):6} {s:12} {p:17} {100 * p // max(s, 1):4}%\n')
        for i, v in enumerate((a, rm, len(rs), s, p)):
            T[i] += v
    o.write(f'{"TOTAL":24} {T[0]:6} {T[1]:8} {T[2]:6} {T[3]:12} {T[4]:17} {100 * T[4] // max(T[3], 1):4}%\n')
print(open(os.path.join(OUT, 'overlap_summary.txt')).read())
subprocess.run(['rm', '-rf', tmp])
