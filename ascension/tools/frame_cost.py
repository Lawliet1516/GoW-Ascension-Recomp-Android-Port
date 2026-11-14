"""Per-frame cost in the heavy windows of one or more runs (needs SPURS_WID_STATS=1).

    python tools/frame_cost.py build/logs/m1on.log build/logs/m1off.log

For each log, over the 5 s windows with >= --min-dpf draws per frame:
fps, SPU work per frame (policy-module time minus runs that found nothing),
split into EDGE (wids 15-17) and the rest, and the RSX walker's walking time
per frame. Medians, so a stray window does not move them.
"""
import argparse
import re
import statistics as st

WID = re.compile(r'w(\d+) (\d+)/(\d+)/\d+/\d+/\d+')


def windows(path):
    out, pend, walk = [], None, None
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('[spurs-wid]'):
            pend = {int(m[1]): int(m[2]) - int(m[3]) for m in WID.finditer(line)}
        elif line.startswith('[walk]'):
            m = re.search(r'walking (\d+)ms', line)
            walk = int(m[1]) if m else None
        elif line.startswith('[perf] fps='):
            out.append(dict(fps=float(re.search(r'fps=([\d.]+)', line)[1]), wid=pend, walk=walk))
            pend = None
        elif line.startswith('[draws] per 5s:') and ' draws (' in line and out and 'draws' not in out[-1]:
            out[-1]['draws'] = int(re.search(r'(\d+) draws', line)[1])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--min-dpf', type=float, default=3000)
    a = ap.parse_args()
    print(f"{'log':28s} {'win':>4s} {'fps':>5s} {'dpf':>5s} {'SPU ms/f':>9s} {'EDGE':>6s} {'other':>6s} {'walk ms/f':>9s}")
    for p in a.logs:
        H = [w for w in windows(p) if w.get('draws') and w['wid'] and w['fps'] < 100
             and w['draws'] / (w['fps'] * 5) >= a.min_dpf]
        if not H:
            print(f"{p[-28:]:28s}  no heavy windows")
            continue
        fr = lambda w: w['fps'] * 5
        spu = st.median(sum(w['wid'].values()) / fr(w) for w in H)
        edge = st.median(sum(w['wid'].get(k, 0) for k in (15, 16, 17)) / fr(w) for w in H)
        walk = st.median(w['walk'] / fr(w) for w in H if w['walk'] is not None)
        dpf = st.median(w['draws'] / fr(w) for w in H)
        print(f"{p[-28:]:28s} {len(H):4d} {st.mean(w['fps'] for w in H):5.1f} {dpf:5.0f} {spu:9.1f} {edge:6.1f} {spu - edge:6.1f} {walk:9.1f}")


if __name__ == '__main__':
    main()
