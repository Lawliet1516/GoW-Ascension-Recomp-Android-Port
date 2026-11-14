"""Before/after a timed change in one run: fps and draws per frame.

    python tools/flag_compare.py build/logs/fl1.log 170

Windows are the 5 s [perf] lines counted from boot; "before" is the minute
before <at> s, "after" the 70 s starting 10 s after it.
"""
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from heavy_stats import windows  # noqa: E402

log, at = sys.argv[1], int(sys.argv[2])
ws = windows(log)


def sel(lo, hi):
    return [w for i, w in enumerate(ws) if lo <= 5 * (i + 1) < hi and w['fps'] < 100 and w.get('draws')]


for lab, g in (("before", sel(at - 60, at)), ("after", sel(at + 10, at + 80))):
    if g:
        print(f"{lab:6s}: {len(g)} windows  fps {st.mean(w['fps'] for w in g):5.1f}  "
              f"draws/frame {st.mean(w['dpf'] for w in g):5.0f}  per window: "
              + ' '.join(f"{w['fps']:.0f}/{w['dpf'] / 1000:.1f}k" for w in g))
