#!/usr/bin/env python3
"""Turn the sweep CSVs into the table the release README carries.

Each sweep cycles through its whole list before repeating, so a comparison inside one cycle is fair
even if the machine drifts between cycles. This averages the measured cycles and drops the warm-up,
which the sweep already excludes, and any run that did not report `ok`.

The first measured cycle is dropped. A DLL that has just changed can still be compiling shaders
during it, which once showed up as a 2 ms error on a configuration whose later cycles agreed to
0.01 ms. Pass --keep-first to include it anyway.

    table_from_sweeps.py [--keep-first] <csv> [<csv> ...]
"""
import csv
import sys
from collections import defaultdict


def main():
    keep_first = "--keep-first" in sys.argv
    rows = defaultdict(list)
    for path in [a for a in sys.argv[1:] if not a.startswith("--")]:
        with open(path) as f:
            for r in csv.DictReader(f):
                if r.get("status") != "ok" or not r.get("frametime_ms"):
                    continue
                if not keep_first and r.get("cycle") == "1":
                    continue
                rows[(r["dll"], r["set"])].append(
                    (float(r["frametime_ms"]), float(r["fps"]), float(r["upscaler_ms"] or 0)))
    print(f"{'dll':10} {'set':18} {'frametime':>10} {'fps':>8} {'upscaler':>9} {'runs':>5}")
    out = []
    for (dll, name), vals in rows.items():
        n = len(vals)
        ft = sum(v[0] for v in vals) / n
        fps = sum(v[1] for v in vals) / n
        up = sum(v[2] for v in vals) / n
        out.append((ft, dll, name, fps, up, n))
    for ft, dll, name, fps, up, n in sorted(out):
        print(f"{dll:10} {name:18} {ft:10.2f} {fps:8.1f} {up:9.2f} {n:5d}")


if __name__ == "__main__":
    main()
