#!/usr/bin/env python3
"""
stop0_decode.py -- read SPU_STOP0_DIAG's output (the `[spu] stop 0 in job
module (first)` line and its local-store dump) for God of War: Ascension's
module 216 command interpreter, and say what it choked on.

Module 216 (jobmod_759dedc8269b_e9DA8) runs a command stream: 0xD100 sets it
up, 0xD46C is the loop. Per command it reads the word at the command pointer
r83 (unaligned: two quadwords, selb, rotqby), takes the opcode (header >> 24)
and a table index ((header >> 14) & mask, >> 2), and jumps through the
handler table at LS 0xE850 -- unless the opcode is above 8 or the index above
61, which goes to 0xDB58 (`stop 0`). At the stop, r2/r3 hold the command
word, r82 the index, r83 the command pointer, r84 = 0xDB58.

Usage:
  python tools/stop0_decode.py <log with the stop 0 line> [--dump <spu_stop0_*.ls>]
"""
import argparse, re, struct, sys

RE = re.compile(r"\[spu\] stop (?:0|0x[0-9A-Fa-f]+) in job module \(first(?: at this site)?\): (.*)")


def regs(line):
    d = {}
    for k, v in re.findall(r"(\w+)=([0-9A-Fa-fx|]+)", line):
        d[k] = v
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--dump")
    a = ap.parse_args()
    line = None
    extra = []
    for l in open(a.log, encoding="utf-8", errors="replace"):
        m = RE.search(l)
        if m and not line:
            line = m.group(1)
        elif line and l.startswith("[spu] stop 0: ") and len(extra) < 6:
            extra.append(l.rstrip())   # the source GET, main memory vs LS, file paths
    for l in extra:
        print(l)
    if not line:
        sys.exit("no '[spu] stop 0 in job module (first)' line in " + a.log)
    r = regs(line)
    print("stop 0:", " ".join(f"{k}={r[k]}" for k in ("img", "ovl", "pc", "depth", "drain_ret") if k in r))
    w = int(r.get("r3", "0"), 16)
    r82 = int(r.get("r82", "0"), 16)
    r83 = int(r.get("r83", "0"), 16)
    print(f"command word r3=0x{w:08X} at LS 0x{r83 & 0x3FFFF:05X}: opcode {w >> 24} (valid 0..8), "
          f"table index r82={r82} (valid 0..61)")
    bad = []
    if (w >> 24) > 8:
        bad.append("opcode")
    if r82 > 61:
        bad.append("index")
    print("rejected for:", ", ".join(bad) or "(neither -- the registers moved on; read the dump)")
    if a.dump:
        ls = open(a.dump, "rb").read()
        c = r83 & 0x3FFF0
        print("\ncommand stream around r83 (big-endian words):")
        for q in range(max(0, c - 0x60), min(len(ls), c + 0x60), 16):
            ws = struct.unpack(">4I", ls[q:q + 16])
            mark = " <- r83" if q == c else ""
            print(f"  {q:05X}: " + " ".join(f"{x:08X}" for x in ws) + mark)
        print("\njob state the interpreter reads (LS 0xE840, 0xE990, 0xE9A0, 0xE9D0, 0xEA70):")
        for q in (0xE840, 0xE990, 0xE9A0, 0xE9D0, 0xEA70):
            ws = struct.unpack(">4I", ls[q:q + 16])
            print(f"  {q:05X}: " + " ".join(f"{x:08X}" for x in ws))
        print("\nhandler table at LS 0xE850 (62 entries):")
        tab = struct.unpack(">64I", ls[0xE850:0xE850 + 256])
        for i in range(0, 62, 8):
            print(f"  [{i:2}] " + " ".join(f"{x:05X}" for x in tab[i:i + 8]))


if __name__ == "__main__":
    main()
