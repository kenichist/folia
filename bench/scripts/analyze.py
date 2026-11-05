#!/usr/bin/env python3
"""Folia benchmark analysis and markdown report (python3 standard library only).

Inputs (any mix, any order):
  * bench JSON written by the app's bench mode (an object with "scenario")
      Folia --bench <scenario> --bench-out run.json
    schema: {"scenario": str, "env": {...}, "metrics": {name: {"unit":
    "us"|"ms"|"bytes"|"ratio", "samples": [numbers], "proxy": str?}},
    "counters": {name: number}, "process": {...}, "notes": [str], ...}
  * JSON-lines files from the external sampler
      folia_procstat <pid> <interval_ms> <count> > idle.jsonl
  * any other JSON object (e.g. bench/run_suite.py's environment.json) is
    shown in the report's Environment section and never gated

Gates are scoped: a threshold only counts runs of the scenario(s) that define
it (THRESHOLDS[...]["scenarios"]); other scenarios emitting the same metric
(integration, stress, recovery...) are reported but not gated.

Output: a markdown report with, per metric, n / p50 / p95 / p99 / min / max /
mean, per-run rows when several runs share a configuration (scenario + bench
args), and a PASS / MISS / UNMEASURED verdict per requirement.

Statistics: percentiles use the nearest-rank method on the raw samples (the
smallest sample with at least p% of samples at or below it; no
interpolation). p99 is labelled "exploratory" when n < 1000.

Usage:
  analyze.py run1.json run2.json idle.jsonl -o report.md [--json summary.json]
             [--fail-on-miss] [--procstat-skip S] [--procstat-until S]
  analyze.py --selftest

Adding a mapping: add an entry to THRESHOLDS below (metric name -> list of
checks). Each check names the requirement it belongs to (a key of
REQUIREMENTS). Thresholds are product requirements: never relax them here.
"""

import argparse
import datetime
import json
import math
import os
import sys
import tempfile

# --------------------------------------------------------------------------
# Requirements (product thresholds, verbatim). Order = report order.
# --------------------------------------------------------------------------
REQUIREMENTS = {
    "open_uncached": "Open ordinary page -> first editable, uncached: p95 <= 100 ms",
    "open_cached": "Open ordinary page -> first editable, cached: p95 <= 30 ms",
    "create_title": "Create page -> editable title: p95 <= 30 ms",
    "expand_main": "Expand branch, main-thread time: p95 <= 16 ms",
    "typing_main": "Typing, main-thread time per edit: p95 <= 8 ms",
    "typing_presented": "Typing, event -> presented glyph: p95 <= 33 ms",
    "scroll": "Large-note scroll: >= 95% of frames <= 16.7 ms (longest hitch reported)",
    "code_visible": "Visible short code block until presentation: <= 30 ms",
    "formula_visible": "Typical formula until presentation: <= 100 ms",
    "idle_footprint": "Idle footprint <= 60 MB (stretch target)",
    "idle_cpu": "Idle CPU: mean < 1% of one core over 60 s",
    "nav_plateau": "Repeated navigation (100 switches x 5 batches): footprint plateau within cache caps",
}

MB = 1000 * 1000          # "60 MB" is evaluated in decimal megabytes (the stricter reading)
MiB = 1024 * 1024

# Cache caps from the app defaults (src/app/AppController.h: docCacheBytes 48 MiB,
# docCacheEntries 16; src/app/AppController.cpp: PreviewService(4 << 20)).
CACHE_CAPS = {
    "doc_cache_bytes": 48 * MiB,
    "doc_cache_entries": 16,
    "preview_cache_bytes": 4 * MiB,
}

# Plateau analysis parameters (analysis method, not a product threshold): the
# settled footprint after batch `baseline_batch` (1-based; batch 1 warms the
# caches) must not grow by more than max(abs_bytes, rel * baseline) by the
# last batch.
PLATEAU = {"baseline_batch": 2, "abs_bytes": 4 * MiB, "rel": 0.05}

# metric name -> list of checks.
#   req    requirement id (REQUIREMENTS key)
#   scenarios  bench scenarios whose runs this check gates ("procstat" = a
#          folia_procstat file). Runs of other scenarios that emit the same
#          metric (integration, stress, recovery...) are reported, never gated.
#   stat   p50 | p95 | p99 | min | max | mean | last | frac_le:<x> | plateau |
#          window_mean | hitches:<x> (report-only: count/longest above x)
#   op     <= | < | >= | >
#   limit  number in `unit`
#   unit   ms | bytes | ratio | percent | count   (unit of `limit`)
#   sample_unit optional: unit the samples are converted to (default `unit`;
#          frac_le compares samples in ms but its limit is a ratio)
#   gate   optional, default True; False = INFO row that never changes a verdict
#   min_n  optional: fewer samples -> UNMEASURED
#   min_window_s optional (procstat window_mean): shorter window -> UNMEASURED
# The statistic for "code_visible" / "formula_visible" is p95 (the requirement
# names no percentile; p95 matches the rest of the table). Use "max" for the
# strictest reading.
THRESHOLDS = {
    "open_uncached_to_editable_ms": [
        {"req": "open_uncached", "scenarios": ["open"], "stat": "p95", "op": "<=", "limit": 100, "unit": "ms"}],
    "open_cached_to_editable_ms": [
        {"req": "open_cached", "scenarios": ["open"], "stat": "p95", "op": "<=", "limit": 30, "unit": "ms"}],
    "create_to_editable_title_ms": [
        {"req": "create_title", "scenarios": ["create"], "stat": "p95", "op": "<=", "limit": 30, "unit": "ms"}],
    "page_new_to_editable_title_us": [
        {"req": "create_title", "scenarios": ["create"], "stat": "p95", "op": "<=", "limit": 30, "unit": "ms"}],
    "expand_main_thread_ms": [
        {"req": "expand_main", "scenarios": ["expand"], "stat": "p95", "op": "<=", "limit": 16, "unit": "ms"}],
    "tree_expand_main_us": [
        {"req": "expand_main", "scenarios": ["expand"], "stat": "p95", "op": "<=", "limit": 16, "unit": "ms"}],
    "edit_main_thread_us": [
        {"req": "typing_main", "scenarios": ["typing"], "stat": "p95", "op": "<=", "limit": 8, "unit": "ms"}],
    "input_to_flush_us": [
        {"req": "typing_presented", "scenarios": ["typing"], "stat": "p95", "op": "<=", "limit": 33, "unit": "ms"}],
    "input_to_paint_us": [
        {"req": "typing_presented", "scenarios": ["typing"], "stat": "p95", "op": "<=", "limit": 33, "unit": "ms"}],
    # Main-thread scroll handling + editor paint per frame.
    "scroll_frame_work_ms": [
        {"req": "scroll", "scenarios": ["scroll"], "stat": "frac_le:16.7", "op": ">=", "limit": 0.95,
         "unit": "ratio", "sample_unit": "ms"}],
    # Display-paced paint intervals: longest interval and hitches (> 25 ms), report only.
    "scroll_paint_interval_ms": [
        {"req": "scroll", "scenarios": ["scroll"], "stat": "hitches:25", "op": "<=", "limit": None,
         "unit": "count", "sample_unit": "ms", "gate": False}],
    "code_block_visible_ms": [
        {"req": "code_visible", "scenarios": ["presentation"], "stat": "p95", "op": "<=", "limit": 30, "unit": "ms"}],
    "formula_uncached_ms": [
        {"req": "formula_visible", "scenarios": ["presentation"], "stat": "p95", "op": "<=", "limit": 100, "unit": "ms"}],
    "formula_cache_hit_ms": [
        {"req": "formula_visible", "scenarios": ["presentation"], "stat": "p95", "op": "<=", "limit": 100, "unit": "ms"}],
    "formula_first_in_process_ms": [
        {"req": "formula_visible", "scenarios": ["presentation"], "stat": "max", "op": "<=", "limit": 100, "unit": "ms"}],
    "idle_footprint_bytes": [
        {"req": "idle_footprint", "scenarios": ["idle"], "stat": "max", "op": "<=", "limit": 60 * MB, "unit": "bytes"}],
    "idle_cpu_percent_of_core": [
        {"req": "idle_cpu", "scenarios": ["idle"], "stat": "mean", "op": "<", "limit": 1.0, "unit": "percent",
         "min_n": 60}],
    "navigate_settled_footprint_bytes": [
        {"req": "nav_plateau", "scenarios": ["navigate"], "stat": "plateau", "op": "<=", "limit": None,
         "unit": "bytes", "min_n": 3}],
    "navigate_doc_cache_bytes_est": [
        {"req": "nav_plateau", "scenarios": ["navigate"], "stat": "max", "op": "<=",
         "limit": CACHE_CAPS["doc_cache_bytes"], "unit": "bytes"}],
    "navigate_doc_cache_entries": [
        {"req": "nav_plateau", "scenarios": ["navigate"], "stat": "max", "op": "<=",
         "limit": CACHE_CAPS["doc_cache_entries"], "unit": "count"}],
    "navigate_preview_cache_bytes": [
        {"req": "nav_plateau", "scenarios": ["navigate"], "stat": "max", "op": "<=",
         "limit": CACHE_CAPS["preview_cache_bytes"], "unit": "bytes"}],
    # Derived from folia_procstat JSON lines (measured outside the app).
    "procstat_cpu_percent_of_core": [
        {"req": "idle_cpu", "scenarios": ["procstat"], "stat": "window_mean", "op": "<", "limit": 1.0,
         "unit": "percent", "min_window_s": 60}],
    "procstat_phys_footprint_bytes": [
        {"req": "idle_footprint", "scenarios": ["procstat"], "stat": "max", "op": "<=", "limit": 60 * MB,
         "unit": "bytes"}],
}

EXPLORATORY_N = 1000
# procstat samples sit on a fixed grid but each wake-up is a few ms late
# (timer coalescing); a 60 x 1000 ms grid may span 59.99x s. Windows shorter
# than min_window_s - WINDOW_JITTER_S are UNMEASURED.
WINDOW_JITTER_S = 0.05

# --------------------------------------------------------------------------
# statistics
# --------------------------------------------------------------------------


def clean(samples):
    out = []
    for v in samples or []:
        if isinstance(v, bool):
            continue
        if isinstance(v, (int, float)) and not math.isnan(v) and not math.isinf(v):
            out.append(float(v))
    return out


def percentile(sorted_vals, p):
    """Nearest-rank percentile of an ascending list."""
    n = len(sorted_vals)
    if n == 0:
        return None
    if p <= 0:
        return sorted_vals[0]
    rank = int(math.ceil(p / 100.0 * n))
    return sorted_vals[min(max(rank, 1), n) - 1]


def summarize(samples):
    s = sorted(samples)
    n = len(s)
    if n == 0:
        return {"n": 0}
    return {
        "n": n,
        "p50": percentile(s, 50),
        "p95": percentile(s, 95),
        "p99": percentile(s, 99),
        "min": s[0],
        "max": s[-1],
        "mean": sum(s) / n,
        "last": samples[-1],
    }


UNIT_SCALE = {  # to canonical: ms for time, bytes for sizes
    "us": ("ms", 1e-3),
    "ms": ("ms", 1.0),
    "s": ("ms", 1e3),
    "ns": ("ms", 1e-6),
    "bytes": ("bytes", 1.0),
    "ratio": ("ratio", 1.0),
    "percent": ("percent", 1.0),
    "count": ("count", 1.0),
}


def convert(samples, unit, want):
    """Convert samples in `unit` to `want`; returns (values, error)."""
    src = UNIT_SCALE.get(unit)
    dst = UNIT_SCALE.get(want)
    if src is None or dst is None:
        return None, "unknown unit %r -> %r" % (unit, want)
    # ratio-typed metrics may carry percentages or counts (the bench marks
    # several non-time series as "ratio"); accept them for percent/count checks.
    if src[0] != dst[0] and not (src[0] == "ratio" and dst[0] in ("percent", "count")):
        return None, "unit mismatch: metric is %s, check needs %s" % (unit, want)
    f = src[1] / dst[1]
    return [v * f for v in samples], None


def fmt_num(v, unit=None):
    if v is None:
        return "—"
    if unit == "bytes":
        return "%s (%.2f MiB)" % (format(int(round(v)), ","), v / MiB)
    av = abs(v)
    if av >= 1000:
        return "%.1f" % v
    if av >= 100:
        return "%.2f" % v
    if av >= 1:
        return "%.3f" % v
    return "%.4g" % v


def fmt_plain(v):
    if v is None:
        return "—"
    av = abs(v)
    if float(v).is_integer() and av < 1e15:
        return format(int(v), ",") if av >= 1e6 else str(int(v))
    if av >= 1e6:
        return format(int(round(v)), ",")
    if av >= 1000:
        return "%.1f" % v
    if av >= 100:
        return "%.2f" % v
    if av >= 1:
        return "%.3f" % v
    return "%.4g" % v


def compare(value, op, limit):
    if op == "<=":
        return value <= limit
    if op == "<":
        return value < limit
    if op == ">=":
        return value >= limit
    if op == ">":
        return value > limit
    raise ValueError("bad op " + op)


# --------------------------------------------------------------------------
# input
# --------------------------------------------------------------------------


class Run(object):
    def __init__(self, path, data):
        self.path = path
        self.name = os.path.basename(path)
        self.data = data
        self.scenario = data.get("scenario", "?")
        self.env = data.get("env", {}) or {}
        self.metrics = data.get("metrics", {}) or {}
        self.counters = data.get("counters", {}) or {}
        self.process = data.get("process", {}) or {}
        self.notes = data.get("notes", []) or []
        known = {"scenario", "env", "metrics", "counters", "process", "notes"}
        self.extra = dict((k, v) for k, v in data.items() if k not in known)
        args = self.env.get("args", {}) or {}
        # The manifest path differs per machine; it does not define a configuration.
        sig = dict((k, v) for k, v in args.items() if k not in ("manifest",))
        cfg = self.scenario + ("" if not sig else " " + " ".join(
            "%s=%s" % (k, json.dumps(sig[k]) if not isinstance(sig[k], str) else sig[k]) for k in sorted(sig)))
        # Command-line flags (--preset, --shell, --no-math) are not bench args;
        # they show up in env as the active layout name and the math renderer.
        if self.env.get("preset"):
            cfg += " preset=%s" % self.env["preset"]
        if self.env.get("math_renderer") is False:
            cfg += " math=off"
        self.config = cfg


class ProcSeries(object):
    def __init__(self, path, rows, errors):
        self.path = path
        self.name = os.path.basename(path)
        self.rows = rows
        self.errors = errors

    def derived(self):
        """Derived metrics: name -> (unit, samples, extra)."""
        rows = self.rows
        out = {}
        if not rows:
            return out
        fp = [float(r.get("ri_phys_footprint", 0)) for r in rows]
        out["procstat_phys_footprint_bytes"] = ("bytes", fp, {})
        out["procstat_resident_bytes"] = ("bytes", [float(r.get("ri_resident_size", 0)) for r in rows], {})
        cpu, wk = [], []
        for a, b in zip(rows, rows[1:]):
            dt_ms = float(b["wall_ms"]) - float(a["wall_ms"])
            if dt_ms <= 0:
                continue
            dcpu = (b["ri_user_time_ns"] + b["ri_system_time_ns"]) - (a["ri_user_time_ns"] + a["ri_system_time_ns"])
            cpu.append(dcpu / (dt_ms * 1e6) * 100.0)
            dw = (b["ri_interrupt_wkups"] + b["ri_pkg_idle_wkups"]) - (a["ri_interrupt_wkups"] + a["ri_pkg_idle_wkups"])
            wk.append(dw / (dt_ms / 1000.0))
        first, last = rows[0], rows[-1]
        window_s = (float(last["wall_ms"]) - float(first["wall_ms"])) / 1000.0
        total_cpu = (last["ri_user_time_ns"] + last["ri_system_time_ns"]) - (
            first["ri_user_time_ns"] + first["ri_system_time_ns"])
        window_mean = total_cpu / (window_s * 1e9) * 100.0 if window_s > 0 else None
        out["procstat_cpu_percent_of_core"] = ("percent", cpu, {"window_s": window_s, "window_mean": window_mean})
        out["procstat_wakeups_per_s"] = ("ratio", wk, {})
        out["procstat_lifetime_max_phys_footprint_bytes"] = (
            "bytes", [float(last.get("ri_lifetime_max_phys_footprint", 0))], {})
        return out


def load_inputs(paths, procstat_skip_s=0.0, procstat_until_s=None):
    """Classify and load inputs. procstat rows are kept when
    procstat_skip_s <= (wall_ms - first wall_ms) / 1000 <= procstat_until_s."""
    runs, procs, problems, contexts = [], [], [], []
    for p in paths:
        try:
            with open(p, "r", encoding="utf-8") as f:
                text = f.read()
        except (IOError, OSError) as e:
            problems.append("%s: %s" % (p, e))
            continue
        stripped = text.strip()
        data = None
        if stripped.startswith("{"):
            try:
                data = json.loads(stripped)
            except ValueError:
                data = None
        if isinstance(data, dict) and "scenario" in data:
            runs.append(Run(p, data))
            continue
        if isinstance(data, dict) and "ri_phys_footprint" not in data:
            # Not a bench result (no "scenario") and not procstat: context such as
            # the suite runner's environment.json; shown, never gated.
            contexts.append((p, data))
            continue
        rows, errors = [], []
        for i, line in enumerate(text.splitlines()):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                errors.append("line %d: not JSON" % (i + 1))
                continue
            if not isinstance(obj, dict):
                continue
            if "error" in obj:
                errors.append("line %d: %s" % (i + 1, obj["error"]))
                continue
            if "ri_phys_footprint" in obj and "wall_ms" in obj:
                rows.append(obj)
        if rows and (procstat_skip_s or procstat_until_s is not None):
            t0 = float(rows[0]["wall_ms"])
            kept = [r for r in rows if (float(r["wall_ms"]) - t0) / 1000.0 >= procstat_skip_s
                    and (procstat_until_s is None or (float(r["wall_ms"]) - t0) / 1000.0 <= procstat_until_s)]
            errors.append("window: kept %d of %d samples (from %.1f s to %s after the first sample)" % (
                len(kept), len(rows), procstat_skip_s,
                "the end" if procstat_until_s is None else "%.1f s" % procstat_until_s))
            rows = kept
        if rows:
            procs.append(ProcSeries(p, rows, errors))
        else:
            problems.append("%s: neither a bench JSON object (no 'scenario'), a context JSON object, nor procstat JSON lines" % p)
    return runs, procs, problems, contexts


# --------------------------------------------------------------------------
# evaluation
# --------------------------------------------------------------------------


def evaluate(check, samples_by_run, unit, extra=None):
    """Evaluate one check against pooled samples. Returns a result dict."""
    res = {"status": "UNMEASURED", "value": None, "detail": "", "n": 0}
    pooled = []
    for s in samples_by_run:
        pooled.extend(s)
    res["n"] = len(pooled)
    if not pooled:
        res["detail"] = "no samples"
        return res
    vals, err = convert(pooled, unit, check.get("sample_unit", check["unit"]))
    if err:
        res["detail"] = err
        return res
    stat = check["stat"]
    min_n = check.get("min_n")
    if min_n and len(vals) < min_n:
        res["detail"] = "n=%d < %d required" % (len(vals), min_n)
        return res
    limit = check["limit"]
    if stat.startswith("frac_le:"):
        x = float(stat.split(":", 1)[1])
        frac = sum(1 for v in vals if v <= x) / float(len(vals))
        value = frac
        res["detail"] = "%d/%d samples <= %g ms; longest %s ms" % (
            sum(1 for v in vals if v <= x), len(vals), x, fmt_plain(max(vals)))
    elif stat == "plateau":
        # Uses the run order of samples (batches); evaluated per run, worst run counts.
        worst = None
        details = []
        b = PLATEAU["baseline_batch"]
        for s in samples_by_run:
            sv, _ = convert(s, unit, "bytes")
            if len(sv) < max(b, 2):
                continue
            base = sv[b - 1]
            growth = sv[-1] - base
            allowed = max(PLATEAU["abs_bytes"], PLATEAU["rel"] * base)
            margin = growth - allowed
            details.append("batch%d %.1f MiB -> last %.1f MiB (growth %+.2f MiB, allowed %.2f MiB)" % (
                b, base / MiB, sv[-1] / MiB, growth / MiB, allowed / MiB))
            if worst is None or margin > worst[0]:
                worst = (margin, growth, allowed)
        if worst is None:
            res["detail"] = "need >= %d batches per run" % max(b, 2)
            return res
        value = worst[1]
        limit = worst[2]
        res["detail"] = "; ".join(details)
    elif stat.startswith("hitches:"):
        x = float(stat.split(":", 1)[1])
        over = sum(1 for v in vals if v > x)
        res.update({"value": float(over), "limit": None, "status": "INFO",
                    "detail": "longest interval %s ms; %d of %d intervals > %g ms" % (
                        fmt_plain(max(vals)), over, len(vals), x)})
        return res
    elif stat == "window_mean":
        if not extra or extra.get("window_mean") is None:
            res["detail"] = "no window"
            return res
        mw = check.get("min_window_s")
        if mw and extra["window_s"] < mw - WINDOW_JITTER_S:
            res["detail"] = "window %.3f s < %g s required" % (extra["window_s"], mw)
            res["value"] = extra["window_mean"]
            return res
        value = extra["window_mean"]
        res["detail"] = "window %.3f s" % extra["window_s"]
    else:
        st = summarize(vals)
        if stat not in st:
            res["detail"] = "unknown stat " + stat
            return res
        value = st[stat]
        if stat == "p99" and len(vals) < EXPLORATORY_N:
            res["detail"] = "p99 exploratory (n < %d)" % EXPLORATORY_N
    res["value"] = value
    res["limit"] = limit
    res["status"] = "PASS" if compare(value, check["op"], limit) else "MISS"
    return res


def check_text(check, limit=None):
    lim = check["limit"] if limit is None else limit
    if check["stat"].startswith("hitches:"):
        return "report only: longest interval, count > %s ms" % check["stat"].split(":", 1)[1]
    if check["stat"] == "plateau":
        return "growth after batch %d <= max(%g MiB, %g%%)" % (
            PLATEAU["baseline_batch"], PLATEAU["abs_bytes"] / MiB, PLATEAU["rel"] * 100)
    unit = check["unit"]
    if unit == "bytes":
        ls = "%s B" % format(int(lim), ",")
        if lim % MB == 0:
            ls = "%d MB (%s B)" % (lim // MB, format(int(lim), ","))
        elif lim % MiB == 0:
            ls = "%d MiB" % (lim // MiB)
    elif unit == "ratio":
        ls = "%g" % lim
    elif unit == "percent":
        ls = "%g%% of a core" % lim
    else:
        ls = "%g %s" % (lim, unit)
    stat = check["stat"]
    if stat.startswith("frac_le:"):
        stat = "share of samples <= %s ms" % stat.split(":", 1)[1]
    return "%s %s %s" % (stat, check["op"], ls)


def analyze(runs, procs):
    """Group samples, compute stats and threshold results."""
    groups = {}  # config -> list of runs
    for r in runs:
        groups.setdefault(r.config, []).append(r)
    rows = []  # threshold result rows
    metric_tables = {}  # config -> metric -> info
    for config in sorted(groups):
        rs = groups[config]
        names = []
        for r in rs:
            for m in r.metrics:
                if m not in names:
                    names.append(m)
        tables = {}
        for m in sorted(names):
            per_run = []
            unit = None
            proxy = None
            for r in rs:
                md = r.metrics.get(m)
                if not md:
                    per_run.append((r, []))
                    continue
                unit = md.get("unit", unit)
                proxy = md.get("proxy") or proxy
                per_run.append((r, clean(md.get("samples"))))
            pooled = [v for _, s in per_run for v in s]
            tables[m] = {"unit": unit, "proxy": proxy, "per_run": per_run, "pooled": summarize(pooled)}
            for check in THRESHOLDS.get(m, []):
                if rs[0].scenario not in check.get("scenarios", [rs[0].scenario]):
                    continue  # reported above, but this scenario does not define the gate
                res = evaluate(check, [s for _, s in per_run], unit or "")
                per = []
                for r, s in per_run:
                    if s:
                        pr = evaluate(check, [s], unit or "")
                        per.append((r.name, pr["status"], pr["value"]))
                res.update({"req": check["req"], "metric": m, "config": config, "check": check,
                            "per_run": per, "unit_out": check["unit"]})
                rows.append(res)
        metric_tables[config] = tables
    proc_tables = []
    for p in procs:
        d = p.derived()
        tbl = {}
        for m, (unit, samples, extra) in sorted(d.items()):
            tbl[m] = {"unit": unit, "summary": summarize(samples), "extra": extra}
            for check in THRESHOLDS.get(m, []):
                if "procstat" not in check.get("scenarios", ["procstat"]):
                    continue
                res = evaluate(check, [samples], unit, extra)
                res.update({"req": check["req"], "metric": m, "config": "procstat " + p.name, "check": check,
                            "per_run": [], "unit_out": check["unit"]})
                rows.append(res)
        proc_tables.append((p, tbl))
    # Requirement verdicts.
    verdicts = {}
    for req in REQUIREMENTS:
        mine = [r for r in rows if r["req"] == req]
        measured = [r for r in mine if r["status"] in ("PASS", "MISS")]  # INFO rows never gate
        if not measured:
            verdicts[req] = "UNMEASURED"
        elif any(r["status"] == "MISS" for r in measured):
            verdicts[req] = "MISS"
        else:
            verdicts[req] = "PASS"
    return groups, metric_tables, proc_tables, rows, verdicts


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------


def md_escape(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def value_text(r):
    if r["value"] is None:
        return "—"
    u = r["unit_out"]
    if u == "bytes":
        return fmt_num(r["value"], "bytes")
    if u == "ratio":
        return "%.4f" % r["value"]
    if u == "percent":
        return "%.3f%%" % r["value"]
    if u == "count":
        return fmt_plain(r["value"])
    return "%s %s" % (fmt_plain(r["value"]), u)


def context_lines(contexts):
    """Scalar facts from non-bench JSON objects (machine, power, fixture...)."""
    L = []
    for path, data in contexts:
        L.append("**%s**" % md_escape(os.path.basename(path)))
        L.append("")
        for k in sorted(data):
            v = data[k]
            if isinstance(v, dict):
                keep = dict((kk, v[kk]) for kk in ("seed", "content_hash", "pages", "db_bytes", "schema_version")
                            if kk in v)
                if keep:
                    L.append("- %s: %s" % (md_escape(k), md_escape(json.dumps(keep, sort_keys=True))))
            elif isinstance(v, list):
                if all(not isinstance(x, (dict, list)) for x in v) and len(v) <= 8:
                    L.append("- %s: %s" % (md_escape(k), md_escape("; ".join(str(x) for x in v))))
            else:
                L.append("- %s: %s" % (md_escape(k), md_escape(v)))
        L.append("")
    return L


def render(runs, procs, problems, analysis, title="Folia benchmark report", contexts=()):
    groups, metric_tables, proc_tables, rows, verdicts = analysis
    L = []
    L.append("# " + title)
    L.append("")
    L.append("Generated %s from %d bench file(s) and %d procstat file(s). Percentiles: nearest rank; "
             "p99 is exploratory when n < %d. Raw samples only; nothing is estimated." % (
                 datetime.datetime.now().isoformat(timespec="seconds"), len(runs), len(procs), EXPLORATORY_N))
    L.append("")
    if contexts:
        L.append("## Environment")
        L.append("")
        L.extend(context_lines(contexts))
    if problems:
        L.append("**Input problems:**")
        for p in problems:
            L.append("- " + md_escape(p))
        L.append("")
    # Requirement summary.
    L.append("## Requirements")
    L.append("")
    L.append("| Requirement | Verdict |")
    L.append("|---|---|")
    for req, text in REQUIREMENTS.items():
        L.append("| %s | **%s** |" % (md_escape(text), verdicts[req]))
    L.append("")
    L.append("## Threshold checks")
    L.append("")
    if rows:
        L.append("| Requirement | Configuration | Metric | Check | Measured | n | Status | Detail |")
        L.append("|---|---|---|---|---|---|---|---|")
        order = list(REQUIREMENTS)
        for r in sorted(rows, key=lambda x: (order.index(x["req"]), x["config"], x["metric"])):
            lim = r.get("limit")
            L.append("| %s | %s | `%s` | %s | %s | %d | **%s** | %s |" % (
                r["req"], md_escape(r["config"]), r["metric"], md_escape(check_text(r["check"], lim)),
                md_escape(value_text(r)), r["n"], r["status"], md_escape(r["detail"])))
    else:
        L.append("No metric in the inputs maps to a threshold.")
    L.append("")
    # Environment.
    if runs:
        L.append("## Runs")
        L.append("")
        L.append("| File | Configuration | OS | Qt | Preset | Window | DPR | Thermal | Power | Fixture | Time |")
        L.append("|---|---|---|---|---|---|---|---|---|---|---|")
        for r in runs:
            e = r.env
            fx = "%s / %s" % (e.get("fixture_seed", "—"), e.get("fixture_hash", "—"))
            L.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % tuple(
                md_escape(x) for x in (r.name, r.config, e.get("os", "—"), e.get("qt", "—"), e.get("preset", "—"),
                                       e.get("window_size", "—"), e.get("device_pixel_ratio", "—"),
                                       e.get("thermal_state", "—"), e.get("power_mode", "—"), fx, e.get("time", "—"))))
        L.append("")
    # Per configuration.
    for config in sorted(groups):
        rs = groups[config]
        L.append("## %s (%d run%s)" % (md_escape(config), len(rs), "" if len(rs) == 1 else "s"))
        L.append("")
        tables = metric_tables[config]
        for m, t in sorted(tables.items()):
            unit = t["unit"] or "?"
            head = "### `%s` (%s)" % (m, unit)
            L.append(head)
            if t["proxy"]:
                L.append("")
                L.append("Proxy: " + md_escape(t["proxy"]))
            L.append("")
            exploratory = t["pooled"].get("n", 0) < EXPLORATORY_N
            p99h = "p99 (exploratory)" if exploratory else "p99"
            L.append("| Run | n | p50 | p95 | %s | min | max | mean |" % p99h)
            L.append("|---|---|---|---|---|---|---|---|")
            entries = []
            if len(rs) > 1:
                for r, s in t["per_run"]:
                    entries.append((r.name, summarize(s)))
                entries.append(("**pooled**", t["pooled"]))
            else:
                entries.append((rs[0].name, t["pooled"]))
            for name, st in entries:
                if st.get("n", 0) == 0:
                    L.append("| %s | 0 | — | — | — | — | — | — |" % md_escape(name))
                    continue
                L.append("| %s | %d | %s | %s | %s | %s | %s | %s |" % (
                    md_escape(name), st["n"], fmt_plain(st["p50"]), fmt_plain(st["p95"]), fmt_plain(st["p99"]),
                    fmt_plain(st["min"]), fmt_plain(st["max"]), fmt_plain(st["mean"])))
            checks = [r for r in rows if r["config"] == config and r["metric"] == m]
            for c in checks:
                per = ", ".join("%s %s" % (n, s) for n, s, _ in c["per_run"]) if len(rs) > 1 else ""
                L.append("")
                L.append("Threshold (%s): %s -> measured %s: **%s**%s%s" % (
                    c["req"], check_text(c["check"], c.get("limit")), value_text(c), c["status"],
                    (" (" + c["detail"] + ")") if c["detail"] else "", ("; per run: " + per) if per else ""))
            L.append("")
        # counters / extras / notes per run
        L.append("### Counters, extra fields and notes")
        L.append("")
        for r in rs:
            L.append("**%s**" % md_escape(r.name))
            L.append("")
            if r.counters:
                L.append("- counters: " + ", ".join("%s=%s" % (k, fmt_plain(v) if isinstance(v, (int, float)) else v)
                                                    for k, v in sorted(r.counters.items())))
            if r.extra:
                L.append("- extra: " + md_escape(json.dumps(r.extra, sort_keys=True, ensure_ascii=False)))
            if r.process:
                L.append("- process: " + md_escape(json.dumps(r.process, sort_keys=True)))
            for n in r.notes:
                L.append("- note: " + md_escape(n))
            L.append("")
    for p, tbl in proc_tables:
        rows0 = p.rows
        L.append("## procstat %s (%d samples, %.1f s)" % (
            md_escape(p.name), len(rows0), (rows0[-1]["wall_ms"] - rows0[0]["wall_ms"]) / 1000.0))
        L.append("")
        L.append("| Series | unit | n | p50 | p95 | min | max | mean | last |")
        L.append("|---|---|---|---|---|---|---|---|---|")
        for m, t in sorted(tbl.items()):
            st = t["summary"]
            if st.get("n", 0) == 0:
                continue
            L.append("| `%s` | %s | %d | %s | %s | %s | %s | %s | %s |" % (
                m, t["unit"], st["n"], fmt_plain(st["p50"]), fmt_plain(st["p95"]), fmt_plain(st["min"]),
                fmt_plain(st["max"]), fmt_plain(st["mean"]), fmt_plain(st["last"])))
        cpu = tbl.get("procstat_cpu_percent_of_core")
        if cpu and cpu["extra"].get("window_mean") is not None:
            L.append("")
            L.append("Mean CPU over the whole window: %.4f%% of one core (%.1f s, %d intervals)." % (
                cpu["extra"]["window_mean"], cpu["extra"]["window_s"], cpu["summary"].get("n", 0)))
        for e in p.errors:
            L.append("- " + md_escape(e))
        L.append("")
    return "\n".join(L) + "\n"


def summary_json(analysis):
    groups, metric_tables, proc_tables, rows, verdicts = analysis
    out = {"verdicts": verdicts, "checks": []}
    for r in rows:
        out["checks"].append({"req": r["req"], "config": r["config"], "metric": r["metric"],
                              "check": check_text(r["check"], r.get("limit")), "value": r["value"],
                              "unit": r["unit_out"], "n": r["n"], "status": r["status"], "detail": r["detail"]})
    out["metrics"] = {}
    for config, tables in metric_tables.items():
        out["metrics"][config] = dict((m, dict(t["pooled"], unit=t["unit"])) for m, t in tables.items())
    return out


# --------------------------------------------------------------------------
# selftest
# --------------------------------------------------------------------------


def _bench(scenario, metrics, args=None, extra=None):
    d = {"scenario": scenario,
         "env": {"os": "selftest", "qt": "6.x", "preset": "Default", "args": args or {}, "fixture_seed": 1,
                 "fixture_hash": "fnv1a64:0"},
         "metrics": metrics, "counters": {"LuaRenders": 3}, "process": {"footprint_bytes": 1},
         "notes": ["synthetic"]}
    if extra:
        d.update(extra)
    return d


def selftest():
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    # Nearest-rank percentiles on 1..100.
    s = list(range(1, 101))
    expect(percentile(s, 50) == 50 and percentile(s, 95) == 95 and percentile(s, 99) == 99, "nearest rank 1..100")
    expect(percentile([5.0], 95) == 5.0, "single sample")
    expect(percentile(list(range(1, 11)), 95) == 10, "nearest rank n=10")
    st = summarize([3.0, 1.0, 2.0])
    expect(st["min"] == 1 and st["max"] == 3 and abs(st["mean"] - 2) < 1e-12 and st["last"] == 2.0, "summarize")
    expect(clean([1, None, float("nan"), True, 2.5, "x"]) == [1.0, 2.5], "clean")
    vals, err = convert([1500.0], "us", "ms")
    expect(err is None and abs(vals[0] - 1.5) < 1e-12, "us->ms")
    expect(convert([1.0], "bytes", "ms")[1] is not None, "unit mismatch detected")

    tmp = tempfile.mkdtemp(prefix="folia_analyze_selftest_")
    files = []

    def put(name, obj, indent=None):
        p = os.path.join(tmp, name)
        with open(p, "w", encoding="utf-8") as f:
            if isinstance(obj, str):
                f.write(obj)
            else:
                json.dump(obj, f, indent=indent)
        files.append(p)
        return p

    # open: two runs of the same config; uncached passes (p95 95 ms), cached misses (p95 40 ms).
    put("open_1.json", _bench("open", {
        "open_uncached_to_editable_ms": {"unit": "ms", "samples": list(range(1, 101)), "proxy": "paint end"},
        "open_cached_to_editable_ms": {"unit": "ms", "samples": [10.0] * 50}}, {"n": 120, "manifest": "/a.json"}))
    put("open_2.json", _bench("open", {
        "open_uncached_to_editable_ms": {"unit": "ms", "samples": list(range(1, 101))},
        "open_cached_to_editable_ms": {"unit": "ms", "samples": [40.0] * 50}}, {"n": 120, "manifest": "/b.json"}))
    # typing in us: main-thread p95 = 9 ms -> MISS; flush p95 = 20 ms -> PASS.
    put("typing.json", _bench("typing", {
        "edit_main_thread_us": {"unit": "us", "samples": [1000.0] * 94 + [9000.0] * 6},
        "input_to_flush_us": {"unit": "us", "samples": [20000.0] * 100},
        "input_to_paint_us": {"unit": "us", "samples": [15000.0] * 100}}, {"doc": "ordinary"}))
    # scroll: frame work 96/100 <= 16.7 -> PASS; paint intervals: 2 hitches > 25 ms, longest 120 ms (INFO).
    put("scroll.json", _bench("scroll", {
        "scroll_frame_work_ms": {"unit": "ms", "samples": [8.0] * 96 + [17.0, 20.0, 30.0, 40.0]},
        "scroll_paint_interval_ms": {"unit": "ms", "samples": [16.7] * 98 + [30.0, 120.0]},
        "scroll_frame_ms": {"unit": "ms", "samples": [99.0] * 100}}))
    # Runs of other scenarios that emit gated metrics are reported, never gated:
    # bad typing and open numbers from integration/stress must not change verdicts.
    put("integration.json", _bench("integration", {
        "edit_main_thread_us": {"unit": "us", "samples": [50000.0] * 100},
        "input_to_flush_us": {"unit": "us", "samples": [500000.0] * 100},
        "open_uncached_to_editable_ms": {"unit": "ms", "samples": [5000.0] * 10}}))
    put("stress.json", _bench("stress", {
        "tree_expand_main_us": {"unit": "us", "samples": [90000.0] * 10},
        "idle_footprint_bytes": {"unit": "bytes", "samples": [900.0 * MB]}}))
    # Typing configurations stay separate rows (doc / bgrender / recover / preset).
    put("typing_bg.json", _bench("typing", {
        "edit_main_thread_us": {"unit": "us", "samples": [2000.0] * 100}}, {"doc": "ordinary", "bgrender": "true"}))
    # idle: 60 one-second samples, mean 0.5% -> PASS; footprint 61 MB -> MISS.
    put("idle.json", _bench("idle", {
        "idle_cpu_percent_of_core": {"unit": "ratio", "samples": [0.5] * 60},
        "idle_footprint_bytes": {"unit": "bytes", "samples": [61.0 * MB] * 60}}))
    # navigate: 5 batches, plateau after batch 2 (+1 MiB) and caches within caps -> PASS.
    put("navigate.json", _bench("navigate", {
        "navigate_settled_footprint_bytes": {"unit": "bytes", "samples": [
            80 * MiB, 90 * MiB, 90.5 * MiB, 91 * MiB, 91 * MiB]},
        "navigate_doc_cache_bytes_est": {"unit": "bytes", "samples": [40 * MiB] * 5},
        "navigate_doc_cache_entries": {"unit": "ratio", "samples": [16] * 5},
        "navigate_preview_cache_bytes": {"unit": "bytes", "samples": [1 * MiB] * 5}}))
    # procstat: 61 samples, 1 s apart, 5 ms CPU per second = 0.5% -> PASS; footprint 50 MB.
    lines = []
    for i in range(61):
        lines.append(json.dumps({"sample": i, "wall_ms": i * 1000.0, "ri_phys_footprint": 50 * MB,
                                 "ri_lifetime_max_phys_footprint": 52 * MB, "ri_resident_size": 70 * MB,
                                 "ri_user_time_ns": i * 4000000, "ri_system_time_ns": i * 1000000,
                                 "ri_interrupt_wkups": i * 3, "ri_pkg_idle_wkups": i * 2}))
    lines.append(json.dumps({"sample": 61, "wall_ms": 61000.0, "error": "proc_pid_rusage: No such process"}))
    put("idle_procstat.jsonl", "\n".join(lines) + "\n")
    # Short procstat window -> UNMEASURED for CPU (needs 60 s).
    put("short_procstat.jsonl", "\n".join(json.dumps(
        {"sample": i, "wall_ms": i * 100.0, "ri_phys_footprint": 10 * MB, "ri_resident_size": 1,
         "ri_user_time_ns": 0, "ri_system_time_ns": 0, "ri_interrupt_wkups": 0, "ri_pkg_idle_wkups": 0})
        for i in range(5)) + "\n")
    put("garbage.txt", "hello\n")
    # The window option: skipping 30 s of a 60 s series leaves 30 s -> CPU UNMEASURED (needs 60 s).
    _, wprocs, _, _ = load_inputs([files[-3]], procstat_skip_s=30.0)
    wrows = analyze([], wprocs)[3]
    wcpu = [r for r in wrows if r["metric"] == "procstat_cpu_percent_of_core"]
    expect(len(wprocs) == 1 and len(wprocs[0].rows) == 31 and wcpu and wcpu[0]["status"] == "UNMEASURED",
           "procstat window option")

    put("environment.json", {"machine": "MacBookAir10,1", "low_power_mode": False, "power": ["AC", "100%"],
                             "ram_bytes": 8589934592,
                             "fixture": {"seed": 42, "content_hash": "fnv1a64:0", "sample_pages": [1, 2]}},
        indent=2)  # pretty-printed, like bench/run_suite.py writes it
    # Same scenario and args but another layout (--preset keyboard): its own configuration.
    put("idle_keyboard.json", _bench("idle", {
        "idle_cpu_percent_of_core": {"unit": "ratio", "samples": [0.2] * 60}}))
    with open(files[-1], "r+", encoding="utf-8") as f:
        d = json.load(f)
        d["env"]["preset"] = "Keyboard"
        f.seek(0)
        f.truncate()
        json.dump(d, f)
    runs, procs, problems, contexts = load_inputs(files)
    expect(len(runs) == 10 and len(procs) == 2 and len(problems) == 1 and len(contexts) == 1, "input classification")
    expect(sorted(r.config for r in runs if r.scenario == "idle") == ["idle preset=Default", "idle preset=Keyboard"],
           "flags that change the layout split configurations")
    analysis = analyze(runs, procs)
    groups, metric_tables, proc_tables, rows, verdicts = analysis
    expect(len(groups["open n=120 preset=Default"]) == 2, "runs grouped by config (manifest path ignored)")

    def row(metric, config_prefix=""):
        for r in rows:
            if r["metric"] == metric and r["config"].startswith(config_prefix):
                return r
        return None

    r = row("open_uncached_to_editable_ms")
    expect(r and r["status"] == "PASS" and r["value"] == 95 and r["n"] == 200, "uncached pooled p95")
    r = row("open_cached_to_editable_ms")
    expect(r and r["status"] == "MISS" and r["value"] == 40, "cached pooled p95 40 -> MISS")
    expect(r and [x[1] for x in r["per_run"]] == ["PASS", "MISS"], "per-run verdicts")
    r = row("edit_main_thread_us", "typing doc=")
    expect(r and r["status"] == "MISS" and abs(r["value"] - 9.0) < 1e-9, "us converted, p95 9 ms MISS")
    r = row("input_to_flush_us")
    expect(r and r["status"] == "PASS" and abs(r["value"] - 20.0) < 1e-9, "flush p95 20 ms PASS")
    r = row("scroll_frame_work_ms")
    expect(r and r["status"] == "PASS" and abs(r["value"] - 0.96) < 1e-12, "scroll frame-work share")
    r = row("scroll_paint_interval_ms")
    expect(r and r["status"] == "INFO" and r["value"] == 2 and "longest interval 120" in r["detail"], "scroll hitches")
    expect(row("scroll_frame_ms") is None, "old scroll_frame_ms is not a gate")
    gated = set((x["config"].split(" ")[0], x["metric"]) for x in rows)
    expect(not any(sc in ("integration", "stress") for sc, _ in gated), "non-defining scenarios are not gated")
    expect("integration" in metric_tables.get("integration preset=Default", {}).get(
        "edit_main_thread_us", {}).get("per_run", [[None]])[0][0].scenario, "integration metrics still reported")
    typing_rows = sorted(x["config"] for x in rows if x["metric"] == "edit_main_thread_us")
    expect(typing_rows == ["typing bgrender=true doc=ordinary preset=Default", "typing doc=ordinary preset=Default"],
           "typing configurations are separate rows")
    r = row("idle_cpu_percent_of_core")
    expect(r and r["status"] == "PASS", "idle cpu mean")
    r = row("idle_footprint_bytes")
    expect(r and r["status"] == "MISS", "61 MB > 60 MB")
    r = row("navigate_settled_footprint_bytes")
    expect(r and r["status"] == "PASS" and abs(r["value"] - 1 * MiB) < 1, "plateau growth after batch 2")
    r = row("procstat_cpu_percent_of_core", "procstat idle")
    expect(r and r["status"] == "PASS" and abs(r["value"] - 0.5) < 1e-9, "procstat window mean 0.5%")
    r = row("procstat_cpu_percent_of_core", "procstat short")
    expect(r and r["status"] == "UNMEASURED" and "window" in r["detail"], "short window unmeasured")
    r = row("procstat_phys_footprint_bytes", "procstat idle")
    expect(r and r["status"] == "PASS", "procstat footprint 50 MB")
    expect(verdicts["open_uncached"] == "PASS" and verdicts["open_cached"] == "MISS", "verdicts open")
    expect(verdicts["typing_main"] == "MISS" and verdicts["typing_presented"] == "PASS", "verdicts typing")
    expect(verdicts["idle_cpu"] == "PASS" and verdicts["idle_footprint"] == "MISS", "verdicts idle")
    expect(verdicts["create_title"] == "UNMEASURED" and verdicts["expand_main"] == "UNMEASURED", "unmeasured")
    expect(verdicts["nav_plateau"] == "PASS" and verdicts["scroll"] == "PASS", "verdicts nav/scroll")
    report = render(runs, procs, problems, analysis, title="Folia benchmark report (selftest)", contexts=contexts)
    expect("MacBookAir10,1" in report and "fnv1a64:0" in report, "environment context rendered")
    expect("p99 (exploratory)" in report, "p99 exploratory label")
    expect("**pooled**" in report, "pooled row for multi-run config")
    expect("UNMEASURED" in report and "MISS" in report and "PASS" in report, "verdict words")
    # p99 is not labelled exploratory at n >= 1000.
    big = summarize(list(range(1000)))
    expect(big["p99"] == 989, "p99 n=1000 nearest rank")
    return failures, report


# --------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("inputs", nargs="*", help="bench JSON files and procstat JSON-lines files")
    ap.add_argument("-o", "--output", help="write the markdown report here (default: stdout)")
    ap.add_argument("--json", help="also write a machine-readable summary")
    ap.add_argument("--title", default="Folia benchmark report")
    ap.add_argument("--fail-on-miss", action="store_true", help="exit 1 if any threshold check is MISS")
    ap.add_argument("--procstat-skip", type=float, default=0.0, metavar="S",
                    help="drop procstat samples taken in the first S seconds (e.g. start-up / settle)")
    ap.add_argument("--procstat-until", type=float, default=None, metavar="S",
                    help="drop procstat samples taken more than S seconds after the first one")
    ap.add_argument("--selftest", action="store_true", help="run against synthetic data and exit")
    a = ap.parse_args(argv)
    if a.selftest:
        failures, report = selftest()
        sys.stdout.write(report)
        if failures:
            sys.stdout.write("\nselftest FAILED: %d check(s): %s\n" % (len(failures), "; ".join(failures)))
            return 1
        sys.stdout.write("\nselftest OK\n")
        return 0
    if not a.inputs:
        ap.error("no input files (or use --selftest)")
    runs, procs, problems, contexts = load_inputs(a.inputs, a.procstat_skip, a.procstat_until)
    analysis = analyze(runs, procs)
    report = render(runs, procs, problems, analysis, a.title, contexts)
    if a.output:
        with open(a.output, "w", encoding="utf-8") as f:
            f.write(report)
    else:
        sys.stdout.write(report)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(summary_json(analysis), f, indent=2, sort_keys=True)
    if a.fail_on_miss and any(r["status"] == "MISS" for r in analysis[3]):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
