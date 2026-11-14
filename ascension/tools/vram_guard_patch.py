"""Hook God of War: Ascension's VRAM copy/move into host/ascension_vram_guard.cpp.

usage: python tools/vram_guard_patch.py src/recomp [src/recomp-lean ...]

1. At the top of the lifted func_00374A70 (CopyLocalMemory) and func_00374EBC
   (MoveLocalMemory), before anything they do:
       if (ppu_vram_guard(ctx, <0|1>)) return;
   which returns at once when PPU_VRAM_GUARD=1 and the arguments are ones the
   guest itself rejects.
2. Their own checks end in `tw 31,r1,r1` (an unconditional trap that ends the
   game on a PS3; the lifter emits it as a comment). Each becomes
       if (ppu_vram_trap(ctx, <0|1>)) goto <the function's epilogue>;
   so under PPU_VRAM_GUARD a bad extent found anywhere in the copy -- not only
   in its arguments -- abandons the call instead of walking on. Every lift
   reloads its registers from ctx at labels, so the extra jump is safe there.
3. At the top of func_00370E38 (shrinks a texture's block)
       if (ppu_vram_shrink_check(ctx)) return;
   which skips a shrink larger than what is left of the block (a texture
   picked twice; PPU_VRAM_SHRINK_SAFE=0 lets it through) and, with
   PPU_VRAM_SHRINK_LOG=1 / PPU_VRAM_FAULT=<n>, logs or provokes one.
4. At the top of func_0036EEF8 (writes a texture's RSX descriptor)
       ppu_vram_desc_check(ctx);
   which gives a low block that lost its streamed flag the flag back before
   the descriptor is written (PPU_VRAM_REPAIR).

Idempotent; a relift rewrites the files, so run it again after one
(apply-patches.ps1 does not).
"""
import glob
import os
import sys

GUARD_DECL = 'extern "C" int ppu_vram_guard(ppu_context* ctx, int kind);   /* tools/vram_guard_patch.py */'
# function -> (statement at its top, the declaration it needs)
TARGETS = {
    "func_00374A70": ("if (ppu_vram_guard(ctx, 0)) return;   /* PPU_VRAM_GUARD */", GUARD_DECL),
    "func_00374EBC": ("if (ppu_vram_guard(ctx, 1)) return;   /* PPU_VRAM_GUARD */", GUARD_DECL),
    "func_00370E38": ("if (ppu_vram_shrink_check(ctx)) return;   /* PPU_VRAM_SHRINK_SAFE */",
                      'extern "C" int ppu_vram_shrink_check(ppu_context* ctx);   /* tools/vram_guard_patch.py */'),
    "func_0036EEF8": ("ppu_vram_desc_check(ctx);   /* PPU_VRAM_REPAIR */",
                      'extern "C" void ppu_vram_desc_check(ppu_context* ctx);   /* tools/vram_guard_patch.py */'),
}
# function -> (its epilogue label, kind)
EPILOGUE = {"func_00374A70": ("loc_00374E70", 0), "func_00374EBC": ("loc_00375190", 1)}
TRAP_DECL = 'extern "C" int ppu_vram_trap(ppu_context* ctx, int kind);   /* tools/vram_guard_patch.py */'
TRAP_MARK = "/* trap: tw 31, r1, r1"


def newline_of(text):
    k = text.find("\n")
    return "\r\n" if k > 0 and text[k - 1] == "\r" else "\n"


def patch_entries(text, nl, done):
    changed = False
    for fn, (stmt, decl) in TARGETS.items():
        head = "void %s(ppu_context* ctx) {" % fn
        i = text.find(head)
        if i < 0:
            continue
        done.add(fn)
        hook = "        " + stmt + nl
        j = i + len(head) + len(nl)
        if text[j:j + len(hook)] == hook:
            continue
        text = text[:i] + decl + nl + head + nl + hook + text[j:]
        changed = True
    return text, changed


def patch_traps(text, nl):
    changed = False
    for fn, (label, kind) in EPILOGUE.items():
        head = "void %s(ppu_context* ctx) {" % fn
        i = text.find(head)
        if i < 0:
            continue
        end = text.find(nl + "void func_", i + len(head))
        end = len(text) if end < 0 else end
        body = text[i:end]
        if (label + ":") not in body:
            sys.exit("%s: epilogue %s not found" % (fn, label))
        lines, n = body.split(nl), 0
        for k, line in enumerate(lines):
            if TRAP_MARK in line and "ppu_vram_trap" not in line:
                lines[k] = line + " if (ppu_vram_trap(ctx, %d)) goto %s;   /* PPU_VRAM_GUARD */" % (kind, label)
                n += 1
        if not n:
            continue
        body = nl.join(lines)
        text = text[:i] + body + text[end:]
        if TRAP_DECL not in text:
            text = text[:i] + TRAP_DECL + nl + text[i:]
        changed = True
    return text, changed


def patch_dir(d):
    done = set()
    for path in sorted(glob.glob(os.path.join(d, "ppu_recomp_*.cpp"))):
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            text = f.read()
        nl = newline_of(text)
        text, c1 = patch_entries(text, nl, done)
        text, c2 = patch_traps(text, nl)
        if c1 or c2:
            with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(text)
            print("patched", path)
    missing = set(TARGETS) - done
    if missing:
        sys.exit("%s: not found: %s" % (d, ", ".join(sorted(missing))))
    print(d, "ok")


for d in sys.argv[1:]:
    patch_dir(d)
