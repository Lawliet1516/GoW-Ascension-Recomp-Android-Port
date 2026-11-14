"""Diagnostic: scope the VRAM manager's lifted functions for PPU_VRAM_TRACE.

usage: python tools/vram_trace_patch.py apply|remove src/recomp [src/recomp-lean ...]

apply puts, at the top of each function in FUNCS,
    ppu_vram_scope_t _vscope(ctx, 0x<addr>u);   /* PPU_VRAM_TRACE */
whose constructor/destructor call ppu_vram_enter/ppu_vram_leave
(host/ascension_vram_guard.cpp): with PPU_VRAM_TRACE=1 they keep a ring of
the manager's calls, report two threads inside it at once and print the ring
at the first bad copy/move/shrink. remove takes every such line out again.
Idempotent either way.
"""
import glob
import os
import sys

FUNCS = ["00372B60", "00373680", "00370E38", "00370FF4", "0037144C", "0037130C",
         "003707D8", "00370724", "00370D7C", "0036F6FC", "0036F7B0", "00370054",
         "003717A4", "00374A70", "00374EBC", "00366E18", "003729D8", "00372070",
         "00372630", "0037122C", "0036E480", "00370B38", "00370BF8", "003721B8",
         "003723E0", "0036DAD4", "0036DD0C", "0036E7F4", "0036E328", "003C98CC",
         "003CA738", "003763C8", "003761B8", "00375E3C", "0036ECC0", "0036FC38",
         "003C8E38", "003C8F54"]
MARK = "/* PPU_VRAM_TRACE */"
DECL = ('extern "C" void ppu_vram_enter(ppu_context* ctx, uint32_t fn); '
        'extern "C" void ppu_vram_leave(ppu_context* ctx, uint32_t fn); '
        'struct ppu_vram_scope_t { ppu_context* c; uint32_t f; '
        'ppu_vram_scope_t(ppu_context* c_, uint32_t f_) : c(c_), f(f_) { ppu_vram_enter(c, f); } '
        '~ppu_vram_scope_t() { ppu_vram_leave(c, f); } };   ' + MARK)


def newline_of(text):
    k = text.find("\n")
    return "\r\n" if k > 0 and text[k - 1] == "\r" else "\n"


def apply(text, nl, done):
    changed = False
    for fn in FUNCS:
        head = "void func_%s(ppu_context* ctx) {" % fn
        i = text.find(head)
        if i < 0:
            continue
        done.add(fn)
        hook = "        ppu_vram_scope_t _vscope(ctx, 0x%su);   %s" % (fn, MARK) + nl
        j = i + len(head) + len(nl)
        if text[j:j + len(hook)] == hook:
            continue
        text = text[:j] + hook + text[j:]
        if DECL not in text:   # once, ahead of the file's first function
            k = text.find(nl + "void func_") + len(nl)
            text = text[:k] + DECL + nl + text[k:]
        changed = True
    return text, changed


def remove(text, nl):
    lines = text.split(nl)
    kept = [l for l in lines if MARK not in l or "PPU_VRAM_TRACE */" not in l or
            not (l.strip().startswith("ppu_vram_scope_t _vscope") or l.startswith('extern "C" void ppu_vram_enter'))]
    return nl.join(kept), len(kept) != len(lines)


mode, dirs = sys.argv[1], sys.argv[2:]
for d in dirs:
    done = set()
    for path in sorted(glob.glob(os.path.join(d, "ppu_recomp_*.cpp"))):
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            text = f.read()
        nl = newline_of(text)
        if mode == "apply":
            text, c = apply(text, nl, done)
        else:
            text, c = remove(text, nl)
        if c:
            with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(text)
            print(mode, path)
    if mode == "apply" and set(FUNCS) - done:
        sys.exit("%s: not found: %s" % (d, ", ".join(sorted(set(FUNCS) - done))))
    print(d, "ok")
