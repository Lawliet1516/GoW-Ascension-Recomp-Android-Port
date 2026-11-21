#!/usr/bin/env python3
"""Hand-assembled SPU test program (no SPU toolchain needed): float ops whose result depends on the SPU float mode,
stored to LS 0x1000+, then `stop`. Writes prog.elf (wrapped with ps3kit/tools/wrap_spu_elf.py)."""
import struct, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', '..', 'ps3kit', 'tools'))
from wrap_spu_elf import wrap
def ri16(op, imm, rt): return (op << 23) | ((imm & 0xFFFF) << 7) | rt
def rr(op, rb, ra, rt): return (op << 21) | (rb << 14) | (ra << 7) | rt
def rrr(op, rt, rb, ra, rc): return (op << 28) | (rt << 21) | (rb << 14) | (ra << 7) | rc
def ri10(op, imm, ra, rt): return (op << 24) | ((imm & 0x3FF) << 14) | (ra << 7) | rt
ILHU, IOHL = 0x082, 0x0C1; FA, FS, FM = 0x2C4, 0x2C5, 0x2C6; FMA = 0xE; STQD = 0x24
code = [
    ri16(ILHU, 0x3F80, 10),            # r10 = 1.0
    ri16(ILHU, 0x4000, 11),            # r11 = 2.0
    ri16(ILHU, 0x33C0, 16),            # r16 = 1.5 * 2^-24  (0.75 ulp of 1.0)
    ri16(ILHU, 0x0040, 18),            # r18 = 2^-127 (a denormal)
    rr(FA, 11, 10, 12),                # r12 = 1+2   = 3.0   0x40400000
    rr(FM, 11, 12, 13),                # r13 = 3*2   = 6.0   0x40C00000
    rrr(FMA, 14, 11, 12, 10),          # r14 = 3*2+1 = 7.0   0x40E00000
    rr(FS, 10, 13, 15),                # r15 = 6-1   = 5.0   0x40A00000
    rr(FA, 16, 10, 17),                # r17 = 1 + 0.75ulp -> TRUNCATES to 1.0 (0x3F800000); round-to-nearest would give 0x3F800001
    rr(FM, 11, 18, 19),                # r19 = denormal*2 -> 0 (denormal read as zero); IEEE would give 0x00800000
    ri10(STQD, 0x100, 0, 12), ri10(STQD, 0x101, 0, 13), ri10(STQD, 0x102, 0, 14), ri10(STQD, 0x103, 0, 15),
    ri10(STQD, 0x104, 0, 17), ri10(STQD, 0x105, 0, 19),
    0x00000000,                        # stop 0
]
raw = b''.join(struct.pack('>I', w) for w in code)
open(os.path.join(os.path.dirname(__file__), 'prog.elf'), 'wb').write(wrap(raw, base=0, entry=0, symbols=[{"name": "main", "addr": 0, "size": len(raw)}]))
print("prog.elf", len(raw), "bytes")
