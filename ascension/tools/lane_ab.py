"""In-run A/B by lane: for each window tag (a renderer *_AB switch, "[draws] per
5s: <name> window ON|OFF", or "[xxx-ab] on|off" events), the mean fps, draws a
frame, and the graphics thread's walking time per frame ([walk] per 5s:
walking N ms), so one lane's change shows even when fps is held by another.

usage: python tools/lane_ab.py <log> [--min-dpf 1500] [--tag <name>]

At each 5 s boundary the log prints [walk], then [perf], then the [draws]
block with the window tags: all of them describe the window just ended.
--tag picks one switch's tag (e.g. psosame) when several are printed.
"""
import re
import sys
import statistics

PERF = re.compile(r'^\[perf\] fps=([\d.]+)')
DRAWS = re.compile(r'^\[draws\] per 5s: (\d+) draws')
WIN = re.compile(r'^\[draws\] per 5s: (\w+) window (ON|OFF)')
ABEV = re.compile(r'^\[[\w-]+-ab\] (on|off)\b')
WALK = re.compile(r'^\[walk\] per 5s: walking (\d+)ms')


def main():
    path = sys.argv[1]
    min_dpf = 1500
    if '--min-dpf' in sys.argv:
        min_dpf = int(sys.argv[sys.argv.index('--min-dpf') + 1])
    tag = sys.argv[sys.argv.index('--tag') + 1] if '--tag' in sys.argv else None
    wins, cur, state, last_walk = [], None, None, None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = WALK.match(line)
        if m:
            last_walk = int(m[1])      # printed just before this window's [perf]
            continue
        m = ABEV.match(line)
        if m:
            state = m[1].upper()
            continue
        m = PERF.match(line)
        if m:
            if cur:
                wins.append(cur)
            cur = {'fps': float(m[1]), 'tag': state}
            if last_walk is not None:
                cur['walk'] = last_walk
                last_walk = None
            continue
        if cur is None:
            continue
        m = WIN.match(line)
        if m:
            if tag is None or m[1] == tag:
                cur['tag'] = m[2]
            continue
        m = DRAWS.match(line)
        if m and 'draws' not in cur:
            cur['draws'] = int(m[1])
            continue
    if cur:
        wins.append(cur)
    out = {}
    for w in wins:
        if 'draws' not in w or 'walk' not in w or not w['tag']:
            continue
        frames = max(w['fps'] * 5, 1)
        dpf = w['draws'] / frames
        if dpf < min_dpf:
            continue
        o = out.setdefault(w['tag'], {'fps': [], 'dpf': [], 'walk': [], 'us': []})
        o['fps'].append(w['fps'])
        o['dpf'].append(dpf)
        o['walk'].append(w['walk'] / frames)
        o['us'].append(w['walk'] * 1000.0 / max(w['draws'], 1))
    for tag in sorted(out):
        o = out[tag]
        print('%-3s %2d windows: fps %.1f (median %.1f), draws/frame %.0f, walker %.2f ms/frame, %.2f us/draw'
              % (tag, len(o['fps']), statistics.mean(o['fps']), statistics.median(o['fps']), statistics.mean(o['dpf']),
                 statistics.mean(o['walk']), statistics.mean(o['us'])))


if __name__ == '__main__':
    main()
