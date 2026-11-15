# Online multiplayer: post-lift source patches for the translated game code (src\recomp), run by
# scripts\windows\prepare.ps1 and apply-patches.ps1 after the PPU lift. Each one replaces exactly one translated
# statement; the online ones are OFF unless the runtime switches them on through mpnet_flag()
# (ps3kit/libs/network/mpnet.c: only while online play through a community server is on):
#   1 SSL off      the game's matchmaker connection without its SSL wrapper (two config bytes, 0x60D/0x60E), so it
#                  can talk to a community server (which has no Sony certificate).
#   2 TSS plain    the downloaded match settings (TSS) are read as plain text: a community server cannot sign them.
# Two NULL guards (not switched; they only act where a NULL would otherwise hang the game): func_00158014 with a
# NULL owner and func_00157F4C walking a NULL list (a freeze seen in an online match after a 4 s host gap).
# The game's Online Pass check is NOT patched here and must never be: every player needs their own pass.
# Usage: python -I mksrc.py <recomp_dir>     (idempotent: a patch already in place is skipped)
import sys, os, hashlib
D = sys.argv[1]
PATCHES = [
    ("ppu_recomp_005.cpp",
     "        vm_write8(ctx->gpr[29] + 0x60E, ctx->gpr[30]);",
     "        vm_write8(ctx->gpr[29] + 0x60E, mpnet_flag(1) ? 0 : ctx->gpr[30]);  /* mpnet: SSL off */"),
    ("ppu_recomp_005.cpp",
     "        ctx->lr = 0x003319C0; func_003E28E8(ctx); DRAIN_TRAMPOLINE(ctx);",
     "        if (mpnet_flag(2)) { ctx->gpr[3] = 1; } else { ctx->lr = 0x003319C0; func_003E28E8(ctx); DRAIN_TRAMPOLINE(ctx); }  /* mpnet: TSS plaintext */"),
    # 2026-11-13 (MEASURED run 113904): with only +0x60E cleared the client still sent an SSL 3.0 ClientHello.
    # SamConn_factory needs BOTH conn +0x52 (from config +0x60D, read at 0x3D4E24) and +0x53 at 0 for plain TCP.
    ("ppu_recomp_007.cpp",
     "        ctx->gpr[24] = vm_read8(ctx->gpr[4] + 0x60D);",
     "        ctx->gpr[24] = mpnet_flag(1) ? 0 : vm_read8(ctx->gpr[4] + 0x60D);  /* mpnet: SSL off, second byte 0x3D4E24 */"),
    ("ppu_recomp_007.cpp",
     "        ctx->gpr[5] = vm_read8(ctx->gpr[4] + 0x60E);",
     "        ctx->gpr[5] = mpnet_flag(1) ? 0 : vm_read8(ctx->gpr[4] + 0x60E);  /* mpnet: SSL off at the reader 0x3D4E38 */"),
    # 2026-11-14 guards (NOT switch-gated; they only act on a NULL that would otherwise hang the game). MEASURED: a
    # laptop froze in an online match after a 4.4 s host gap: func_00158014 got owner r4 = NULL (lr 0x00542E60), the
    # list head became NULL+0x1314+0xF0 and func_00157F4C walked zero-filled memory forever (a PS3 would fault).
    ("ppu_recomp_002.cpp",
     "void func_00158014(ppu_context* ctx) {",
     "void func_00158014(ppu_context* ctx) { if ((uint32_t)ctx->gpr[4] == 0) { static int said; if (!said) { said = 1; fprintf(stderr, \"[mpnet-guard] func_00158014: owner is NULL (lr 0x%08X): list removal skipped%c\", (unsigned)ctx->lr, 10); } return; }  /* mpnet guard 2026-11-14 */"),
    ("ppu_recomp_002.cpp",
     "loc_00157F60:",
     "loc_00157F60: if ((uint32_t)ctx->gpr[3] == 0) goto loc_00157FB8;  /* mpnet guard 2026-11-14: a NULL link never reaches the list head */"),
]
DECL = '\nextern "C" int mpnet_flag(int which);   /* mpnet private build: ps3kit-mpnet libs/network/mpnet.c */'
HDR = '#include "ppu_recomp.h"'
files = {}
for fn, old, new in PATCHES:
    if fn not in files:
        files[fn] = open(os.path.join(D, fn), "r", encoding="latin1", newline="").read()
    src = files[fn]
    if src.count(new) == 1:
        print("already in place:", fn, new.strip()[:60]); continue
    n = src.count(old)
    if n != 1:
        print("pattern count %d (want 1) in %s: %r" % (n, fn, old)); sys.exit(1)
    files[fn] = src.replace(old, new)
    print("patched", fn, ":", new.strip()[:70])
for fn, src in files.items():
    if "extern \"C\" int mpnet_flag(" not in src:
        assert src.count(HDR) == 1
        src = src.replace(HDR, HDR + DECL, 1)
    open(os.path.join(D, fn), "w", encoding="latin1", newline="").write(src)
    print("wrote", fn, hashlib.md5(src.encode("latin1")).hexdigest()[:8])
