"""Give God of War: Ascension's VRAM copy plan room past its 32 entries.

usage: python tools/vram_plan_patch.py apply|remove src/recomp [src/recomp-lean ...]

The VRAM manager keeps a plan of instance copies -- {dst, src, size}, 12
bytes each, 32 of them at 0x96E58C, the count at 0x96E70C and the queued-move
count at 0x96E710 right behind them. func_0036ECC0 (the stream loader's
texture-instance callback) appends to it with no bound check, and
func_00372B60 drains it only while the loader is idle. On a PS3 the disc keeps
the loader waiting often enough; here reads are instant, the loader seldom
idles, and the 33rd entry overwrote the counts: the next drain and move flush
walked garbage ("invalid size", thousands of "CopyLocalMemory ... offset
error" lines), textures were scrambled and the game froze.

apply makes three edits (host/ascension_vram_guard.cpp has the functions):
1. func_0036ECC0: the entry is stored by ppu_vram_plan_put, which keeps
   entries past 32 on the host instead of over the counts.
2. func_00372B60: the drain takes at most 32 entries (ppu_vram_plan_batch),
   as many as the game's list could ever hold.
3. func_00372B60: instead of zeroing the count, ppu_vram_plan_consume moves
   the entries not yet drained to the front and returns how many; while some
   are left, the drain skips the compaction after it (as when the loader is
   busy), so no entry is left pointing at a block that compaction moved.
remove restores the original lines. Idempotent; a relift rewrites the files,
so run it again after one.
"""
import glob
import os
import re
import sys

MARK = "/* PPU_VRAM_PLAN */"
DECL = ('extern "C" void ppu_vram_plan_put(uint32_t base, uint32_t off, uint32_t w0, uint32_t w1, uint32_t w2); '
        'extern "C" uint32_t ppu_vram_plan_batch(uint32_t count); '
        'extern "C" uint32_t ppu_vram_plan_consume(void);   ' + MARK)
R = r"(?:ctx->gpr\[\d+\]|r\d+_)"


def newline_of(text):
    k = text.find("\n")
    return "\r\n" if k > 0 and text[k - 1] == "\r" else "\n"


def body(text, name):
    """(start, end) of the lifted function's text, or None."""
    head = "void %s(ppu_context* ctx) {" % name
    a = text.find(head)
    if a < 0:
        return None
    b = text.find("\nvoid func_", a + len(head))
    return a, (b if b >= 0 else len(text))


def apply(text, nl):
    if MARK in text:
        return text, False
    b1 = body(text, "func_0036ECC0")
    b2 = body(text, "func_00372B60")
    if not b1 or not b2:
        return text, False
    # 1. the entry store in func_0036ECC0
    f = text[b1[0]:b1[1]]
    m = re.search(r"( *)vm_write32\(\((%s) \+ (%s)\), (%s)\);(?: *//[^\n]*)?\r?\n"
                  r" *vm_write32\((%s) \+ 0x4, (%s)\);\r?\n"
                  r" *vm_write32\(\5 \+ 0x8, (%s)\);\r?\n" % (R, R, R, R, R, R), f)
    if not m:
        raise SystemExit("func_0036ECC0: entry store not found")
    ind, base, off, w0, w1, w2 = m.group(1), m.group(2), m.group(3), m.group(4), m.group(6), m.group(7)
    new = ("%sppu_vram_plan_put((uint32_t)%s, (uint32_t)%s, (uint32_t)%s, (uint32_t)%s, (uint32_t)%s);   %s%s"
           "%s/* PPU_VRAM_PLAN was: vm_write32((%s + %s), %s); vm_write32(%s + 0x4, %s); vm_write32(%s + 0x8, %s); */%s"
           % (ind, base, off, w0, w1, w2, MARK, nl, ind, base, off, w0, m.group(5), w1, m.group(5), w2, nl))
    f = f[:m.start()] + new + f[m.end():]
    text = text[:b1[0]] + f + text[b1[1]:]
    # 2. and 3. the drain in func_00372B60
    b2 = body(text, "func_00372B60")
    f = text[b2[0]:b2[1]]
    m = re.search(r"( *)(%s) = vm_read32\((%s) \+ -0x18F4\);" % (R, R), f)
    if not m:
        raise SystemExit("func_00372B60: count read not found")
    f = (f[:m.start()] + "%s%s = ppu_vram_plan_batch((uint32_t)vm_read32(%s + -0x18F4));   %s"
         % (m.group(1), m.group(2), m.group(3), MARK) + f[m.end():])
    m = re.search(r"( *)vm_write32\((%s) \+ -0x18F4, (%s)\);" % (R, R), f)
    if not m:
        raise SystemExit("func_00372B60: count reset not found")
    if "loc_00372DE4:" not in f:
        raise SystemExit("func_00372B60: loc_00372DE4 not found")
    f = (f[:m.start()] + "%s{ const uint32_t _rem = ppu_vram_plan_consume(); vm_write32(%s + -0x18F4, _rem);"
         " if (_rem) goto loc_00372DE4; }   %s /* was: vm_write32(%s + -0x18F4, %s); */"
         % (m.group(1), m.group(2), MARK, m.group(2), m.group(3)) + f[m.end():])
    text = text[:b2[0]] + f + text[b2[1]:]
    # the declarations, before the first lifted function
    k = text.find("\nvoid func_")
    text = text[:k + 1] + DECL + nl + text[k + 1:]
    return text, True


def remove(text, nl):
    if MARK not in text:
        return text, False
    out = []
    lines = text.split(nl)
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith('extern "C" void ppu_vram_plan_put('):
            i += 1
            continue
        if "ppu_vram_plan_put(" in ln and MARK in ln:
            ind = ln[:len(ln) - len(ln.lstrip())]
            m = re.search(r"was: (vm_write32.*); (vm_write32.*); (vm_write32.*); \*/", lines[i + 1])
            out += [ind + m.group(1) + ";", ind + m.group(2) + ";", ind + m.group(3) + ";"]
            i += 2
            continue
        if "ppu_vram_plan_batch(" in ln and MARK in ln:
            out.append(re.sub(r"ppu_vram_plan_batch\(\(uint32_t\)(vm_read32\([^;]*\))\);   /\* PPU_VRAM_PLAN \*/",
                              r"\1;", ln))
            i += 1
            continue
        if "ppu_vram_plan_consume(" in ln and MARK in ln:
            ind = ln[:len(ln) - len(ln.lstrip())]
            out.append(ind + re.search(r"was: (vm_write32[^*]*;) \*/", ln).group(1))
            i += 1
            continue
        out.append(ln)
        i += 1
    return nl.join(out), True


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("apply", "remove"):
        raise SystemExit(__doc__)
    fn = apply if sys.argv[1] == "apply" else remove
    for d in sys.argv[2:]:
        hits = 0
        for path in sorted(glob.glob(os.path.join(d, "ppu_recomp_*.cpp"))):
            with open(path, "rb") as fh:
                raw = fh.read().decode("latin-1")
            if "void func_0036ECC0(ppu_context* ctx) {" not in raw:
                continue
            nl = newline_of(raw)
            new, changed = fn(raw, nl)
            if changed:
                with open(path, "wb") as fh:
                    fh.write(new.encode("latin-1"))
                print("%s %s" % (sys.argv[1], path))
            hits += 1
        print("%s %s" % (d, "ok" if hits else "no file with func_0036ECC0"))


if __name__ == "__main__":
    main()
