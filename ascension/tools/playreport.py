"""Report on a played session: fps over time, where it was slow, what limited it, and
what the screen looked like at each point.

    python -I tools/playreport.py RUN.log [--shots DIR] [--out report.png] [--every 30]

RUN.log is the game's log (run.ps1 or the launcher's build/logs/last_run.log); its
[perf] lines come every 5 s. --shots is a burst.ps1 folder (tSSS_NN.png); the first
frame of each burst becomes a thumbnail labelled with that stretch's fps, and the
bursts also go through gfxcheck's flicker test. Prints a table per --every seconds:
avg / min fps, SPU and RSX busy (whichever is near 100% is the limit), and a summary
(time spent below 30 / 45 / 60 fps).
"""
import os, re, sys, glob

def parse_perf(path):
    rows = []
    for line in open(path, errors="replace"):
        if line.startswith("[perf]"):
            kv = dict(re.findall(r"(\w+)=([0-9.]+)", line))
            try:
                rows.append({k: float(kv[k]) for k in ("fps", "spu", "rsx", "cpu") if k in kv})
            except ValueError:
                pass
    return rows   # one row per 5 s, first at ~5 s

def main():
    a = sys.argv[1:]
    log = a[0]
    opt = lambda k, d=None: a[a.index(k) + 1] if k in a else d
    shots, out, every = opt("--shots"), opt("--out"), int(opt("--every", "30"))
    rows = parse_perf(log)
    if not rows:
        print("no [perf] lines"); return
    per = max(1, every // 5)
    print(f"{'time':>7}  {'avg':>5} {'min':>5}  {'spu%':>4} {'rsx%':>4}  limit")
    stretches = []
    for i in range(0, len(rows), per):
        chunk = rows[i:i + per]
        fps = [r["fps"] for r in chunk]
        spu = sum(r.get("spu", 0) for r in chunk) / len(chunk)
        rsx = sum(r.get("rsx", 0) for r in chunk) / len(chunk)
        avg, mn = sum(fps) / len(fps), min(fps)
        limit = "" if avg >= 59 else ("RSX (graphics thread)" if rsx >= spu + 10 else "SPU" if spu >= rsx + 10 else "both")
        t = (i + len(chunk)) * 5
        stretches.append((t, avg, mn))
        print(f"{t // 60:4d}:{t % 60:02d}  {avg:5.1f} {mn:5.1f}  {spu:4.0f} {rsx:4.0f}  {limit}")
    allf = [r["fps"] for r in rows]
    n = len(allf)
    print(f"\nwhole session: {n * 5 // 60} min, avg {sum(allf) / n:.1f} fps, lowest 5 s {min(allf):.1f}")
    for lim in (30, 45, 59):
        print(f"  time below {lim if lim != 59 else 60} fps: {100 * sum(f < lim for f in allf) / n:.0f}%")
    if shots and os.path.isdir(shots):
        here = os.path.dirname(os.path.abspath(__file__))
        sys.path.insert(0, here)
        import gfxcheck
        lines, bad = gfxcheck.check_dir(shots)
        flagged = [l for l in lines if "CHECK" in l]
        print(f"\nscreenshot check: {len(lines)} bursts, {len(flagged)} flagged")
        for l in flagged: print("  " + l)
        if out:
            from PIL import Image, ImageDraw
            firsts = sorted(glob.glob(os.path.join(shots, "t*_00.png")))
            if firsts:
                tw, th, cols = 320, 180, 6
                sheet = Image.new("RGB", (tw * cols, (th + 18) * ((len(firsts) + cols - 1) // cols)), (16, 16, 16))
                d = ImageDraw.Draw(sheet)
                for k, p in enumerate(firsts):
                    sec = int(re.search(r"t(\d+)_", os.path.basename(p)).group(1))
                    near = min(stretches, key=lambda s: abs(s[0] - sec))
                    x, y = (k % cols) * tw, (k // cols) * (th + 18)
                    sheet.paste(Image.open(p).convert("RGB").resize((tw, th)), (x, y))
                    col = (120, 220, 120) if near[1] >= 55 else (230, 200, 90) if near[1] >= 40 else (240, 110, 100)
                    d.text((x + 4, y + th + 3), f"{sec // 60}:{sec % 60:02d}  {near[1]:.0f} fps (min {near[2]:.0f})", fill=col)
                sheet.save(out)
                print(f"\nthumbnails: {out}")

if __name__ == "__main__":
    main()
