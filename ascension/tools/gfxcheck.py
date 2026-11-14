"""Automatic look at test-run screenshots and renderer counters for graphics trouble.

    python -I tools/gfxcheck.py SHOTS_DIR [SHOTS_DIR ...] [--log RUN.log] [--csv out.csv]

Each SHOTS_DIR holds burst.ps1 bursts (tSSS_NN.png: NN consecutive captures at
second SSS). Per burst it reports:
  - flicker: a frame that differs a lot from both neighbours while the
    neighbours agree with each other (A-B-A), which camera motion cannot do;
  - local flicker: the same test per 80x80-pixel tile, for one object whose
    texture changes for a frame while the rest of the screen holds still
    (the worst tile's position is printed; the perf overlay is skipped);
    both tests forgive a whole-image shift of up to 6 px (camera shake on a
    hit goes A-B-A too), and a frame such a shift explains is not tile-tested;
  - spike: one frame far brighter / darker / more magenta than both
    neighbours (a NaN wash or exposure flash under fast camera motion);
  - luma swing: the burst's brightest vs darkest frame (whole-screen flashes);
  - white / black: the share of blown-out (all channels >= 250) and fully
    black (all < 6) pixels, which catches white blobs and black patches;
  - magenta / NaN-ish: pixels at pure (255,0,255)-like colours, the usual
    look of a missing texture or a NaN written as colour.
With --log, the run log's renderer counters are summarised as well.
Exit code 0; the verdict is in the output (OK / CHECK lines).
"""
import os, re, sys, glob
import numpy as np
from PIL import Image

def load(path, step=2):
    a = np.asarray(Image.open(path).convert("RGB"))[::step, ::step].astype(np.int16)
    return a

def luma(a):
    return (0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2])

def frame_stats(a):
    n = a.shape[0] * a.shape[1]
    white = np.all(a >= 250, axis=2).sum() / n
    black = np.all(a < 6, axis=2).sum() / n
    mag = ((a[..., 0] > 230) & (a[..., 1] < 30) & (a[..., 2] > 230)).sum() / n
    return float(luma(a).mean()), white, black, mag

def mad(a, b):
    return float(np.abs(luma(a) - luma(b)).mean())

# Camera shake on a hit moves the whole image a few pixels and back (A-B-A
# too). The middle frame's difference to each neighbour is therefore taken as
# the smallest over integer shifts of up to SHIFT pixels (of the step-2 image,
# so 2*SHIFT full-size), on the interior the shift leaves inside the frame.
SHIFT = 3

def shift_diffs(la, lb):
    """|la - lb shifted| (interior only, same shape for every shift), per shift."""
    h, w = la.shape
    core = la[SHIFT:h - SHIFT, SHIFT:w - SHIFT]
    for dy in range(-SHIFT, SHIFT + 1):
        for dx in range(-SHIFT, SHIFT + 1):
            yield np.abs(core - lb[SHIFT + dy:h - SHIFT + dy, SHIFT + dx:w - SHIFT + dx])

def mad_shift(la, lb):
    """Mean |luma| difference, smallest over the shifts."""
    return min(float(d.mean()) for d in shift_diffs(la, lb))

# Local flicker: the frame is cut into TILE x TILE tiles (in the step-2 image)
# and the A-B-A test runs per tile, so one object whose texture changes for a
# frame shows even when the whole-screen difference is small. Camera motion
# changes most tiles and keeps prev->next as far apart as prev->mid, so it
# does not pass. The perf overlay (top right) is skipped. A tile flags when
# it differs from both neighbours by > LOCAL_DIFF (mean |luma|, 0-255) while
# the neighbours agree to within LOCAL_KEEP of that and LOCAL_ABS absolute.
TILE, LOCAL_DIFF, LOCAL_KEEP, LOCAL_ABS = 40, 12.0, 0.35, 6.0

def tile_mean(d):
    """Mean of a difference image per TILE x TILE tile, as a 2-D array."""
    h, w = d.shape[0] // TILE * TILE, d.shape[1] // TILE * TILE
    return d[:h, :w].reshape(h // TILE, TILE, w // TILE, TILE).mean(axis=(1, 3))

def tile_luma_diff(la, lb, shift=False):
    """Mean |la - lb| per tile; with shift, the smallest over the shifts per
    tile (on the interior, so the grid starts SHIFT pixels in)."""
    if not shift:
        h, w = la.shape
        return tile_mean(np.abs(la - lb)[SHIFT:h - SHIFT, SHIFT:w - SHIFT])
    out = None
    for d in shift_diffs(la, lb):
        t = tile_mean(d)
        out = t if out is None else np.minimum(out, t)
    return out

def local_flicker(frames, step=2):
    """[(frame index, tiles flagged, worst tile (x, y) in full-size pixels, its diff)]"""
    hits = []
    if len(frames) < 3:
        return hits
    lums = [luma(f) for f in frames]
    rows, cols = (frames[0].shape[0] - 2 * SHIFT) // TILE, (frames[0].shape[1] - 2 * SHIFT) // TILE
    keep = np.ones((rows, cols), bool)
    keep[: max(1, int(rows * 0.16 + 0.999)), int(cols * 0.90):] = False   # perf overlay
    for i in range(1, len(frames) - 1):
        dp = tile_luma_diff(lums[i - 1], lums[i], shift=True)
        dn = tile_luma_diff(lums[i], lums[i + 1], shift=True)
        ds = tile_luma_diff(lums[i - 1], lums[i + 1])
        m = np.minimum(dp, dn)
        flag = keep & (m > LOCAL_DIFF) & (ds < LOCAL_KEEP * m) & (ds < LOCAL_ABS)
        n = int(flag.sum())
        if n:
            score = np.where(flag, m, 0)
            r, c = np.unravel_index(int(np.argmax(score)), score.shape)
            hits.append((i, n, ((c * TILE + SHIFT) * step, (r * TILE + SHIFT) * step), float(score[r, c])))
    return hits

def check_dir(d):
    out = []
    shots = sorted(glob.glob(os.path.join(d, "t*_*.png")))
    bursts = {}
    for s in shots:
        m = re.match(r"t(\d+)_(\d+)\.png$", os.path.basename(s))
        if m:
            bursts.setdefault(int(m.group(1)), []).append((int(m.group(2)), s))
    if not bursts:
        return [f"{os.path.basename(d)}: no screenshots"], True
    bad = False
    for sec in sorted(bursts):
        frames = [load(p) for _, p in sorted(bursts[sec])]
        st = [frame_stats(f) for f in frames]
        lum = [s[0] for s in st]
        white = max(s[1] for s in st); black = max(s[2] for s in st); mag = max(s[3] for s in st)
        flick = []
        lums = [luma(f) for f in frames]
        shaken = set()   # frames a whole-image shift explains: no local test there
        for i in range(1, len(frames) - 1):
            # shift-tolerant mid->neighbour differences: camera shake is not flicker
            d_prev, d_next = mad_shift(lums[i], lums[i - 1]), mad_shift(lums[i], lums[i + 1])
            d_skip = mad(frames[i - 1], frames[i + 1])
            r_prev, r_next = mad(frames[i], frames[i - 1]), mad(frames[i], frames[i + 1])
            if r_prev > 6 and r_next > 6 and d_prev + d_next < 0.8 * (r_prev + r_next):
                shaken.add(i)   # the HUD stays put while the scene shifts: mixed tiles
            elif min(r_prev, r_next) > 12:
                # the whole frame differs from both neighbours (motion blur
                # switching, a cut): the frame tests above judge it; a tile
                # test there only finds the HUD sitting on a changed scene
                shaken.add(i)
            if d_prev > 12 and d_next > 12 and d_skip < 0.5 * min(d_prev, d_next):
                flick.append(f"#{i} ({d_prev:.0f}/{d_next:.0f} vs {d_skip:.0f})")
        swing = max(lum) - min(lum) if lum else 0
        notes = []
        if flick: notes.append("FLICKER " + ", ".join(flick))
        flagged = {int(f.split()[0][1:]) for f in flick}
        # One frame far brighter, darker or more magenta than both neighbours:
        # a flash (NaN wash, blown exposure) under camera motion fast enough
        # that the neighbours differ too, which the A-B-A test needs them not to.
        spikes = []
        for i in range(1, len(frames) - 1):
            if i in flagged:
                continue
            lo, hi = min(lum[i - 1], lum[i + 1]), max(lum[i - 1], lum[i + 1])
            mags = st[i][3], st[i - 1][3], st[i + 1][3]
            if lum[i] > 1.6 * hi and lum[i] - hi > 15:
                spikes.append(f"#{i} bright ({lum[i]:.0f} vs {lum[i - 1]:.0f}/{lum[i + 1]:.0f})")
            elif lo > 5 and lum[i] < lo / 1.6 and lo - lum[i] > 15:
                spikes.append(f"#{i} dark ({lum[i]:.0f} vs {lum[i - 1]:.0f}/{lum[i + 1]:.0f})")
            if mags[0] > 0.01 and max(mags[1], mags[2]) < 0.001:
                spikes.append(f"#{i} magenta ({mags[0]:.1%})")
        # The burst's first and last frames have one neighbour only: flag a
        # jump to it that motion cannot make (magenta from ~none, luma x1.6).
        if len(frames) >= 2:
            for i, j in ((0, 1), (len(frames) - 1, len(frames) - 2)):
                if i in flagged:
                    continue
                if st[i][3] > 0.01 and st[j][3] < 0.001:
                    spikes.append(f"#{i} magenta at burst edge ({st[i][3]:.1%})")
                elif lum[i] > 1.6 * lum[j] and lum[i] - lum[j] > 15:
                    spikes.append(f"#{i} bright at burst edge ({lum[i]:.0f} vs {lum[j]:.0f})")
                elif lum[j] > 5 and lum[i] < lum[j] / 1.6 and lum[j] - lum[i] > 15:
                    spikes.append(f"#{i} dark at burst edge ({lum[i]:.0f} vs {lum[j]:.0f})")
        if spikes: notes.append("SPIKE " + ", ".join(spikes))
        flagged |= {int(s.split()[0][1:]) for s in spikes}
        local = [h for h in local_flicker(frames) if h[0] not in flagged and h[0] not in shaken]
        if local:
            notes.append("LOCAL FLICKER " + ", ".join(
                f"#{i} ({n} tile{'s' if n > 1 else ''}, worst at x={x} y={y} d={v:.0f})"
                for i, n, (x, y), v in local))
        if swing > 25: notes.append(f"luma swing {swing:.0f}")
        if white > 0.08: notes.append(f"white {white:.0%}")
        if black > 0.60: notes.append(f"black {black:.0%}")
        if mag > 0.005: notes.append(f"magenta {mag:.1%}")
        verdict = "CHECK" if notes else "OK"
        bad |= bool(notes)
        out.append(f"{os.path.basename(d)} t{sec}: {verdict}  frames={len(frames)} luma={np.mean(lum):.0f} "
                   f"white={white:.1%} black={black:.1%}" + ("  <- " + "; ".join(notes) if notes else ""))
    return out, bad

COUNTERS = [
    (r"drop\{([^}]*)\}", "drops"),
    (r"stale surface binds[ =:]*(\d+)", "stale surface binds"),
    (r"intra-frame rewrites[ =:]*(\d+)", "intra-frame rewrites"),
    (r"fp precision clamps[^\n]*?total[ =:]*(\d+)", "fp clamps"),
    (r"surface table full", "surface table full"),
]

def check_log(path):
    out = []
    try:
        text = open(path, "r", errors="replace").read()
    except OSError:
        return [f"{path}: no log"]
    for pat, name in COUNTERS:
        ms = re.findall(pat, text)
        if not ms:
            continue
        if name == "drops":
            nz = [m for m in ms if re.search(r"=[1-9]", m)]
            out.append(f"  {name}: last {{{ms[-1]}}}" + (f"  ({len(nz)} lines with non-zero drops)" if nz else ""))
        elif name == "surface table full":
            out.append(f"  {name}: {len(ms)} lines")
        else:
            vals = [int(x) for x in ms]
            out.append(f"  {name}: max {max(vals)} per 5 s, last {vals[-1]}")
    for pat, name in [(r"DEVICE_REMOVED|device removed|TDR", "GPU device removed"),
                      (r"\[live-draw\][^\n]*(error|failed)", "live-draw errors"),
                      (r"shader compile (failed|error)", "shader compile errors")]:
        n = len(re.findall(pat, text, re.I))
        if n:
            out.append(f"  {name}: {n}")
    return out

def main():
    args = sys.argv[1:]
    logs = [args[i + 1] for i, a in enumerate(args) if a == "--log"]
    dirs = [a for i, a in enumerate(args) if a != "--log" and (i == 0 or args[i - 1] != "--log")]
    any_bad = False
    for d in dirs:
        lines, bad = check_dir(d)
        any_bad |= bad
        print("\n".join(lines))
    for l in logs:
        print(f"{os.path.basename(l)} renderer counters:")
        print("\n".join(check_log(l)) or "  (none found)")
    print("GRAPHICS: " + ("CHECK the flagged bursts" if any_bad else "nothing unusual in the screenshots"))

if __name__ == "__main__":
    main()
