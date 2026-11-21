#!/usr/bin/env python3
"""Three-way merge of one file into render/libs/video: BASE v0.15.0, OURS = the current render/libs/video file, THEIRS v0.20.0.
Writes the merged text (with conflict markers if any) to render/libs/video/<file> and prints the number of conflict hunks.
Conflict markers are never committed: resolve them by hand and record the decision in android/render/MERGE_LOG.md."""
import os, subprocess, sys, tempfile
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
def show(ref, path): return subprocess.run(['git', '-C', ROOT, 'show', f'{ref}:{path}'], capture_output=True).stdout
f = sys.argv[1]; ours = os.path.join(ROOT, 'render', 'libs', 'video', f)
with tempfile.TemporaryDirectory() as d:
    open(os.path.join(d, 'base'), 'wb').write(show('refs/upstream/ps3recomp/v0.15.0', f'libs/video/{f}'))
    open(os.path.join(d, 'theirs'), 'wb').write(show('refs/upstream/ps3recomp/v0.20.0', f'libs/video/{f}'))
    open(os.path.join(d, 'ours'), 'wb').write(open(ours, 'rb').read())
    r = subprocess.run(['git', 'merge-file', '-L', 'fork', '-L', 'v0.15.0', '-L', 'v0.20.0', os.path.join(d, 'ours'), os.path.join(d, 'base'), os.path.join(d, 'theirs')])
    open(ours, 'wb').write(open(os.path.join(d, 'ours'), 'rb').read())
print(f'{f}: {r.returncode} conflict hunk(s)')
