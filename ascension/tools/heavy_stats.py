"""Heavy-window summary of one or more game logs.

Every 5 s the game logs [perf] (fps, busy %), [flush] (frame-time histogram)
and [draws] (draws in the window). A window is "heavy" when it has at least
--min-dpf draws per frame (default 3000, the egg fight; HANDOFF 0.12).

    python tools/heavy_stats.py build/logs/base1.log [more.log ...]
"""
import argparse
import re
import statistics as st

PERF = re.compile(r'^\[perf\] fps=([\d.]+) ms=([\d.]+) cpu=(\d+) gpu=(\d+) spu=(\d+) rsx=(\d+)')
FLUSH = re.compile(r'^\[flush\] .*max_frame=(\d+)ms hitches=(\d+) frames<20/34/50/67/100/\+=([\d/]+)')
DRAWS = re.compile(r'^\[draws\] per 5s: (\d+) draws')
BOUNDS = [20, 34, 50, 67, 100, 1000]   # upper edge (ms) of each histogram bucket


ABEV = re.compile(r'^\[[\w-]+-ab\] (on|off)\b')                  # [pin-ab] on t=...: state until the next one
ABWIN = re.compile(r'^\[draws\] per 5s: \w+ window (ON|OFF)')      # renderer *_AB switches: tag of this window


def windows(path):
    """5 s windows; each gets 'ab' = 'on' / 'off' / 'mixed' / None (no A/B switch in the run)."""
    out, cur = [], None
    state, switched = None, False
    for line in open(path, encoding='utf-8', errors='replace'):
        m = ABEV.match(line)
        if m:
            if state is not None and m[1] != state:
                switched = True
            state = m[1]
            continue
        m = PERF.match(line)
        if m:
            if cur:
                out.append(cur)
            cur = dict(fps=float(m[1]), cpu=int(m[3]), gpu=int(m[4]), spu=int(m[5]), rsx=int(m[6]),
                       ab=('mixed' if switched else state))
            switched = False
            continue
        if cur is None:
            continue
        m = ABWIN.match(line)
        if m:
            cur['ab'] = m[1].lower()
            continue
        m = FLUSH.match(line)
        if m and 'hist' not in cur:
            cur['max'] = int(m[1])
            cur['hist'] = [int(x) for x in m[3].split('/')]
            continue
        m = DRAWS.match(line)
        if m and 'draws' not in cur:
            cur['draws'] = int(m[1])
    if cur:
        out.append(cur)
    for w in out:
        frames = max(w['fps'] * 5, 1)
        w['dpf'] = w.get('draws', 0) / frames
    return out


def pct_over(hists, ms):
    """Share of frames slower than `ms` (bucket edges only: 20/34/50/67/100)."""
    tot = sum(sum(h) for h in hists) or 1
    idx = BOUNDS.index(ms)
    return 100.0 * sum(sum(h[idx + 1:]) for h in hists) / tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--min-dpf', type=float, default=3000)
    a = ap.parse_args()
    for path in a.logs:
        ws = windows(path)
        heavy = [w for w in ws if w['dpf'] >= a.min_dpf and w['fps'] < 100]
        print(f'{path}: {len(ws)} windows, {len(heavy)} heavy (>= {a.min_dpf:.0f} draws/frame)')
        if not heavy:
            continue
        summarize(heavy, '  ')
        tags = {w['ab'] for w in heavy}
        if tags & {'on', 'off'}:
            for t in ('on', 'off'):
                g = [w for w in heavy if w['ab'] == t]
                print(f'  --- A/B {t.upper()}: {len(g)} heavy windows'
                      + (f' ({sum(w["ab"] == "mixed" for w in heavy)} mixed dropped)' if t == 'on' else ''))
                if g:
                    summarize(g, '    ')


def summarize(heavy, ind):
    f = [w['fps'] for w in heavy]
    hists = [w['hist'] for w in heavy if 'hist' in w]
    print(f'{ind}heavy fps: mean {st.mean(f):.1f}  median {st.median(f):.1f}  min {min(f):.1f}  max {max(f):.1f}')
    print(f'{ind}draws/frame: median {st.median(w["dpf"] for w in heavy):.0f}')
    print(ind + 'busy % (median): spu {} rsx {} cpu {} gpu {}'.format(
        *(int(st.median(w[k] for w in heavy)) for k in ('spu', 'rsx', 'cpu', 'gpu'))))
    if hists:
        print(f'{ind}frames slower than 20/34/50 ms: {pct_over(hists, 20):.0f}% / {pct_over(hists, 34):.0f}% / '
              f'{pct_over(hists, 50):.0f}%   worst frame {max(w.get("max", 0) for w in heavy)} ms')
    print(ind + 'per window fps: ' + ' '.join(f'{x:.0f}' for x in f))


if __name__ == '__main__':
    main()
