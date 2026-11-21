#!/usr/bin/env python3
"""Independent model of the SPU single-precision float semantics (exact rationals, no host float), used to check the
exact C reference (spu_*_ref) that the AArch64 fast path is compared against.

Semantics modelled (Cell BE SPU, as RPCS3 'precise'): value = (-1)^s * 1.m * 2^(e-127) for biased exponent e = 1..255
(e = 255 is an ordinary number, no Inf/NaN); e = 0 (zero or denormal) is read as +-0; results are TRUNCATED toward zero to 24
bits; a result below 2^-126 is flushed to +0; a result >= 2^128 saturates to sign|0x7FFFFFFF; a zero result is +0; fma is
a single rounding of the exact a*b+c.   usage: spu_float_model.py dump.bin [stride]  (checks every stride-th record; the exact-rational model is slow)"""
import struct, sys
from fractions import Fraction

def val(x):
    e = (x >> 23) & 0xFF
    if e == 0: return Fraction(0)
    m = (x & 0x7FFFFF) | 0x800000
    v = Fraction(m, 1 << 23) * (Fraction(2) ** (e - 127))
    return -v if x >> 31 else v

def pack(v):
    if v == 0: return 0
    s = 0x80000000 if v < 0 else 0
    v = abs(v)
    # exponent: largest E with 2^E <= v
    n, d = v.numerator, v.denominator
    E = n.bit_length() - d.bit_length()
    if Fraction(2) ** E > v: E -= 1
    if E < -126: return 0                      # flush (below the smallest normal)
    if E > 128 or (E == 128 and False): return s | 0x7FFFFFFF
    # 24-bit truncated significand: floor(v / 2^(E-23))
    q = v / (Fraction(2) ** (E - 23))
    m = q.numerator // q.denominator           # in [2^23, 2^24)
    if E >= 129: return s | 0x7FFFFFFF         # >= 2^129 cannot happen for E<=128 handled above
    biased = E + 127
    if biased > 255: return s | 0x7FFFFFFF
    return s | (biased << 23) | (m & 0x7FFFFF)

def fa(a, b): return pack(val(a) + val(b))
def fs(a, b): return pack(val(a) - val(b))
def fm(a, b): return pack(val(a) * val(b))
def fma(a, b, c): return pack(val(a) * val(b) + val(c))
def fms(a, b, c): return pack(val(a) * val(b) - val(c))
def fnms(a, b, c): return pack(val(c) - val(a) * val(b))

def main():
    data = open(sys.argv[1], 'rb').read(); stride = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    n = len(data) // 36; bad = {}; shown = 0; checked = 0
    names = ['fa', 'fs', 'fm', 'fma', 'fms', 'fnms']
    for i in range(0, n, stride):
        checked += 1
        r = struct.unpack_from('<9I', data, 36 * i)
        a, b, c = r[0:3]
        want = [fa(a, b), fs(a, b), fm(a, b), fma(a, b, c), fms(a, b, c), fnms(a, b, c)]
        for k, nm in enumerate(names):
            if r[3 + k] != want[k]:
                bad[nm] = bad.get(nm, 0) + 1
                if shown < 15:
                    shown += 1; print(f"DIFF {nm} a={a:08x} b={b:08x} c={c:08x} C-ref={r[3+k]:08x} model={want[k]:08x}")
    print(f"checked {checked} of {n} records x 6 ops against the independent model: {'ALL EQUAL' if not bad else 'DIFFERENCES ' + str(bad)}")
    return 1 if bad else 0
sys.exit(main())
