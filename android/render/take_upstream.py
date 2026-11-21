#!/usr/bin/env python3
"""Copies upstream v0.20.0 files into render/libs/video and prints a commit message that names the upstream commit and every blob id.
   take_upstream.py MODE   MODE = untouched (files the fork never changed vs v0.15.0) | new (files that did not exist in v0.15.0)
   take_upstream.py files p1 p2 ...   (explicit list, for a deliberate single-file step)"""
import os, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BASE, NEW = 'refs/upstream/ps3recomp/v0.15.0', 'refs/upstream/ps3recomp/v0.20.0'
SKIP = ('rsx_metal_backend', 'rsx_d3d12', 'rsx_live_draw', 'metal')       # Windows/Apple-only: not part of render/libs/video
def git(*a, text=True): return subprocess.run(['git', '-C', ROOT, *a], capture_output=True, text=text).stdout
def ls(ref, prefix):
    out = git('ls-tree', '-r', '--format=%(objectname) %(path)', ref, '--', prefix).split('\n')
    return {l.split(' ', 1)[1][len(prefix):].lstrip('/'): l.split(' ', 1)[0] for l in out if l}
base, new = ls(BASE, 'libs/video'), ls(NEW, 'libs/video')
fork = ls('HEAD', 'render/libs/video')
mode = sys.argv[1]
if mode == 'untouched':
    # "untouched" is judged against the ORIGINAL fork tree, not the render/ copy (which may already differ)
    orig = ls('8d28fa8', 'ps3kit/libs/video')
    paths = [p for p in new if p in base and p in orig and base[p] == orig[p] and new[p] != base[p] and not any(s in p for s in SKIP)]
elif mode == 'new':
    paths = [p for p in new if p not in base and p not in fork and not any(s in p for s in SKIP)]
elif mode == 'files': paths = sys.argv[2:]
else: sys.exit('mode?')
lines = []
for p in sorted(paths):
    dst = os.path.join(ROOT, 'render', 'libs', 'video', p); os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(git('cat-file', 'blob', new[p], text=False))
    lines.append(f'  {p}  v0.20.0:{new[p][:12]}' + (f'  (was {fork[p][:12]})' if p in fork else '  (new)'))
open('/tmp/take_upstream_msg.txt', 'w').write('\n'.join(lines))
print(len(paths), 'files'); print('\n'.join(lines[:6]), '...' if len(lines) > 6 else '')
