#!/usr/bin/env python3
"""Summarize bench_results.csv: median seconds per cell and grouped-vs-old speedup, as a Markdown table."""
import csv
import statistics
import sys
from collections import defaultdict

cells = defaultdict(list)
order = []
with open(sys.argv[1]) as f:
    for row in csv.DictReader(f):
        key = (row["workload"], row["threads"], row["grouped"])
        cells[key].append(float(row["seconds"]))
        if row["workload"] not in order:
            order.append(row["workload"])

threads = sorted({k[1] for k in cells}, key=int)
print("| workload | " + " | ".join(f"{t} thr: grouped / old / speedup" for t in threads) + " |")
print("|---|" + "---|" * len(threads))
for w in order:
    parts = []
    for t in threads:
        g = cells.get((w, t, "true"))
        o = cells.get((w, t, "false"))
        if not g or not o or min(g) == 0 or min(o) == 0:
            parts.append("n/a")
            continue
        gm, om = statistics.median(g), statistics.median(o)
        parts.append(f"{gm:.2f} s / {om:.2f} s / {om / gm:.2f}x")
    print(f"| {w} | " + " | ".join(parts) + " |")
