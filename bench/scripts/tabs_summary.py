#!/usr/bin/env python3
"""Summarize `--bench tabs` runs (docs/BENCHMARKS.md §6d).

    python3 bench/scripts/tabs_summary.py bench/results/tabs-2026-11-09/*.json
"""
import json
import statistics
import sys

ROWS = [
    ("idle_one_tab_footprint_bytes", "MB", 1e6),
    ("idle_all_tabs_footprint_bytes", "MB", 1e6),
    ("idle_one_tab_mean_cpu_percent", "%", 1),
    ("idle_all_tabs_mean_cpu_percent", "%", 1),
    ("idle_one_tab_mean_wakeups_per_s", "/s", 1),
    ("idle_all_tabs_mean_wakeups_per_s", "/s", 1),
    ("background_open_lua_renders", "", 1),
    ("background_open_pages_loaded", "", 1),
    ("background_open_footprint_delta", "MB", 1e6),
    ("switch_lua_renders_per_switch", "", 1),
    ("switch_strip_paints_per_switch", "", 1),
    ("switch_paints_missing", "", 1),
    ("switch_doc_cache_entries", "", 1),
    ("typing_strip_paints", "", 1),
    ("closed_footprint_bytes", "MB", 1e6),
    ("closed_doc_cache_entries", "", 1),
]
METRICS = [
    "tab_open_background_us",
    "tab_switch_dispatch_us",
    "tab_switch_to_editable_uncached_us",
    "tab_switch_to_editable_cached_us",
    "tab_switch_to_paint_uncached_us",
    "tab_switch_to_paint_cached_us",
    "window_resize_step_us",
    "footprint_after_round",
]


def pct(s, p):
    s = sorted(s)
    return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]


def main(paths):
    runs = [(p.rsplit("/", 1)[-1], json.load(open(p))) for p in paths]
    print("| measure | " + " | ".join(n for n, _ in runs) + " |")
    print("|---|" + "---|" * len(runs))
    for key, unit, div in ROWS:
        vals = []
        for _, d in runs:
            v = d.get(key)
            vals.append("—" if v is None else ("%.2f" % (v / div) if div != 1 or isinstance(v, float) else str(v)))
        print("| %s%s | %s |" % (key, " (%s)" % unit if unit else "", " | ".join(vals)))
    for m in METRICS:
        vals = []
        for _, d in runs:
            s = [x for x in d["metrics"].get(m, {}).get("samples", []) if x >= 0]
            if not s:
                vals.append("—")
            elif m == "footprint_after_round":
                vals.append(" → ".join("%.1f" % (x / 1e6) for x in s) + " MB")
            else:
                vals.append("n=%d, med %.0f, p95 %.0f, max %.0f µs" % (len(s), statistics.median(s), pct(s, 95), max(s)))
        print("| %s | %s |" % (m, " | ".join(vals)))


if __name__ == "__main__":
    main(sys.argv[1:])
