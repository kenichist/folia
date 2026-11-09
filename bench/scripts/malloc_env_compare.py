#!/usr/bin/env python3
"""Summarise the allocator-setting A/B (docs/BENCHMARKS.md §6f).

    python3 -I bench/scripts/malloc_env_compare.py bench/results/malloc-env-2026-11-09/*.json

Files are named <scenario>-<mode>-<rep>.json (import, idle, typing). Modes:
base (nothing set; Folia re-executes itself with MallocLargeCache=0), nano
(MallocNanoZone=0), aggr (MallocAggressiveMadvise=1), space
(MallocSpaceEfficient=1), sys (MallocLargeCache=1, the system default).
Prints one row per run, then per mode the median [min-max] over runs.
Footprints in MB (10^6 bytes). When the run was taken with vm_snapshots=1, the
`footprint -p` reports next to it add MALLOC_* dirty memory (malloc) and
IOSurface (the window's backing buffers, which depend on how often macOS
painted the window, not on the allocator) at each checkpoint.
"""
import json
import os
import re
import statistics
import sys

MODES = ("base", "nano", "aggr", "space", "sys")
MB = 1e6


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    k = (len(xs) - 1) * p / 100
    f = int(k)
    c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


UNITS = {"B": 1, "KB": 1024, "MB": 1024 ** 2, "GB": 1024 ** 3}


def footprint_report(path):
    """{category: dirty bytes} from a `footprint -p` report, or None."""
    if not os.path.exists(path):
        return None
    out = {}
    for line in open(path):
        m = re.match(r"\s*([\d.]+)\s*(B|KB|MB|GB)\s+([\d.]+)\s*(B|KB|MB|GB)\s+([\d.]+)\s*(B|KB|MB|GB)\s+(\d+)\s+(.*)$", line)
        if m:
            out[m.group(8).strip()] = float(m.group(1)) * UNITS[m.group(2)]
    return out


def vm_cols(r, prefix, path):
    rep = footprint_report(path)
    if rep is None:
        return
    r[prefix + "_malloc"] = sum(v for k, v in rep.items() if k.startswith("MALLOC")) / MB
    r[prefix + "_iosurf"] = rep.get("IOSurface", 0) / MB


def samples(d, key):
    return d.get("metrics", {}).get(key, {}).get("samples", [])


def import_row(d, path):
    r = {}
    for kind in ("markdown", "docx"):
        k = kind[:2]
        r[k + "_before"] = d.get(kind + "_before_footprint_bytes", float("nan")) / MB
        r[k + "_shown"] = d[kind + "_shown_footprint_bytes"] / MB
        r[k + "_closed"] = d[kind + "_closed_footprint_bytes"] / MB  # 3 s after the close
        later = samples(d, kind + "_later_footprint")  # 5, 10, 15, 20 s after the relief
        r[k + "_later_min"] = min(later) / MB
        r[k + "_later_max"] = max(later) / MB
        r[k + "_relief"] = d[kind + "_relief_released_bytes"] / MB
        r[k + "_peak"] = d[kind + "_peak_growth_bytes"] / MB
        r[k + "_editable_ms"] = samples(d, kind + "_upload_to_editable_us")[0] / 1e3
        r[k + "_sessions"] = d[kind + "_closed_sessions_alive"]
        if kind + "_closed_imported_session_alive" in d:
            r[k + "_page_alive"] = int(d[kind + "_closed_imported_session_alive"])
            r[k + "_still_active"] = int(d[kind + "_closed_active_is_imported"])
        if kind + "_freed_footprint_bytes" in d:  # free=1: the page really gone
            r[k + "_freed_page_alive"] = int(d[kind + "_freed_imported_session_alive"])
            r[k + "_freed"] = d[kind + "_freed_footprint_bytes"] / MB  # 3 s after the reclaim
            r[k + "_freed_relief"] = d[kind + "_freed_relief_released_bytes"] / MB
            r[k + "_freed_after_relief"] = d[kind + "_freed_after_relief_footprint_bytes"] / MB
        base = path[:-len(".json")]
        vm_cols(r, k + "_shown", "%s.footprint-%s-shown.txt" % (base, kind))
        vm_cols(r, k + "_closed", "%s.footprint-%s-closed.txt" % (base, kind))
        vm_cols(r, k + "_freed", "%s.footprint-%s-freed.txt" % (base, kind))
        vm_cols(r, k + "_freed_ar", "%s.footprint-%s-freed-after-relief.txt" % (base, kind))
    w = samples(d, "document_import_worker_us")
    b = samples(d, "session_build_us")
    r["md_worker_ms"], r["dx_worker_ms"] = (w[0] / 1e3, w[1] / 1e3) if len(w) >= 2 else (float("nan"),) * 2
    # The two uploads are the two largest builds (first_page=1 adds a tiny one first).
    big = sorted(range(len(b)), key=lambda i: -b[i])[:2]
    r["md_build_ms"], r["dx_build_ms"] = (b[min(big)] / 1e3, b[max(big)] / 1e3) if len(b) >= 2 else (float("nan"),) * 2
    return r


def idle_row(d, path):
    fp = samples(d, "idle_footprint_bytes")
    wk = samples(d, "idle_wakeups_per_s")
    r = {
        "footprint_median": statistics.median(fp) / MB,
        "footprint_last": fp[-1] / MB,
        "cpu_mean_pct": d["idle_mean_cpu_percent"],
        "wakeups_mean": statistics.mean(wk),
    }
    vm_cols(r, "end", path[:-len(".json")] + ".footprint-end.txt")
    return r


def typing_row(d, path):
    paint = samples(d, "input_to_paint_us")
    flush = samples(d, "input_to_flush_us")
    return {
        "cpu_ms_per_edit": samples(d, "cpu_ms_per_edit_all_threads")[0],
        "input_to_paint_p50_ms": pct(paint, 50) / 1e3,
        "input_to_paint_p95_ms": pct(paint, 95) / 1e3,
        "input_to_flush_p50_ms": pct(flush, 50) / 1e3,
        "wall_s": d["typing_wall_ms"] / 1e3,
        "peak_mb": d.get("process", {}).get("peak_footprint_bytes", float("nan")) / MB,
        "end_mb": d.get("process", {}).get("footprint_bytes", float("nan")) / MB,
    }


ROWS = {"import": import_row, "idle": idle_row, "typing": typing_row}


def fmt(v):
    if isinstance(v, float):
        return "%.2f" % v if abs(v) < 10 else "%.1f" % v
    return str(v)


def main():
    runs = {}
    for p in sys.argv[1:]:
        m = re.match(r"(import|idle|typing)-(%s)-(\d+)\.json$" % "|".join(MODES), os.path.basename(p))
        if not m:
            continue
        d = json.load(open(p))
        if d.get("notes"):
            print("note in %s: %s" % (os.path.basename(p), d["notes"]))
        runs.setdefault(m.group(1), {}).setdefault(m.group(2), []).append((int(m.group(3)), ROWS[m.group(1)](d, p), d))
    for scen in ("import", "idle", "typing"):
        if scen not in runs:
            continue
        print("\n## %s" % scen)
        keys = None
        for mode in MODES:
            for rep, r, d in sorted(runs[scen].get(mode, []), key=lambda t: t[0]):
                keys = keys or list(r)
                env = d.get("env", {})
                print("%-6s %d  %s  [%s]" % (mode, rep, "  ".join("%s=%s" % (k, fmt(r[k])) for k in keys),
                                              ",".join("%s=%s" % kv for kv in sorted(env.get("malloc_env", {}).items()))))
        print("median [min-max] per mode:")
        for k in keys:
            cells = []
            for mode in MODES:
                xs = [r[k] for _, r, _ in runs[scen].get(mode, [])]
                if xs:
                    cells.append("%s %s [%s-%s]" % (mode, fmt(statistics.median(xs)), fmt(min(xs)), fmt(max(xs))))
            print("  %-22s %s" % (k, "   ".join(cells)))


if __name__ == "__main__":
    main()
