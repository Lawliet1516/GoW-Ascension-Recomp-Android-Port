#!/usr/bin/env python3
"""Apply the SPU_LIFT_DIRECT / SPU_LIFT_CALLTAIL / SPU_LIFT_CHFAST code forms
(spu_lifter.py, my-changes/spu-cg2/README.md) to an existing lift folder,
without relifting.

    python spu_direct_pass.py <lift folder> <new folder> [--direct] [--calltail] [--chfast]

Copies every file of <lift folder> (e.g. ascension/src/spu) to <new folder>
and rewrites each <image>/spu_recomp.c line by line with the same functions
the lifter uses (direct_exits, calltail_brackets, chfast_channels), so the
result is what a relift with those switches gives from the same lift, and
nothing else changes. --direct leaves position-independent images (pic_*)
alone, as the lifter does. A relift (relift-spu.ps1) does not reproduce
src/spu byte for byte today (newer extra-function lists and fragment
tables), which is why this pass exists."""
import argparse, os, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spu_lifter import direct_exits, calltail_brackets, chfast_channels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("dst")
    ap.add_argument("--direct", action="store_true")
    ap.add_argument("--calltail", action="store_true")
    ap.add_argument("--chfast", action="store_true")
    a = ap.parse_args()
    if os.path.abspath(a.src) == os.path.abspath(a.dst):
        sys.exit("dst must differ from src")
    if os.path.exists(a.dst):
        shutil.rmtree(a.dst)
    # Fresh timestamps: a build that used another form of this folder must
    # recompile every image (ninja compares times).
    shutil.copytree(a.src, a.dst, copy_function=shutil.copy)
    tot = [0, 0, 0]
    for img in sorted(os.listdir(a.dst)):
        p = os.path.join(a.dst, img, "spu_recomp.c")
        if not os.path.isfile(p):
            continue
        raw = open(p, "rb").read().decode("utf-8")
        eol = "\r\n" if "\r\n" in raw else "\n"
        lines = raw.split(eol)
        pic = "_pic = ctx->pic_delta" in raw
        out = lines
        if a.direct and not pic:
            out = direct_exits(out)
        if a.calltail:
            out = calltail_brackets(out)
        if a.chfast:
            out = chfast_channels(out)
        n = [sum(o.count(k) for o in out) for k in ("SPU_XFER", "SPU_CALL_END", "spu_wrch_k(") ]
        n[2] += sum(o.count("spu_rdch_k(") for o in out)
        tot = [x + y for x, y in zip(tot, n)]
        if out != lines:
            open(p, "wb").write(eol.join(out).encode("utf-8"))
        print(f"{img}: {'pic, ' if pic else ''}{n[0]} direct exits, {n[1]} call ends, {n[2]} channel sites")
    print(f"total: {tot[0]} direct exits, {tot[1]} call ends, {tot[2]} channel sites")


if __name__ == "__main__":
    main()
