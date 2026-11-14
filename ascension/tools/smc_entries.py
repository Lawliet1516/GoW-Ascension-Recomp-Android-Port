#!/usr/bin/env python3
"""
smc_entries.py -- rank the SPU code that runs through the SMC path (microstep +
interpreter) and say which lift would take each entry off it. Read-only: it
writes its report and proposals to --out and never touches tools\\*.json or
src\\spu.

The run (environment when the game runs; all diagnostic, off when unset):
  SPU_SMC_PROF=1 SPU_SMC_PROF_TOP=60 SPU_INTERP_HIST=1 SPU_SMC_SEEN_MAX=4096
  SPU_SMC_LSDUMP=<dir> SPU_SMC_LSDUMP_MAX=64        (the dir must exist)

Usage (from ascension\\):
  python tools/smc_entries.py build/logs/rc4diag-1.log --lsdump build/logs/rc4diag-lsdump
         --out <dir> [--windows A:B] [--hist-windows A:B] [--min-share 0.25] [--tsc-ghz 4.3]

What it checks for each logged entry (resident module image, LS start, the
first 32 code bytes there when SPU_SMC_PROF logged them):
  MODULE-EXTRA    inside the resident job module's image, bytes equal to the
                  module file there and no lifted function at that address:
                  a tools/jobmods_extra_funcs.json entry (static code, exact).
  MODULE-LIFTED   as above but a lifted function exists there: a dispatch miss,
                  not a missing lift.
  MODULE-PATCHED  inside the image, bytes differ from the module file: code the
                  job wrote at run time (no static lift).
  FRAG-ENTRY      the bytes are in the executable inside a lifted fragment but
                  not at one of its lifted entries: a job_fragments.json
                  "entries" EA (the claim also needs the fragment's first
                  quadword in LS at the implied address: checked with a dump).
  FRAG-LIFTED     at a lifted entry of a lifted fragment: the content claim
                  missed (see the dump check).
  EBOOT-NEW       in the executable outside every lifted fragment: a new
                  fragment; its extent comes from the LS dump.
  MODULE-COPY     bytes of another job module.   WORKLOAD  bytes of a meta\\spu image.
  NOT-FOUND       none of those (built at run time, or data).
  NO-BYTES        an entry seen only in a [spu-smc] line (no code bytes).
"""
import argparse, collections, difflib, glob, json, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # ascension\
BASE = 0x4000                                     # job modules' LS base
LS_SIZE = 0x40000

RE_PROF_HDR = re.compile(r"\[smc-prof\] ([\d.]+) Mticks in the SMC path in 5 s")
RE_PROF = re.compile(r"\[smc-prof\]\s+img=(-?\d+) ovl=(-?\d+) start=0x([0-9A-Fa-f]+) ticks=(\d+) "
                     r"calls=(\d+) total=(\d+) code=([0-9A-Fa-f]{64})")
RE_SMC = re.compile(r"\[spu-smc\] microstep img=(-?\d+) ovl=(-?\d+) start=0x([0-9A-Fa-f]+) "
                    r"pc=0x([0-9A-Fa-f]+) word 0x([0-9A-Fa-f]+).*?steps=(\d+)")
RE_HIST_HDR = re.compile(r"\[interp-hist\] (\d+) steps in 5 s")
RE_HIST = re.compile(r"\[interp-hist\]\s+ovl=(-?\d+) start=0x([0-9A-Fa-f]+) steps=(\d+) calls=(\d+)")


# ---------------------------------------------------------------- inputs
def work_dir(root):
    p = os.path.join(root, ".env")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8", errors="replace"):
            l = line.strip()
            if l.startswith("WORK_DIR="):
                w = l.split("=", 1)[1].strip().strip('"').strip("'")
                return w if os.path.isabs(w) else os.path.normpath(os.path.join(root, w))
    return None


def elf_segments(path):
    """[(va, bytes)] of the PT_LOAD segments of a big-endian ELF (32- or 64-bit)."""
    f = open(path, "rb").read()
    if f[:4] != b"\x7fELF":
        sys.exit(f"{path}: not an ELF")
    segs = []
    if f[4] == 2:
        phoff = struct.unpack_from(">Q", f, 0x20)[0]
        phes, phn = struct.unpack_from(">HH", f, 0x36)
        for i in range(phn):
            t, _fl, off, va, _pa, fs, _ms = struct.unpack_from(">IIQQQQQ", f, phoff + i * phes)
            if t == 1 and fs:
                segs.append((va, f[off:off + fs]))
    else:
        phoff = struct.unpack_from(">I", f, 0x1C)[0]
        phes, phn = struct.unpack_from(">HH", f, 0x2A)
        for i in range(phn):
            t, off, va, _pa, fs, _ms, _fl, _al = struct.unpack_from(">IIIIIIII", f, phoff + i * phes)
            if t == 1 and fs:
                segs.append((va, f[off:off + fs]))
    return segs


def find_all(segs, needle, limit=16):
    """Word-aligned hits of needle: [(va, seg_index, offset)]."""
    hits = []
    for si, (va, data) in enumerate(segs):
        i = data.find(needle)
        while i >= 0 and len(hits) < limit:
            if (va + i) % 4 == 0:
                hits.append((va + i, si, i))
            i = data.find(needle, i + 1)
    return hits


def lifted_table(path):
    """(lifted function addresses, wrapped addresses) of one generated spu_recomp.c."""
    addrs, wrapped = set(), set()
    if not os.path.exists(path):
        return addrs, wrapped
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*\{ 0x([0-9A-Fa-f]{8})u, ", line)
        if m:
            addrs.add(int(m.group(1), 16))
            continue
        m = re.match(r"static spu_fn_desc s_fnw_([0-9A-F]{8}) = \{ (\d+),", line)
        if m:
            wrapped.add(int(m.group(1), 16))
    return addrs, wrapped


class Images:
    def __init__(self, root, work, spu_dir, elf):
        self.spu_dir = spu_dir
        self.mods = {}        # image id -> dict(name, raw, lifted, wrapped)
        self.frags = {}       # image id -> dict(name, ea, span, base, lifted, wrapped)
        self.workloads = {}   # image id -> dict(name, segs)
        reg = open(os.path.join(spu_dir, "jobmods_register.c"), encoding="utf-8").read()
        for img, name in re.findall(r"spu_begin_image\((\d+)\); (\w+)_spu_recomp_register\(\);", reg):
            raw_path = os.path.join(work, "jobmods", name + ".bin")
            raw = open(raw_path, "rb").read() if os.path.exists(raw_path) else b""
            lifted, wrapped = lifted_table(os.path.join(spu_dir, name, "spu_recomp.c"))
            self.mods[int(img)] = dict(name=name, raw=raw, lifted=lifted, wrapped=wrapped)
        pic = open(os.path.join(spu_dir, "pic_register.c"), encoding="utf-8").read()
        for img, name, ea, span, base, img2 in re.findall(
                r"spu_begin_image\((\d+)\); (pic_[0-9A-F]{8})_spu_recomp_register\(\);\s*"
                r"spu_overlay_register_pic\(0x([0-9A-Fa-f]+)u, 0x([0-9A-Fa-f]+)u, 0x([0-9A-Fa-f]+)u, (\d+)\);", pic):
            lifted, wrapped = lifted_table(os.path.join(spu_dir, name, "spu_recomp.c"))
            self.frags[int(img)] = dict(name=name, ea=int(ea, 16), span=int(span, 16), base=int(base, 16),
                                        lifted=lifted, wrapped=wrapped)
        wl = os.path.join(spu_dir, "spu_workloads.c")
        if os.path.exists(wl):
            for img, name in re.findall(r"spu_begin_image\((\d+)\); (\w+)_spu_recomp_register\(\);",
                                        open(wl, encoding="utf-8").read()):
                p = os.path.join(root, "meta", "spu", name + ".elf")
                if os.path.exists(p):
                    self.workloads[int(img)] = dict(name=name, segs=elf_segments(p))
        self.eboot = elf_segments(elf)

    def frag_at(self, ea):
        for img, fr in self.frags.items():
            if fr["ea"] <= ea < fr["ea"] + fr["span"]:
                return img, fr
        return None, None


# ---------------------------------------------------------------- log
def parse_log(paths):
    prof_windows = []                     # [Mticks]
    prof = collections.defaultdict(lambda: dict(win={}, calls={}, total=0, code=None))
    smc = {}                              # (img, ovl, start) -> (pc, word, steps)
    smc_by_pc = collections.defaultdict(set)
    hist_windows = []
    hist = collections.defaultdict(dict)  # (ovl, pc) -> {window: steps}
    hist_calls = collections.defaultdict(dict)
    for path in paths:
        for line in open(path, encoding="utf-8", errors="replace"):
            m = RE_PROF_HDR.search(line)
            if m:
                prof_windows.append(float(m.group(1)))
                continue
            m = RE_PROF.search(line)
            if m:
                img, ovl, start = int(m.group(1)), int(m.group(2)), int(m.group(3), 16)
                e = prof[(img, ovl, start)]
                w = len(prof_windows) - 1
                e["win"][w] = e["win"].get(w, 0) + int(m.group(4))
                e["calls"][w] = e["calls"].get(w, 0) + int(m.group(5))
                e["total"] = max(e["total"], int(m.group(6)))
                e["code"] = bytes.fromhex(m.group(7))
                continue
            m = RE_SMC.search(line)
            if m:
                key = (int(m.group(1)), int(m.group(2)), int(m.group(3), 16))
                smc[key] = (int(m.group(4), 16), int(m.group(5), 16), int(m.group(6)))
                smc_by_pc[(key[1], int(m.group(4), 16))].add(key)
                continue
            m = RE_HIST_HDR.search(line)
            if m:
                hist_windows.append(int(m.group(1)))
                continue
            m = RE_HIST.search(line)
            if m:
                k = (int(m.group(1)), int(m.group(2), 16))
                w = len(hist_windows) - 1
                hist[k][w] = hist[k].get(w, 0) + int(m.group(3))
                hist_calls[k][w] = hist_calls[k].get(w, 0) + int(m.group(4))
    return prof_windows, prof, smc, smc_by_pc, hist_windows, hist, hist_calls


def window_set(spec, n):
    if not spec:
        return set(range(-1, n))
    a, _, b = spec.partition(":")
    lo = int(a) if a else 0
    hi = int(b) if b else n
    return set(range(lo, hi))


# ---------------------------------------------------------------- checks
def extent(ls, lsa, data, off):
    """How far LS around lsa equals data around off, in words: (back, forward) bytes."""
    lo = 0
    while lsa - lo - 4 >= 0 and off - lo - 4 >= 0 and ls[lsa - lo - 4:lsa - lo] == data[off - lo - 4:off - lo]:
        lo += 4
    hi = 0
    while lsa + hi + 4 <= len(ls) and off + hi + 4 <= len(data) and ls[lsa + hi:lsa + hi + 4] == data[off + hi:off + hi + 4]:
        hi += 4
    return lo, hi


def classify(I, ovl, start, code, dump):
    """-> (cls, target, detail, proposal) for one entry."""
    mod = I.mods.get(ovl)
    if mod and mod["raw"] and BASE <= start < BASE + len(mod["raw"]):
        name, off = mod["name"], start - BASE
        lifted = start in mod["lifted"]
        if code is None:
            if dump is not None:
                code = dump[start:start + 32]
            else:
                return ("MODULE-LIFTED?" if lifted else "MODULE-EXTRA?", (name, start),
                        "no code bytes logged", None if lifted else ("extra", name, start))
        static = mod["raw"][off:off + len(code)]
        if code != static:
            diff = [f"+0x{k:X}" for k in range(0, min(len(code), len(static)), 4)
                    if code[k:k + 4] != static[k:k + 4]]
            return ("MODULE-PATCHED", (name, start), "words differ at " + " ".join(diff), None)
        detail = []
        below = [a for a in mod["lifted"] if a <= start]
        owner = max(below) if below else None
        if owner is not None and owner != start:
            detail.append(f"inside lifted func 0x{owner:X}")
            if owner in mod["wrapped"]:
                detail.append(f"WARNING 0x{owner:X} is wrapped (native/record): check the split")
        if dump is not None:
            img = dump[BASE:BASE + len(mod["raw"])]
            lo, hi = extent(dump, start, mod["raw"], off)
            detail.append(f"dump: LS = module file over [0x{start - lo:X},0x{start + hi:X})")
        if lifted:
            return ("MODULE-LIFTED", (name, start), "; ".join(detail) or "lifted entry exists", None)
        return ("MODULE-EXTRA", (name, start), "; ".join(detail), ("extra", name, start))

    if code is None:
        if dump is not None:
            code = dump[start:start + 32]
        else:
            return ("NO-BYTES", (ovl, start), "outside the module image; run with SPU_SMC_PROF=1", None)

    hits = find_all(I.eboot, code)
    if hits:
        best = hits[0]
        ext = None
        if dump is not None:
            scored = []
            for h in hits:
                lo, hi = extent(dump, start, I.eboot[h[1]][1], h[2])
                scored.append((lo + hi, h, lo, hi))
            scored.sort(key=lambda s: -s[0])
            _, best, lo, hi = scored[0]
            ext = (best[0] - lo, best[0] + hi, start - lo)
        ea = best[0]
        more = f" ({len(hits)} hits in the executable)" if len(hits) > 1 else ""
        fimg, fr = I.frag_at(ea)
        if fr is not None:
            rel = ea - fr["ea"]
            at = fr["base"] + rel
            det = f"{fr['name']} (img {fimg}) +0x{rel:X}{more}"
            if ext is not None:
                lsa0 = start - rel
                sig_ok = lsa0 >= 0 and dump[lsa0:lsa0 + 16] == fr_bytes(I, fr["ea"], 16)
                det += (f"; dump: copy [0x{ext[0]:X},0x{ext[1]:X}) at LS 0x{ext[2]:X}, fragment start "
                        + ("resident" if sig_ok else "NOT resident (claim cannot match: needs its own fragment)"))
            if ext is not None and not sig_ok:
                # A copy of part of the fragment that leaves out its start: the
                # content claim checks that start, so this needs a fragment of
                # its own from where the copy begins.
                lo_ea = (ext[0] + 15) & ~15
                hi_ea = (ext[1] + 15) & ~15
                return ("FRAG-COPY", ("fragcopy", lo_ea, ea), det + f"; overlaps {fr['name']}",
                        ("newfrag", lo_ea, hi_ea - lo_ea, ea))
            if at in fr["lifted"]:
                return ("FRAG-LIFTED", ("frag", fr["ea"], ea), det, None)
            below = [a for a in fr["lifted"] if a <= at]
            if below and max(below) in fr["wrapped"]:
                det += f"; WARNING inside wrapped func 0x{max(below):X}"
            return ("FRAG-ENTRY", ("frag", fr["ea"], ea), det, ("entry", fr["ea"], ea))
        det = f"EA 0x{ea:X}{more}"
        prop = None
        if ext is not None:
            lo_ea = (ext[0] + 15) & ~15
            hi_ea = (ext[1] + 15) & ~15
            det += f"; dump: copy [0x{ext[0]:X},0x{ext[1]:X}) from LS 0x{ext[2]:X}"
            ov = [f["name"] for f in I.frags.values()
                  if f["ea"] < hi_ea and lo_ea < f["ea"] + f["span"]]
            if ov:
                det += "; overlaps " + ",".join(ov)
            prop = ("newfrag", lo_ea, hi_ea - lo_ea, ea)
        else:
            det += "; needs an LS dump for the fragment's extent"
        return ("EBOOT-NEW", ("eboot", ea), det, prop)

    for img, m in I.mods.items():
        i = m["raw"].find(code)
        if i >= 0 and i % 4 == 0:
            return ("MODULE-COPY", ("mod", m["name"], BASE + i), f"{m['name']} at LS 0x{BASE + i:X}", None)
    for img, w in I.workloads.items():
        h = find_all(w["segs"], code, 1)
        if h:
            return ("WORKLOAD", ("wl", w["name"], h[0][0]), f"{w['name']} at 0x{h[0][0]:X}", None)
    return ("NOT-FOUND", ("code", code.hex()), "built at run time or data", None)


def fr_bytes(I, ea, n):
    for va, data in I.eboot:
        if va <= ea and ea + n <= va + len(data):
            return data[ea - va:ea - va + n]
    return b""


def diff_trees(new, old):
    """--diff-trees NEW OLD: which lifted images a relift changed, and how
    (function entries added/removed, lines changed). Logs and _tmp.elf skipped."""
    def files(top):
        r = set()
        for d, _, fs in os.walk(top):
            for f in fs:
                if f.endswith(".log") or f == "_tmp.elf":
                    continue
                r.add(os.path.relpath(os.path.join(d, f), top))
        return r
    fa, fb = files(new), files(old)
    same = changed = 0
    for rel in sorted(fa | fb):
        if rel not in fb:
            print(f"only in {new}: {rel}"); continue
        if rel not in fa:
            print(f"only in {old}: {rel}"); continue
        A = open(os.path.join(new, rel), "rb").read()
        B = open(os.path.join(old, rel), "rb").read()
        if A == B:
            same += 1; continue
        changed += 1
        la, lb = A.decode("utf-8", "replace").splitlines(), B.decode("utf-8", "replace").splitlines()
        n = sum(1 for l in difflib.unified_diff(lb, la, lineterm="", n=0)
                if l[:1] in "+-" and not l.startswith(("+++", "---")))
        msg = f"changed: {rel} ({n} lines)"
        if rel.endswith("spu_recomp.c"):
            ea, wa = lifted_table(os.path.join(new, rel))
            eb, wb = lifted_table(os.path.join(old, rel))
            if ea - eb:
                msg += "; entries added " + " ".join(f"0x{x:X}" for x in sorted(ea - eb))
            if eb - ea:
                msg += "; entries REMOVED " + " ".join(f"0x{x:X}" for x in sorted(eb - ea))
            if wa != wb:
                msg += f"; WRAPPERS differ ({len(wa)} vs {len(wb)})"
        print(msg)
    print(f"{same} identical, {changed} changed")


# ---------------------------------------------------------------- main
def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--diff-trees":
        return diff_trees(sys.argv[2], sys.argv[3])
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--lsdump", help="SPU_SMC_LSDUMP directory (ls_ovl<ovl>_<start>.bin)")
    ap.add_argument("--out", required=True, help="Directory for the report and the proposals.")
    ap.add_argument("--windows", help="[smc-prof] 5 s windows to count, Python slice A:B (default all)")
    ap.add_argument("--hist-windows", help="[interp-hist] windows to count, A:B (default all)")
    ap.add_argument("--min-share", type=float, default=0.25,
                    help="Propose only targets with at least this %% of the counted SMC ticks (default 0.25).")
    ap.add_argument("--tsc-ghz", type=float, default=4.3, help="TSC rate for ms (Ryzen 9 9950X3D base 4.3).")
    ap.add_argument("--fps", type=float, default=0, help="Frames per second of the counted windows (for ms/frame).")
    ap.add_argument("--work", help="Work dir (default WORK_DIR from .env)")
    ap.add_argument("--spu-dir", default=os.path.join(ROOT, "src", "spu"))
    ap.add_argument("--elf", help="Executable (default <work>/disc/PS3_GAME/USRDIR/EBOOT.ELF)")
    ap.add_argument("--top", type=int, default=60)
    a = ap.parse_args()

    work = a.work or work_dir(ROOT)
    if not work:
        sys.exit("no --work and no WORK_DIR in .env")
    elf = a.elf or os.path.join(work, "disc", "PS3_GAME", "USRDIR", "EBOOT.ELF")
    I = Images(ROOT, work, a.spu_dir, elf)
    os.makedirs(a.out, exist_ok=True)

    pw, prof, smc, smc_by_pc, hw, hist, hist_calls = parse_log(a.logs)
    wsel = window_set(a.windows, len(pw))
    hsel = window_set(a.hist_windows, len(hw))

    dumps = {}
    if a.lsdump and os.path.isdir(a.lsdump):
        for p in glob.glob(os.path.join(a.lsdump, "ls_ovl*_*.bin")):
            m = re.match(r"ls_ovl(-?\d+)_([0-9A-Fa-f]+)\.bin$", os.path.basename(p))
            if m:
                dumps[(int(m.group(1)), int(m.group(2), 16))] = p

    # Interpreted steps per entry: [interp-hist] keys on where the microstep
    # gave up (pc); the [spu-smc] line of each entry maps that back to start.
    steps = collections.Counter()
    unmapped = 0
    for (ovl, pc), per in hist.items():
        s = sum(v for w, v in per.items() if w in hsel)
        starts = smc_by_pc.get((ovl, pc))
        if not starts:
            unmapped += s
            continue
        for key in starts:
            steps[key] += s / len(starts)

    entries = {}
    for key, e in prof.items():
        entries[key] = dict(ticks=sum(v for w, v in e["win"].items() if w in wsel),
                            calls=sum(v for w, v in e["calls"].items() if w in wsel),
                            total=e["total"], code=e["code"])
    for key in smc:
        entries.setdefault(key, dict(ticks=0, calls=0, total=0, code=None))

    groups = {}
    for (img, ovl, start), e in entries.items():
        dump = None
        p = dumps.get((ovl, start))
        if p:
            dump = open(p, "rb").read()
        cls, target, detail, prop = classify(I, ovl, start, e["code"], dump)
        g = groups.setdefault((cls, target), dict(cls=cls, target=target, ticks=0, calls=0, total=0, steps=0,
                                                   pos=[], details=set(), prop=prop, ovl=set()))
        g["ticks"] += e["ticks"]; g["calls"] += e["calls"]; g["total"] += e["total"]
        g["steps"] += steps.get((img, ovl, start), 0)
        g["pos"].append(start); g["ovl"].add(ovl); g["details"].add(detail)
        if prop and not g["prop"]:
            g["prop"] = prop

    tot_ticks = sum(g["ticks"] for g in groups.values()) or 1
    tot_steps = sum(g["steps"] for g in groups.values()) + unmapped
    sel_mticks = sum(v for i, v in enumerate(pw) if i in wsel)
    out = []
    P = out.append
    P(f"logs: {', '.join(a.logs)}")
    P(f"[smc-prof] windows: {len(pw)} (counted {len([i for i in range(len(pw)) if i in wsel])}); "
      f"Mticks per window: {' '.join(f'{v:.0f}' for v in pw)}")
    secs = max(1, len([i for i in range(len(pw)) if i in wsel])) * 5.0
    P(f"SMC path in the counted windows: {sel_mticks:.0f} Mticks = {sel_mticks / a.tsc_ghz:.0f} ms of SPU"
      f" thread time over ~{secs:.0f} s = {sel_mticks / a.tsc_ghz / secs / 10:.2f}% of one thread"
      + (f", {sel_mticks / a.tsc_ghz / (secs * a.fps):.3f} ms/frame" if a.fps else ""))
    P(f"[interp-hist] windows: {len(hw)}; steps counted {tot_steps:.0f} ({unmapped:.0f} not mapped to an entry)")
    P(f"entries: {len(entries)} ({len(prof)} with code bytes, {len(smc)} [spu-smc] lines); LS dumps: {len(dumps)}")
    P("")
    by_cls = collections.Counter()
    for g in groups.values():
        by_cls[g["cls"]] += g["ticks"]
    P("ticks by class: " + ", ".join(f"{c} {100.0 * t / tot_ticks:.1f}%" for c, t in by_cls.most_common()))
    P("")
    P(f"{'rank':>4} {'share':>6} {'cum':>6} {'Mticks':>8} {'calls':>8} {'steps':>9} {'npos':>4} class / target / detail")
    cum = 0.0
    ranked = sorted(groups.values(), key=lambda g: (-g["ticks"], -g["steps"], -g["total"]))
    for r, g in enumerate(ranked[:a.top], 1):
        share = 100.0 * g["ticks"] / tot_ticks
        cum += share
        t = g["target"]
        if g["cls"].startswith("MODULE"):
            tgt = f"{t[0]} 0x{t[1]:X}"
        elif t[0] == "fragcopy":
            tgt = f"copy from EA 0x{t[1]:X}, entry EA 0x{t[2]:X}"
        elif t[0] == "frag":
            tgt = f"frag 0x{t[1]:X} EA 0x{t[2]:X}"
        elif t[0] == "eboot":
            tgt = f"EA 0x{t[1]:X}"
        elif t[0] in ("mod", "wl"):
            tgt = f"{t[1]} 0x{t[2]:X}"
        elif t[0] == "code":
            tgt = "code " + t[1][:24] + ".."
        else:
            tgt = f"ovl {t[0]} 0x{t[1]:X}"
        pos = sorted(set(g["pos"]))
        P(f"{r:4} {share:5.1f}% {cum:5.1f}% {g['ticks'] / 1e6:8.1f} {g['calls']:8} {g['steps']:9.0f} {len(pos):4} "
          f"{g['cls']} {tgt} ovl={','.join(map(str, sorted(g['ovl'])))}")
        P(f"{'':45}LS {' '.join(f'0x{p:05X}' for p in pos[:6])}{' ..' if len(pos) > 6 else ''}")
        for d in sorted(g["details"]):
            if d:
                P(f"{'':45}{d}")
    P("")

    # ---- proposals
    extra_path = os.path.join(ROOT, "tools", "jobmods_extra_funcs.json")
    frag_path = os.path.join(ROOT, "tools", "job_fragments.json")
    extra_old = json.load(open(extra_path))
    frags_old = json.load(open(frag_path))
    extra = {k: list(v) for k, v in extra_old.items()}
    frags = [dict(f) for f in frags_old]
    added = []
    for g in ranked:
        if not g["prop"] or 100.0 * g["ticks"] / tot_ticks < a.min_share:
            continue
        p = g["prop"]
        if p[0] == "extra":
            lst = extra.setdefault(p[1], [])
            if p[2] not in [int(x, 0) for x in lst]:
                lst.append(f"0x{p[2]:x}")
                added.append(f"extra   {p[1]} 0x{p[2]:X} ({100.0 * g['ticks'] / tot_ticks:.1f}%)")
        elif p[0] == "entry":
            for f in frags:
                if int(f["ea"], 0) == p[1]:
                    ents = f.setdefault("entries", [])
                    if p[2] not in [int(x, 0) for x in ents]:
                        ents.append(f"0x{p[2]:x}")
                        added.append(f"entry   fragment 0x{p[1]:X} EA 0x{p[2]:X} ({100.0 * g['ticks'] / tot_ticks:.1f}%)"
                                     + ("  WARNING: fragment start not resident at some LS positions"
                                        if any("NOT resident" in d for d in g["details"]) else ""))
                    break
        elif p[0] == "newfrag":
            if not any(int(f["ea"], 0) == p[1] for f in frags):
                nf = {"ea": f"0x{p[1]:x}", "span": f"0x{p[2]:x}", "seen": True}
                if p[3] != p[1]:
                    nf["entries"] = [f"0x{p[3]:x}"]
                frags.append(nf)          # appended: image ids 300+ of the others stay
                added.append(f"newfrag EA 0x{p[1]:X} span 0x{p[2]:X} entry 0x{p[3]:X} "
                             f"({100.0 * g['ticks'] / tot_ticks:.1f}%)")
    for k in extra:
        extra[k] = sorted(set(extra[k]), key=lambda x: int(x, 0))
    for f in frags:
        if "entries" in f:
            f["entries"] = sorted(set(f["entries"]), key=lambda x: int(x, 0))
    new_extra = json.dumps(extra, indent=1) + "\n"
    new_frags = json.dumps(frags, indent=1) + "\n"
    ep = os.path.join(a.out, "jobmods_extra_funcs.proposed.json")
    fp = os.path.join(a.out, "job_fragments.proposed.json")
    open(ep, "w", newline="\n").write(new_extra)
    open(fp, "w", newline="\n").write(new_frags)
    P(f"proposals (targets >= {a.min_share}% of counted ticks): {len(added)}")
    for s in added:
        P("  " + s)
    for old, new, name, path in ((json.dumps(extra_old, indent=1) + "\n", new_extra, "jobmods_extra_funcs.json", ep),
                                 (json.dumps(frags_old, indent=1) + "\n", new_frags, "job_fragments.json", fp)):
        d = list(difflib.unified_diff(old.splitlines(), new.splitlines(), "tools/" + name, path, lineterm="", n=1))
        P(f"diff {name}: {'(none)' if not d else ''}")
        out.extend("  " + l for l in d)
    P("")
    P("relift (from ascension\\, only after the go-ahead; Idle priority):")
    P(f"  copy {ep} tools\\jobmods_extra_funcs.json")
    P(f"  copy {fp} tools\\job_fragments.json")
    P("  .\\scripts\\windows\\relift-spu.ps1 -Out build\\spu-relift     # xregs form, as src\\spu; re-applies the wrappers")
    P("  python tools\\smc_entries.py --diff-trees build\\spu-relift src\\spu   # only the touched images may differ")
    P("  .\\scripts\\windows\\relift-spu.ps1 -Install                  # backs src\\spu up to build\\spu-backup-<time>")
    P("  then agent-build.ps1 -Dir agent11 and the rec1..rec4 replays")
    text = "\n".join(out) + "\n"
    open(os.path.join(a.out, "smc_entries.txt"), "w", newline="\n").write(text)
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
