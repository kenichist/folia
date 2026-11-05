# Folia acceptance run — MacBook Air M1 8 GB, macOS 26.6.2

Generated 2026-11-05T10:44:03 from 45 bench file(s) and 0 procstat file(s). Percentiles: nearest rank; p99 is exploratory when n < 1000. Raw samples only; nothing is estimated.

## Environment

**environment.json**

- app: <repo>/build/src/Folia.app/Contents/MacOS/Folia
- cpu: Apple M1
- display: built-in Retina 60 Hz (2560x1600 panel)
- fixture: {"content_hash": "fnv1a64:f5ecfb8eecd9058b", "db_bytes": 47779840, "pages": 50000, "schema_version": 1, "seed": 42}
- git_commit: uncommitted working tree
- host: <host>
- low_power_mode: False
- machine: MacBookAir10,1
- os: 26.6.2 (25G83)
- power: Now drawing from 'Battery Power';  -InternalBattery-0 (id=22544483)	82%; discharging; (no estimate) present: true
- ram_bytes: 8589934592
- thermal: Note: No thermal warning level has been recorded Note: No performance warning level has been recorded Note: No CPU power status has been recorded
- time: 2026-11-05T07:57:30+0700

**fixture-manifest.json**

- blocks: 214616
- broad_children: 5000
- broad_parent: 9
- code_page: 7
- content_hash: fnv1a64:f5ecfb8eecd9058b
- db_bytes: 47779840
- db_path: <scratch>/fx/workspace.db
- deep_leaf: 5265
- deep_leaf_depth: 256
- deep_root: 5010
- fixtures_parent: 1
- generator: folia_fixturegen
- generator_version: 1
- hash_algorithm: FNV-1a 64 over page rows then block rows in insertion order; ints as 8 LE bytes, strings as 8-byte length + UTF-8 (tools/fixturegen.cpp)
- math_page: 8
- max_depth: 256
- ordinary_page: 2
- page: 2
- pages: 50000
- schema_version: 1
- seed: 42
- stress_page: 6
- text_bytes_utf8: 30125739
- top_level_pages: 20

**recovery.json**

- all_ok: True

**summary.json**


## Requirements

| Requirement | Verdict |
|---|---|
| Open ordinary page -> first editable, uncached: p95 <= 100 ms | **PASS** |
| Open ordinary page -> first editable, cached: p95 <= 30 ms | **PASS** |
| Create page -> editable title: p95 <= 30 ms | **PASS** |
| Expand branch, main-thread time: p95 <= 16 ms | **PASS** |
| Typing, main-thread time per edit: p95 <= 8 ms | **PASS** |
| Typing, event -> presented glyph: p95 <= 33 ms | **PASS** |
| Large-note scroll: >= 95% of frames <= 16.7 ms (longest hitch reported) | **PASS** |
| Visible short code block until presentation: <= 30 ms | **PASS** |
| Typical formula until presentation: <= 100 ms | **MISS** |
| Idle footprint <= 60 MB (stretch target) | **MISS** |
| Idle CPU: mean < 1% of one core over 60 s | **PASS** |
| Repeated navigation (100 switches x 5 batches): footprint plateau within cache caps | **PASS** |

## Threshold checks

| Requirement | Configuration | Metric | Check | Measured | n | Status | Detail |
|---|---|---|---|---|---|---|---|
| open_uncached | open n=120 preset=Default | `open_uncached_to_editable_ms` | p95 <= 100 ms | 17.920 ms | 360 | **PASS** |  |
| open_cached | open n=120 preset=Default | `open_cached_to_editable_ms` | p95 <= 30 ms | 13.201 ms | 357 | **PASS** |  |
| create_title | create n=120 preset=Default | `create_to_editable_title_ms` | p95 <= 30 ms | 0.2245 ms | 360 | **PASS** |  |
| create_title | create n=120 preset=Default | `page_new_to_editable_title_us` | p95 <= 30 ms | 0.218 ms | 360 | **PASS** |  |
| expand_main | expand n=100 preset=Default | `expand_main_thread_ms` | p95 <= 16 ms | 0.9139 ms | 600 | **PASS** |  |
| expand_main | expand n=100 preset=Default | `tree_expand_main_us` | p95 <= 16 ms | 0.433 ms | 614 | **PASS** |  |
| typing_main | typing bgrender=1 doc=ordinary n=1000 preset=Default | `edit_main_thread_us` | p95 <= 8 ms | 0.956 ms | 2150 | **PASS** |  |
| typing_main | typing doc=code n=500 preset=Default | `edit_main_thread_us` | p95 <= 8 ms | 0.365 ms | 1072 | **PASS** |  |
| typing_main | typing doc=ordinary n=1000 preset=Default | `edit_main_thread_us` | p95 <= 8 ms | 1.036 ms | 3225 | **PASS** |  |
| typing_main | typing doc=ordinary n=1000 preset=Keyboard | `edit_main_thread_us` | p95 <= 8 ms | 1.049 ms | 3225 | **PASS** |  |
| typing_main | typing doc=ordinary n=1000 recover=1 preset=Default | `edit_main_thread_us` | p95 <= 8 ms | 1.050 ms | 2150 | **PASS** |  |
| typing_main | typing doc=stress n=500 preset=Default | `edit_main_thread_us` | p95 <= 8 ms | 0.777 ms | 1072 | **PASS** |  |
| typing_presented | typing bgrender=1 doc=ordinary n=1000 preset=Default | `input_to_flush_us` | p95 <= 33 ms | 10.139 ms | 2002 | **PASS** |  |
| typing_presented | typing bgrender=1 doc=ordinary n=1000 preset=Default | `input_to_paint_us` | p95 <= 33 ms | 7.945 ms | 2002 | **PASS** |  |
| typing_presented | typing doc=code n=500 preset=Default | `input_to_flush_us` | p95 <= 33 ms | 6.466 ms | 1000 | **PASS** |  |
| typing_presented | typing doc=code n=500 preset=Default | `input_to_paint_us` | p95 <= 33 ms | 4.506 ms | 1000 | **PASS** |  |
| typing_presented | typing doc=ordinary n=1000 preset=Default | `input_to_flush_us` | p95 <= 33 ms | 10.695 ms | 3003 | **PASS** |  |
| typing_presented | typing doc=ordinary n=1000 preset=Default | `input_to_paint_us` | p95 <= 33 ms | 8.399 ms | 3003 | **PASS** |  |
| typing_presented | typing doc=ordinary n=1000 preset=Keyboard | `input_to_flush_us` | p95 <= 33 ms | 10.798 ms | 3003 | **PASS** |  |
| typing_presented | typing doc=ordinary n=1000 preset=Keyboard | `input_to_paint_us` | p95 <= 33 ms | 8.189 ms | 3003 | **PASS** |  |
| typing_presented | typing doc=ordinary n=1000 recover=1 preset=Default | `input_to_flush_us` | p95 <= 33 ms | 10.344 ms | 2002 | **PASS** |  |
| typing_presented | typing doc=ordinary n=1000 recover=1 preset=Default | `input_to_paint_us` | p95 <= 33 ms | 7.881 ms | 2002 | **PASS** |  |
| typing_presented | typing doc=stress n=500 preset=Default | `input_to_flush_us` | p95 <= 33 ms | 11.870 ms | 1000 | **PASS** |  |
| typing_presented | typing doc=stress n=500 preset=Default | `input_to_paint_us` | p95 <= 33 ms | 9.399 ms | 1000 | **PASS** |  |
| scroll | scroll frames=1200 preset=Default | `scroll_frame_work_ms` | share of samples <= 16.7 ms >= 0.95 | 1.0000 | 3600 | **PASS** | 3600/3600 samples <= 16.7 ms; longest 9.324 ms |
| scroll | scroll frames=1200 preset=Default | `scroll_paint_interval_ms` | report only: longest interval, count > 25 ms | 11 | 4344 | **INFO** | longest interval 57.819 ms; 11 of 4344 intervals > 25 ms |
| code_visible | presentation n=100 preset=Default | `code_block_visible_ms` | p95 <= 30 ms | 16.706 ms | 300 | **PASS** |  |
| formula_visible | presentation n=100 preset=Default | `formula_cache_hit_ms` | p95 <= 100 ms | 33.386 ms | 297 | **PASS** |  |
| formula_visible | presentation n=100 preset=Default | `formula_first_in_process_ms` | max <= 100 ms | 249.97 ms | 3 | **MISS** |  |
| formula_visible | presentation n=100 preset=Default | `formula_uncached_ms` | p95 <= 100 ms | 33.388 ms | 297 | **PASS** |  |
| idle_footprint | idle page=2 seconds=60 preset=Default | `idle_footprint_bytes` | max <= 60 MB (60,000,000 B) | 80,432,384 (76.71 MiB) | 120 | **MISS** |  |
| idle_footprint | idle page=2 seconds=60 preset=Default math=off | `idle_footprint_bytes` | max <= 60 MB (60,000,000 B) | 81,005,824 (77.25 MiB) | 60 | **MISS** |  |
| idle_footprint | idle page=2 seconds=60 preset=Keyboard | `idle_footprint_bytes` | max <= 60 MB (60,000,000 B) | 79,400,192 (75.72 MiB) | 120 | **MISS** |  |
| idle_footprint | idle page=2 seconds=60 preset=Minimal math=off | `idle_footprint_bytes` | max <= 60 MB (60,000,000 B) | 76,188,928 (72.66 MiB) | 60 | **MISS** |  |
| idle_footprint | idle page=6 seconds=60 preset=Default | `idle_footprint_bytes` | max <= 60 MB (60,000,000 B) | 114,462,016 (109.16 MiB) | 60 | **MISS** |  |
| idle_cpu | idle page=2 seconds=60 preset=Default | `idle_cpu_percent_of_core` | mean < 1% of a core | 0.184% | 120 | **PASS** |  |
| idle_cpu | idle page=2 seconds=60 preset=Default math=off | `idle_cpu_percent_of_core` | mean < 1% of a core | 0.173% | 60 | **PASS** |  |
| idle_cpu | idle page=2 seconds=60 preset=Keyboard | `idle_cpu_percent_of_core` | mean < 1% of a core | 0.216% | 120 | **PASS** |  |
| idle_cpu | idle page=2 seconds=60 preset=Minimal math=off | `idle_cpu_percent_of_core` | mean < 1% of a core | 0.171% | 60 | **PASS** |  |
| idle_cpu | idle page=6 seconds=60 preset=Default | `idle_cpu_percent_of_core` | mean < 1% of a core | 0.189% | 60 | **PASS** |  |
| nav_plateau | navigate batches=5 per_batch=100 preset=Default | `navigate_doc_cache_bytes_est` | max <= 48 MiB | 3,733,058 (3.56 MiB) | 5 | **PASS** |  |
| nav_plateau | navigate batches=5 per_batch=100 preset=Default | `navigate_doc_cache_entries` | max <= 16 count | 16 | 5 | **PASS** |  |
| nav_plateau | navigate batches=5 per_batch=100 preset=Default | `navigate_preview_cache_bytes` | max <= 4 MiB | 0 (0.00 MiB) | 5 | **PASS** |  |
| nav_plateau | navigate batches=5 per_batch=100 preset=Default | `navigate_settled_footprint_bytes` | growth after batch 2 <= max(4 MiB, 5%) | 688,128 (0.66 MiB) | 5 | **PASS** | batch2 104.9 MiB -> last 105.6 MiB (growth +0.66 MiB, allowed 5.24 MiB) |

## Runs

| File | Configuration | OS | Qt | Preset | Window | DPR | Thermal | Power | Fixture | Time |
|---|---|---|---|---|---|---|---|---|---|---|
| create.0.json | create n=120 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:25:34 |
| create.1.json | create n=120 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:25:42 |
| create.2.json | create n=120 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:25:49 |
| expand.0.json | expand n=100 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x763 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:27:35 |
| expand.1.json | expand n=100 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x763 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:30:46 |
| expand.2.json | expand n=100 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x763 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:33:50 |
| idle_default.0.json | idle page=2 seconds=60 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x763 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:36:09 |
| idle_default.1.json | idle page=2 seconds=60 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:37:16 |
| idle_editor_only_shell.0.json | idle page=2 seconds=60 preset=Minimal math=off | macOS 26.6.2 | 6.12.0 | Minimal | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:41:44 |
| idle_keyboard_preset.0.json | idle page=2 seconds=60 preset=Keyboard | macOS 26.6.2 | 6.12.0 | Keyboard | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:38:23 |
| idle_keyboard_preset.1.json | idle page=2 seconds=60 preset=Keyboard | macOS 26.6.2 | 6.12.0 | Keyboard | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:39:31 |
| idle_no_math.0.json | idle page=2 seconds=60 preset=Default math=off | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:40:38 |
| idle_stress_note.0.json | idle page=6 seconds=60 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:42:52 |
| integration.0.json | integration math_page=2 ordinary_page=6010 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x763 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:20:20 |
| launch.0.json | launch start_page=2 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:43:30 |
| launch.1.json | launch start_page=2 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:43:33 |
| launch.2.json | launch start_page=2 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:43:36 |
| launch.3.json | launch start_page=2 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:43:38 |
| launch.4.json | launch start_page=2 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:43:41 |
| navigate.0.json | navigate batches=5 per_batch=100 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | fair | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:37:17 |
| open.0.json | open n=120 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x762 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:17:06 |
| open.1.json | open n=120 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x763 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:17:16 |
| open.2.json | open n=120 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x763 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-05T10:17:26 |
| presentation.0.json | presentation n=100 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:09:09 |
| presentation.1.json | presentation n=100 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:09:24 |
| presentation.2.json | presentation n=100 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:09:38 |
| scroll.0.json | scroll frames=1200 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:07:58 |
| scroll.1.json | scroll frames=1200 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:08:20 |
| scroll.2.json | scroll frames=1200 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:08:43 |
| stress.0.json | stress n=20 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:08:49 |
| stress.1.json | stress n=20 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:08:54 |
| typing.0.json | typing doc=ordinary n=1000 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:04:11 |
| typing.1.json | typing doc=ordinary n=1000 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:04:29 |
| typing.2.json | typing doc=ordinary n=1000 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:04:47 |
| typing_after_invalid_theme.0.json | typing doc=ordinary n=1000 recover=1 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:06:00 |
| typing_after_invalid_theme.1.json | typing doc=ordinary n=1000 recover=1 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:06:19 |
| typing_during_bg_render.0.json | typing bgrender=1 doc=ordinary n=1000 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:06:37 |
| typing_during_bg_render.1.json | typing bgrender=1 doc=ordinary n=1000 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:06:56 |
| typing_huge_code_block.0.json | typing doc=code n=500 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:07:26 |
| typing_huge_code_block.1.json | typing doc=code n=500 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:07:36 |
| typing_keyboard_preset.0.json | typing doc=ordinary n=1000 preset=Keyboard | macOS 26.6.2 | 6.12.0 | Keyboard | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:05:05 |
| typing_keyboard_preset.1.json | typing doc=ordinary n=1000 preset=Keyboard | macOS 26.6.2 | 6.12.0 | Keyboard | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:05:24 |
| typing_keyboard_preset.2.json | typing doc=ordinary n=1000 preset=Keyboard | macOS 26.6.2 | 6.12.0 | Keyboard | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:05:42 |
| typing_stress_note.0.json | typing doc=stress n=500 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:07:06 |
| typing_stress_note.1.json | typing doc=stress n=500 preset=Default | macOS 26.6.2 | 6.12.0 | Default | 1180x765 | 2 | nominal | normal-power-mode | 42 / fnv1a64:f5ecfb8eecd9058b | 2026-11-04T17:07:16 |

## create n=120 preset=Default (3 runs)

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 190 | 838 | 2890 | 103 | 4280 | 342.44 |
| create.1.json | 120 | 188 | 436 | 7821 | 110 | 18178 | 426.57 |
| create.2.json | 120 | 190 | 513 | 3842 | 99 | 4076 | 277.87 |
| **pooled** | 360 | 189 | 604 | 4076 | 99 | 18178 | 348.96 |

### `create_to_durable_commit_ms` (ms)

Proxy: page.new dispatch to Store::committed for the new page's first revision

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 52.876 | 252.93 | 253.21 | 41.034 | 289.63 | 97.712 |
| create.1.json | 120 | 52.741 | 54.034 | 80.047 | 47.909 | 316.81 | 55.530 |
| create.2.json | 120 | 52.676 | 53.967 | 67.429 | 51.914 | 307.43 | 55.240 |
| **pooled** | 360 | 52.736 | 239.48 | 253.21 | 41.034 | 316.81 | 69.494 |

### `create_to_editable_title_ms` (ms)

Proxy: page.new dispatch until the new page is active and its title field has focus (synchronous)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 0.1745 | 0.2296 | 0.5344 | 0.1285 | 0.9536 | 0.1859 |
| create.1.json | 120 | 0.1885 | 0.2132 | 0.2959 | 0.167 | 1.082 | 0.198 |
| create.2.json | 120 | 0.1878 | 0.2165 | 0.2582 | 0.1661 | 1.068 | 0.1975 |
| **pooled** | 360 | 0.1865 | 0.2245 | 0.5344 | 0.1285 | 1.082 | 0.1938 |

Threshold (create_title): p95 <= 30 ms -> measured 0.2245 ms: **PASS**; per run: create.0.json PASS, create.1.json PASS, create.2.json PASS

### `create_to_first_paint_ms` (ms)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 16.681 | 216.70 | 216.72 | 6.636 | 216.72 | 60.198 |
| create.1.json | 120 | 16.698 | 16.718 | 16.764 | 12.266 | 16.767 | 16.659 |
| create.2.json | 120 | 16.695 | 16.721 | 16.738 | 16.636 | 16.776 | 16.694 |
| **pooled** | 360 | 16.694 | 202.70 | 216.70 | 6.636 | 216.72 | 31.184 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 52703 | 252747 | 253004 | 40848 | 289457 | 97527.8 |
| create.1.json | 120 | 52557 | 53826 | 79841 | 46830 | 316628 | 55333.1 |
| create.2.json | 120 | 52490 | 53801 | 67237 | 51161 | 307234 | 55043.7 |
| **pooled** | 360 | 52547 | 239307 | 253004 | 40848 | 316628 | 69301.5 |

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 186 | 19 | 38 | 114 | 11 | 143 | 22.419 |
| create.1.json | 240 | 26 | 35 | 43 | 16 | 44 | 26.988 |
| create.2.json | 240 | 25 | 36 | 48 | 15 | 52 | 25.692 |
| **pooled** | 666 | 25 | 36 | 48 | 11 | 143 | 25.245 |

### `open_cached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 83 | 109 | 277 | 62 | 753 | 92.242 |
| create.1.json | 120 | 90 | 101 | 128 | 79 | 830 | 96.550 |
| create.2.json | 120 | 90 | 102 | 122 | 79 | 832 | 97.067 |
| **pooled** | 360 | 89 | 105 | 277 | 62 | 832 | 95.286 |

### `open_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 93 | 1416 | 2960 | 13352 | 965 | 13352 | 1716.2 |
| create.1.json | 120 | 1383 | 2269 | 2510 | 1087 | 4072 | 1551.5 |
| create.2.json | 120 | 1346 | 2407 | 3034 | 1136 | 4958 | 1578.9 |
| **pooled** | 333 | 1363 | 2501 | 4072 | 965 | 13352 | 1607.3 |

### `page_new_to_editable_title_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 168 | 223 | 511 | 123 | 946 | 178.86 |
| create.1.json | 120 | 182 | 205 | 289 | 160 | 1073 | 190.93 |
| create.2.json | 120 | 181 | 210 | 251 | 160 | 1059 | 190.54 |
| **pooled** | 360 | 179 | 218 | 511 | 123 | 1073 | 186.78 |

Threshold (create_title): p95 <= 30 ms -> measured 0.218 ms: **PASS**; per run: create.0.json PASS, create.1.json PASS, create.2.json PASS

### `page_new_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 173 | 228 | 533 | 127 | 953 | 184.58 |
| create.1.json | 120 | 187 | 211 | 295 | 166 | 1081 | 196.71 |
| create.2.json | 120 | 186 | 215 | 257 | 165 | 1067 | 196.23 |
| **pooled** | 360 | 185 | 223 | 533 | 127 | 1081 | 192.51 |

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 6 | 8 | 15 | 3 | 25 | 5.908 |
| create.1.json | 120 | 6 | 9 | 11 | 5 | 12 | 6.675 |
| create.2.json | 120 | 6 | 9 | 15 | 4 | 15 | 6.400 |
| **pooled** | 360 | 6 | 9 | 15 | 3 | 25 | 6.328 |

### `shell_reconcile_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 2207 | 2595 | 3949 | 1914 | 4393 | 2241.8 |
| create.1.json | 120 | 2343 | 2494 | 2754 | 2076 | 2769 | 2335.8 |
| create.2.json | 120 | 2337 | 2480 | 2548 | 2156 | 2572 | 2335.4 |
| **pooled** | 360 | 2304 | 2524 | 2769 | 1914 | 4393 | 2304.3 |

### `shell_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 120 | 2287 | 2698 | 4049 | 2000 | 4518 | 2333.3 |
| create.1.json | 120 | 2438 | 2588 | 2850 | 2168 | 2859 | 2428.6 |
| create.2.json | 120 | 2428 | 2576 | 2661 | 2254 | 2701 | 2429.6 |
| **pooled** | 360 | 2398 | 2626 | 2931 | 2000 | 4518 | 2397.1 |

### `tree_expand_main_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 122 | 91 | 132 | 173 | 49 | 455 | 98.352 |
| create.1.json | 121 | 103 | 116 | 128 | 56 | 188 | 103.41 |
| create.2.json | 120 | 104 | 119 | 128 | 65 | 338 | 105.63 |
| **pooled** | 363 | 102 | 123 | 173 | 49 | 455 | 102.45 |

### `tree_expand_total_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| create.0.json | 122 | 14355 | 15437 | 15615 | 446 | 16412 | 12384.5 |
| create.1.json | 121 | 14634 | 15226 | 15534 | 943 | 15881 | 14091.9 |
| create.2.json | 120 | 14669 | 15204 | 15318 | 2671 | 15362 | 14288.2 |
| **pooled** | 363 | 14629 | 15291 | 15581 | 446 | 16412 | 13583.0 |

### Counters, extra fields and notes

**create.0.json**

- counters: block_height_estimates=272, blocks_laid_out=257, bytes_copied_for_save=0, doc_cache_bytes=69360, doc_cache_entries=16, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=124, lua_renders=122, paint_events=189, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=120, save_snapshots=120, widgets_created=19, widgets_reused=2299
- extra: {"exec_to_main_ms": 35.066, "window_exposed_and_active_ratio": 0.391304347826087}
- process: {"doc_cache_bytes_est": 69360, "doc_cache_entries": 16, "footprint_bytes": 78007232, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 1404, "laid_out_blocks": 1, "lua_bytes": 44559, "peak_footprint_bytes": 104664064, "preview_cache_bytes": 0, "resident_bytes": 132628480, "system_s": 0.47786025, "user_s": 1.6452959583333333}
- note: title not focused after create #84
- note: title not focused after create #85
- note: title not focused after create #86
- note: title not focused after create #87
- note: title not focused after create #88
- note: title not focused after create #89
- note: title not focused after create #90
- note: title not focused after create #91
- note: title not focused after create #92
- note: title not focused after create #93
- note: title not focused after create #94
- note: title not focused after create #95
- note: title not focused after create #96
- note: title not focused after create #97
- note: title not focused after create #98
- note: title not focused after create #99
- note: title not focused after create #100
- note: title not focused after create #101
- note: title not focused after create #102
- note: title not focused after create #103
- note: title not focused after create #104
- note: title not focused after create #105
- note: title not focused after create #106
- note: title not focused after create #107
- note: title not focused after create #108
- note: title not focused after create #109
- note: title not focused after create #110
- note: title not focused after create #111
- note: title not focused after create #112
- note: title not focused after create #113
- note: title not focused after create #114
- note: title not focused after create #115
- note: title not focused after create #116
- note: title not focused after create #117
- note: title not focused after create #118
- note: title not focused after create #119

**create.1.json**

- counters: block_height_estimates=272, blocks_laid_out=257, bytes_copied_for_save=0, doc_cache_bytes=69360, doc_cache_entries=16, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=124, lua_renders=122, paint_events=243, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=120, save_snapshots=120, widgets_created=19, widgets_reused=2299
- extra: {"exec_to_main_ms": 35.628, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 69360, "doc_cache_entries": 16, "footprint_bytes": 78138432, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 897, "laid_out_blocks": 1, "lua_bytes": 44559, "peak_footprint_bytes": 105581632, "preview_cache_bytes": 0, "resident_bytes": 152043520, "system_s": 0.5683387083333332, "user_s": 1.8876707083333333}

**create.2.json**

- counters: block_height_estimates=272, blocks_laid_out=257, bytes_copied_for_save=0, doc_cache_bytes=69360, doc_cache_entries=16, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=124, lua_renders=122, paint_events=243, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=120, save_snapshots=120, widgets_created=19, widgets_reused=2299
- extra: {"exec_to_main_ms": 66.971, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 69360, "doc_cache_entries": 16, "footprint_bytes": 78236672, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 905, "laid_out_blocks": 1, "lua_bytes": 44559, "peak_footprint_bytes": 106449920, "preview_cache_bytes": 0, "resident_bytes": 157925376, "system_s": 0.5706985833333332, "user_s": 1.8828761666666665}

## expand n=100 preset=Default (3 runs)

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 0 | — | — | — | — | — | — |
| expand.1.json | 0 | — | — | — | — | — | — |
| expand.2.json | 1 | 815 | 815 | 815 | 815 | 815 | 815 |
| **pooled** | 1 | 815 | 815 | 815 | 815 | 815 | 815 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 0 | — | — | — | — | — | — |
| expand.1.json | 0 | — | — | — | — | — | — |
| expand.2.json | 1 | 313712 | 313712 | 313712 | 313712 | 313712 | 313712 |
| **pooled** | 1 | 313712 | 313712 | 313712 | 313712 | 313712 | 313712 |

### `edit_main_thread_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 0 | — | — | — | — | — | — |
| expand.1.json | 0 | — | — | — | — | — | — |
| expand.2.json | 1 | 705 | 705 | 705 | 705 | 705 | 705 |
| **pooled** | 1 | 705 | 705 | 705 | 705 | 705 | 705 |

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 5 | 2158 | 8105 | 8105 | 48 | 8105 | 3408.8 |
| expand.1.json | 6 | 132 | 23351 | 23351 | 49 | 23351 | 4857.5 |
| expand.2.json | 7 | 1218 | 15363 | 15363 | 30 | 15363 | 2791.7 |
| **pooled** | 18 | 1218 | 23351 | 23351 | 30 | 23351 | 3651.7 |

### `expand_main_thread_ms` (ms)

Proxy: main-thread time: expand request + inserting the first page of rows (reader thread excluded)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 200 | 0.1428 | 1.144 | 1.655 | 0.03087 | 4.241 | 0.3248 |
| expand.1.json | 200 | 0.1062 | 0.2278 | 0.9761 | 0.05329 | 4.255 | 0.157 |
| expand.2.json | 200 | 0.1354 | 0.4531 | 1.725 | 0.04842 | 1.962 | 0.2023 |
| **pooled** | 600 | 0.1199 | 0.9139 | 1.725 | 0.03087 | 4.255 | 0.2281 |

Threshold (expand_main): p95 <= 16 ms -> measured 0.9139 ms: **PASS**; per run: expand.0.json PASS, expand.1.json PASS, expand.2.json PASS

### `expand_rows_available_ms` (ms)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 200 | 1.327 | 8.820 | 23.536 | 0.299 | 35.214 | 2.605 |
| expand.1.json | 200 | 1.125 | 5.841 | 8.474 | 0.278 | 50.597 | 1.950 |
| expand.2.json | 200 | 1.563 | 6.090 | 18.851 | 0.419 | 32.567 | 2.563 |
| **pooled** | 600 | 1.284 | 7.249 | 21.793 | 0.278 | 50.597 | 2.373 |

### `expand_to_list_paint_ms` (ms)

Proxy: expand request to the first sidebar list paint after it (-1 = no paint within 1 s)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 200 | -1 | 27.902 | 814.29 | -1 | 964.99 | 27.424 |
| expand.1.json | 200 | -1 | 5.775 | 8.294 | -1 | 20.327 | -0.213 |
| expand.2.json | 200 | -1 | 6.510 | 7.765 | -1 | 9.875 | -0.1063 |
| **pooled** | 600 | -1 | 7.456 | 571.89 | -1 | 964.99 | 9.035 |

### `input_to_flush_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 0 | — | — | — | — | — | — |
| expand.1.json | 0 | — | — | — | — | — | — |
| expand.2.json | 1 | 3345 | 3345 | 3345 | 3345 | 3345 | 3345 |
| **pooled** | 1 | 3345 | 3345 | 3345 | 3345 | 3345 | 3345 |

### `input_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 0 | — | — | — | — | — | — |
| expand.1.json | 0 | — | — | — | — | — | — |
| expand.2.json | 1 | 2885 | 2885 | 2885 | 2885 | 2885 | 2885 |
| **pooled** | 1 | 2885 | 2885 | 2885 | 2885 | 2885 | 2885 |

### `open_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 1 | 110682 | 110682 | 110682 | 110682 | 110682 | 110682 |
| expand.1.json | 1 | 477274 | 477274 | 477274 | 477274 | 477274 | 477274 |
| expand.2.json | 1 | 225670 | 225670 | 225670 | 225670 | 225670 | 225670 |
| **pooled** | 3 | 225670 | 477274 | 477274 | 110682 | 477274 | 271208.7 |

### `open_uncached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 1 | 6703 | 6703 | 6703 | 6703 | 6703 | 6703 |
| expand.1.json | 1 | 442544 | 442544 | 442544 | 442544 | 442544 | 442544 |
| expand.2.json | 1 | 193724 | 193724 | 193724 | 193724 | 193724 | 193724 |
| **pooled** | 3 | 193724 | 442544 | 442544 | 6703 | 442544 | 214323.7 |

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 0 | — | — | — | — | — | — |
| expand.1.json | 0 | — | — | — | — | — | — |
| expand.2.json | 1 | 17 | 17 | 17 | 17 | 17 | 17 |
| **pooled** | 1 | 17 | 17 | 17 | 17 | 17 | 17 |

### `session_build_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 1 | 2177 | 2177 | 2177 | 2177 | 2177 | 2177 |
| expand.1.json | 1 | 5586 | 5586 | 5586 | 5586 | 5586 | 5586 |
| expand.2.json | 1 | 2320 | 2320 | 2320 | 2320 | 2320 | 2320 |
| **pooled** | 3 | 2320 | 5586 | 5586 | 2177 | 5586 | 3361 |

### `shell_load_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 1 | 37842 | 37842 | 37842 | 37842 | 37842 | 37842 |
| expand.1.json | 1 | 72818 | 72818 | 72818 | 72818 | 72818 | 72818 |
| expand.2.json | 1 | 64550 | 64550 | 64550 | 64550 | 64550 | 64550 |
| **pooled** | 3 | 64550 | 72818 | 72818 | 37842 | 72818 | 58403.3 |

### `shell_reconcile_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 2 | 2506 | 36459 | 36459 | 2506 | 36459 | 19482.5 |
| expand.1.json | 2 | 4753 | 71505 | 71505 | 4753 | 71505 | 38129 |
| expand.2.json | 2 | 2469 | 63262 | 63262 | 2469 | 63262 | 32865.5 |
| **pooled** | 6 | 4753 | 71505 | 71505 | 2469 | 71505 | 30159 |

### `shell_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 1 | 2593 | 2593 | 2593 | 2593 | 2593 | 2593 |
| expand.1.json | 1 | 4867 | 4867 | 4867 | 4867 | 4867 | 4867 |
| expand.2.json | 1 | 2565 | 2565 | 2565 | 2565 | 2565 | 2565 |
| **pooled** | 3 | 2593 | 4867 | 4867 | 2565 | 4867 | 3341.7 |

### `tree_expand_main_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 212 | 81 | 628 | 1219 | 1 | 1700 | 181.04 |
| expand.1.json | 201 | 37 | 125 | 645 | 13 | 4215 | 78.806 |
| expand.2.json | 201 | 45 | 167 | 683 | 11 | 1669 | 82.119 |
| **pooled** | 614 | 45 | 433 | 1105 | 1 | 4215 | 115.19 |

Threshold (expand_main): p95 <= 16 ms -> measured 0.433 ms: **PASS**; per run: expand.0.json PASS, expand.1.json PASS, expand.2.json PASS

### `tree_expand_request_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 195 | 60 | 552 | 1393 | 16 | 3140 | 157.68 |
| expand.1.json | 200 | 55 | 122 | 228 | 18 | 2234 | 70.845 |
| expand.2.json | 200 | 62 | 233 | 418 | 15 | 1692 | 90.620 |
| **pooled** | 595 | 58 | 417 | 860 | 15 | 3140 | 105.95 |

### `tree_expand_total_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| expand.0.json | 212 | 1412 | 11240 | 23542 | 298 | 83903 | 3342.8 |
| expand.1.json | 201 | 1139 | 5991 | 15736 | 277 | 369633 | 3778.2 |
| expand.2.json | 201 | 1580 | 8388 | 21793 | 419 | 389240 | 4486.1 |
| **pooled** | 614 | 1321 | 8820 | 23542 | 277 | 389240 | 3859.6 |

### Counters, extra fields and notes

**expand.0.json**

- counters: block_height_estimates=32, blocks_laid_out=17, bytes_copied_for_save=0, doc_cache_bytes=10824, doc_cache_entries=1, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=4, lua_renders=2, paint_events=5, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, tree_rows_loaded=3622, widgets_created=19, widgets_reused=19
- extra: {"exec_to_main_ms": 69.906, "list_viewport_paints_total": 149, "list_viewport_size": "264x683", "list_visible": true, "window_exposed_and_active_ratio": 0.006896551724137931}
- process: {"doc_cache_bytes_est": 72264, "doc_cache_entries": 1, "footprint_bytes": 117116032, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 333, "interrupt_wakeups": 21931, "laid_out_blocks": 16, "lua_bytes": 59370, "peak_footprint_bytes": 117116032, "preview_cache_bytes": 0, "resident_bytes": 25182208, "system_s": 2.3222352916666664, "user_s": 6.714152791666666}

**expand.1.json**

- counters: block_height_estimates=32, blocks_laid_out=17, bytes_copied_for_save=0, doc_cache_bytes=10824, doc_cache_entries=1, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=4, lua_renders=2, paint_events=5, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, tree_rows_loaded=222, widgets_created=19, widgets_reused=19
- extra: {"exec_to_main_ms": 131.357, "list_viewport_paints_total": 60, "list_viewport_size": "264x683", "list_visible": true, "window_exposed_and_active_ratio": 0.007926023778071334}
- process: {"doc_cache_bytes_est": 72264, "doc_cache_entries": 1, "footprint_bytes": 89426944, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 280, "interrupt_wakeups": 22490, "laid_out_blocks": 16, "lua_bytes": 59370, "peak_footprint_bytes": 104991744, "preview_cache_bytes": 0, "resident_bytes": 29638656, "system_s": 2.729573708333333, "user_s": 7.106286666666666}

**expand.2.json**

- counters: block_height_estimates=32, blocks_laid_out=34, bytes_copied_for_save=960, doc_cache_bytes=76599, doc_cache_entries=1, highlight_lines=14, highlight_state_steps=0, lua_errors=0, lua_invocations=4, lua_renders=2, paint_events=6, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=1, save_snapshots=1, tree_rows_loaded=222, widgets_created=19, widgets_reused=19
- extra: {"exec_to_main_ms": 184.623, "list_viewport_paints_total": 77, "list_viewport_size": "264x683", "list_visible": true, "window_exposed_and_active_ratio": 0.00684931506849315}
- process: {"doc_cache_bytes_est": 76599, "doc_cache_entries": 1, "footprint_bytes": 90360896, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 289, "interrupt_wakeups": 21900, "laid_out_blocks": 17, "lua_bytes": 59370, "peak_footprint_bytes": 105532480, "preview_cache_bytes": 0, "resident_bytes": 31162368, "system_s": 3.069214875, "user_s": 7.672747958333333}

## idle page=2 seconds=60 preset=Default (2 runs)

### `idle_cpu_percent_of_core` (ratio)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_default.0.json | 60 | 0.1454 | 0.2274 | 0.8422 | 0.08498 | 0.8422 | 0.1624 |
| idle_default.1.json | 60 | 0.2007 | 0.3022 | 0.4073 | 0.1333 | 0.4073 | 0.206 |
| **pooled** | 120 | 0.1773 | 0.2506 | 0.4073 | 0.08498 | 0.8422 | 0.1842 |

Threshold (idle_cpu): mean < 1% of a core -> measured 0.184%: **PASS**; per run: idle_default.0.json PASS, idle_default.1.json PASS

### `idle_footprint_bytes` (bytes)

Proxy: task_vm_info.phys_footprint

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_default.0.json | 60 | 80,219,392 | 80,383,232 | 80,432,384 | 80,219,392 | 80,432,384 | 80,261,717 |
| idle_default.1.json | 60 | 80,120,960 | 80,202,880 | 80,235,648 | 80,120,960 | 80,235,648 | 80,142,259 |
| **pooled** | 120 | 80,219,392 | 80,383,232 | 80,383,232 | 80,120,960 | 80,432,384 | 80,201,988 |

Threshold (idle_footprint): max <= 60 MB (60,000,000 B) -> measured 80,432,384 (76.71 MiB): **MISS**; per run: idle_default.0.json MISS, idle_default.1.json MISS

### `idle_wakeups_per_s` (ratio)

Proxy: task_power_info idle + interrupt wakeups per second (includes the 1 s sampling timer)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_default.0.json | 60 | 5 | 6 | 8 | 5 | 8 | 5.267 |
| idle_default.1.json | 60 | 5 | 7 | 8 | 5 | 8 | 5.400 |
| **pooled** | 120 | 5 | 6 | 8 | 5 | 8 | 5.333 |

### Counters, extra fields and notes

**idle_default.0.json**

- counters: block_height_estimates=300, blocks_laid_out=29, bytes_copied_for_save=0, doc_cache_bytes=235232, doc_cache_entries=2, highlight_lines=8, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=12, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 237.673, "idle_mean_cpu_percent": 0.1620208495065492, "idle_seconds": 60.131741458, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 276192, "doc_cache_entries": 2, "footprint_bytes": 80235776, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 427, "laid_out_blocks": 11, "lua_bytes": 66078, "peak_footprint_bytes": 95784192, "preview_cache_bytes": 0, "resident_bytes": 52494336, "system_s": 0.5254621249999999, "user_s": 0.5015912916666666}

**idle_default.1.json**

- counters: block_height_estimates=300, blocks_laid_out=29, bytes_copied_for_save=0, doc_cache_bytes=235232, doc_cache_entries=2, highlight_lines=8, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=13, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 65.77, "idle_mean_cpu_percent": 0.20549582081242262, "idle_seconds": 60.148587375, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 276192, "doc_cache_entries": 2, "footprint_bytes": 80120960, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 10, "interrupt_wakeups": 404, "laid_out_blocks": 11, "lua_bytes": 66078, "peak_footprint_bytes": 81136768, "preview_cache_bytes": 0, "resident_bytes": 99844096, "system_s": 0.34629133333333334, "user_s": 0.4574457916666666}

## idle page=2 seconds=60 preset=Default math=off (1 run)

### `idle_cpu_percent_of_core` (ratio)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_no_math.0.json | 60 | 0.1441 | 0.2659 | 1.075 | 0.08921 | 1.075 | 0.1734 |

Threshold (idle_cpu): mean < 1% of a core -> measured 0.173%: **PASS**

### `idle_footprint_bytes` (bytes)

Proxy: task_vm_info.phys_footprint

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_no_math.0.json | 60 | 80,841,984 | 80,940,288 | 81,005,824 | 80,776,448 | 81,005,824 | 80,849,357 |

Threshold (idle_footprint): max <= 60 MB (60,000,000 B) -> measured 81,005,824 (77.25 MiB): **MISS**

### `idle_wakeups_per_s` (ratio)

Proxy: task_power_info idle + interrupt wakeups per second (includes the 1 s sampling timer)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_no_math.0.json | 60 | 5 | 7 | 8 | 5 | 8 | 5.333 |

### Counters, extra fields and notes

**idle_no_math.0.json**

- counters: block_height_estimates=300, blocks_laid_out=27, bytes_copied_for_save=0, doc_cache_bytes=235232, doc_cache_entries=2, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=10, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 78.569, "idle_mean_cpu_percent": 0.17303386542363475, "idle_seconds": 60.118578375, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 268000, "doc_cache_entries": 2, "footprint_bytes": 80858368, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 4, "interrupt_wakeups": 424, "laid_out_blocks": 9, "lua_bytes": 66076, "peak_footprint_bytes": 96374016, "preview_cache_bytes": 0, "resident_bytes": 91078656, "system_s": 0.40530704166666665, "user_s": 0.4374309583333333}

## idle page=2 seconds=60 preset=Keyboard (2 runs)

### `idle_cpu_percent_of_core` (ratio)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_keyboard_preset.0.json | 60 | 0.1931 | 0.3721 | 2.406 | 0.1256 | 2.406 | 0.2703 |
| idle_keyboard_preset.1.json | 60 | 0.157 | 0.3142 | 0.3379 | 0.06507 | 0.3379 | 0.1618 |
| **pooled** | 120 | 0.1728 | 0.3245 | 1.614 | 0.06507 | 2.406 | 0.2161 |

Threshold (idle_cpu): mean < 1% of a core -> measured 0.216%: **PASS**; per run: idle_keyboard_preset.0.json PASS, idle_keyboard_preset.1.json PASS

### `idle_footprint_bytes` (bytes)

Proxy: task_vm_info.phys_footprint

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_keyboard_preset.0.json | 60 | 79,023,296 | 79,170,752 | 79,170,752 | 78,990,528 | 79,170,752 | 79,047,872 |
| idle_keyboard_preset.1.json | 60 | 79,285,504 | 79,400,192 | 79,400,192 | 79,269,120 | 79,400,192 | 79,318,545 |
| **pooled** | 120 | 79,170,752 | 79,351,040 | 79,400,192 | 78,990,528 | 79,400,192 | 79,183,209 |

Threshold (idle_footprint): max <= 60 MB (60,000,000 B) -> measured 79,400,192 (75.72 MiB): **MISS**; per run: idle_keyboard_preset.0.json MISS, idle_keyboard_preset.1.json MISS

### `idle_wakeups_per_s` (ratio)

Proxy: task_power_info idle + interrupt wakeups per second (includes the 1 s sampling timer)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_keyboard_preset.0.json | 60 | 5 | 7 | 30 | 5 | 30 | 5.700 |
| idle_keyboard_preset.1.json | 60 | 5 | 6 | 7 | 5 | 7 | 5.317 |
| **pooled** | 120 | 5 | 6 | 8 | 5 | 30 | 5.508 |

### Counters, extra fields and notes

**idle_keyboard_preset.0.json**

- counters: block_height_estimates=300, blocks_laid_out=27, bytes_copied_for_save=0, doc_cache_bytes=235232, doc_cache_entries=2, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=12, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=15, widgets_reused=30
- extra: {"exec_to_main_ms": 61.56, "idle_mean_cpu_percent": 0.2696346274430177, "idle_seconds": 60.140597125, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 268000, "doc_cache_entries": 2, "footprint_bytes": 79105216, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 5, "interrupt_wakeups": 429, "laid_out_blocks": 9, "lua_bytes": 55778, "peak_footprint_bytes": 79940800, "preview_cache_bytes": 0, "resident_bytes": 29687808, "system_s": 0.3233199583333333, "user_s": 0.4842200416666666}

**idle_keyboard_preset.1.json**

- counters: block_height_estimates=300, blocks_laid_out=27, bytes_copied_for_save=0, doc_cache_bytes=235232, doc_cache_entries=2, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=11, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=15, widgets_reused=30
- extra: {"exec_to_main_ms": 89.419, "idle_mean_cpu_percent": 0.1615159099136169, "idle_seconds": 60.1225085, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 268000, "doc_cache_entries": 2, "footprint_bytes": 79285504, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 5, "interrupt_wakeups": 436, "laid_out_blocks": 9, "lua_bytes": 55778, "peak_footprint_bytes": 80416000, "preview_cache_bytes": 0, "resident_bytes": 36978688, "system_s": 0.4600620833333333, "user_s": 0.42234229166666665}

## idle page=2 seconds=60 preset=Minimal math=off (1 run)

### `idle_cpu_percent_of_core` (ratio)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_editor_only_shell.0.json | 60 | 0.136 | 0.3384 | 0.7964 | 0.08034 | 0.7964 | 0.171 |

Threshold (idle_cpu): mean < 1% of a core -> measured 0.171%: **PASS**

### `idle_footprint_bytes` (bytes)

Proxy: task_vm_info.phys_footprint

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_editor_only_shell.0.json | 60 | 76,041,472 | 76,188,928 | 76,188,928 | 75,975,936 | 76,188,928 | 76,064,137 |

Threshold (idle_footprint): max <= 60 MB (60,000,000 B) -> measured 76,188,928 (72.66 MiB): **MISS**

### `idle_wakeups_per_s` (ratio)

Proxy: task_power_info idle + interrupt wakeups per second (includes the 1 s sampling timer)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_editor_only_shell.0.json | 60 | 5 | 6 | 6 | 4 | 6 | 5.217 |

### Counters, extra fields and notes

**idle_editor_only_shell.0.json**

- counters: block_height_estimates=300, blocks_laid_out=31, bytes_copied_for_save=0, doc_cache_bytes=235232, doc_cache_entries=2, highlight_lines=10, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=11, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=2, widgets_reused=4
- extra: {"exec_to_main_ms": 72.508, "idle_mean_cpu_percent": 0.1706151659798515, "idle_seconds": 60.123978083, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 284384, "doc_cache_entries": 2, "footprint_bytes": 76057856, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 4, "interrupt_wakeups": 418, "laid_out_blocks": 13, "lua_bytes": 30805, "peak_footprint_bytes": 77188352, "preview_cache_bytes": 0, "resident_bytes": 43614208, "system_s": 0.394820875, "user_s": 0.3931598333333333}

## idle page=6 seconds=60 preset=Default (1 run)

### `idle_cpu_percent_of_core` (ratio)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_stress_note.0.json | 60 | 0.1522 | 0.2725 | 1.654 | 0.05327 | 1.654 | 0.1886 |

Threshold (idle_cpu): mean < 1% of a core -> measured 0.189%: **PASS**

### `idle_footprint_bytes` (bytes)

Proxy: task_vm_info.phys_footprint

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_stress_note.0.json | 60 | 114,232,576 | 114,462,016 | 114,462,016 | 114,232,576 | 114,462,016 | 114,278,751 |

Threshold (idle_footprint): max <= 60 MB (60,000,000 B) -> measured 114,462,016 (109.16 MiB): **MISS**

### `idle_wakeups_per_s` (ratio)

Proxy: task_power_info idle + interrupt wakeups per second (includes the 1 s sampling timer)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| idle_stress_note.0.json | 60 | 5 | 7 | 16 | 5 | 16 | 5.617 |

### Counters, extra fields and notes

**idle_stress_note.0.json**

- counters: block_height_estimates=32756, blocks_laid_out=21, bytes_copied_for_save=0, doc_cache_bytes=54,763,006, doc_cache_entries=1, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=12, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 71.006, "idle_mean_cpu_percent": 0.18816386479774905, "idle_seconds": 60.135107125, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 54771198, "doc_cache_entries": 1, "footprint_bytes": 114248960, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 11, "interrupt_wakeups": 442, "laid_out_blocks": 3, "lua_bytes": 66080, "peak_footprint_bytes": 165989696, "preview_cache_bytes": 0, "resident_bytes": 102776832, "system_s": 0.5219784583333333, "user_s": 0.625402}

## integration math_page=2 ordinary_page=6010 preset=Default (1 run)

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 6 | 1626 | 3625 | 3625 | 677 | 3625 | 2142.5 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 6 | 287308 | 376957 | 376957 | 6308 | 376957 | 225256 |

### `edit_main_thread_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 72 | 752 | 2354 | 70005 | 258 | 70005 | 1852.3 |

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 47 | 506 | 9988 | 136259 | 49 | 136259 | 4646.5 |

### `input_to_flush_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 40 | 3697 | 12425 | 22868 | 1185 | 22868 | 5011.6 |

### `input_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 40 | 1986 | 10411 | 22482 | 996 | 22482 | 3651.2 |

### `open_cached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 2 | 2158 | 4956 | 4956 | 2158 | 4956 | 3557 |

### `open_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 2 | 139334 | 147020 | 147020 | 139334 | 147020 | 143177 |

### `open_uncached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 3 | 10272 | 120985 | 120985 | 9587 | 120985 | 46948 |

### `preview_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 32 | 228 | 18992 | 71040 | 146 | 71040 | 3076.6 |

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 6 | 77 | 568 | 568 | 16 | 568 | 196 |

### `session_build_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 4 | 486 | 6603 | 6603 | 422 | 6603 | 2377.5 |

### `shell_load_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 3 | 10974 | 24982 | 24982 | 9768 | 24982 | 15241.3 |

### `shell_reconcile_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 8 | 3509 | 24525 | 24525 | 2606 | 24525 | 7627.5 |

### `shell_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 5 | 3569 | 9379 | 9379 | 2678 | 9379 | 4504.8 |

### `tree_expand_main_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 7 | 33 | 89 | 89 | 11 | 89 | 45 |

### `tree_expand_request_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 6 | 12 | 24 | 24 | 9 | 24 | 13.500 |

### `tree_expand_total_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| integration.0.json | 7 | 9801 | 62604 | 62604 | 271 | 62604 | 17771.3 |

### Counters, extra fields and notes

**integration.0.json**

- counters: block_height_estimates=1845, blocks_laid_out=173, bytes_copied_for_save=174230, doc_cache_bytes=450933, doc_cache_entries=3, highlight_lines=13, highlight_state_steps=0, lua_errors=0, lua_invocations=14, lua_renders=8, paint_events=46, preview_cache_bytes=525732, preview_cache_hits=0, preview_cancelled=28, preview_outstanding=0, preview_renders=32, preview_requests=32, preview_stale_drops=28, save_snapshot_full_walks=6, save_snapshots=6, widgets_created=19, widgets_reused=133
- extra: {"checks": {"a_edit_during_save_durable": true, "a_stored_rev_matches": true, "a_stored_text_matches_editor": true, "b_preview_is_current_source": true, "b_preview_shown_after_switch": true, "c_caret_after_commit": true, "c_lua_renders_during": 1, "c_preedit_kept": true, "c_reload_applied_after_commit": true, "c_reload_deferred_during_composition": true, "c_same_document_after_reload": true, "d_rapid_edit_final_preview_is_current": true, "d_superseded_renders_cancelled_or_dropped": 56}, "exec_to_main_ms": 69.17100000000005, "window_exposed_and_active_ratio": 0.06756756756756757}
- process: {"doc_cache_bytes_est": 328053, "doc_cache_entries": 3, "footprint_bytes": 90099200, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 42, "interrupt_wakeups": 2028, "laid_out_blocks": 0, "lua_bytes": 53078, "peak_footprint_bytes": 114969664, "preview_cache_bytes": 525732, "resident_bytes": 124633088, "system_s": 0.4013647916666666, "user_s": 1.18402025}

## launch start_page=2 preset=Default (5 runs)

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 9 | 3713 | 68139 | 68139 | 2421 | 68139 | 11057.7 |
| launch.1.json | 11 | 4702 | 32608 | 32608 | 33 | 32608 | 6813.4 |
| launch.2.json | 11 | 4396 | 34414 | 34414 | 25 | 34414 | 7534.7 |
| launch.3.json | 12 | 5867 | 31872 | 31872 | 32 | 31872 | 7712.8 |
| launch.4.json | 11 | 5330 | 33306 | 33306 | 37 | 33306 | 7762.7 |
| **pooled** | 54 | 5052 | 33306 | 68139 | 25 | 68139 | 8061.0 |

### `footprint_after_launch_bytes` (bytes)

Proxy: task_vm_info.phys_footprint

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 1 | 80,448,704 | 80,448,704 | 80,448,704 | 80,448,704 | 80,448,704 | 80,448,704 |
| launch.1.json | 1 | 80,383,168 | 80,383,168 | 80,383,168 | 80,383,168 | 80,383,168 | 80,383,168 |
| launch.2.json | 1 | 80,465,088 | 80,465,088 | 80,465,088 | 80,465,088 | 80,465,088 | 80,465,088 |
| launch.3.json | 1 | 80,039,104 | 80,039,104 | 80,039,104 | 80,039,104 | 80,039,104 | 80,039,104 |
| launch.4.json | 1 | 80,350,336 | 80,350,336 | 80,350,336 | 80,350,336 | 80,350,336 | 80,350,336 |
| **pooled** | 5 | 80,383,168 | 80,465,088 | 80,465,088 | 80,039,104 | 80,465,088 | 80,337,280 |

### `launch_main_to_editable_ms` (ms)

Proxy: main() entry to end of the first editor paint of the start page

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 1 | 1198.6 | 1198.6 | 1198.6 | 1198.6 | 1198.6 | 1198.6 |
| launch.1.json | 1 | 373.02 | 373.02 | 373.02 | 373.02 | 373.02 | 373.02 |
| launch.2.json | 1 | 355.78 | 355.78 | 355.78 | 355.78 | 355.78 | 355.78 |
| launch.3.json | 1 | 351.49 | 351.49 | 351.49 | 351.49 | 351.49 | 351.49 |
| launch.4.json | 1 | 352.86 | 352.86 | 352.86 | 352.86 | 352.86 | 352.86 |
| **pooled** | 5 | 355.78 | 1198.6 | 1198.6 | 351.49 | 1198.6 | 526.34 |

### `launch_process_to_editable_ms` (ms)

Proxy: kernel process start (sysctl KERN_PROC p_starttime) to end of the first editor paint of the start page

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 1 | 1406.1 | 1406.1 | 1406.1 | 1406.1 | 1406.1 | 1406.1 |
| launch.1.json | 1 | 404.12 | 404.12 | 404.12 | 404.12 | 404.12 | 404.12 |
| launch.2.json | 1 | 378.72 | 378.72 | 378.72 | 378.72 | 378.72 | 378.72 |
| launch.3.json | 1 | 379.79 | 379.79 | 379.79 | 379.79 | 379.79 | 379.79 |
| launch.4.json | 1 | 375.36 | 375.36 | 375.36 | 375.36 | 375.36 | 375.36 |
| **pooled** | 5 | 379.79 | 1406.1 | 1406.1 | 375.36 | 1406.1 | 588.82 |

### `open_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 1 | 228791 | 228791 | 228791 | 228791 | 228791 | 228791 |
| launch.1.json | 1 | 119751 | 119751 | 119751 | 119751 | 119751 | 119751 |
| launch.2.json | 1 | 121849 | 121849 | 121849 | 121849 | 121849 | 121849 |
| launch.3.json | 1 | 125051 | 125051 | 125051 | 125051 | 125051 | 125051 |
| launch.4.json | 1 | 132956 | 132956 | 132956 | 132956 | 132956 | 132956 |
| **pooled** | 5 | 125051 | 228791 | 228791 | 119751 | 228791 | 145679.6 |

### `open_uncached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 1 | 82954 | 82954 | 82954 | 82954 | 82954 | 82954 |
| launch.1.json | 1 | 78420 | 78420 | 78420 | 78420 | 78420 | 78420 |
| launch.2.json | 1 | 78925 | 78925 | 78925 | 78925 | 78925 | 78925 |
| launch.3.json | 1 | 74284 | 74284 | 74284 | 74284 | 74284 | 74284 |
| launch.4.json | 1 | 80499 | 80499 | 80499 | 80499 | 80499 | 80499 |
| **pooled** | 5 | 78925 | 82954 | 82954 | 74284 | 82954 | 79016.4 |

### `session_build_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 1 | 7913 | 7913 | 7913 | 7913 | 7913 | 7913 |
| launch.1.json | 1 | 6906 | 6906 | 6906 | 6906 | 6906 | 6906 |
| launch.2.json | 1 | 6624 | 6624 | 6624 | 6624 | 6624 | 6624 |
| launch.3.json | 1 | 6722 | 6722 | 6722 | 6722 | 6722 | 6722 |
| launch.4.json | 1 | 7051 | 7051 | 7051 | 7051 | 7051 | 7051 |
| **pooled** | 5 | 6906 | 7913 | 7913 | 6624 | 7913 | 7043.2 |

### `shell_load_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 1 | 60416 | 60416 | 60416 | 60416 | 60416 | 60416 |
| launch.1.json | 1 | 19743 | 19743 | 19743 | 19743 | 19743 | 19743 |
| launch.2.json | 1 | 18349 | 18349 | 18349 | 18349 | 18349 | 18349 |
| launch.3.json | 1 | 18534 | 18534 | 18534 | 18534 | 18534 | 18534 |
| launch.4.json | 1 | 17740 | 17740 | 17740 | 17740 | 17740 | 17740 |
| **pooled** | 5 | 18534 | 60416 | 60416 | 17740 | 60416 | 26956.4 |

### `shell_reconcile_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 2 | 3413 | 59601 | 59601 | 3413 | 59601 | 31507 |
| launch.1.json | 2 | 2599 | 19318 | 19318 | 2599 | 19318 | 10958.5 |
| launch.2.json | 2 | 2353 | 17917 | 17917 | 2353 | 17917 | 10135 |
| launch.3.json | 2 | 2396 | 18123 | 18123 | 2396 | 18123 | 10259.5 |
| launch.4.json | 2 | 2287 | 17311 | 17311 | 2287 | 17311 | 9799 |
| **pooled** | 10 | 3413 | 59601 | 59601 | 2287 | 59601 | 14531.8 |

### `shell_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 1 | 3485 | 3485 | 3485 | 3485 | 3485 | 3485 |
| launch.1.json | 1 | 2675 | 2675 | 2675 | 2675 | 2675 | 2675 |
| launch.2.json | 1 | 2422 | 2422 | 2422 | 2422 | 2422 | 2422 |
| launch.3.json | 1 | 2479 | 2479 | 2479 | 2479 | 2479 | 2479 |
| launch.4.json | 1 | 2359 | 2359 | 2359 | 2359 | 2359 | 2359 |
| **pooled** | 5 | 2479 | 3485 | 3485 | 2359 | 3485 | 2684 |

### `tree_expand_main_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 2 | 10 | 324 | 324 | 10 | 324 | 167 |
| launch.1.json | 2 | 13 | 54 | 54 | 13 | 54 | 33.500 |
| launch.2.json | 2 | 11 | 54 | 54 | 11 | 54 | 32.500 |
| launch.3.json | 2 | 11 | 51 | 51 | 11 | 51 | 31 |
| launch.4.json | 2 | 10 | 50 | 50 | 10 | 50 | 30 |
| **pooled** | 10 | 13 | 324 | 324 | 10 | 324 | 58.800 |

### `tree_expand_request_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 1 | 188 | 188 | 188 | 188 | 188 | 188 |
| launch.1.json | 1 | 29 | 29 | 29 | 29 | 29 | 29 |
| launch.2.json | 1 | 26 | 26 | 26 | 26 | 26 | 26 |
| launch.3.json | 1 | 28 | 28 | 28 | 28 | 28 | 28 |
| launch.4.json | 1 | 21 | 21 | 21 | 21 | 21 | 21 |
| **pooled** | 5 | 28 | 188 | 188 | 21 | 188 | 58.400 |

### `tree_expand_total_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| launch.0.json | 2 | 9004 | 378168 | 378168 | 9004 | 378168 | 193586 |
| launch.1.json | 2 | 9056 | 53529 | 53529 | 9056 | 53529 | 31292.5 |
| launch.2.json | 2 | 8381 | 48855 | 48855 | 8381 | 48855 | 28618 |
| launch.3.json | 2 | 11033 | 47046 | 47046 | 11033 | 47046 | 29039.5 |
| launch.4.json | 2 | 8229 | 46634 | 46634 | 8229 | 46634 | 27431.5 |
| **pooled** | 10 | 11033 | 378168 | 378168 | 8229 | 378168 | 61993.5 |

### Counters, extra fields and notes

**launch.0.json**

- counters: block_height_estimates=268, blocks_laid_out=12, bytes_copied_for_save=0, doc_cache_bytes=162968, doc_cache_entries=1, highlight_lines=1, highlight_state_steps=0, lua_errors=0, lua_invocations=4, lua_renders=2, paint_events=9, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=19
- extra: {"editable": true, "exec_to_main_ms": 207.624, "start_page": 2, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 203928, "doc_cache_entries": 1, "footprint_bytes": 80448704, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 75, "laid_out_blocks": 11, "lua_bytes": 59392, "peak_footprint_bytes": 80989376, "preview_cache_bytes": 0, "resident_bytes": 108478464, "system_s": 0.4495510833333333, "user_s": 0.3279472083333333}

**launch.1.json**

- counters: block_height_estimates=268, blocks_laid_out=12, bytes_copied_for_save=0, doc_cache_bytes=162968, doc_cache_entries=1, highlight_lines=1, highlight_state_steps=0, lua_errors=0, lua_invocations=4, lua_renders=2, paint_events=10, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=19
- extra: {"editable": true, "exec_to_main_ms": 31.141, "start_page": 2, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 203928, "doc_cache_entries": 1, "footprint_bytes": 80383168, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 2, "interrupt_wakeups": 75, "laid_out_blocks": 11, "lua_bytes": 59392, "peak_footprint_bytes": 80841920, "preview_cache_bytes": 0, "resident_bytes": 141099008, "system_s": 0.07415320833333333, "user_s": 0.3318170833333333}

**launch.2.json**

- counters: block_height_estimates=268, blocks_laid_out=12, bytes_copied_for_save=0, doc_cache_bytes=162968, doc_cache_entries=1, highlight_lines=1, highlight_state_steps=0, lua_errors=0, lua_invocations=4, lua_renders=2, paint_events=10, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=19
- extra: {"editable": true, "exec_to_main_ms": 22.975, "start_page": 2, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 203928, "doc_cache_entries": 1, "footprint_bytes": 80481472, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 1, "interrupt_wakeups": 66, "laid_out_blocks": 11, "lua_bytes": 59392, "peak_footprint_bytes": 80809152, "preview_cache_bytes": 0, "resident_bytes": 144392192, "system_s": 0.06081808333333333, "user_s": 0.33345479166666664}

**launch.3.json**

- counters: block_height_estimates=268, blocks_laid_out=12, bytes_copied_for_save=0, doc_cache_bytes=162968, doc_cache_entries=1, highlight_lines=1, highlight_state_steps=0, lua_errors=0, lua_invocations=4, lua_renders=2, paint_events=11, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=19
- extra: {"editable": true, "exec_to_main_ms": 28.367, "start_page": 2, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 203928, "doc_cache_entries": 1, "footprint_bytes": 80055488, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 5, "interrupt_wakeups": 74, "laid_out_blocks": 11, "lua_bytes": 59392, "peak_footprint_bytes": 81071296, "preview_cache_bytes": 0, "resident_bytes": 131317760, "system_s": 0.06549654166666666, "user_s": 0.34497975}

**launch.4.json**

- counters: block_height_estimates=268, blocks_laid_out=12, bytes_copied_for_save=0, doc_cache_bytes=162968, doc_cache_entries=1, highlight_lines=1, highlight_state_steps=0, lua_errors=0, lua_invocations=4, lua_renders=2, paint_events=10, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=19
- extra: {"editable": true, "exec_to_main_ms": 22.526, "start_page": 2, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 203928, "doc_cache_entries": 1, "footprint_bytes": 80383104, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 74, "laid_out_blocks": 11, "lua_bytes": 59392, "peak_footprint_bytes": 80645248, "preview_cache_bytes": 0, "resident_bytes": 145276928, "system_s": 0.05953245833333333, "user_s": 0.329453125}

## navigate batches=5 per_batch=100 preset=Default (1 run)

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 960 | 5193 | 10339 | 20057 | 123 | 272004 | 6088.5 |

### `navigate_doc_cache_bytes_est` (bytes)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 5 | 3,679,231 | 3,733,058 | 3,733,058 | 3,411,656 | 3,733,058 | 3,616,854 |

Threshold (nav_plateau): max <= 48 MiB -> measured 3,733,058 (3.56 MiB): **PASS**

### `navigate_doc_cache_entries` (ratio)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 5 | 16 | 16 | 16 | 16 | 16 | 16 |

Threshold (nav_plateau): max <= 16 count -> measured 16: **PASS**

### `navigate_preview_cache_bytes` (bytes)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 5 | 0 | 0 | 0 | 0 | 0 | 0 |

Threshold (nav_plateau): max <= 4 MiB -> measured 0 (0.00 MiB): **PASS**

### `navigate_settled_footprint_bytes` (bytes)

Proxy: task_vm_info.phys_footprint

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 5 | 110,546,368 | 110,677,440 | 110,677,440 | 107,220,416 | 110,677,440 | 109,809,088 |

Threshold (nav_plateau): growth after batch 2 <= max(4 MiB, 5%) -> measured 688,128 (0.66 MiB): **PASS** (batch2 104.9 MiB -> last 105.6 MiB (growth +0.66 MiB, allowed 5.24 MiB))

### `navigate_switch_ms` (ms)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 500 | 15.959 | 38.344 | 71.055 | 6.347 | 292.69 | 19.073 |

### `open_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 501 | 15965 | 38428 | 79051 | 6347 | 546885 | 20126.4 |

### `open_uncached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 501 | 7816 | 20972 | 40141 | 1899 | 474144 | 9384.6 |

### `session_build_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 501 | 293 | 605 | 1754 | 250 | 11384 | 374.07 |

### `shell_load_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 1 | 56128 | 56128 | 56128 | 56128 | 56128 | 56128 |

### `shell_reconcile_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 355 | 3155 | 5518 | 10274 | 2443 | 55015 | 3579.3 |

### `shell_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 354 | 3244 | 5615 | 10531 | 2533 | 10813 | 3539.9 |

### `tree_expand_main_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 910 | 78 | 193 | 245 | 12 | 411 | 88.470 |

### `tree_expand_request_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 909 | 53 | 102 | 139 | 1 | 603 | 57.030 |

### `tree_expand_total_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| navigate.0.json | 910 | 11924 | 29901 | 43779 | 3511 | 364676 | 14880.2 |

### Counters, extra fields and notes

**navigate.0.json**

- counters: block_height_estimates=138490, blocks_laid_out=8646, bytes_copied_for_save=0, doc_cache_bytes=3,643,129, doc_cache_entries=16, highlight_lines=2179, highlight_state_steps=0, lua_errors=0, lua_invocations=357, lua_renders=355, paint_events=959, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=6726
- extra: {"exec_to_main_ms": 109.009, "window_exposed_and_active_ratio": 0}
- process: {"doc_cache_bytes_est": 3720953, "doc_cache_entries": 16, "footprint_bytes": 110759360, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 650, "laid_out_blocks": 20, "lua_bytes": 70101, "peak_footprint_bytes": 111037888, "preview_cache_bytes": 0, "resident_bytes": 164986880, "system_s": 1.5021030416666665, "user_s": 13.318695291666666}

## open n=120 preset=Default (3 runs)

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 470 | 3844 | 7982 | 12703 | 15 | 232430 | 4816.6 |
| open.1.json | 469 | 3867 | 7713 | 9736 | 17 | 52033 | 4341.0 |
| open.2.json | 469 | 3898 | 7619 | 9984 | 26 | 57688 | 4400.0 |
| **pooled** | 1408 | 3864 | 7865 | 10202 | 15 | 232430 | 4519.4 |

### `open_cached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 119 | 451 | 640 | 752 | 397 | 760 | 478.03 |
| open.1.json | 119 | 458 | 631 | 714 | 396 | 790 | 480.29 |
| open.2.json | 119 | 471 | 656 | 798 | 404 | 1883 | 507.51 |
| **pooled** | 357 | 460 | 652 | 790 | 396 | 1883 | 488.61 |

### `open_cached_to_editable_ms` (ms)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 119 | 9.326 | 14.036 | 15.590 | 5.855 | 15.892 | 9.829 |
| open.1.json | 119 | 9.633 | 13.009 | 15.264 | 5.299 | 15.286 | 9.982 |
| open.2.json | 119 | 9.353 | 13.090 | 15.310 | 2.497 | 15.613 | 9.808 |
| **pooled** | 357 | 9.435 | 13.201 | 15.361 | 2.497 | 15.892 | 9.873 |

Threshold (open_cached): p95 <= 30 ms -> measured 13.201 ms: **PASS**; per run: open.0.json PASS, open.1.json PASS, open.2.json PASS

### `open_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 239 | 10187 | 17055 | 35906 | 5855 | 245233 | 12185.5 |
| open.1.json | 239 | 10631 | 15980 | 28180 | 5299 | 64462 | 11291.0 |
| open.2.json | 239 | 10036 | 15310 | 28059 | 2497 | 67978 | 11024.5 |
| **pooled** | 717 | 10248 | 15892 | 31020 | 2497 | 245233 | 11500.4 |

### `open_uncached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 120 | 2431 | 8199 | 18272 | 1430 | 18272 | 3447.2 |
| open.1.json | 120 | 2773 | 6895 | 10052 | 1429 | 20952 | 3324.1 |
| open.2.json | 120 | 2091 | 5896 | 10366 | 1311 | 34723 | 2910.2 |
| **pooled** | 360 | 2351 | 6895 | 18272 | 1311 | 34723 | 3227.2 |

### `open_uncached_to_editable_ms` (ms)

Proxy: page.open dispatch to end of the first editor paint showing the page (app-internal; excludes compositor)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 120 | 11.393 | 18.287 | 37.699 | 6.778 | 245.23 | 14.522 |
| open.1.json | 120 | 11.710 | 17.529 | 31.020 | 7.134 | 64.462 | 12.589 |
| open.2.json | 120 | 11.044 | 16.032 | 41.962 | 6.955 | 67.978 | 12.231 |
| **pooled** | 360 | 11.291 | 17.920 | 41.962 | 6.778 | 245.23 | 13.114 |

Threshold (open_uncached): p95 <= 100 ms -> measured 17.920 ms: **PASS**; per run: open.0.json PASS, open.1.json PASS, open.2.json PASS

### `session_build_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 120 | 306 | 363 | 2757 | 271 | 5873 | 412.70 |
| open.1.json | 120 | 309 | 430 | 2925 | 255 | 6613 | 424.85 |
| open.2.json | 120 | 305 | 349 | 2656 | 259 | 5564 | 402.88 |
| **pooled** | 360 | 307 | 366 | 2925 | 255 | 6613 | 413.48 |

### `shell_reconcile_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 221 | 2845 | 3281 | 3786 | 2191 | 5770 | 2877.0 |
| open.1.json | 223 | 2838 | 3222 | 3387 | 2262 | 3570 | 2840.7 |
| open.2.json | 224 | 2818 | 3267 | 3438 | 2220 | 4365 | 2847.9 |
| **pooled** | 668 | 2840 | 3267 | 3570 | 2191 | 5770 | 2855.1 |

### `shell_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 221 | 2929 | 3373 | 3879 | 2256 | 5897 | 2961.6 |
| open.1.json | 223 | 2914 | 3313 | 3496 | 2338 | 3676 | 2924.1 |
| open.2.json | 224 | 2899 | 3364 | 3514 | 2300 | 4456 | 2933.4 |
| **pooled** | 668 | 2920 | 3364 | 3676 | 2256 | 5897 | 2939.6 |

### `tree_expand_main_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 692 | 70 | 156 | 221 | 11 | 418 | 77.516 |
| open.1.json | 696 | 72 | 156 | 194 | 13 | 441 | 78.700 |
| open.2.json | 696 | 73 | 154 | 205 | 14 | 474 | 78.454 |
| **pooled** | 2084 | 72 | 154 | 204 | 11 | 474 | 78.225 |

### `tree_expand_request_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 693 | 37 | 68 | 78 | 1 | 90 | 37.850 |
| open.1.json | 696 | 38 | 71 | 81 | 3 | 154 | 38.904 |
| open.2.json | 696 | 38 | 68 | 78 | 4 | 144 | 38.284 |
| **pooled** | 2085 | 38 | 69 | 79 | 1 | 154 | 38.347 |

### `tree_expand_total_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| open.0.json | 692 | 6391 | 17527 | 20934 | 1342 | 31569 | 6556.1 |
| open.1.json | 696 | 6400 | 16526 | 18781 | 1328 | 20639 | 6490.5 |
| open.2.json | 696 | 6568 | 16463 | 20061 | 1348 | 27394 | 6533.4 |
| **pooled** | 2084 | 6457 | 16593 | 20061 | 1328 | 31569 | 6526.6 |

### Counters, extra fields and notes

**open.0.json**

- counters: block_height_estimates=33260, blocks_laid_out=2156, bytes_copied_for_save=0, doc_cache_bytes=3,707,990, doc_cache_entries=16, highlight_lines=576, highlight_state_steps=0, lua_errors=0, lua_invocations=225, lua_renders=223, open_failures=0, paint_events=473, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=4218
- extra: {"exec_to_main_ms": 397.525, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 3707990, "doc_cache_entries": 16, "footprint_bytes": 122244672, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 25, "interrupt_wakeups": 454, "laid_out_blocks": 21, "lua_bytes": 76357, "peak_footprint_bytes": 137563712, "preview_cache_bytes": 0, "resident_bytes": 180453376, "system_s": 1.303057625, "user_s": 6.409928708333333}

**open.1.json**

- counters: block_height_estimates=33260, blocks_laid_out=2163, bytes_copied_for_save=0, doc_cache_bytes=3,712,086, doc_cache_entries=16, highlight_lines=578, highlight_state_steps=0, lua_errors=0, lua_invocations=227, lua_renders=225, open_failures=0, paint_events=472, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=4256
- extra: {"exec_to_main_ms": 67.324, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 3712086, "doc_cache_entries": 16, "footprint_bytes": 106958272, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 50, "interrupt_wakeups": 448, "laid_out_blocks": 21, "lua_bytes": 76453, "peak_footprint_bytes": 135843328, "preview_cache_bytes": 0, "resident_bytes": 191168512, "system_s": 1.11520075, "user_s": 6.34310025}

**open.2.json**

- counters: block_height_estimates=33260, blocks_laid_out=2163, bytes_copied_for_save=0, doc_cache_bytes=3,712,086, doc_cache_entries=16, highlight_lines=578, highlight_state_steps=0, lua_errors=0, lua_invocations=228, lua_renders=226, open_failures=0, paint_events=472, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=4275
- extra: {"exec_to_main_ms": 48.777, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 3712086, "doc_cache_entries": 16, "footprint_bytes": 107220544, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 46, "interrupt_wakeups": 445, "laid_out_blocks": 21, "lua_bytes": 69996, "peak_footprint_bytes": 136105536, "preview_cache_bytes": 0, "resident_bytes": 190939136, "system_s": 0.9100444166666666, "user_s": 6.37095975}

## presentation n=100 preset=Default (3 runs)

### `code_block_visible_ms` (ms)

Proxy: insert of a 7-line code block to end of the paint that shows it highlighted

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 100 | 16.679 | 16.701 | 16.742 | 16.636 | 39.239 | 16.905 |
| presentation.1.json | 100 | 16.679 | 16.721 | 16.776 | 16.646 | 33.348 | 16.848 |
| presentation.2.json | 100 | 16.678 | 16.699 | 16.725 | 16.613 | 32.435 | 16.837 |
| **pooled** | 300 | 16.679 | 16.706 | 16.776 | 16.613 | 39.239 | 16.863 |

Threshold (code_visible): p95 <= 30 ms -> measured 16.706 ms: **PASS**; per run: presentation.0.json PASS, presentation.1.json PASS, presentation.2.json PASS

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 14 | 1258 | 3891 | 3891 | 855 | 3891 | 1423.9 |
| presentation.1.json | 14 | 1137 | 2057 | 2057 | 850 | 2057 | 1217.8 |
| presentation.2.json | 14 | 1232 | 1524 | 1524 | 818 | 1524 | 1211.4 |
| **pooled** | 42 | 1232 | 1567 | 3891 | 818 | 3891 | 1284.3 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 14 | 883182 | 940340 | 940340 | 872748 | 940340 | 887676.2 |
| presentation.1.json | 14 | 866652 | 907897 | 907897 | 866488 | 907897 | 875388.4 |
| presentation.2.json | 14 | 892345 | 938044 | 938044 | 891599 | 938044 | 895589.4 |
| **pooled** | 42 | 883467 | 907897 | 940340 | 866488 | 940340 | 886218 |

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 922 | 1488 | 2552 | 3434 | 35 | 31458 | 1430.6 |
| presentation.1.json | 923 | 1482 | 2641 | 3810 | 32 | 28466 | 1422.7 |
| presentation.2.json | 921 | 1483 | 2568 | 4083 | 30 | 32882 | 1421.9 |
| **pooled** | 2766 | 1486 | 2594 | 3961 | 30 | 32882 | 1425.1 |

### `formula_cache_hit_ms` (ms)

Proxy: insert of a display formula to the paint that shows its rendered preview

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 99 | 33.349 | 33.385 | 33.435 | 33.265 | 33.435 | 33.349 |
| presentation.1.json | 99 | 33.351 | 33.387 | 33.401 | 33.252 | 33.401 | 33.349 |
| presentation.2.json | 99 | 33.354 | 33.388 | 33.441 | 33.283 | 33.441 | 33.353 |
| **pooled** | 297 | 33.351 | 33.386 | 33.435 | 33.252 | 33.441 | 33.350 |

Threshold (formula_visible): p95 <= 100 ms -> measured 33.386 ms: **PASS**; per run: presentation.0.json PASS, presentation.1.json PASS, presentation.2.json PASS

### `formula_first_in_process_ms` (ms)

Proxy: insert of a display formula to the paint that shows its rendered preview

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 1 | 249.97 | 249.97 | 249.97 | 249.97 | 249.97 | 249.97 |
| presentation.1.json | 1 | 33.339 | 33.339 | 33.339 | 33.339 | 33.339 | 33.339 |
| presentation.2.json | 1 | 33.335 | 33.335 | 33.335 | 33.335 | 33.335 | 33.335 |
| **pooled** | 3 | 33.339 | 249.97 | 249.97 | 33.335 | 249.97 | 105.55 |

Threshold (formula_visible): max <= 100 ms -> measured 249.97 ms: **MISS**; per run: presentation.0.json MISS, presentation.1.json PASS, presentation.2.json PASS

### `formula_uncached_ms` (ms)

Proxy: insert of a display formula to the paint that shows its rendered preview

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 99 | 33.350 | 33.400 | 49.992 | 33.276 | 49.992 | 33.517 |
| presentation.1.json | 99 | 33.351 | 33.381 | 33.392 | 33.276 | 33.392 | 33.351 |
| presentation.2.json | 99 | 33.353 | 33.388 | 33.414 | 33.249 | 33.414 | 33.350 |
| **pooled** | 297 | 33.351 | 33.388 | 33.414 | 33.249 | 49.992 | 33.406 |

Threshold (formula_visible): p95 <= 100 ms -> measured 33.388 ms: **PASS**; per run: presentation.0.json PASS, presentation.1.json PASS, presentation.2.json PASS

### `open_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 2 | 52563 | 133153 | 133153 | 52563 | 133153 | 92858 |
| presentation.1.json | 2 | 41200 | 131670 | 131670 | 41200 | 131670 | 86435 |
| presentation.2.json | 2 | 55707 | 126510 | 126510 | 55707 | 126510 | 91108.5 |
| **pooled** | 6 | 55707 | 133153 | 133153 | 41200 | 133153 | 90133.8 |

### `open_uncached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 2 | 11640 | 120703 | 120703 | 11640 | 120703 | 66171.5 |
| presentation.1.json | 2 | 9810 | 118988 | 118988 | 9810 | 118988 | 64399 |
| presentation.2.json | 2 | 14018 | 113783 | 113783 | 14018 | 113783 | 63900.5 |
| **pooled** | 6 | 14018 | 120703 | 120703 | 9810 | 120703 | 64823.7 |

### `preview_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 100 | 32 | 117 | 350 | 22 | 7283 | 115.25 |
| presentation.1.json | 100 | 30 | 112 | 1225 | 22 | 2838 | 78.130 |
| presentation.2.json | 100 | 31 | 105 | 205 | 23 | 2670 | 67.440 |
| **pooled** | 300 | 31 | 115 | 1225 | 22 | 7283 | 86.940 |

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 14 | 282 | 329 | 329 | 101 | 329 | 266 |
| presentation.1.json | 14 | 272 | 335 | 335 | 111 | 335 | 262.64 |
| presentation.2.json | 14 | 290 | 343 | 343 | 128 | 343 | 274.21 |
| **pooled** | 42 | 286 | 329 | 343 | 101 | 343 | 267.62 |

### `session_build_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 2 | 2257 | 5854 | 5854 | 2257 | 5854 | 4055.5 |
| presentation.1.json | 2 | 2876 | 5788 | 5788 | 2876 | 5788 | 4332 |
| presentation.2.json | 2 | 2149 | 7111 | 7111 | 2149 | 7111 | 4630 |
| **pooled** | 6 | 2876 | 7111 | 7111 | 2149 | 7111 | 4339.2 |

### `shell_load_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 1 | 24518 | 24518 | 24518 | 24518 | 24518 | 24518 |
| presentation.1.json | 1 | 22488 | 22488 | 22488 | 22488 | 22488 | 22488 |
| presentation.2.json | 1 | 25815 | 25815 | 25815 | 25815 | 25815 | 25815 |
| **pooled** | 3 | 24518 | 25815 | 25815 | 22488 | 25815 | 24273.7 |

### `shell_reconcile_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 3 | 2533 | 24072 | 24072 | 2412 | 24072 | 9672.3 |
| presentation.1.json | 3 | 2528 | 21991 | 21991 | 2379 | 21991 | 8966 |
| presentation.2.json | 3 | 3082 | 25353 | 25353 | 2542 | 25353 | 10325.7 |
| **pooled** | 9 | 2542 | 25353 | 25353 | 2379 | 25353 | 9654.7 |

### `shell_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 2 | 2493 | 2636 | 2636 | 2493 | 2636 | 2564.5 |
| presentation.1.json | 2 | 2459 | 2617 | 2617 | 2459 | 2617 | 2538 |
| presentation.2.json | 2 | 2645 | 3184 | 3184 | 2645 | 3184 | 2914.5 |
| **pooled** | 6 | 2617 | 3184 | 3184 | 2459 | 3184 | 2672.3 |

### `tree_expand_main_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 2 | 11 | 70 | 70 | 11 | 70 | 40.500 |
| presentation.1.json | 2 | 13 | 72 | 72 | 13 | 72 | 42.500 |
| presentation.2.json | 2 | 16 | 76 | 76 | 16 | 76 | 46 |
| **pooled** | 6 | 16 | 76 | 76 | 11 | 76 | 43 |

### `tree_expand_request_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 1 | 17 | 17 | 17 | 17 | 17 | 17 |
| presentation.1.json | 1 | 17 | 17 | 17 | 17 | 17 | 17 |
| presentation.2.json | 1 | 22 | 22 | 22 | 22 | 22 | 22 |
| **pooled** | 3 | 17 | 22 | 22 | 17 | 22 | 18.667 |

### `tree_expand_total_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| presentation.0.json | 2 | 22404 | 193472 | 193472 | 22404 | 193472 | 107938 |
| presentation.1.json | 2 | 17100 | 50981 | 50981 | 17100 | 50981 | 34040.5 |
| presentation.2.json | 2 | 22932 | 48966 | 48966 | 22932 | 48966 | 35949 |
| **pooled** | 6 | 22932 | 193472 | 193472 | 17100 | 193472 | 59309.2 |

### Counters, extra fields and notes

**presentation.0.json**

- counters: block_height_estimates=299666, blocks_laid_out=7451, bytes_copied_for_save=1,014,202, doc_cache_bytes=760596, doc_cache_entries=2, highlight_lines=2825, highlight_state_steps=206, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=921, preview_cache_bytes=247666, preview_cache_hits=99, preview_cancelled=0, preview_outstanding=0, preview_renders=100, preview_requests=199, preview_stale_drops=0, save_snapshot_full_walks=14, save_snapshots=14, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 177.779, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 779271, "doc_cache_entries": 2, "footprint_bytes": 89787584, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 1722, "laid_out_blocks": 16, "lua_bytes": 66078, "peak_footprint_bytes": 124898560, "preview_cache_bytes": 247666, "resident_bytes": 162267136, "system_s": 0.9616542916666666, "user_s": 3.01654775}

**presentation.1.json**

- counters: block_height_estimates=299666, blocks_laid_out=7451, bytes_copied_for_save=1,020,620, doc_cache_bytes=765420, doc_cache_entries=2, highlight_lines=2825, highlight_state_steps=206, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=922, preview_cache_bytes=247666, preview_cache_hits=99, preview_cancelled=0, preview_outstanding=0, preview_renders=100, preview_requests=199, preview_stale_drops=0, save_snapshot_full_walks=14, save_snapshots=14, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 56.57600000000002, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 779271, "doc_cache_entries": 2, "footprint_bytes": 90017088, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 1626, "laid_out_blocks": 16, "lua_bytes": 66078, "peak_footprint_bytes": 118918464, "preview_cache_bytes": 247666, "resident_bytes": 163037184, "system_s": 0.882614625, "user_s": 3.051638083333333}

**presentation.2.json**

- counters: block_height_estimates=299666, blocks_laid_out=7451, bytes_copied_for_save=1,021,340, doc_cache_bytes=765420, doc_cache_entries=2, highlight_lines=2825, highlight_state_steps=206, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=920, preview_cache_bytes=247666, preview_cache_hits=99, preview_cancelled=0, preview_outstanding=0, preview_renders=100, preview_requests=199, preview_stale_drops=0, save_snapshot_full_walks=14, save_snapshots=14, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 48.064, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 779271, "doc_cache_entries": 2, "footprint_bytes": 89246976, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 1678, "laid_out_blocks": 16, "lua_bytes": 66078, "peak_footprint_bytes": 117624128, "preview_cache_bytes": 247666, "resident_bytes": 174292992, "system_s": 0.865524625, "user_s": 3.032051833333333}

## scroll frames=1200 preset=Default (3 runs)

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| scroll.0.json | 1449 | 5415 | 6979 | 8754 | 1911 | 46568 | 5390.5 |
| scroll.1.json | 1449 | 5436 | 6784 | 8413 | 1909 | 43787 | 5382.5 |
| scroll.2.json | 1449 | 5537 | 6891 | 8471 | 2166 | 47128 | 5479.5 |
| **pooled** | 4347 | 5467 | 6867 | 8471 | 1909 | 47128 | 5417.5 |

### `preview_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| scroll.0.json | 8 | 286 | 7610 | 7610 | 126 | 7610 | 2027 |
| scroll.1.json | 8 | 264 | 6330 | 6330 | 115 | 6330 | 1289.5 |
| scroll.2.json | 8 | 239 | 5863 | 5863 | 118 | 5863 | 1256.4 |
| **pooled** | 24 | 270 | 6330 | 7610 | 115 | 7610 | 1524.3 |

### `scroll_frame_work_ms` (ms)

Proxy: main-thread scroll handling + editor paint for one frame (backing-store flush excluded)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| scroll.0.json | 1200 | 5.453 | 6.457 | 7.247 | 1.925 | 9.324 | 5.314 |
| scroll.1.json | 1200 | 5.473 | 6.433 | 7.011 | 1.921 | 8.462 | 5.322 |
| scroll.2.json | 1200 | 5.564 | 6.560 | 7.129 | 2.187 | 9.053 | 5.414 |
| **pooled** | 3600 | 5.496 | 6.484 | 7.117 | 1.921 | 9.324 | 5.350 |

Threshold (scroll): share of samples <= 16.7 ms >= 0.95 -> measured 1.0000: **PASS** (3600/3600 samples <= 16.7 ms; longest 9.324 ms); per run: scroll.0.json PASS, scroll.1.json PASS, scroll.2.json PASS

### `scroll_paint_interval_ms` (ms)

Proxy: interval between consecutive presented editor paints during continuous scrolling

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| scroll.0.json | 1448 | 16.346 | 18.748 | 23.430 | 3.378 | 57.819 | 14.323 |
| scroll.1.json | 1448 | 16.321 | 18.463 | 23.457 | 3.386 | 55.070 | 14.257 |
| scroll.2.json | 1448 | 16.350 | 19.255 | 23.980 | 4.064 | 56.446 | 14.420 |
| **pooled** | 4344 | 16.335 | 18.748 | 23.648 | 3.378 | 57.819 | 14.334 |

Threshold (scroll): report only: longest interval, count > 25 ms -> measured 11: **INFO** (longest interval 57.819 ms; 11 of 4344 intervals > 25 ms); per run: scroll.0.json INFO, scroll.1.json INFO, scroll.2.json INFO

### Counters, extra fields and notes

**scroll.0.json**

- counters: block_height_estimates=32756, blocks_laid_out=447, bytes_copied_for_save=0, doc_cache_bytes=54,763,006, doc_cache_entries=1, highlight_lines=141, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1454, preview_cache_bytes=23304, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=8, preview_requests=8, preview_stale_drops=2, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 62.125, "frames": 1200, "frames_work_within_16_7ms_ratio": 1, "hitches_over_25ms": 4, "longest_frame_work_ms": 9.324083, "longest_paint_interval_ms": 57.819041, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 56491518, "doc_cache_entries": 1, "footprint_bytes": 142724736, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 2187, "laid_out_blocks": 423, "lua_bytes": 66080, "peak_footprint_bytes": 199069120, "preview_cache_bytes": 23304, "resident_bytes": 173981696, "system_s": 1.4312455833333333, "user_s": 9.9967385}

**scroll.1.json**

- counters: block_height_estimates=32756, blocks_laid_out=447, bytes_copied_for_save=0, doc_cache_bytes=54,763,006, doc_cache_entries=1, highlight_lines=141, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1454, preview_cache_bytes=23304, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=8, preview_requests=8, preview_stale_drops=2, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 114.58, "frames": 1200, "frames_work_within_16_7ms_ratio": 1, "hitches_over_25ms": 1, "longest_frame_work_ms": 8.462207999999999, "longest_paint_interval_ms": 55.069833, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 56491518, "doc_cache_entries": 1, "footprint_bytes": 149228992, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 2185, "laid_out_blocks": 423, "lua_bytes": 66080, "peak_footprint_bytes": 199085376, "preview_cache_bytes": 23304, "resident_bytes": 169934848, "system_s": 1.4426690416666665, "user_s": 10.043859458333332}

**scroll.2.json**

- counters: block_height_estimates=32756, blocks_laid_out=447, bytes_copied_for_save=0, doc_cache_bytes=54,763,006, doc_cache_entries=1, highlight_lines=141, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1457, preview_cache_bytes=23304, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=8, preview_requests=8, preview_stale_drops=2, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=38
- extra: {"exec_to_main_ms": 48.508, "frames": 1200, "frames_work_within_16_7ms_ratio": 1, "hitches_over_25ms": 6, "longest_frame_work_ms": 9.052624999999999, "longest_paint_interval_ms": 56.445875, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 56491518, "doc_cache_entries": 1, "footprint_bytes": 141758016, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 2165, "laid_out_blocks": 423, "lua_bytes": 66080, "peak_footprint_bytes": 199658880, "preview_cache_bytes": 23304, "resident_bytes": 145391616, "system_s": 1.5376717916666665, "user_s": 10.257593125}

## stress n=20 preset=Default (2 runs)

### `copy_selection_ms` (ms)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 20 | 11.018 | 12.383 | 13.093 | 10.489 | 13.093 | 11.297 |
| stress.1.json | 20 | 11.415 | 13.939 | 15.919 | 10.695 | 15.919 | 11.869 |
| **pooled** | 40 | 11.217 | 13.093 | 15.919 | 10.489 | 15.919 | 11.583 |

### `edit_main_thread_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 40 | 23 | 33 | 115 | 12 | 115 | 26.375 |
| stress.1.json | 40 | 25 | 109 | 125 | 19 | 125 | 32.575 |
| **pooled** | 80 | 24 | 56 | 125 | 12 | 125 | 29.475 |

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 109 | 2712 | 67844 | 74077 | 17 | 74303 | 15231.0 |
| stress.1.json | 111 | 2725 | 71258 | 73625 | 17 | 79277 | 15315.4 |
| **pooled** | 220 | 2712 | 69222 | 74077 | 17 | 79277 | 15273.6 |

### `input_to_flush_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 40 | 1473 | 2168 | 2874 | 397 | 2874 | 1416.8 |
| stress.1.json | 40 | 1440 | 2093 | 2538 | 376 | 2538 | 1324.8 |
| **pooled** | 80 | 1445 | 2141 | 2874 | 376 | 2874 | 1370.8 |

### `input_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 40 | 378 | 598 | 652 | 232 | 652 | 403.80 |
| stress.1.json | 40 | 382 | 608 | 641 | 251 | 641 | 405.07 |
| **pooled** | 80 | 382 | 608 | 652 | 232 | 652 | 404.44 |

### `jump_to_end_ms` (ms)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 20 | 16.653 | 16.677 | 16.685 | 5.813 | 16.685 | 16.110 |
| stress.1.json | 20 | 16.653 | 16.682 | 16.734 | 11.253 | 16.734 | 16.385 |
| **pooled** | 40 | 16.653 | 16.682 | 16.734 | 5.813 | 16.734 | 16.248 |

### `jump_to_start_ms` (ms)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 20 | 16.664 | 16.695 | 16.710 | 16.636 | 16.710 | 16.668 |
| stress.1.json | 20 | 16.649 | 16.700 | 16.710 | 16.605 | 16.710 | 16.657 |
| **pooled** | 40 | 16.658 | 16.700 | 16.710 | 16.605 | 16.710 | 16.662 |

### `open_to_paint_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 2 | 257907 | 273782 | 273782 | 257907 | 273782 | 265844.5 |
| stress.1.json | 2 | 217798 | 257162 | 257162 | 217798 | 257162 | 237480 |
| **pooled** | 4 | 257162 | 273782 | 273782 | 217798 | 273782 | 251662.2 |

### `open_uncached_activate_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 2 | 192760 | 231482 | 231482 | 192760 | 231482 | 212121 |
| stress.1.json | 2 | 180642 | 194744 | 194744 | 180642 | 194744 | 187693 |
| **pooled** | 4 | 192760 | 231482 | 231482 | 180642 | 231482 | 199907 |

### `select_all_to_paint_ms` (ms)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 20 | 83.333 | 83.361 | 143.53 | 66.680 | 143.53 | 83.847 |
| stress.1.json | 20 | 83.357 | 83.406 | 146.83 | 66.693 | 146.83 | 85.692 |
| **pooled** | 40 | 83.340 | 83.406 | 146.83 | 66.680 | 146.83 | 84.769 |

### `session_build_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 2 | 2505 | 69862 | 69862 | 2505 | 69862 | 36183.5 |
| stress.1.json | 2 | 2209 | 73364 | 73364 | 2209 | 73364 | 37786.5 |
| **pooled** | 4 | 2505 | 73364 | 73364 | 2209 | 73364 | 36985 |

### `shell_load_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 1 | 54914 | 54914 | 54914 | 54914 | 54914 | 54914 |
| stress.1.json | 1 | 41193 | 41193 | 41193 | 41193 | 41193 | 41193 |
| **pooled** | 2 | 41193 | 54914 | 54914 | 41193 | 54914 | 48053.5 |

### `shell_reconcile_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 3 | 2575 | 53330 | 53330 | 2465 | 53330 | 19456.7 |
| stress.1.json | 3 | 2656 | 40286 | 40286 | 2221 | 40286 | 15054.3 |
| **pooled** | 6 | 2575 | 53330 | 53330 | 2221 | 53330 | 17255.5 |

### `shell_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 2 | 2570 | 2659 | 2659 | 2570 | 2659 | 2614.5 |
| stress.1.json | 2 | 2304 | 2752 | 2752 | 2304 | 2752 | 2528 |
| **pooled** | 4 | 2570 | 2752 | 2752 | 2304 | 2752 | 2571.2 |

### `stress_first_editable_ms` (ms)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 1 | 273.78 | 273.78 | 273.78 | 273.78 | 273.78 | 273.78 |
| stress.1.json | 1 | 257.16 | 257.16 | 257.16 | 257.16 | 257.16 | 257.16 |
| **pooled** | 2 | 257.16 | 273.78 | 273.78 | 257.16 | 273.78 | 265.47 |

### `tree_expand_main_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 2 | 16 | 66 | 66 | 16 | 66 | 41 |
| stress.1.json | 2 | 19 | 57 | 57 | 19 | 57 | 38 |
| **pooled** | 4 | 19 | 66 | 66 | 16 | 66 | 39.500 |

### `tree_expand_request_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 1 | 168 | 168 | 168 | 168 | 168 | 168 |
| stress.1.json | 1 | 211 | 211 | 211 | 211 | 211 | 211 |
| **pooled** | 2 | 168 | 211 | 211 | 168 | 211 | 189.50 |

### `tree_expand_total_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| stress.0.json | 2 | 13249 | 469689 | 469689 | 13249 | 469689 | 241469 |
| stress.1.json | 2 | 14256 | 277640 | 277640 | 14256 | 277640 | 145948 |
| **pooled** | 4 | 14256 | 469689 | 469689 | 13249 | 469689 | 193708.5 |

### Counters, extra fields and notes

**stress.0.json**

- counters: block_height_estimates=32756, blocks_laid_out=24, bytes_copied_for_save=0, doc_cache_bytes=54,763,006, doc_cache_entries=1, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=108, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=38
- extra: {"copied_chars": 8491093, "doc_chars": 8491094, "doc_lines": 16362, "exec_to_main_ms": 262.42, "peak_footprint_bytes": 214060416, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 54783486, "doc_cache_entries": 1, "footprint_bytes": 179867072, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 252, "laid_out_blocks": 6, "lua_bytes": 66080, "peak_footprint_bytes": 214060416, "preview_cache_bytes": 0, "resident_bytes": 265306112, "system_s": 0.72809475, "user_s": 2.2888315416666667}

**stress.1.json**

- counters: block_height_estimates=32756, blocks_laid_out=24, bytes_copied_for_save=0, doc_cache_bytes=54,763,006, doc_cache_entries=1, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=110, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=0, save_snapshots=0, widgets_created=19, widgets_reused=38
- extra: {"copied_chars": 8491093, "doc_chars": 8491094, "doc_lines": 16362, "exec_to_main_ms": 196.285, "peak_footprint_bytes": 213929216, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 54783486, "doc_cache_entries": 1, "footprint_bytes": 170446080, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 283, "laid_out_blocks": 6, "lua_bytes": 66080, "peak_footprint_bytes": 213929216, "preview_cache_bytes": 0, "resident_bytes": 265158656, "system_s": 0.519916625, "user_s": 2.3154075}

## typing bgrender=1 doc=ordinary n=1000 preset=Default (2 runs)

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_during_bg_render.0.json | 19 | 999 | 1267 | 1267 | 202 | 1267 | 912 |
| typing_during_bg_render.1.json | 19 | 705 | 11708 | 11708 | 261 | 11708 | 1328.2 |
| **pooled** | 38 | 863 | 1380 | 11708 | 202 | 11708 | 1120.1 |

### `cpu_ms_per_edit_all_threads` (ms)

Proxy: process user+system CPU over the typing run / edits (includes paint, save, previews)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_during_bg_render.0.json | 1 | 5.562 | 5.562 | 5.562 | 5.562 | 5.562 | 5.562 |
| typing_during_bg_render.1.json | 1 | 4.768 | 4.768 | 4.768 | 4.768 | 4.768 | 4.768 |
| **pooled** | 2 | 4.768 | 5.562 | 5.562 | 4.768 | 5.562 | 5.165 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_during_bg_render.0.json | 19 | 901164 | 941711 | 941711 | 621168 | 941711 | 887629.9 |
| typing_during_bg_render.1.json | 19 | 900661 | 949830 | 949830 | 651572 | 949830 | 887373.2 |
| **pooled** | 38 | 900866 | 941711 | 949830 | 621168 | 949830 | 887501.6 |

### `edit_main_thread_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_during_bg_render.0.json | 1075 | 499 | 1006 | 4327 | 26 | 24199 | 627.64 |
| typing_during_bg_render.1.json | 1075 | 404 | 890 | 3432 | 20 | 26363 | 522.48 |
| **pooled** | 2150 | 447 | 956 | 4012 | 20 | 26363 | 575.06 |

Threshold (typing_main): p95 <= 8 ms -> measured 0.956 ms: **PASS**; per run: typing_during_bg_render.0.json PASS, typing_during_bg_render.1.json PASS

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_during_bg_render.0.json | 1015 | 421 | 5355 | 6417 | 39 | 7440 | 1224.2 |
| typing_during_bg_render.1.json | 1014 | 320 | 4720 | 6399 | 61 | 7437 | 1012.0 |
| **pooled** | 2029 | 377 | 5226 | 6417 | 39 | 7440 | 1118.2 |

### `input_to_flush_us` (us)

Proxy: event delivery to the event-loop turn after the paint (backing store flushed); excludes WindowServer composition

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_during_bg_render.0.json | 1001 | 3516 | 10833 | 14842 | 910 | 30947 | 4988.6 |
| typing_during_bg_render.1.json | 1001 | 3249 | 9386 | 13307 | 747 | 32834 | 4222.5 |
| **pooled** | 2002 | 3360 | 10139 | 13896 | 747 | 32834 | 4605.6 |

Threshold (typing_presented): p95 <= 33 ms -> measured 10.139 ms: **PASS**; per run: typing_during_bg_render.0.json PASS, typing_during_bg_render.1.json PASS

### `input_to_paint_us` (us)

Proxy: event delivery to end of editor viewport paint (synthetic event; excludes compositor)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_during_bg_render.0.json | 1001 | 2012 | 8308 | 12581 | 536 | 30504 | 3055.7 |
| typing_during_bg_render.1.json | 1001 | 1648 | 7286 | 10935 | 595 | 32525 | 2560.5 |
| **pooled** | 2002 | 1834 | 7945 | 11374 | 536 | 32525 | 2808.1 |

Threshold (typing_presented): p95 <= 33 ms -> measured 7.945 ms: **PASS**; per run: typing_during_bg_render.0.json PASS, typing_during_bg_render.1.json PASS

### `preview_render_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_during_bg_render.0.json | 2003 | 44 | 105 | 150 | 12 | 24121 | 63.663 |
| typing_during_bg_render.1.json | 2072 | 45 | 127 | 433 | 12 | 26265 | 84.931 |
| **pooled** | 4075 | 44 | 118 | 205 | 12 | 26265 | 74.477 |

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_during_bg_render.0.json | 19 | 35 | 57 | 57 | 13 | 57 | 35.474 |
| typing_during_bg_render.1.json | 19 | 33 | 54 | 54 | 15 | 54 | 33.632 |
| **pooled** | 38 | 33 | 54 | 57 | 13 | 57 | 34.553 |

### Counters, extra fields and notes

**typing_during_bg_render.0.json**

- counters: block_height_estimates=300, blocks_laid_out=1241, bytes_copied_for_save=16674, doc_cache_bytes=498468, doc_cache_entries=2, highlight_lines=15, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1022, preview_cache_bytes=4,192,024, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=2026, preview_requests=2027, preview_stale_drops=1, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 67.93, "idle_wakeups_delta": 53, "revisions": 952, "typing_wall_ms": 16743.54225, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 498468, "doc_cache_entries": 2, "footprint_bytes": 94981504, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 57, "interrupt_wakeups": 3333, "laid_out_blocks": 63, "lua_bytes": 66078, "peak_footprint_bytes": 123833728, "preview_cache_bytes": 4192024, "resident_bytes": 170835968, "system_s": 1.82424725, "user_s": 4.211414749999999}

**typing_during_bg_render.1.json**

- counters: block_height_estimates=300, blocks_laid_out=1241, bytes_copied_for_save=16522, doc_cache_bytes=498468, doc_cache_entries=2, highlight_lines=15, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1021, preview_cache_bytes=4,192,052, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=2096, preview_requests=2097, preview_stale_drops=1, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 55.827, "idle_wakeups_delta": 65, "revisions": 952, "typing_wall_ms": 16746.030166, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 498468, "doc_cache_entries": 2, "footprint_bytes": 94342656, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 66, "interrupt_wakeups": 3577, "laid_out_blocks": 63, "lua_bytes": 66078, "peak_footprint_bytes": 123227648, "preview_cache_bytes": 4192052, "resident_bytes": 165740544, "system_s": 1.57400075, "user_s": 3.7048715}

## typing doc=code n=500 preset=Default (2 runs)

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_huge_code_block.0.json | 10 | 7338 | 28200 | 28200 | 3703 | 28200 | 10057.7 |
| typing_huge_code_block.1.json | 10 | 8039 | 26913 | 26913 | 3738 | 26913 | 10987.9 |
| **pooled** | 20 | 7966 | 26913 | 28200 | 3703 | 28200 | 10522.8 |

### `cpu_ms_per_edit_all_threads` (ms)

Proxy: process user+system CPU over the typing run / edits (includes paint, save, previews)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_huge_code_block.0.json | 1 | 3.044 | 3.044 | 3.044 | 3.044 | 3.044 | 3.044 |
| typing_huge_code_block.1.json | 1 | 3.049 | 3.049 | 3.049 | 3.049 | 3.049 | 3.049 |
| **pooled** | 2 | 3.044 | 3.049 | 3.049 | 3.044 | 3.049 | 3.046 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_huge_code_block.0.json | 10 | 917115 | 967128 | 967128 | 499641 | 967128 | 878659.6 |
| typing_huge_code_block.1.json | 10 | 917101 | 932266 | 932266 | 536431 | 932266 | 881092.5 |
| **pooled** | 20 | 917115 | 933295 | 967128 | 499641 | 967128 | 879876.1 |

### `edit_main_thread_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_huge_code_block.0.json | 536 | 198 | 378 | 1920 | 13 | 49711 | 369.27 |
| typing_huge_code_block.1.json | 536 | 211 | 354 | 1768 | 15 | 20697 | 295.21 |
| **pooled** | 1072 | 204 | 365 | 1920 | 13 | 49711 | 332.24 |

Threshold (typing_main): p95 <= 8 ms -> measured 0.365 ms: **PASS**; per run: typing_huge_code_block.0.json PASS, typing_huge_code_block.1.json PASS

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_huge_code_block.0.json | 505 | 249 | 2729 | 3349 | 153 | 8414 | 674.38 |
| typing_huge_code_block.1.json | 504 | 252 | 2791 | 3298 | 123 | 4400 | 661.26 |
| **pooled** | 1009 | 250 | 2751 | 3349 | 123 | 8414 | 667.83 |

### `input_to_flush_us` (us)

Proxy: event delivery to the event-loop turn after the paint (backing store flushed); excludes WindowServer composition

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_huge_code_block.0.json | 500 | 2107 | 6468 | 9309 | 442 | 55704 | 2739.8 |
| typing_huge_code_block.1.json | 500 | 1835 | 6352 | 8214 | 554 | 25836 | 2613.4 |
| **pooled** | 1000 | 1959 | 6466 | 8587 | 442 | 55704 | 2676.6 |

Threshold (typing_presented): p95 <= 33 ms -> measured 6.466 ms: **PASS**; per run: typing_huge_code_block.0.json PASS, typing_huge_code_block.1.json PASS

### `input_to_paint_us` (us)

Proxy: event delivery to end of editor viewport paint (synthetic event; excludes compositor)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_huge_code_block.0.json | 500 | 843 | 4506 | 6050 | 406 | 55107 | 1582.0 |
| typing_huge_code_block.1.json | 500 | 883 | 4502 | 6841 | 492 | 25373 | 1492.0 |
| **pooled** | 1000 | 862 | 4506 | 6525 | 406 | 55107 | 1537.0 |

Threshold (typing_presented): p95 <= 33 ms -> measured 4.506 ms: **PASS**; per run: typing_huge_code_block.0.json PASS, typing_huge_code_block.1.json PASS

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_huge_code_block.0.json | 10 | 11331 | 13226 | 13226 | 11298 | 13226 | 11757.6 |
| typing_huge_code_block.1.json | 10 | 11360 | 13518 | 13518 | 11221 | 13518 | 11661.7 |
| **pooled** | 20 | 11360 | 13226 | 13518 | 11221 | 13518 | 11709.6 |

### Counters, extra fields and notes

**typing_huge_code_block.0.json**

- counters: block_height_estimates=71637, blocks_laid_out=758, bytes_copied_for_save=20,985,170, doc_cache_bytes=13,729,897, doc_cache_entries=2, highlight_lines=746, highlight_state_steps=20418, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=512, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=10, save_snapshots=10, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 500, "exec_to_main_ms": 61.611, "idle_wakeups_delta": 3, "revisions": 482, "typing_wall_ms": 8410.094417, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 13729897, "doc_cache_entries": 2, "footprint_bytes": 110890240, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 3, "interrupt_wakeups": 1399, "laid_out_blocks": 51, "lua_bytes": 66055, "peak_footprint_bytes": 149343488, "preview_cache_bytes": 0, "resident_bytes": 172474368, "system_s": 0.5515575416666666, "user_s": 1.4413335416666666}

**typing_huge_code_block.1.json**

- counters: block_height_estimates=71637, blocks_laid_out=758, bytes_copied_for_save=20,985,182, doc_cache_bytes=13,729,897, doc_cache_entries=2, highlight_lines=746, highlight_state_steps=20418, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=511, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=10, save_snapshots=10, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 500, "exec_to_main_ms": 123.258, "idle_wakeups_delta": 0, "revisions": 482, "typing_wall_ms": 8361.342417, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 13729897, "doc_cache_entries": 2, "footprint_bytes": 109890880, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 0, "interrupt_wakeups": 1406, "laid_out_blocks": 51, "lua_bytes": 66055, "peak_footprint_bytes": 148360512, "preview_cache_bytes": 0, "resident_bytes": 171606016, "system_s": 0.568424, "user_s": 1.4905292916666666}

## typing doc=ordinary n=1000 preset=Default (3 runs)

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing.0.json | 19 | 865 | 1452 | 1452 | 212 | 1452 | 832.05 |
| typing.1.json | 19 | 854 | 1236 | 1236 | 321 | 1236 | 856 |
| typing.2.json | 19 | 988 | 1364 | 1364 | 404 | 1364 | 949.53 |
| **pooled** | 57 | 929 | 1261 | 1452 | 212 | 1452 | 879.19 |

### `cpu_ms_per_edit_all_threads` (ms)

Proxy: process user+system CPU over the typing run / edits (includes paint, save, previews)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing.0.json | 1 | 5.024 | 5.024 | 5.024 | 5.024 | 5.024 | 5.024 |
| typing.1.json | 1 | 5.387 | 5.387 | 5.387 | 5.387 | 5.387 | 5.387 |
| typing.2.json | 1 | 5.322 | 5.322 | 5.322 | 5.322 | 5.322 | 5.322 |
| **pooled** | 3 | 5.322 | 5.387 | 5.387 | 5.024 | 5.387 | 5.245 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing.0.json | 19 | 900715 | 907020 | 907020 | 865350 | 907020 | 894328.4 |
| typing.1.json | 19 | 900766 | 951659 | 951659 | 621246 | 951659 | 892293.9 |
| typing.2.json | 19 | 900827 | 958991 | 958991 | 617190 | 958991 | 890063.2 |
| **pooled** | 57 | 900772 | 951485 | 958991 | 617190 | 958991 | 892228.5 |

### `edit_main_thread_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing.0.json | 1075 | 469 | 1011 | 4154 | 16 | 45932 | 615.39 |
| typing.1.json | 1075 | 533 | 1050 | 4135 | 33 | 27070 | 665.74 |
| typing.2.json | 1075 | 521 | 1051 | 4146 | 29 | 26454 | 647.33 |
| **pooled** | 3225 | 507 | 1036 | 4135 | 16 | 45932 | 642.82 |

Threshold (typing_main): p95 <= 8 ms -> measured 1.036 ms: **PASS**; per run: typing.0.json PASS, typing.1.json PASS, typing.2.json PASS

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing.0.json | 1014 | 411 | 5372 | 6500 | 92 | 6983 | 1176.6 |
| typing.1.json | 1015 | 445 | 5855 | 6558 | 66 | 7408 | 1306.5 |
| typing.2.json | 1015 | 430 | 5583 | 6601 | 76 | 7696 | 1293.4 |
| **pooled** | 3044 | 428 | 5570 | 6591 | 66 | 7696 | 1258.8 |

### `input_to_flush_us` (us)

Proxy: event delivery to the event-loop turn after the paint (backing store flushed); excludes WindowServer composition

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing.0.json | 1001 | 3403 | 10614 | 14490 | 664 | 51823 | 4741.3 |
| typing.1.json | 1001 | 3516 | 10794 | 14861 | 883 | 34973 | 5046.2 |
| typing.2.json | 1001 | 3607 | 10843 | 14481 | 499 | 33143 | 5000.9 |
| **pooled** | 3003 | 3491 | 10695 | 14791 | 499 | 51823 | 4929.5 |

Threshold (typing_presented): p95 <= 33 ms -> measured 10.695 ms: **PASS**; per run: typing.0.json PASS, typing.1.json PASS, typing.2.json PASS

### `input_to_paint_us` (us)

Proxy: event delivery to end of editor viewport paint (synthetic event; excludes compositor)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing.0.json | 1001 | 1893 | 8229 | 11360 | 511 | 51406 | 2911.4 |
| typing.1.json | 1001 | 2139 | 8637 | 12157 | 612 | 34402 | 3158.1 |
| typing.2.json | 1001 | 2094 | 8424 | 11354 | 428 | 32889 | 3103.9 |
| **pooled** | 3003 | 2032 | 8399 | 11544 | 428 | 51406 | 3057.8 |

Threshold (typing_presented): p95 <= 33 ms -> measured 8.399 ms: **PASS**; per run: typing.0.json PASS, typing.1.json PASS, typing.2.json PASS

### `preview_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing.0.json | 58 | 37 | 95 | 165 | 22 | 165 | 50.741 |
| typing.1.json | 59 | 41 | 145 | 26992 | 26 | 26992 | 515.03 |
| typing.2.json | 59 | 39 | 104 | 26355 | 24 | 26355 | 496.22 |
| **pooled** | 176 | 39 | 105 | 26355 | 22 | 26992 | 355.72 |

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing.0.json | 19 | 36 | 53 | 53 | 14 | 53 | 33.579 |
| typing.1.json | 19 | 32 | 67 | 67 | 17 | 67 | 34.526 |
| typing.2.json | 19 | 36 | 63 | 63 | 18 | 63 | 35.579 |
| **pooled** | 57 | 34 | 58 | 67 | 14 | 67 | 34.561 |

### Counters, extra fields and notes

**typing.0.json**

- counters: block_height_estimates=300, blocks_laid_out=1241, bytes_copied_for_save=16776, doc_cache_bytes=498468, doc_cache_entries=2, highlight_lines=15, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1022, preview_cache_bytes=95282, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=60, preview_requests=62, preview_stale_drops=0, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 44.859, "idle_wakeups_delta": 18, "revisions": 952, "typing_wall_ms": 16767.358459, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 498468, "doc_cache_entries": 2, "footprint_bytes": 89607552, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 18, "interrupt_wakeups": 2485, "laid_out_blocks": 63, "lua_bytes": 66078, "peak_footprint_bytes": 118852992, "preview_cache_bytes": 95282, "resident_bytes": 162201600, "system_s": 1.696574625, "user_s": 3.830431875}

**typing.1.json**

- counters: block_height_estimates=300, blocks_laid_out=1241, bytes_copied_for_save=16660, doc_cache_bytes=498468, doc_cache_entries=2, highlight_lines=15, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1023, preview_cache_bytes=96928, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=61, preview_requests=62, preview_stale_drops=1, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 51.402, "idle_wakeups_delta": 35, "revisions": 952, "typing_wall_ms": 16782.898917, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 498468, "doc_cache_entries": 2, "footprint_bytes": 89132544, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 36, "interrupt_wakeups": 2457, "laid_out_blocks": 63, "lua_bytes": 66078, "peak_footprint_bytes": 118902272, "preview_cache_bytes": 96928, "resident_bytes": 168378368, "system_s": 1.73371025, "user_s": 4.198390583333333}

**typing.2.json**

- counters: block_height_estimates=300, blocks_laid_out=1241, bytes_copied_for_save=16662, doc_cache_bytes=498468, doc_cache_entries=2, highlight_lines=15, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1022, preview_cache_bytes=96928, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=61, preview_requests=62, preview_stale_drops=1, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 57.21, "idle_wakeups_delta": 37, "revisions": 952, "typing_wall_ms": 16778.315, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 498468, "doc_cache_entries": 2, "footprint_bytes": 104123840, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 38, "interrupt_wakeups": 2470, "laid_out_blocks": 63, "lua_bytes": 66078, "peak_footprint_bytes": 119147968, "preview_cache_bytes": 96928, "resident_bytes": 188973056, "system_s": 1.7180674583333333, "user_s": 4.121988291666667}

## typing doc=ordinary n=1000 preset=Keyboard (3 runs)

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_keyboard_preset.0.json | 19 | 932 | 1430 | 1430 | 301 | 1430 | 911.21 |
| typing_keyboard_preset.1.json | 19 | 935 | 1442 | 1442 | 553 | 1442 | 942.42 |
| typing_keyboard_preset.2.json | 19 | 954 | 1267 | 1267 | 344 | 1267 | 877.58 |
| **pooled** | 57 | 935 | 1381 | 1442 | 301 | 1442 | 910.40 |

### `cpu_ms_per_edit_all_threads` (ms)

Proxy: process user+system CPU over the typing run / edits (includes paint, save, previews)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_keyboard_preset.0.json | 1 | 5.228 | 5.228 | 5.228 | 5.228 | 5.228 | 5.228 |
| typing_keyboard_preset.1.json | 1 | 5.289 | 5.289 | 5.289 | 5.289 | 5.289 | 5.289 |
| typing_keyboard_preset.2.json | 1 | 5.021 | 5.021 | 5.021 | 5.021 | 5.021 | 5.021 |
| **pooled** | 3 | 5.228 | 5.289 | 5.289 | 5.021 | 5.289 | 5.179 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_keyboard_preset.0.json | 19 | 900582 | 935028 | 935028 | 619238 | 935028 | 885604.1 |
| typing_keyboard_preset.1.json | 19 | 901047 | 950900 | 950900 | 616984 | 950900 | 886743.3 |
| typing_keyboard_preset.2.json | 19 | 900971 | 950910 | 950910 | 651128 | 950910 | 892370.7 |
| **pooled** | 57 | 900951 | 950701 | 950910 | 616984 | 950910 | 888239.4 |

### `edit_main_thread_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_keyboard_preset.0.json | 1075 | 499 | 1030 | 6286 | 30 | 27118 | 654.88 |
| typing_keyboard_preset.1.json | 1075 | 527 | 1073 | 6522 | 33 | 26046 | 672.48 |
| typing_keyboard_preset.2.json | 1075 | 485 | 1045 | 6145 | 20 | 28860 | 631.28 |
| **pooled** | 3225 | 505 | 1049 | 6369 | 20 | 28860 | 652.88 |

Threshold (typing_main): p95 <= 8 ms -> measured 1.049 ms: **PASS**; per run: typing_keyboard_preset.0.json PASS, typing_keyboard_preset.1.json PASS, typing_keyboard_preset.2.json PASS

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_keyboard_preset.0.json | 1015 | 438 | 5275 | 5948 | 53 | 6441 | 1214.8 |
| typing_keyboard_preset.1.json | 1015 | 438 | 5342 | 5968 | 77 | 6340 | 1224.9 |
| typing_keyboard_preset.2.json | 1015 | 422 | 5218 | 5833 | 84 | 6735 | 1203.6 |
| **pooled** | 3045 | 433 | 5265 | 5930 | 53 | 6735 | 1214.5 |

### `input_to_flush_us` (us)

Proxy: event delivery to the event-loop turn after the paint (backing store flushed); excludes WindowServer composition

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_keyboard_preset.0.json | 1001 | 3429 | 10754 | 14785 | 745 | 33825 | 4905.3 |
| typing_keyboard_preset.1.json | 1001 | 3542 | 10878 | 14980 | 960 | 32100 | 4960.0 |
| typing_keyboard_preset.2.json | 1001 | 3367 | 10727 | 14549 | 725 | 35409 | 4716.1 |
| **pooled** | 3003 | 3427 | 10798 | 14777 | 725 | 35409 | 4860.5 |

Threshold (typing_presented): p95 <= 33 ms -> measured 10.798 ms: **PASS**; per run: typing_keyboard_preset.0.json PASS, typing_keyboard_preset.1.json PASS, typing_keyboard_preset.2.json PASS

### `input_to_paint_us` (us)

Proxy: event delivery to end of editor viewport paint (synthetic event; excludes compositor)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_keyboard_preset.0.json | 1001 | 2001 | 8209 | 13703 | 599 | 33404 | 3075.8 |
| typing_keyboard_preset.1.json | 1001 | 2063 | 8227 | 13850 | 679 | 31818 | 3101.2 |
| typing_keyboard_preset.2.json | 1001 | 1930 | 8016 | 13516 | 491 | 34935 | 2970.6 |
| **pooled** | 3003 | 1999 | 8189 | 13703 | 491 | 34935 | 3049.2 |

Threshold (typing_presented): p95 <= 33 ms -> measured 8.189 ms: **PASS**; per run: typing_keyboard_preset.0.json PASS, typing_keyboard_preset.1.json PASS, typing_keyboard_preset.2.json PASS

### `preview_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_keyboard_preset.0.json | 59 | 42 | 120 | 27025 | 28 | 27025 | 512.54 |
| typing_keyboard_preset.1.json | 59 | 47 | 224 | 26012 | 27 | 26012 | 505.71 |
| typing_keyboard_preset.2.json | 59 | 39 | 103 | 28753 | 25 | 28753 | 537.10 |
| **pooled** | 177 | 42 | 120 | 27025 | 25 | 28753 | 518.45 |

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_keyboard_preset.0.json | 19 | 33 | 60 | 60 | 11 | 60 | 34.053 |
| typing_keyboard_preset.1.json | 19 | 32 | 63 | 63 | 18 | 63 | 35.158 |
| typing_keyboard_preset.2.json | 19 | 36 | 53 | 53 | 19 | 53 | 34.474 |
| **pooled** | 57 | 34 | 54 | 63 | 11 | 63 | 34.561 |

### Counters, extra fields and notes

**typing_keyboard_preset.0.json**

- counters: block_height_estimates=300, blocks_laid_out=1237, bytes_copied_for_save=16500, doc_cache_bytes=498468, doc_cache_entries=2, highlight_lines=14, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1021, preview_cache_bytes=96928, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=61, preview_requests=62, preview_stale_drops=1, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=15, widgets_reused=30
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 58.845, "idle_wakeups_delta": 29, "revisions": 952, "typing_wall_ms": 16723.272709, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 498468, "doc_cache_entries": 2, "footprint_bytes": 88706496, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 29, "interrupt_wakeups": 2450, "laid_out_blocks": 63, "lua_bytes": 55778, "peak_footprint_bytes": 117788096, "preview_cache_bytes": 96928, "resident_bytes": 159924224, "system_s": 1.715557375, "user_s": 3.92037825}

**typing_keyboard_preset.1.json**

- counters: block_height_estimates=300, blocks_laid_out=1237, bytes_copied_for_save=16528, doc_cache_bytes=498468, doc_cache_entries=2, highlight_lines=14, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1022, preview_cache_bytes=96928, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=61, preview_requests=62, preview_stale_drops=1, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=15, widgets_reused=30
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 68.961, "idle_wakeups_delta": 21, "revisions": 952, "typing_wall_ms": 16699.426625, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 498468, "doc_cache_entries": 2, "footprint_bytes": 102665728, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 21, "interrupt_wakeups": 2453, "laid_out_blocks": 63, "lua_bytes": 55778, "peak_footprint_bytes": 117853696, "preview_cache_bytes": 96928, "resident_bytes": 186171392, "system_s": 1.7719595416666665, "user_s": 4.060480541666666}

**typing_keyboard_preset.2.json**

- counters: block_height_estimates=300, blocks_laid_out=1237, bytes_copied_for_save=16670, doc_cache_bytes=498468, doc_cache_entries=2, highlight_lines=14, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=1022, preview_cache_bytes=96928, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=61, preview_requests=62, preview_stale_drops=1, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=15, widgets_reused=30
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 57, "idle_wakeups_delta": 14, "revisions": 952, "typing_wall_ms": 16718.072459, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 498468, "doc_cache_entries": 2, "footprint_bytes": 88919488, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 14, "interrupt_wakeups": 2482, "laid_out_blocks": 63, "lua_bytes": 55778, "peak_footprint_bytes": 117902784, "preview_cache_bytes": 96928, "resident_bytes": 168394752, "system_s": 1.650724625, "user_s": 3.854730833333333}

## typing doc=ordinary n=1000 recover=1 preset=Default (2 runs)

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_after_invalid_theme.0.json | 19 | 711 | 6449 | 6449 | 311 | 6449 | 989.21 |
| typing_after_invalid_theme.1.json | 19 | 752 | 2260 | 2260 | 436 | 2260 | 843.16 |
| **pooled** | 38 | 731 | 2260 | 6449 | 311 | 6449 | 916.18 |

### `cpu_ms_per_edit_all_threads` (ms)

Proxy: process user+system CPU over the typing run / edits (includes paint, save, previews)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_after_invalid_theme.0.json | 1 | 5.028 | 5.028 | 5.028 | 5.028 | 5.028 | 5.028 |
| typing_after_invalid_theme.1.json | 1 | 5.323 | 5.323 | 5.323 | 5.323 | 5.323 | 5.323 |
| **pooled** | 2 | 5.028 | 5.323 | 5.323 | 5.028 | 5.323 | 5.175 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_after_invalid_theme.0.json | 19 | 900618 | 933628 | 933628 | 638556 | 933628 | 885295.4 |
| typing_after_invalid_theme.1.json | 19 | 900921 | 950629 | 950629 | 622430 | 950629 | 888841.6 |
| **pooled** | 38 | 900889 | 934672 | 950629 | 622430 | 950629 | 887068.5 |

### `edit_main_thread_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_after_invalid_theme.0.json | 1075 | 515 | 1016 | 5303 | 16 | 28091 | 633.07 |
| typing_after_invalid_theme.1.json | 1075 | 528 | 1088 | 4058 | 32 | 41166 | 680.72 |
| **pooled** | 2150 | 523 | 1050 | 4689 | 16 | 41166 | 656.90 |

Threshold (typing_main): p95 <= 8 ms -> measured 1.050 ms: **PASS**; per run: typing_after_invalid_theme.0.json PASS, typing_after_invalid_theme.1.json PASS

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_after_invalid_theme.0.json | 1015 | 414 | 5072 | 5978 | 47 | 6571 | 1151.3 |
| typing_after_invalid_theme.1.json | 1014 | 450 | 5342 | 6013 | 43 | 7578 | 1226.6 |
| **pooled** | 2029 | 435 | 5251 | 5988 | 43 | 7578 | 1188.9 |

### `input_to_flush_us` (us)

Proxy: event delivery to the event-loop turn after the paint (backing store flushed); excludes WindowServer composition

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_after_invalid_theme.0.json | 1001 | 3391 | 10103 | 14273 | 510 | 34787 | 4744.7 |
| typing_after_invalid_theme.1.json | 1001 | 3524 | 10454 | 14356 | 950 | 49308 | 5008.5 |
| **pooled** | 2002 | 3447 | 10344 | 14276 | 510 | 49308 | 4876.6 |

Threshold (typing_presented): p95 <= 33 ms -> measured 10.344 ms: **PASS**; per run: typing_after_invalid_theme.0.json PASS, typing_after_invalid_theme.1.json PASS

### `input_to_paint_us` (us)

Proxy: event delivery to end of editor viewport paint (synthetic event; excludes compositor)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_after_invalid_theme.0.json | 1001 | 2078 | 7876 | 11380 | 439 | 34337 | 2907.0 |
| typing_after_invalid_theme.1.json | 1001 | 2151 | 7892 | 11383 | 724 | 48995 | 3075.0 |
| **pooled** | 2002 | 2110 | 7881 | 11383 | 439 | 48995 | 2991.0 |

Threshold (typing_presented): p95 <= 33 ms -> measured 7.881 ms: **PASS**; per run: typing_after_invalid_theme.0.json PASS, typing_after_invalid_theme.1.json PASS

### `preview_render_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_after_invalid_theme.0.json | 59 | 39 | 130 | 27989 | 29 | 27989 | 530.78 |
| typing_after_invalid_theme.1.json | 58 | 37 | 102 | 201 | 23 | 201 | 52.397 |
| **pooled** | 117 | 38 | 112 | 242 | 23 | 27989 | 293.63 |

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_after_invalid_theme.0.json | 19 | 32 | 50 | 50 | 13 | 50 | 33.526 |
| typing_after_invalid_theme.1.json | 19 | 34 | 62 | 62 | 16 | 62 | 35.789 |
| **pooled** | 38 | 33 | 50 | 62 | 13 | 62 | 34.658 |

### Counters, extra fields and notes

**typing_after_invalid_theme.0.json**

- counters: block_height_estimates=300, blocks_laid_out=1238, bytes_copied_for_save=16560, doc_cache_bytes=494372, doc_cache_entries=2, highlight_lines=15, highlight_state_steps=0, lua_errors=0, lua_invocations=6, lua_renders=3, paint_events=1022, preview_cache_bytes=96928, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=61, preview_requests=62, preview_stale_drops=1, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 48.134, "idle_wakeups_delta": 21, "invalid_shell_rejected": true, "revisions": 952, "shell_after_invalid": "Default", "typing_wall_ms": 16761.393542, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 494372, "doc_cache_entries": 2, "footprint_bytes": 89787840, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 22, "interrupt_wakeups": 2478, "laid_out_blocks": 62, "lua_bytes": 66078, "peak_footprint_bytes": 119573952, "preview_cache_bytes": 96928, "resident_bytes": 161218560, "system_s": 1.6660008333333332, "user_s": 3.9249881666666666}

**typing_after_invalid_theme.1.json**

- counters: block_height_estimates=300, blocks_laid_out=1238, bytes_copied_for_save=16606, doc_cache_bytes=494372, doc_cache_entries=2, highlight_lines=15, highlight_state_steps=0, lua_errors=0, lua_invocations=6, lua_renders=3, paint_events=1021, preview_cache_bytes=95282, preview_cache_hits=1, preview_cancelled=1, preview_outstanding=0, preview_renders=60, preview_requests=62, preview_stale_drops=0, save_snapshot_full_walks=19, save_snapshots=19, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 1000, "exec_to_main_ms": 64.15099999999995, "idle_wakeups_delta": 19, "invalid_shell_rejected": true, "revisions": 952, "shell_after_invalid": "Default", "typing_wall_ms": 16768.472625, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 494372, "doc_cache_entries": 2, "footprint_bytes": 90181056, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 20, "interrupt_wakeups": 2473, "laid_out_blocks": 62, "lua_bytes": 66078, "peak_footprint_bytes": 119311808, "preview_cache_bytes": 95282, "resident_bytes": 161660928, "system_s": 1.9286620416666664, "user_s": 4.112972208333333}

## typing doc=stress n=500 preset=Default (2 runs)

### `commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_stress_note.0.json | 10 | 3645 | 25800 | 25800 | 606 | 25800 | 5677.8 |
| typing_stress_note.1.json | 10 | 2812 | 13806 | 13806 | 999 | 13806 | 3988.6 |
| **pooled** | 20 | 3536 | 13806 | 25800 | 606 | 25800 | 4833.2 |

### `cpu_ms_per_edit_all_threads` (ms)

Proxy: process user+system CPU over the typing run / edits (includes paint, save, previews)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_stress_note.0.json | 1 | 5.314 | 5.314 | 5.314 | 5.314 | 5.314 | 5.314 |
| typing_stress_note.1.json | 1 | 4.748 | 4.748 | 4.748 | 4.748 | 4.748 | 4.748 |
| **pooled** | 2 | 4.748 | 5.314 | 5.314 | 4.748 | 5.314 | 5.031 |

### `dirty_to_commit_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_stress_note.0.json | 10 | 900860 | 923763 | 923763 | 494225 | 923763 | 864216.1 |
| typing_stress_note.1.json | 10 | 899779 | 918501 | 918501 | 510842 | 918501 | 859055 |
| **pooled** | 20 | 900570 | 922846 | 923763 | 494225 | 923763 | 861635.6 |

### `edit_main_thread_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_stress_note.0.json | 536 | 398 | 782 | 3099 | 30 | 12595 | 504.50 |
| typing_stress_note.1.json | 536 | 360 | 764 | 4535 | 29 | 8950 | 453.94 |
| **pooled** | 1072 | 380 | 777 | 3490 | 29 | 12595 | 479.22 |

Threshold (typing_main): p95 <= 8 ms -> measured 0.777 ms: **PASS**; per run: typing_stress_note.0.json PASS, typing_stress_note.1.json PASS

### `editor_paint_us` (us)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_stress_note.0.json | 503 | 514 | 6710 | 8514 | 156 | 20960 | 1459.6 |
| typing_stress_note.1.json | 503 | 459 | 6140 | 8278 | 107 | 10795 | 1259.2 |
| **pooled** | 1006 | 492 | 6543 | 8428 | 107 | 20960 | 1359.4 |

### `input_to_flush_us` (us)

Proxy: event delivery to the event-loop turn after the paint (backing store flushed); excludes WindowServer composition

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_stress_note.0.json | 500 | 3253 | 11907 | 15527 | 752 | 37492 | 4872.8 |
| typing_stress_note.1.json | 500 | 2898 | 11677 | 15302 | 630 | 18739 | 4393.9 |
| **pooled** | 1000 | 3106 | 11870 | 15527 | 630 | 37492 | 4633.4 |

Threshold (typing_presented): p95 <= 33 ms -> measured 11.870 ms: **PASS**; per run: typing_stress_note.0.json PASS, typing_stress_note.1.json PASS

### `input_to_paint_us` (us)

Proxy: event delivery to end of editor viewport paint (synthetic event; excludes compositor)

| Run | n | p50 | p95 | p99 | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_stress_note.0.json | 500 | 1862 | 9825 | 13442 | 668 | 36010 | 3029.2 |
| typing_stress_note.1.json | 500 | 1626 | 9029 | 14172 | 529 | 16352 | 2684.3 |
| **pooled** | 1000 | 1731 | 9399 | 13966 | 529 | 36010 | 2856.8 |

Threshold (typing_presented): p95 <= 33 ms -> measured 9.399 ms: **PASS**; per run: typing_stress_note.0.json PASS, typing_stress_note.1.json PASS

### `save_snapshot_us` (us)

| Run | n | p50 | p95 | p99 (exploratory) | min | max | mean |
|---|---|---|---|---|---|---|---|
| typing_stress_note.0.json | 10 | 1607 | 5669 | 5669 | 1154 | 5669 | 2144.7 |
| typing_stress_note.1.json | 10 | 1334 | 3406 | 3406 | 725 | 3406 | 1887.2 |
| **pooled** | 20 | 1607 | 3406 | 5669 | 725 | 5669 | 2016.0 |

### Counters, extra fields and notes

**typing_stress_note.0.json**

- counters: block_height_estimates=38215, blocks_laid_out=636, bytes_copied_for_save=5,834,658, doc_cache_bytes=54,857,808, doc_cache_entries=1, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=509, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=10, save_snapshots=10, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 500, "exec_to_main_ms": 84.024, "idle_wakeups_delta": 23, "revisions": 482, "typing_wall_ms": 8427.35525, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 54857808, "doc_cache_entries": 1, "footprint_bytes": 120016192, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 23, "interrupt_wakeups": 1292, "laid_out_blocks": 23, "lua_bytes": 66080, "peak_footprint_bytes": 198610240, "preview_cache_bytes": 0, "resident_bytes": 238878720, "system_s": 0.8931027083333333, "user_s": 2.336758833333333}

**typing_stress_note.1.json**

- counters: block_height_estimates=38215, blocks_laid_out=636, bytes_copied_for_save=5,834,656, doc_cache_bytes=54,857,808, doc_cache_entries=1, highlight_lines=7, highlight_state_steps=0, lua_errors=0, lua_invocations=5, lua_renders=3, paint_events=508, preview_cache_bytes=0, preview_cache_hits=0, preview_cancelled=0, preview_outstanding=0, preview_renders=0, preview_requests=0, preview_stale_drops=0, save_snapshot_full_walks=10, save_snapshots=10, widgets_created=19, widgets_reused=38
- extra: {"committed_matches_revision": true, "edits": 500, "exec_to_main_ms": 73.653, "idle_wakeups_delta": 16, "revisions": 482, "typing_wall_ms": 8408.8675, "window_exposed_and_active_ratio": 1}
- process: {"doc_cache_bytes_est": 54857808, "doc_cache_entries": 1, "footprint_bytes": 119885056, "footprint_counter": "task_vm_info.phys_footprint", "idle_wakeups": 18, "interrupt_wakeups": 1319, "laid_out_blocks": 23, "lua_bytes": 66080, "peak_footprint_bytes": 198937856, "preview_cache_bytes": 0, "resident_bytes": 230883328, "system_s": 0.7993017916666666, "user_s": 2.1104207083333333}

