#!/usr/bin/env python3
"""Summarise the before/after runs of bench/results/repaint-idle-launch-*/.

    python3 -I bench/scripts/repaint_compare.py bench/results/repaint-idle-launch-2026-11-10
"""
import glob, json, os, statistics as st, sys

d = sys.argv[1]


def load(pattern):
    return [json.load(open(f)) for f in sorted(glob.glob(os.path.join(d, pattern)))]


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float("nan")


def ms(run, key, p):
    return pct(run["metrics"][key]["samples"], p) / 1000.0 if key in run["metrics"] else float("nan")


def rng(vals, fmt="%.2f"):
    vals = [v for v in vals if v == v]
    if not vals:
        return "n/a"
    lo, hi = min(vals), max(vals)
    return fmt % lo if fmt % lo == fmt % hi else (fmt + "–" + fmt) % (lo, hi)


print("## typing (400 keys)")
for case in ("top", "bottom", "formula"):
    for which in ("before", "after"):
        runs = load("typing-%s-%s-*.json" % (case, which))
        print("%-8s %-6s paint p50 %s p95 %s | input→paint p50 %s p95 %s | input→flush p50 %s | cpu/edit %s ms | lines/edit %s" % (
            case, which, rng([ms(r, "editor_paint_us", .5) for r in runs]), rng([ms(r, "editor_paint_us", .95) for r in runs]),
            rng([ms(r, "input_to_paint_us", .5) for r in runs]), rng([ms(r, "input_to_paint_us", .95) for r in runs]),
            rng([ms(r, "input_to_flush_us", .5) for r in runs]),
            rng([r["metrics"]["cpu_ms_per_edit_all_threads"]["samples"][0] for r in runs]),
            rng([r.get("lines_painted_per_edit", float("nan")) for r in runs], "%.1f")))
print("## scroll")
for label in ("scroll-code-before", "scroll-code-after", "scroll-code-unshaped", "scroll-stress-before", "scroll-stress-after"):
    runs = load(label + "-*.json")
    print("%-22s paints %s | paint total %s ms | lines painted %s | frame work p50 %s p95 %s ms | cpu %s s" % (
        label, rng([r["editor_paints"] for r in runs], "%d"), rng([r["editor_paint_total_ms"] for r in runs], "%.0f"),
        rng([r["lines_painted"] for r in runs], "%d"),
        rng([pct(r["metrics"]["scroll_frame_work_ms"]["samples"], .5) for r in runs]),
        rng([pct(r["metrics"]["scroll_frame_work_ms"]["samples"], .95) for r in runs]),
        rng([r["process"]["user_s"] + r["process"]["system_s"] for r in runs])))
print("## wake (6 keys after each 3 s gap)")
for which in ("before", "after"):
    runs = load("wake-%s-*.json" % which)
    fp = [st.median(r["metrics"]["wake_footprint_before_keys_bytes"]["samples"]) / 1e6 for r in runs]
    row = []
    for k in range(1, 7):
        row.append(rng([st.median(r["metrics"]["wake_key%d_input_to_flush_us" % k]["samples"]) / 1000 for r in runs
                        if "wake_key%d_input_to_flush_us" % k in r["metrics"]]))
    trims = [st.median(r["metrics"]["window_buffer_trim_us"]["samples"]) / 1000 for r in runs if "window_buffer_trim_us" in r["metrics"]]
    print("%-6s footprint before keys %s MB | input→flush median per key (ms): %s | trim %s ms" % (which, rng(fp, "%.1f"), ", ".join(row), rng(trims, "%.1f")))
print("## idle")
for which in ("before", "after"):
    for r in load("idle-%s-*.json" % which):
        fp = [x / 1e6 for x in r["metrics"]["idle_footprint_bytes"]["samples"]]
        print("%-6s footprint first %.1f last %.1f MB | cpu %.2f %% | trims %s" % (which, fp[0], fp[-1], r["idle_mean_cpu_percent"], r["counters"].get("window_buffer_trims")))
print("## launch (main() → editable, ms)")
for which in ("before", "prefetch", "after"):
    v = [r["metrics"]["launch_main_to_editable_ms"]["samples"][0] for r in load("launch-%s-*.json" % which)]
    if v:
        print("%-9s n=%d min %.0f median %.0f max %.0f" % (which, len(v), min(v), st.median(v), max(v)))
