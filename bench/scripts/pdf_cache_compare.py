#!/usr/bin/env python3
"""Compare `--bench pdf` runs at several media budgets or cache caps:
    python3 bench/scripts/pdf_cache_compare.py bench/results/pdf-cache-2026-11-06/pdf-*.json"""
import json, sys, re

MB = 1e6


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return float('nan')
    k = (len(xs) - 1) * p / 100
    f = int(k)
    c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def longest_run(xs, pred):
    best = cur = 0
    for x in xs:
        cur = cur + 1 if pred(x) else 0
        best = max(best, cur)
    return best


runs = []
for path in sys.argv[1:]:
    d = json.load(open(path))
    m = {k: v['samples'] for k, v in d['metrics'].items()}
    b = int(round(d['media_budget_bytes'] / 2**20))
    c = int(round(d.get('media_cache_max_bytes', d['media_budget_bytes']) / 2**20))
    runs.append((f'budget {b}' + (f', cache {c}' if c < b else ''), d, m, path))

rows = []


def row(label, fn):
    vals = []
    for mib, d, m, _ in runs:
        try:
            vals.append(fn(d, m))
        except (KeyError, ValueError, ZeroDivisionError, TypeError):
            vals.append('–')
    rows.append((label, vals))


def mb(key):
    return lambda d, m: f"{d[key] / MB:.1f}"


def num(key, fmt='{:.0f}'):
    return lambda d, m: fmt.format(d[key])


def section(t):
    rows.append((f'**{t}**', [''] * len(runs)))


section('200-page document')
row('footprint before open (MB)', mb('long_footprint_before_open'))
row('scroll down: footprint max sampled (MB)', mb('down_footprint_max_sampled'))
row('scroll down: rusage peak (MB)', mb('down_footprint_peak'))
row('second pass: max sampled (MB)', mb('down2_footprint_max_sampled'))
row('media cache max (MB)', mb('down_media_cache_max'))
row('media use max (MB)', mb('down_media_used_max'))
row('first viewport (ms)', num('long_open_first_viewport_ms'))
row('scroll down: renders', num('down_renders'))
row('scroll down: process CPU (s)', num('down_process_cpu_s', '{:.1f}'))
row('scroll up: renders / hits', lambda d, m: f"{d['up_renders']:.0f} / {d['up_cache_hits']:.0f}")
row('scroll up: process CPU (s)', num('up_process_cpu_s', '{:.1f}'))
for ph, key in [('down', 'pdf_scroll_down'), ('up', 'pdf_scroll_up'), ('down 2', 'pdf_scroll_down2')]:
    row(f'{ph}: frames with blank area (%)',
        lambda d, m, k=key: f"{100 * sum(x > 0 for x in m[k + '_blank_permille']) / len(m[k + '_blank_permille']):.1f}")
    row(f'{ph}: blank area mean / p95 (‰)',
        lambda d, m, k=key: f"{sum(m[k + '_blank_permille']) / len(m[k + '_blank_permille']):.1f} / {pct(m[k + '_blank_permille'], 95):.0f}")
    row(f'{ph}: longest blank run (frames)', lambda d, m, k=key: f"{longest_run(m[k + '_blank_permille'], lambda x: x > 0)}")
row('scroll down: viewer paint work p50/p95/p99/max (ms)',
    lambda d, m: '/'.join(f"{pct(m['pdf_scroll_down_view_paint_us'], p) / 1000:.1f}" for p in (50, 95, 99, 100)))
for ph, key in [('down', 'pdf_scroll_down_frame_ms'), ('up', 'pdf_scroll_up_frame_ms')]:
    row(f'{ph}: painted frames / unpainted', lambda d, m, k=key: f"{len(m.get(k, []))} / {d.get(k + '_unpainted_frames', 0)}")
    row(f'{ph}: interval p50/p95/p99/max (ms)',
        lambda d, m, k=key: '/'.join(f"{pct(m[k], p):.2f}" for p in (50, 95, 99, 100)))
    row(f'{ph}: missed frames (>25 ms)', lambda d, m, k=key: f"{sum(x > 25 for x in m[k])}")

section('Zoom')
row('renders / cache hits', lambda d, m: f"{d['zoom_renders']:.0f} / {d['zoom_cache_hits']:.0f}")
row('settle → sharp p50 / max (ms)',
    lambda d, m: f"{pct(m['pdf_zoom_settle_to_sharp_ms'], 50):.0f} / {max(m['pdf_zoom_settle_to_sharp_ms']):.0f}")
row('zoom process CPU (s)', num('zoom_process_cpu_s', '{:.2f}'))
row('4×: first viewport (ms)', num('high_zoom_first_viewport_ms'))
row('4× scroll: frames with blank (%)',
    lambda d, m: f"{100 * sum(x > 0 for x in m['pdf_high_zoom_scroll_blank_permille']) / len(m['pdf_high_zoom_scroll_blank_permille']):.1f}")

section('Close')
row('footprint before close (MB)', mb('long_close_footprint_before'))
row('footprint after close (MB)', mb('long_close_footprint_after'))
row('after malloc pressure relief (MB)', mb('long_close_footprint_after_release'))
row('bench canvas freed first (MB)', mb('long_close_offscreen_canvas_bytes'))

section('Heavy documents')
for kind in ('scanned', 'vector', 'transparency'):
    row(f'{kind}: first / last viewport (ms)', lambda d, m, k=kind: f"{d[k + '_first_viewport_ms']:.0f} / {d[k + '_last_viewport_ms']:.0f}")
    row(f'{kind}: renders / discarded / CPU (s)',
        lambda d, m, k=kind: f"{d[k + '_renders']:.0f} / {d[k + '_jobs_discarded']:.0f} / {d[k + '_process_cpu_s']:.1f}")
    row(f'{kind}: footprint before → peak → after close (MB)',
        lambda d, m, k=kind: f"{d[k + '_footprint_before'] / MB:.0f} → {d[k + '_footprint_peak'] / MB:.0f} → {d[k + '_footprint_after_close'] / MB:.0f}")
    row(f'{kind}: frames with blank (%)',
        lambda d, m, k=kind: f"{100 * sum(x > 0 for x in m['pdf_' + k + '_scroll_blank_permille']) / len(m['pdf_' + k + '_scroll_blank_permille']):.0f}")

section('Pressure')
row('warning: cache before → after (MB)', lambda d, m: f"{d['warning_cache_before'] / MB:.1f} → {d['warning_cache_after_trim'] / MB:.1f}")
row('warning: footprint before → after (MB)', lambda d, m: f"{d['warning_footprint_before'] / MB:.1f} → {d['warning_footprint_after_trim'] / MB:.1f}")
row('critical: footprint after trim (MB)', mb('pressure_footprint_after_trim'))
row('critical: budget (MB) / tiles / waits', lambda d, m: f"{d['pressure_budget_limit'] / MB:.1f} / {d['pressure_tiles']:.0f} / {d['pressure_budget_waits']:.0f}")

section('Repeated open → scroll → close')
row('cycles', num('cycles'))
row('footprint before (MB)', mb('cycles_footprint_before'))
for doc in ('long', 'scanned'):
    row(f'after closing {doc}: first / median / last / max (MB)',
        lambda d, m, k=doc: '/'.join(f"{v / MB:.0f}" for v in (m['cycle_' + k + '_close_footprint_bytes'][0],
                                                               pct(m['cycle_' + k + '_close_footprint_bytes'], 50),
                                                               m['cycle_' + k + '_close_footprint_bytes'][-1],
                                                               max(m['cycle_' + k + '_close_footprint_bytes']))))
row('after all cycles → after relief (MB)', lambda d, m: f"{d['cycles_footprint_after'] / MB:.1f} → {d['cycles_footprint_after_release'] / MB:.1f}")
row('cycles: renders / CPU (s)', lambda d, m: f"{d['cycles_renders']:.0f} / {d['cycles_process_cpu_s']:.1f}")

section('Typing beside a rendering PDF')
row('input → end of paint p50/p95/p99/max (ms)',
    lambda d, m: '/'.join(f"{pct(m['pdf_beside_typing_input_to_paint_us'], p) / 1000:.2f}" for p in (50, 95, 99, 100)))
row('input → after flush p50/p95/p99/max (ms)',
    lambda d, m: '/'.join(f"{pct(m['pdf_beside_typing_input_to_flush_us'], p) / 1000:.2f}" for p in (50, 95, 99, 100)))

section('Idle')
row('text page / viewer open / after close (% core, median)',
    lambda d, m: ' / '.join(f"{pct(m[k + '_cpu_percent_of_core'], 50):.2f}" for k in ('idle_text', 'idle_viewer', 'idle_after_close')))
row('footprint idle after close (MB)', mb('idle_after_close_footprint_bytes'))
row('window exposed ratio', num('window_exposed_and_active_ratio', '{:.2f}'))

print('| Measure (MiB) | ' + ' | '.join(r[0] for r in runs) + ' |')
print('|---|' + '---:|' * len(runs))
for label, vals in rows:
    print(f'| {label} | ' + ' | '.join(vals) + ' |')
