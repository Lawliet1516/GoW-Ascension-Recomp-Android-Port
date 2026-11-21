#!/usr/bin/env python3
"""Join the three spu_ops runs: python3 compare_ops.py ops_scalar.txt ops_neon.txt ops_fastfp.txt"""
import sys
runs = []
for p in sys.argv[1:4]:
    d = {}
    for line in open(p):
        n, l, t = line.split(); d[n] = (float(l), float(t))
    runs.append(d)
sc, ne, ff = runs
print(f"{'op':8} | {'lat ns: scalar':>14} {'neon':>7} {'fastfp':>7} | {'tput ns: scalar':>15} {'neon':>7} {'fastfp':>7} | best (tput)")
for n in sc:
    best = min(("scalar", sc[n][1]), ("neon", ne[n][1]), ("fastfp", ff[n][1]), key=lambda x: x[1])
    print(f"{n:8} | {sc[n][0]:14.2f} {ne[n][0]:7.2f} {ff[n][0]:7.2f} | {sc[n][1]:15.2f} {ne[n][1]:7.2f} {ff[n][1]:7.2f} | {best[0]}")
