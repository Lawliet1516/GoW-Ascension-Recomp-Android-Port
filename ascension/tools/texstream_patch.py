"""Hook the texture-streaming job's parameter block (func_00366C24) into host/ascension_vram_guard.cpp.

usage: python tools/texstream_patch.py src/recomp

func_00366C24 builds the 0x30-byte parameter block of the per-frame
"UpdateTexStreaming" SPU job and stores 7 at +0x20. This puts
`ppu_texstream_budget(ctx);` right after that store (r29 = the block), so
TEX_STREAM_BUDGET can change the 7. Without TEX_STREAM_BUDGET the hook
returns at once and nothing changes. Idempotent; run it again after a relift.
"""
import glob
import os
import sys

FN = "func_00366C24"
DECL = 'extern "C" void ppu_texstream_budget(ppu_context* ctx);   /* tools/texstream_patch.py */'
SITE = "        vm_write16(ctx->gpr[29] + 0x20, ctx->gpr[3]);"
STMT = "        ppu_texstream_budget(ctx);   /* TEX_STREAM_BUDGET */"


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
            s = text.find(SITE + nl, i, end)
            if s < 0:
                sys.exit("%s: the +0x20 store is not where it was in %s" % (path, FN))
            j = s + len(SITE) + len(nl)
            if text[j:j + len(STMT) + len(nl)] == STMT + nl:
                print("%s: already hooked" % path)
                continue
            text = text[:i] + DECL + nl + text[i:j] + STMT + nl + text[j:]
            open(path, "w", encoding="latin-1", newline="").write(text)
            print("%s: hooked %s" % (path, FN))
    if not found:
        sys.exit("%s not found" % FN)


if __name__ == "__main__":
    main()
