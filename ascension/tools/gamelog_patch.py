"""Hook the game's own debug print (func_001B16E4) into host/ascension_vram_guard.cpp.

usage: python tools/gamelog_patch.py src/recomp

In the retail executable func_001B16E4(level, fmt, ...) only spills r5..r10 to
the caller's parameter save area and returns: the developers' log lines
("starting local multiplayer", "enter lobby: ...", "start local game", ...)
go nowhere. This puts `ppu_game_log(ctx);` before its return, so PS3_GAME_LOG=1
prints them ("[game] ..."). Without PS3_GAME_LOG the hook returns at once.
Idempotent; run it again after a relift.
"""
import glob
import os
import sys

FN = "func_001B16E4"
DECL = 'extern "C" void ppu_game_log(ppu_context* ctx);   /* tools/gamelog_patch.py */'
SITE = "        vm_write64(ctx->gpr[3] + 0x68, ctx->gpr[10]);"
STMT = "        ppu_game_log(ctx);   /* PS3_GAME_LOG */"


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
            end = text.find(nl + "}" + nl, i)
            if end < 0:
                sys.exit("%s: the end of %s was not found" % (path, FN))
            s = text.find(SITE + nl, i, end)
            if s < 0:
                sys.exit("%s: the r10 spill is not where it was in %s" % (path, FN))
            j = s + len(SITE) + len(nl)
            if text[j:j + len(STMT) + len(nl)] == STMT + nl:
                print("%s: already hooked" % path)
                continue
            if not text.startswith("        return;" + nl, j):
                sys.exit("%s: %s does not return right after the r10 spill: the stub changed" % (path, FN))
            text = text[:i] + DECL + nl + text[i:j] + STMT + nl + text[j:]
            tmp = path + ".tmp"
            open(tmp, "w", encoding="latin-1", newline="").write(text)
            os.replace(tmp, path)
            print("%s: hooked %s" % (path, FN))
    if not found:
        sys.exit("%s not found" % FN)


if __name__ == "__main__":
    main()
