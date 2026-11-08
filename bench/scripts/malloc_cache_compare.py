#!/usr/bin/env python3
"""Compare bench runs with and without malloc's large cache (docs/BENCHMARKS.md §6b).

    python3 -I bench/scripts/malloc_cache_compare.py bench/results/malloc-large-cache-2026-11-06/long-*.json

Files are named <case>-<mode>-<rep>.json, mode one of default (MallocLargeCache=1),
off (MallocLargeCache=0) or reexec (unset: Folia re-executes itself with 0).
For each metric, prints per mode the median over runs of each run's p50 and
p95 (sample metrics) or of the value (top-level numbers), and the runs' range.
Frame timings only exist for frames the system painted; *_unpainted_frames
says how many were painted offscreen instead. --keys=a,b keeps keys containing
any of those substrings.
"""
import json
import os
import re
import statistics
import sys

SKIP = {"metrics", "env", "counters", "process", "notes", "scenario", "warnings"}


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    k = (len(xs) - 1) * p / 100
    f = int(k)
    c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def fmt(v, key):
    if any(s in key for s in ("footprint", "bytes", "cache_max", "used_max", "budget_limit")) and abs(v) >= 1e5:
        return "%.1fM" % (v / 1e6)
    if abs(v) >= 100:
        return "%.0f" % v
    return "%.3g" % v


def main():
    keys = []
    paths = []
    for a in sys.argv[1:]:
        if a.startswith("--keys="):
            keys = [k for k in a[7:].split(",") if k]
        else:
            paths.append(a)
    groups = {}
    for p in paths:
        m = re.match(r"(.+)-(default|off|reexec)-(\w+)\.json$", os.path.basename(p))
        if not m:
            continue
        d = json.load(open(p))
        groups.setdefault(m.group(2), []).append(d)
    modes = [m for m in ("default", "off", "reexec") if m in groups]
    if not modes:
        sys.exit("no <case>-<mode>-<rep>.json files")
    vals = {}
    for mode in modes:
        for d in groups[mode]:
            for k, v in d.items():
                if k in SKIP or isinstance(v, bool) or not isinstance(v, (int, float)):
                    continue
                vals.setdefault(k, {}).setdefault(mode, []).append(("v", v))
            for k, mm in d.get("metrics", {}).items():
                s = mm.get("samples", [])
                if s:
                    vals.setdefault(k + " p50", {}).setdefault(mode, []).append(("s", pct(s, 50)))
                    vals.setdefault(k + " p95", {}).setdefault(mode, []).append(("s", pct(s, 95)))
                    vals.setdefault(k + " n", {}).setdefault(mode, []).append(("s", len(s)))
    print("runs: " + ", ".join("%s %d" % (m, len(groups[m])) for m in modes))
    print("%-58s" % "key (median over runs [min-max])" + "".join("%34s" % m for m in modes))
    for k in sorted(vals):
        if keys and not any(s in k for s in keys):
            continue
        cells = []
        for mode in modes:
            xs = [v for _, v in vals[k].get(mode, [])]
            if not xs:
                cells.append("-")
                continue
            med = statistics.median(xs)
            cell = "%s [%s-%s]" % (fmt(med, k), fmt(min(xs), k), fmt(max(xs), k)) if len(xs) > 1 else fmt(med, k)
            if len(xs) < len(groups[mode]):
                cell += " (%d/%d runs)" % (len(xs), len(groups[mode]))
            cells.append(cell)
        print("%-58s" % k[:58] + "".join("%34s" % c for c in cells))


main()
