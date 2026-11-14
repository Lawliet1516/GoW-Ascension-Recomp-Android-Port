"""Hook the Z pre-pass builder (func_00411F2C) into host/ascension_vram_guard.cpp.

usage: python tools/zprepass_patch.py src/recomp

Puts `if (ppu_zprepass_skip(ctx)) return;` at the top of the lifted
func_00411F2C (fxZPrePass's per-frame draw-list builder). Without
PPU_ZPREPASS_SKIP / PPU_ZPREPASS_SKIP_AB the hook returns 0 and nothing
changes. Idempotent; run it again after a relift.
"""
import glob
import os
import sys

FN = "func_00411F2C"
DECL = 'extern "C" int ppu_zprepass_skip(ppu_context* ctx);   /* tools/zprepass_patch.py */'
STMT = "if (ppu_zprepass_skip(ctx)) return;   /* PPU_ZPREPASS_SKIP */"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    found = False
    for d in sys.argv[1:]:
        for path in sorted(glob.glob(os.path.join(d, "ppu_recomp_*.cpp"))):
            text = open(path, encoding="latin-1", newline="").read()
            head = "void %s(ppu_context* ctx) {" % FN
            i = text.find(head)
            if i < 0:
                continue
            found = True
            k = text.find("\n")
            nl = "\r\n" if k > 0 and text[k - 1] == "\r" else "\n"
            hook = "        " + STMT + nl
            j = i + len(head) + len(nl)
            if text[j:j + len(hook)] == hook:
                print("%s: already hooked" % path)
                continue
            text = text[:i] + DECL + nl + head + nl + hook + text[j:]
            open(path, "w", encoding="latin-1", newline="").write(text)
            print("%s: hooked %s" % (path, FN))
    if not found:
        sys.exit("%s not found" % FN)


if __name__ == "__main__":
    main()
