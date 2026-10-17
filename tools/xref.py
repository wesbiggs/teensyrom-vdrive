#!/usr/bin/env python3
"""xref.py LO HI : list references into [LO,HI) from code outside it (uses build/kernal.dis)."""
import re, sys
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
rows = []
for l in open("build/kernal.dis"):
    m = re.search(r";\s+([0-9A-F]{4}) ", l)
    if m: rows.append((int(m.group(1), 16), l.split(";")[0]))
for a, t in rows:
    if lo <= a < hi: continue
    for r in re.findall(r"\bL([0-9A-F]{4})\b", t):
        r = int(r, 16)
        if lo <= r < hi: print(f"${a:04X} -> ${r:04X}: {t.strip()}")
