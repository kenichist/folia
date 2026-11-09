# Folia benchmarks: fixtures, sampling, analysis

This covers the measurement tooling owned by performance-and-QA:

| Piece | Source | Built as |
|---|---|---|
| Deterministic workspace generator | `tools/fixturegen.cpp` | `folia_fixturegen` |
| External process sampler (macOS) | `tools/procstat.c` | `folia_procstat` |
| Report / threshold checker | `bench/scripts/analyze.py` | python3, stdlib only |
| Shell robustness suite | `tests/shell/` | `shell_tests` (ctest) |

The in-app benchmark driver (`src/app/BenchDriver.*`, `Folia --bench …`) is
integrator-owned; this document describes how the pieces fit together.

## Build

```sh
cmake -S . -B build-qa -G Ninja -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qtbase
cmake --build build-qa --target folia_fixturegen folia_procstat shell_tests folia
```

Benchmarks must use a Release build (the default `CMAKE_BUILD_TYPE`).

## 1. Fixtures (`folia_fixturegen`)

```sh
build-qa/tools/folia_fixturegen --out /tmp/bench/ws.db --seed 42 \
    --manifest /tmp/bench/manifest.json [--pages 50000] [--verify] [--force]
```

* The schema comes from the real `folia::Store` (opened and closed once, so
  the fixture carries exactly the app's migrations, now `PRAGMA user_version=2`
  with the empty `attachments` table; the 2026-11-04 results were made at 1).
  All rows are then written with raw SQLite in a few large transactions
  (pages in one, blocks in batches of 5,000 pages), `synchronous=OFF`,
  journal mode switched back to WAL at the end.
* An existing `--out` file is replaced only if it is an SQLite database (or
  with `--force`); its `-wal`, `-shm` and `.lock` side files are removed.
* Determinism: all content comes from a splitmix64 stream seeded from
  `--seed`; every page draws from its own stream derived from (seed, page
  id). Timestamps are fixed (`2025-01-01` + id). The same seed gives the same
  rows and, as measured below, a byte-identical database file.
* `--verify` re-opens the file through `folia::Store`, loads the ordinary,
  stress, code, math and broad pages and 8 sample pages into a real
  `PageSession` (offscreen `QGuiApplication`), marks every line dirty and
  checks that the save snapshot reproduces the stored blocks exactly (id,
  kind, lang, text, target, order). It also checks the broad branch child
  count (`fetchChildren`) and the deep chain (`ancestors`). Exit status 1 if
  anything differs.

### Workspace contents (default `--pages 50000`)

| Part | Shape |
|---|---|
| `fixtures_parent` (top level) | holds the four special pages below |
| `ordinary_page` | exactly 100 blocks, ≈25 KiB UTF-8 text (25,600 target): headings, paragraphs with inline `$…$` math, `` `code` `` spans, **bold**/*italic*, bullet/numbered/task lists, quotes, a table row, 3 code blocks in 3 different languages, 3 display formulas (one-line fraction `lang="single"`, multi-line `pmatrix`, multi-line `aligned`), and 3 child pages whose PageLink blocks are the last 3 of the 100 |
| `stress_page` | exactly 10,000 blocks, ≈10 MiB text (10,485,760 target); same mix (300 code, 300 math), longer paragraphs and code blocks |
| `code_page` | one Code block, `lang="cpp"`, ≥1 MiB (1,048,576) of C++: line/block/doc comments, string/char/raw/UTF-16 literals with escapes, hex/binary/float/digit-separator literals, templates, SFINAE, lambdas, enums, macros with line continuations, `#if` blocks |
| `math_page` | 100 blocks with 30 display formulas (fractions, matrices, aligned, cases, integrals, limits, gathered) |
| `broad_parent` (top level) | 5,000 children (leaf pages with short bodies) |
| `deep_root` … `deep_leaf` (top level) | a chain of 256 pages; `deep_leaf` is at depth 256 |
| 17 more top-level roots | random tree below them (depth ≤ 14), the rest of the 50,000 pages |
| `sample_pages` | 160 pages inside the random tree, each exactly 100 blocks ≈25 KiB, never parents (for open/navigation benchmarks) |

Every page with children ends with one PageLink block (kind 3, `target` =
child id) per child, in sibling order. 86% of leaves get a short body (1–5
blocks), the rest are title-only. Titles and text mix scripts: Latin with
combining marks (NFD `e` + U+0301 next to precomposed forms), CJK, Hangul
(including conjoining jamo), Arabic (with harakat), Hebrew (with niqqud),
Devanagari conjuncts, Cyrillic (`и` + U+0306), Thai, Greek, and emoji with
skin-tone modifiers, ZWJ sequences, VS16, flags and keycaps. Monolingual
sentences follow the script's spacing and punctuation (no spaces in CJK/Thai,
`。`, `।`, `؟`).

Text rules that keep the fixture faithful to the editor's line model
(`src/editor/PageSession.cpp`): Text blocks are single lines (no `\n`,
U+2028/2029, U+FFFC) that never start with ```` ``` ```` and are never `$$` or
`$$…$$`; code inner lines are never exactly ```` ``` ````; multi-line math uses
`lang=""` and never has an inner line `$$`.

With smaller `--pages` (minimum 200) the broad branch is `pages/5`, the deep
chain `pages/10` (max 256) and the sample set `pages/20` (max 160); the
manifest records the actual numbers.

### Manifest

Written to `--manifest` (or stdout). Keys:

| Key | Meaning |
|---|---|
| `seed`, `generator`, `generator_version`, `schema_version` | provenance (`schema_version` read back from `PRAGMA user_version`) |
| `pages`, `blocks`, `block_kinds{text,code,math,page_link}`, `text_bytes_utf8`, `db_bytes`, `top_level_pages`, `max_depth` | counts and sizes |
| `ordinary_page`, `stress_page`, `code_page`, `math_page`, `fixtures_parent` | special page ids |
| `page` | default for single-page scenarios (= `ordinary_page`) |
| `broad_parent`, `broad_children` | broad branch |
| `deep_root`, `deep_leaf`, `deep_leaf_depth` | deep chain |
| `sample_pages` | 160 distinct ordinary-sized page ids |
| `page_sizes` | blocks / text bytes / links of the special pages; min/max/mean of the samples |
| `content_hash` | `fnv1a64:<16 hex>`: FNV-1a 64 over every page row then every block row in insertion order (integers as 8 little-endian bytes, strings as 8-byte length + UTF-8). Same seed ⇒ same hash |
| `generation{tree_ms,write_ms,total_ms}` | timing of this run (not part of the hash) |
| `verify{…}` | only with `--verify` |

The workspace also gets `kv` rows `fixture.generator`, `fixture.seed`,
`fixture.content_hash` so a database can be matched to its manifest.

### Measured (M1 MacBook Air 8 GB, macOS 26.6.2, Release, 2026-11-04)

Raw numbers from this machine; the system load average was 8.9–14 during
these runs (concurrent compiles), so treat timings as upper bounds.

| Run | Result |
|---|---|
| `--seed 42` (50,000 pages), run A | 214,616 blocks (text 151,944, code 6,356, math 6,336, page_link 49,980), 30,125,739 text bytes, db 47,779,840 bytes, `fnv1a64:f5ecfb8eecd9058b`; generator `total_ms` 753, `/usr/bin/time` real 1.49 s, max RSS 109,117,440 bytes |
| same seed, run B | identical hash; `total_ms` 802, real 0.83 s; database SHA-256 identical to run A (`59e8d5cd…bc324d08`) |
| same seed with `--verify` | real 1.64 s, verify 735 ms, all 13 pages round-trip, broad children 5,000, deep chain 256; max RSS 214,302,720 bytes |
| page sizes (seed 42) | ordinary 100 blocks / 25,616 bytes; stress 10,000 / 10,485,784; code 1 / 1,048,975; math 100 / 16,465; samples 160 × 100 blocks, 25,560–25,775 bytes |

Observation from `--verify` (not a threshold): loading `broad_parent` (5,003
blocks, 5,000 page links) into a `PageSession` took 386.6 ms, versus 62.0 ms
for the 10,000-block stress page; page-link lines are inserted one
`insertText` call each in `PageSession::load`.

## 2. Running the app's bench scenarios

```sh
APP=build-qa/src/Folia.app/Contents/MacOS/Folia
cp /tmp/bench/ws.db /tmp/bench/run.db          # scenarios may write (create, typing)
$APP --home /tmp/bench/home --workspace /tmp/bench/run.db --no-watch \
     --bench open --bench-out /tmp/bench/open_1.json \
     --bench-arg manifest=/tmp/bench/manifest.json [--bench-arg n=120]
```

The integrator's runner `bench/run_suite.py --app … --fixturegen
build-qa/tools/folia_fixturegen --out bench/results/<name> [--quick]`
generates the fixture once (`--out <dir>/fixture/workspace.db --seed 42
--manifest <dir>/fixture/manifest.json`), runs every scenario on a fresh
copy and writes `<label>.<rep>.json` plus `environment.json`; analyze the
folder with `python3 -I bench/scripts/analyze.py <dir>/*.json`.

Use a fresh copy of the fixture per run for scenarios that write. Scenarios
(see `BenchDriver::run`): `launch`, `open`, `create`, `expand`, `typing`
(`doc=ordinary|stress|code`, `ime=true|false`, `bgrender=true`), `scroll`,
`stress`, `presentation`, `idle` (`seconds=60`, `settle_ms`, `vm_snapshots=0|1`), `navigate`
(`batches=5`, `per_batch=100`), `integration`, `snapshot`, `recovery`,
`settings` (`reps=10`, `window=0|1`, `apply=0|1`, `close_settle_ms`; see below),
`images` (`photos=24`, `idle_seconds=20`, `media_dir`, `step=40`; see §6),
`pdf` (§6b), `split` (`pdf_dir`, `idle_seconds=10`, `drag_steps=120`,
`step_px=6`, `note_lines=100`, `demo_hold_ms`; see §6c), `tabs` (`tabs=30`,
`switches=150`, `idle_seconds=20`, `page_lines=100`, `resize_steps=60`,
`strip=0|1`; see §6d), `import` (`md_mib=8`, `docx_paragraphs=40000`,
`idle_seconds=10`, `free=1`, `vm_snapshots=0|1|2`; see §6e–6f).
Every run records the allocator variables it was launched with as
`env.malloc_env` (§6f: the Claude Code app exports `MallocNanoZone=0`).
Every scenario takes `painter_clip=1`: paint the editor under QTextEdit's own
painter clip, as before 2026-11-08 (§6b; recorded as `editor_painter_clip`).
Bench JSON schema:

```json
{"scenario": "open",
 "env": {"os": "...", "qt": "...", "preset": "...", "args": {...}, "fixture_seed": 42, "fixture_hash": "..."},
 "metrics": {"open_uncached_to_editable_ms": {"unit": "ms", "samples": [..], "proxy": "..."}},
 "counters": {"LuaRenders": 3},
 "process": {"footprint_bytes": 0},
 "notes": ["..."]}
```

Extra top-level keys (e.g. `frames_within_16_7ms_ratio`) are shown by the
report as "extra" fields.

### `settings`: the Settings page

Each rep opens Settings (`showSettings()` → the page's first paint), makes
three text-size changes and then switches to dark and back to light
(main-thread time, and time until the editor, or the Settings page while it
covers the editor, repaints), restores the overrides and goes back to the
notes (the page is deleted). Footprint is sampled before, 500 ms after each
open and after each close; at the end the font lists are filled once.
`window=0` measures theme changes alone, `apply=0` the page alone, `modes=0`
leaves out the light/dark switches.

M1, Release, seeded welcome workspace (not the 50k fixture), 2026-11-05:

| Metric | Result |
|---|---|
| First open (creates the page) | 79–86 ms; `settings_create_us` 14–20 ms |
| Reopen | p50 36 ms (58 ms in runs that also change the theme), p95 50 ms |
| Footprint: before → first open → after close (`apply=0`, 30 reps) | 69.6 → 79.3 → 75.8 MB; then flat at 76.2–76.3 MB closed / 79.7–79.9 MB open |
| Same with theme changes each rep (20 reps) | levels off at 84–86.5 MB closed |
| Text-size change (`window=0 modes=0`) | main thread p50 0.8 ms; the first use of a size 12–16 ms (font engines); to paint ~32 ms |
| Light/dark switch | main thread p50 17–21 ms; to paint p50 48 ms (p95 66–78) |
| Font lists, first popup | 4–7 ms, +0.03 MB |

Findings:

* **Dark mode, editor colors.** A layout's style sheets (`#n_main{background-color:…}`)
  make Qt's style-sheet style save each descendant's palette when it
  polishes it and restore that copy on the next polish. A theme change set
  the editor's dark palette, then the re-render changed the style sheet and
  the editor got its light palette back: a white body with light text.
  `ShellHost::apply()` now re-applies the editor theme after reconciling
  (`app_tests` `darkModeRepaintsEditor` fails without it).
* **Native controls.** The palette does not reach macOS-drawn controls; they
  follow the app's appearance. A light theme on a dark Mac drew dark spin
  boxes and check marks on light colors. `applyTheme` now pins
  `QStyleHints::setColorScheme` to the theme's mode (only when it changes:
  re-sending the same scheme cost ~17 ms per apply). After a light/dark
  switch macOS drops Qt's font engines, and reloading them is most of that
  switch's 17–21 ms.
* **Windows.** The first version was a separate window. On macOS 26 with
  Qt 6.12, a top-level window that was activated and then destroyed by Qt
  (deleted, or closed with an accepted close event) keeps its native
  `QNSWindow` alive; `heap` showed one more per cycle and footprint grew
  3.7–3.9 MB per open/close. The in-window page has no native window.
* **Fonts.** Asking every font family whether it is fixed-pitch cost 300 ms
  and 4 MB, so the code-font list orders families by name.
* **Harness.** Benchmark loops run inside nested event loops, where
  `deleteLater()` is deferred: flush with
  `QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete)` before
  measuring a deletion. While the page covers the editor, wait for the
  page's paints, not the editor's.

## 3. External sampling (`folia_procstat`)

```sh
build-qa/tools/folia_procstat <pid> <interval_ms> <count>     # count 0 = until the process exits
```

One JSON object per line from `proc_pid_rusage(pid, RUSAGE_INFO_V6)` (falls
back to V4 on older kernels), read from outside the target so the sampler's
own work is never attributed to the app. Keys are the exact `rusage_info`
field names: `ri_phys_footprint`, `ri_lifetime_max_phys_footprint`,
`ri_interval_max_phys_footprint`, `ri_resident_size`, `ri_user_time`,
`ri_system_time` (raw Mach absolute units), `ri_user_time_ns`,
`ri_system_time_ns` (converted with `mach_timebase_info`; on Apple silicon
one unit is 125/3 ns), `ri_interrupt_wkups`, `ri_pkg_idle_wkups`,
`ri_pageins`, `ri_instructions`, `ri_cycles`, `ri_energy_nj` (V6), plus
`sample`, `wall_ms` (monotonic, since the first sample), `unix_ms`, `pid`,
`flavor`. When the process is gone the last line is
`{"sample": n, "error": "proc_pid_rusage: No such process"}` and the exit
status is 1; bad arguments exit 2.

Idle measurement (60 s window after the app settles):

```sh
$APP --home … --workspace … --no-watch --bench idle --bench-arg seconds=60 \
     --bench-arg manifest=… --bench-out idle.json &
build-qa/tools/folia_procstat $! 1000 0 > idle.jsonl
python3 bench/scripts/analyze.py idle.json idle.jsonl --procstat-skip 5
```

Sample line (offscreen Folia, idle, this machine):

```json
{"sample": 65, "wall_ms": 65001.336, "unix_ms": 1793785335130.326, "pid": 99839, "flavor": "RUSAGE_INFO_V6", "ri_phys_footprint": 37406976, "ri_lifetime_max_phys_footprint": 37603584, "ri_interval_max_phys_footprint": 37603584, "ri_resident_size": 7651328, "ri_user_time": 8298709, "ri_system_time": 3197820, "ri_user_time_ns": 345779541, "ri_system_time_ns": 133242500, "ri_interrupt_wkups": 81, "ri_pkg_idle_wkups": 0, "ri_pageins": 933, "ri_instructions": 3175582489, "ri_cycles": 1104470937, "ri_energy_nj": 708357210}
```

Caveats: samples sit on an absolute grid (no drift) but each wake-up can be
a few ms late (timer coalescing; `wall_ms` records the real time, and CPU %
is computed from it). `ri_interval_max_phys_footprint` is reset by every
`proc_pid_rusage` call on the process, so do not run two samplers at once.
Sampling another user's process fails with `Operation not permitted`.

## 4. Analysis (`bench/scripts/analyze.py`)

```sh
python3 bench/scripts/analyze.py run*.json idle.jsonl -o report.md --json summary.json \
        [--fail-on-miss] [--procstat-skip S] [--procstat-until S]
python3 bench/scripts/analyze.py --selftest
```

* Inputs are classified by content: bench JSON (an object with `scenario`),
  procstat JSON lines, or any other JSON object (such as
  `bench/run_suite.py`'s pretty-printed `environment.json`), which is shown
  in the report's Environment section and never gated.
* Runs are grouped by configuration = scenario + bench args (the manifest
  path is ignored) + active layout name (`env.preset`, so `--preset` /
  `--shell` runs stay separate) + `math=off` for `--no-math`. Typing runs with
  different `doc`, `bgrender`, `recover` or preset are therefore separate
  rows. For each metric: n, p50, p95, p99, min, max, mean, one
  row per run when a configuration has several runs, plus a pooled row.
  Percentiles are nearest-rank on raw samples (no interpolation); p99 is
  labelled "exploratory" when n < 1000.
* Thresholds live in `REQUIREMENTS` and `THRESHOLDS` at the top of the
  script. To map a new metric, add `"metric_name": [{"req": "<requirement
  id>", "scenarios": ["open"], "stat": "p95", "op": "<=", "limit": 30,
  "unit": "ms"}]`; units are converted (`us`→`ms`, …). Gates are scoped:
  a check only counts runs of its `scenarios` (`open` → open gates, `create`,
  `expand`, `typing`, `scroll`, `presentation` → code/formula, `idle`,
  `navigate`, `procstat` → procstat files); integration / stress / recovery
  runs that emit the same metrics are reported, never gated. A requirement
  is PASS when every measured gating check passes, MISS when any fails,
  UNMEASURED when none was measured (or a check lacks data: `min_n`,
  `min_window_s`). `"gate": False` checks produce INFO rows only.
* Interpretation choices (written down so they are not silently relaxed):
  "60 MB" is 60,000,000 bytes (decimal, the stricter reading); code /
  formula presentation use p95 (the requirement names no percentile; set
  `"stat": "max"` for the strictest reading); `formula_first_in_process_ms`
  is checked with `max`; scroll gates on the share of `scroll_frame_work_ms`
  samples (main-thread scroll handling + editor paint per frame) ≤ 16.7 ms
  (≥ 0.95), and an INFO row reports the longest `scroll_paint_interval_ms`
  and the number of hitches (intervals > 25 ms); idle
  CPU needs ≥ 60 one-second samples (bench) or a ≥ 60 s procstat window
  (minus 50 ms grid jitter); the navigation plateau requires every batch's
  doc-cache bytes ≤ 48 MiB, entries ≤ 16 and preview cache ≤ 4 MiB (app
  defaults, `CACHE_CAPS`) and settled footprint growth from batch 2 to the
  last batch ≤ max(4 MiB, 5%) (`PLATEAU`; batch 1 warms the caches).
* `--selftest` builds synthetic bench, procstat and pretty-printed context
  files, checks the percentile method, unit conversion, grouping (including
  preset and typing configurations), scenario-scoped gates (integration /
  stress numbers must not change verdicts), per-run verdicts, scroll
  frame-work share and hitch INFO row, plateau, procstat CPU window and the
  exploratory label, and prints the report.

## 5. Shell robustness suite (`tests/shell`)

```sh
ctest --test-dir build-qa -R shell_tests --output-on-failure
QT_QPA_PLATFORM=offscreen build-qa/tests/shell/shell_tests [testFunction]
```

Builds a real host (temp `Store`, `EditorContext`, `EditorPane`,
`CommandRegistry` with dummy commands plus every command the presets bind,
`DataContext`, window + container, `ShellHost`) and covers: valid layouts;
syntax/runtime/validation errors (missing or two editors, unknown type,
duplicate ids, list without source, functions in props, children on leaves,
`ctx.set/toggle/run` in `view()`) keeping the previous layout's widget
pointers; instruction/time/memory/host-call budgets in `view()`, at top
level and in handlers; nesting (40 ok / 41 rejected) and node limits (2000
ok / 2001 rejected); shortcut conflicts (`Mod+K` vs `Cmd+K` on macOS /
`Ctrl+K` elsewhere), reserved keys, unknown commands/modifiers; handler
errors; `ctx.run` results; widget identity across renders and reloads; the
`EditorPane`, its `QTextDocument`, text, cursor and undo stack across reloads;
live reload deferred during IME preedit and applied on commit; `renderNow()`
/ `loadSource()` timing of `presets/default/shell.lua` (n = 200, printed via
`qInfo`).

Probes that can hang or abort run in a child process (the test binary
re-executed with `FOLIA_SHELL_CHILD=<scenario>`, killed after a timeout).
Confirmed defects are `QEXPECT_FAIL` with a `BUG:` message, so they show as
XFAIL; a fix turns one into XPASS, which fails the suite until the
annotation is removed. Child probes: `pcall_loop` (budget error caught by
`pcall`), `pattern_bomb` (backtracking `string.find`), `meta_index_error` /
`meta_index_loop` (metamethods on the spec / node tables), `mem_retained`
(heap held at the 24 MB budget), `gc_loop` / `gc_loop_bypass` (`__gc`
finalizers, plain and with the global `type` shadowed), `ctx_newindex_error`
/ `ctx_newindex_loop` (`__newindex` on the host-owned `ctx` table). The
metamethod-on-spec/node, memory-at-budget and plain `__gc` probes were
defects that are now fixed in `src/shell` and are plain assertions.

## 6. Images (attachments)

What docs/ATTACHMENTS.md §6 asks for, for steps 1–3 (images; PDFs come
later). Two tools:

```sh
build/tools/folia_mediaprobe --make /tmp/bench/media 24      # fixtures, out of process
python3 -I bench/scripts/mediaprobe_matrix.py --probe build/tools/folia_mediaprobe \
    --media /tmp/bench/media --out mediaprobe.json             # decode cost per format and size
APP=build/src/Folia.app/Contents/MacOS/Folia
$APP --home /tmp/bench/img-home --no-watch --bench images --bench-out images.json \
    --bench-arg media_dir=/tmp/bench/media [--bench-arg photos=24] [--bench-arg idle_seconds=20]
```

`bench/run_suite.py` writes the fixtures and runs `images` too. The
fixtures: 24 JPEG photos 4032×3024 (5.3 MB each), one HEIC and one WebP of
the same size, three 2880×1800 UI-like PNG screenshots, a 1000×40000 PNG
scrolling screenshot, and a 6000×4000 noisy PNG (36 MB).

`--bench images` measures, in one process:

0. the decode cost of each format, before anything else runs;
1. idle on a text-only page (the baseline);
2. building a 30-image page through the real import path;
3. idle on that page;
4. one continuous scroll down;
5. straight back up;
6. a continuous window resize to a new width, then a small back-and-forth;
7. a 36 MB PNG imported while 150 characters are typed;
8. critical memory pressure (`reclaimMemory(true)`), then 300 more scroll
   frames during the cool-down;
9. idle again.

Results are top-level "extra" keys (`down_decodes`, `up_cache_hit_rate`,
`resize_decodes_during_drag`, `pressure_footprint_after_trim`, …) and
metrics (`images_scroll_*_frame_ms`, `decode_peak_bytes_<format>`,
`images_scroll_decode_ms` and `_cpu_ms` for every decode while scrolling and
resizing, `import_typing_input_to_paint_us`, …).

Peaks use rusage's `ri_interval_max_phys_footprint`
(`platform::intervalPeakFootprint()`). The kernel resets it on every read,
so it is read once right before and once right after the work.

If the system does not paint the window (covered, or on another Space),
nothing is wanted and nothing decodes. The scenario counts such frames
(`*_unpainted_frames`) and paints them through the same code into an
offscreen image, so decodes and memory stay valid. Their timings are left
out. `offscreen_fallback=0` turns this off.

### Measured (M1 MacBook Air 8 GB, macOS 26.6.2, Release, 2026-11-06)

Raw files and provenance: `bench/results/images-2026-11-06/`. Battery,
thermal state nominal, window 1180×759 pt at 2×, so images 760 pt wide
decode at 1520 px. MB = 10⁶ bytes. Budget: 128 MiB (134.2 MB).

**Image decode cost** (`folia_mediaprobe`: a fresh process per point, worst
of 3). "Estimate" is `MediaService::estimateFor` with the final factors.
"Kept after" is footprint growth that outlives the decode beyond the
output bitmap (ImageIO and allocator caches).

| Format | Native | Output | Peak MB | Peak ÷ output | Estimate MB | Estimate ÷ peak | Decode ms | Worker CPU ms | Kept after MB |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| jpeg | 4032×3024 | 400×300 | 2.8 | 5.73 | 8.4 | 3.05 | 33 | 33 | 0.8 |
| jpeg | 4032×3024 | 800×600 | 8.8 | 4.59 | 14.7 | 1.67 | 39 | 39 | 0.9 |
| jpeg | 4032×3024 | 1400×1050 | 32.2 | 5.48 | 36.3 | 1.13 | 79 | 35 | 2.2 |
| jpeg | 4032×3024 | 1520×1140 | 33.3 | 4.80 | 38.4 | 1.16 | 77 | 36 | 2.3 |
| jpeg | 4032×3024 | 2800×2100 | 123.1 | 5.23 | 126.5 | 1.03 | 145 | 97 | 2.6 |
| jpeg | 4032×3024 | 4032×3024 | 148.3 | 3.04 | 177.0 | 1.19 | 106 | 56 | 2.0 |
| heic | 4032×3024 | 400×300 | 6.0 | 12.49 | 10.1 | 1.69 | 184 | 19 | 4.1 |
| heic | 4032×3024 | 800×600 | 11.8 | 6.16 | 15.3 | 1.29 | 183 | 19 | 5.7 |
| heic | 4032×3024 | 1400×1050 | 28.0 | 4.77 | 32.3 | 1.15 | 158 | 21 | 10.4 |
| heic | 4032×3024 | 1520×1140 | 32.3 | 4.65 | 34.4 | 1.07 | 151 | 22 | 11.4 |
| heic | 4032×3024 | 2800×2100 | 99.6 | 4.23 | 104.2 | 1.05 | 172 | 32 | 29.0 |
| heic | 4032×3024 | 4032×3024 | 153.1 | 3.14 | 154.7 | 1.01 | 191 | 44 | 5.1 |
| webp | 4032×3024 | 400×300 | 51.2 | 106.60 | 63.7 | 1.25 | 163 | 162 | 1.2 |
| webp | 4032×3024 | 800×600 | 55.1 | 28.70 | 66.8 | 1.21 | 169 | 169 | 1.4 |
| webp | 4032×3024 | 1400×1050 | 65.6 | 11.16 | 75.1 | 1.14 | 181 | 181 | 1.3 |
| webp | 4032×3024 | 1520×1140 | 68.2 | 9.84 | 77.3 | 1.13 | 182 | 182 | 1.4 |
| webp | 4032×3024 | 2800×2100 | 111.7 | 4.75 | 112.1 | 1.00 | 210 | 209 | 1.6 |
| webp | 4032×3024 | 4032×3024 | 147.4 | 3.02 | 165.1 | 1.12 | 175 | 171 | 1.0 |
| png screenshot | 2880×1800 | 400×250 | 7.3 | 18.15 | 36.9 | 5.08 | 30 | 30 | 4.0 |
| png screenshot | 2880×1800 | 800×500 | 15.2 | 9.48 | 46.8 | 3.08 | 32 | 32 | 3.8 |
| png screenshot | 2880×1800 | 1400×875 | 23.9 | 4.88 | 53.7 | 2.25 | 36 | 36 | 6.0 |
| png screenshot | 2880×1800 | 1520×950 | 36.9 | 6.39 | 85.1 | 2.30 | 44 | 44 | 1.1 |
| png screenshot | 2880×1800 | 2800×1750 | 72.9 | 3.72 | 114.1 | 1.56 | 62 | 62 | 1.2 |
| png screenshot | 2880×1800 | 2880×1800 | 62.9 | 3.04 | 116.5 | 1.85 | 34 | 34 | 0.7 |
| png huge | 6000×4000 | 400×267 | 10.4 | 24.39 | 37.3 | 3.58 | 328 | 327 | 6.9 |
| png huge | 6000×4000 | 800×533 | 19.1 | 11.17 | 48.5 | 2.55 | 331 | 330 | 7.1 |
| png huge | 6000×4000 | 1400×933 | 24.4 | 4.67 | 55.9 | 2.29 | 335 | 334 | 8.7 |
| png huge | 6000×4000 | 1520×1013 | 52.6 | 8.53 | 92.1 | 1.75 | 346 | 345 | 12.4 |
| png huge | 6000×4000 | 2800×1867 | 91.3 | 4.36 | 123.1 | 1.35 | 366 | 365 | 12.6 |
| png huge | 6000×4000 | 6000×4000 | 289.0 | 3.01 | 417.6 | 1.44 | 358 | 353 | 0.9 |
| png tall | 1000×40000 | 400×16000 | 97.8 | 3.82 | 163.3 | 1.67 | 277 | 276 | 3.0 |
| png tall | 1000×40000 | 800×32000 | 361.8 | 3.53 | 552.6 | 1.53 | 454 | 446 | 1.8 |
| png tall | 1000×40000 | 1000×40000 | 481.0 | 3.01 | 673.6 | 1.40 | 298 | 292 | 0.9 |

- **Reduced decoding.** ImageIO decodes JPEG, PNG and HEIC at native/2^k
  (k ≤ 3), the largest power-of-two step that still covers the output.
  That is why a 2880×1800 PNG costs 23.9 MB at 1400 px (k = 1) but 36.9 MB
  at 1520 px (k = 0). WebP is decoded at full size whatever the output.
- **Subsampling.** `kCGImageSourceSubsampleFactor` 2 or 4 lowered no peak
  (JPEG rose from 30.7 to 35.8 MB), so it is not used.
- **QImageReader**, for comparison, with Homebrew's plugins: a 6000×4000 PNG
  at 1400 px peaks at 97 MB (a full decode) against 23 MB with ImageIO; a
  JPEG at 1400 px peaks at 12 MB.
- **Worker CPU** counts only the decoding thread. ImageIO decodes HEIC (and
  some of JPEG) on its own threads: a HEIC decode takes 150–190 ms of wall
  time but only ~20 ms of worker CPU.

**The same decodes inside the app** (`--bench images`, phase 0, main
thread, run 4). PNG peaks here are about 30 MB higher than in the probe
process, so PNG's fixed cost (32 MiB) is set from these numbers:

| Format | Decode | Peak MB | Estimate MB | Estimate ÷ peak | Decode ms (median) |
|---|---|---:|---:|---:|---:|
| jpeg | 4032×3024 → 1520×1140 | 31.9 | 38.4 | 1.21 | 45 |
| heic | 4032×3024 → 1520×1140 | 31.3 | 34.4 | 1.10 | 71 |
| webp | 4032×3024 → 1520×1140 | 66.7 | 77.3 | 1.16 | 181 |
| png screenshot | 2880×1800 → 1520×950 | 66.6 | 85.1 | 1.28 | 43 |
| png huge | 6000×4000 → 1520×1013 | 65.1 | 92.1 | 1.41 | 350 |
| png tall | 1000×40000 → 234×9360 | 66.3 | 71.0 | 1.07 | 242 |

The tall screenshot decodes at 234×9360, not 1000×40000. A decode may
reserve at most 75 % of the budget, and a PNG still needs a native/2^k
intermediate. It is drawn scaled up to 760 pt, so it looks blurry.

**The image-heavy page** (`images.json`, run 4; frame timings from
`images-onscreen-frames.json`, run 3, when the window was painted
on screen):

| Measure | Result |
|---|---|
| Idle, text page (baseline), 20 s after a 3 s settle | 0.27 % of a core, 7.0 wakeups/s (median 6), 55.2 MB, 7 threads |
| Idle, image page (2 images visible) | 0.15 %, 6.0 wakeups/s (median 6), 73.0 MB, 11 threads |
| Idle, after the whole scenario | 0.16 %, 5.5 wakeups/s, 102.0 MB, 12 threads |
| Import, 30 files streamed in turn | 520 ms in total; the 36 MB PNG was stored (copied, hashed, fsynced, renamed, header read) in 156 ms (90 ms in run 3) |
| Typing while the 36 MB PNG is imported | input → end of paint p50 2.9 ms, p95 5.5 ms, max 32.8 ms (run 3: 1.8 / 3.5 / 14.7 ms). The copy holds one 256 KiB buffer. Peak footprint growth over the import (36.6 MB) is mostly decodes of images scrolled into view by the typing. |
| Scroll down the page once (1,202 frames of 40 px) | 29 decodes (the 30th image was decoded at open). 0 jobs dropped, discarded, waiting or too large. Media use peaked at 133.4 MB of the 134.2 MB budget; the cache at 101.7 MB. |
| Footprint during that scroll | 87 → 137 MB, flat across the tall screenshot (one decode), ending at 164 MB (sampled max 185 MB, rusage peak 194 MB). That is about the baseline plus the budget: a plateau. |
| Straight back up | 13 cache hits, 16 decodes (hit rate 0.45). The 30 bitmaps (6.9 MB each, 207 MB) exceed the budget, so the earliest were evicted. Images within the budget all hit (`media_tests`). |
| Scroll frames, on screen (run 3) | down: p50 16.66 ms, p95 16.80 ms, max 18.31 ms; up: p50 16.66 ms, max 16.92 ms; editor paint p50 2.6 ms, p95 3.9 ms |
| Window resize, 40 steps of 10 px | 0 decodes while it moved; 3 once it settled (the 3 visible images). A ±30 px back-and-forth: 0 decodes (reused within ±10 %). Frames (run 3): p50 16.7 ms, p95 33.3 ms, max 65.0 ms |
| Critical memory pressure | cache trimmed to 0 and the footprint fell from 162.5 to 102.0 MB at once; budget 33.6 MB for 60 s |
| Scrolling during the cool-down (300 frames) | 10 decodes at a lower density, 0 waits, media use max 32.9 MB, footprint max 117.9 MB; frames p50 16.66 ms, p95 17.07 ms, max 33.3 ms (on screen) |

What these runs found (fixed in this branch, and recorded in
docs/ATTACHMENTS.md):

- **Lowering the budget under pressure stalled rendering.** Every photo's
  estimate exceeded the lowered budget and waited for the whole
  cool-down. Request sizes now follow the current budget.
- **Freed bitmaps stayed in the footprint.** Trimming ~100 MB of cache left
  it unchanged, and `malloc_zone_pressure_relief` released 0 bytes. Bitmaps
  now live in their own mappings.
- **PNG decodes cost more inside the app than in the probe.** PNG's
  factors are set from the in-app peaks.

Not covered yet: `QImageReader` as the main decoder (other platforms).
PDFs are in §6b.

## 6b. PDFs (attachments)

What docs/ATTACHMENTS.md §6 asks for, for step 4 (PDFs on CoreGraphics).
Three tools:

```sh
build/tools/folia_mediaprobe --make-pdf /tmp/bench/pdf        # fixtures, out of process
python3 -I bench/scripts/pdfprobe_matrix.py --probe build/tools/folia_mediaprobe \
    --pdfs /tmp/bench/pdf --out pdfprobe.json                  # render cost: whole page vs tiles
APP=build/src/Folia.app/Contents/MacOS/Folia
$APP --home /tmp/bench/pdf-home --no-watch --bench pdf --bench-out pdf.json \
    --bench-arg pdf_dir=/tmp/bench/pdf [--bench-arg idle_seconds=20] [--bench-arg step=300]
```

`bench/run_suite.py` writes the PDF fixtures and runs `pdf` too. The
fixtures (CoreGraphics `CGPDFContext`, deterministic):

| File | Pages | What |
|---|---:|---|
| `long-200.pdf` | 200 | Letter, nine paragraphs of Times body text each, a 40-curve figure every 5th page (0.35 MB) |
| `scanned-24.pdf` | 24 | one 2550×3300 (300 dpi) photo-like JPEG per page, kept as DCT (48 MB) |
| `vector-16.pdf` | 16 | 3000 stroked Béziers and 400 filled polygons per page (6.2 MB) |
| `transparency-16.pdf` | 16 | text under 24 transparency groups, each with 6 nested groups, blend modes and an alpha-gradient soft mask (4.5 MB) |
| `mixed-8.pdf` | 8 | Letter, A4, landscape and a 226×600 receipt in turn, red/blue corner markers (tests) |

`folia_mediaprobe --pdf <file> <page> <pageWidthPx> [tile]` renders one
page in a fresh process: the whole page as one bitmap, or the N×N tiles
covering a 2200×1600 px viewport (a 1100×800 pt window at 2×) at the page's
top left, each drawn straight into its own bitmap. It renders twice (cold:
the document just opened; warm: CG's per-document caches filled), and
reports the worst single bitmap's peak (`ri_interval_max_phys_footprint`,
read before and after) and the footprint left after the document closes.

`--bench pdf` measures, in one process:

0. in-app render cost per document kind (thumbnail and one viewer tile);
1. idle on a text page (baseline);
2. a note with the five PDFs as cards, through the real import path;
3. typing 300 characters in that note while a PDF view in a window beside
   it renders vector tiles at 3× (input latency; repaints each side
   caused the other);
4. the 200-page document in the viewer: scrolled to the end, back up, and
   down again (plateau); zoomed in and out 6 times around page 100
   (renders during gestures, time to sharp after settling); scrolled at
   4×; closed (footprint, cache and documents released);
5. scanned, vector and transparency documents: first viewport, a scroll
   through every page, last viewport, closed;
6. idle with the 200-page viewer open and settled; critical memory
   pressure, then 150 scroll frames during the cool-down;
7. closed, idle again.

Results are top-level "extra" keys (`down_renders`, `up_cache_hit_rate`,
`zoom_renders_started_during_gestures`, `long_close_footprint_after`,
`<kind>_first_viewport_ms`, `beside_editor_paints_after_typing`, …) and
metrics (`pdf_scroll_*_frame_ms`, `pdf_zoom_settle_to_sharp_ms`,
`pdf_render_ms_<kind>`, `pdf_beside_typing_input_to_paint_us`, …). Frames
the system did not paint are painted offscreen through the same code, as
in `images` (`*_unpainted_frames`).

### PDF render cost (M1 MacBook Air 8 GB, macOS 26.6.2, Release, 2026-11-06)

Raw files: `bench/results/pdf-2026-11-06/` (`pdfprobe-tiles.json`,
`pdfprobe-estimate.json`). Page 3 of each fixture. Each cell: cold render
time (all bitmaps of the point), worst single-bitmap peak (MiB), footprint
left after the document closed (MiB). Tiles cover one 2200×1600 px
viewport, the whole page covers the whole page, so compare peaks and what
is left, and compare time per area with care.

| Document | Page px | Whole page: ms / peak / left | 1024 px tiles: ms / peak / left | 512 px tiles: ms / peak / left |
|---|---:|---|---|---|
| text | 600 | 10 / 3.4 / 1.7 | 6 / 3.4 / 1.7 | 8 / 2.2 / 1.7 |
| text | 1200 | 27 / 9.1 / 2.5 | 11 / 5.0 / 2.1 | 27 / 3.1 / 2.2 |
| text | 2000 | 13 / 22.4 / 2.6 | 10 / 5.4 / 1.4 | 16 / 2.4 / 1.4 |
| text | 3000 | 21 / 48.0 / 3.5 | 23 / 5.2 / 1.3 | 20 / 2.3 / 1.3 |
| text | 4000 | 46 / 83.8 / 5.3 | 11 / 5.2 / 1.3 | 23 / 2.3 / 1.3 |
| text | 6000 | 97 / 185.8 / 8.0 | 11 / 5.3 / 1.4 | 22 / 2.4 / 1.4 |
| scanned | 600 | 45 / 10.5 / 5.4 | 26 / 12.0 / 5.4 | 43 / 11.0 / 5.2 |
| scanned | 1200 | 143 / 35.5 / 15.9 | 96 / 32.4 / 6.8 | 111 / 28.2 / 6.6 |
| scanned | 2000 | 146 / 92.0 / 12.9 | 181 / 76.4 / 8.3 | 181 / 73.4 / 8.2 |
| scanned | 3000 | 202 / 116.5 / 18.9 | 112 / 76.2 / 7.9 | 138 / 73.3 / 7.9 |
| scanned | 4000 | 251 / 151.1 / 27.5 | 95 / 76.2 / 7.8 | 135 / 73.4 / 8.2 |
| scanned | 6000 | 415 / 262.3 / 52.4 | 98 / 76.1 / 7.8 | 140 / 73.3 / 7.9 |
| vector | 600 | 493 / 2.9 / 1.2 | 489 / 2.9 / 1.1 | 626 / 2.3 / 1.5 |
| vector | 1200 | 1366 / 10.8 / 2.9 | 1520 / 6.9 / 3.1 | 1952 / 2.5 / 1.7 |
| vector | 2000 | 4160 / 54.6 / 26.7 | 2342 / 6.9 / 1.6 | 3956 / 2.7 / 1.7 |
| vector | 3000 | 7343 / 122.3 / 75.1 | 2940 / 7.2 / 2.3 | 4579 / 2.5 / 1.4 |
| vector | 4000 | 12154 / 216.1 / 147.4 | 2345 / 7.0 / 3.1 | 3875 / 2.5 / 1.5 |
| vector | 6000 | 21726 / 602.3 / 424.5 | 1496 / 6.0 / 2.8 | 3260 / 2.6 / 1.5 |
| transparency | 600 | 202 / 12.7 / 10.9 | 191 / 12.6 / 10.9 | 208 / 8.6 / 8.0 |
| transparency | 1200 | 695 / 55.0 / 48.0 | 884 / 33.7 / 32.1 | 744 / 10.3 / 10.8 |
| transparency | 2000 | 2061 / 157.2 / 98.0 | 1605 / 44.9 / 29.8 | 1665 / 11.4 / 10.4 |
| transparency | 3000 | 4416 / 328.9 / 227.9 | 2284 / 45.2 / 28.4 | 2232 / 11.1 / 10.0 |
| transparency | 4000 | 7515 / 634.9 / 556.1 | 2321 / 44.9 / 41.6 | 2076 / 12.4 / 11.4 |
| transparency | 6000 | 23446 / 1200.0 / 742.8 | 1797 / 41.7 / 28.7 | 1590 / 13.3 / 9.2 |

- **CoreGraphics' working set follows content.** Plain text peaks at
  ~1.1× its bitmap, vector pages at ~2.7×, transparency at ~8× (one
  device-resolution layer per group, clipped to the bitmap). A scan costs
  the decoded size of its JPEG (~73 MiB for 2550×3300) at any output from
  about 1200 px up, reduced by a power of two below that.
- **What it frees stays.** The "left" column is live `MALLOC_LARGE`
  memory the allocator keeps for reuse (`footprint -p`; 132 MB after one
  2000 px transparency page). `malloc_zone_pressure_relief` returns none
  of it, and the warm render adds nothing: the footprint plateaus at the
  largest single render's working set. Tiles of 512 px leave ≤ 11.4 MiB on
  every fixture; whole pages up to 742.8 MiB.
- **Tile size.** 512 px against 1024 px: 4× lower peaks with
  transparency (11–13 vs 42–45 MiB), 2.7× lower on vector pages, the same
  for text and scans (the decoded image dominates). Time: equal within
  noise on text and transparency; 512 px tiles cost up to 1.4× on scans at
  3000 px and more, and up to 1.7× on vector pages (each tile walks the
  whole content stream again). Chosen: **512**, for the bounded memory.
- **Whole-page threshold.** A whole page is faster on vector pages up to
  ~1200 px, but its peak grows with its area: at 600 px (1.8 MiB) it is
  ~1.5× a 512 px tile's (transparency 12.7 vs 8.6 MiB), at 1200 px (7 MiB)
  5×. Chosen: **one bitmap up to 2 MiB**, tiles above.

**The reservation against these peaks** (`pdfprobe-estimate.json`, final
`MediaService::pdfEstimate`, using the hints `pdfInfo` reads from each
page's resources):

| Document | Page px | Bitmap | Worst peak MiB | Estimate MiB | Estimate ÷ peak |
|---|---:|---|---:|---:|---:|
| text | 300 | whole page | 1.6 | 3.1 | 1.95 |
| text | 300 | 512 px tile | 1.6 | 3.1 | 1.92 |
| text | 600 | whole page | 3.5 | 6.4 | 1.85 |
| text | 600 | 512 px tile | 2.3 | 4.5 | 2.00 |
| text | 1200 | whole page | 9.2 | 19.8 | 2.14 |
| text | 1200 | 512 px tile | 2.8 | 4.5 | 1.60 |
| text | 2000 | whole page | 22.4 | 51.4 | 2.29 |
| text | 2000 | 512 px tile | 2.6 | 4.5 | 1.76 |
| text | 4000 | whole page | 83.9 | 199.5 | 2.38 |
| text | 4000 | 512 px tile | 2.4 | 4.5 | 1.88 |
| scanned | 300 | whole page | 7.8 | 10.3 | 1.33 |
| scanned | 300 | 512 px tile | 8.2 | 10.3 | 1.25 |
| scanned | 600 | whole page | 10.5 | 17.3 | 1.64 |
| scanned | 600 | 512 px tile | 10.5 | 15.3 | 1.45 |
| scanned | 1200 | whole page | 35.5 | 45.0 | 1.27 |
| scanned | 1200 | 512 px tile | 30.1 | 29.8 | 0.99 |
| scanned | 2000 | whole page | 91.9 | 134.4 | 1.46 |
| scanned | 2000 | 512 px tile | 76.9 | 87.5 | 1.14 |
| scanned | 4000 | whole page | 151.1 | 282.5 | 1.87 |
| scanned | 4000 | 512 px tile | 73.2 | 87.5 | 1.20 |
| vector | 300 | whole page | 1.5 | 3.1 | 2.14 |
| vector | 300 | 512 px tile | 1.6 | 3.1 | 1.93 |
| vector | 600 | whole page | 3.3 | 6.4 | 1.95 |
| vector | 600 | 512 px tile | 2.2 | 4.5 | 2.03 |
| vector | 1200 | whole page | 10.4 | 19.8 | 1.90 |
| vector | 1200 | 512 px tile | 2.7 | 4.5 | 1.66 |
| vector | 2000 | whole page | 61.5 | 51.4 | 0.83 |
| vector | 2000 | 512 px tile | 2.7 | 4.5 | 1.65 |
| vector | 4000 | whole page | 265.1 | 199.5 | 0.75 |
| vector | 4000 | 512 px tile | 2.7 | 4.5 | 1.68 |
| transparency | 300 | whole page | 4.7 | 7.8 | 1.66 |
| transparency | 300 | 512 px tile | 4.7 | 7.8 | 1.67 |
| transparency | 600 | whole page | 11.3 | 25.1 | 2.23 |
| transparency | 600 | 512 px tile | 8.7 | 15.0 | 1.73 |
| transparency | 1200 | whole page | 55.1 | 94.4 | 1.71 |
| transparency | 1200 | 512 px tile | 9.1 | 15.0 | 1.65 |
| transparency | 2000 | whole page | 158.3 | 258.8 | 1.63 |
| transparency | 2000 | 512 px tile | 10.6 | 15.0 | 1.42 |
| transparency | 4000 | whole page | 633.5 | 1028.9 | 1.62 |
| transparency | 4000 | 512 px tile | 10.9 | 15.0 | 1.38 |

Every bitmap the viewer asks for (512 px tiles, whole pages ≤ 2 MiB) is
covered (0.99–2.14×). Whole vector pages of 2000 px and more are not
(0.75–0.83), and are never requested: they are above the threshold, and the
reduced-density fallback under pressure stays within 75 % of the budget.

### The viewer, the card and typing (M1 MacBook Air 8 GB, macOS 26.6.2, Release, 2026-11-06)

Raw files and provenance: `bench/results/pdf-2026-11-06/` (`pdf.json` is
the full run on the final code; `pdf-heavy-and-typing.json` the heavy
documents and typing beside a PDF, final code, window painted while
typing; `pdf-onscreen-frames.json` an earlier full run whose 200-page
scroll was painted on screen, used for frame timings only). Window
1180×759 pt at 2×, so the viewer fits a Letter page 2296×2971 px wide
(zoom 188 %, 30 tiles of 512 px per page). Budget 128 MiB (134.2 MB).
MB = 10⁶ bytes. All runs with `app_nap=0`.

| Measure | Result |
|---|---|
| Import, 5 PDFs (0.35–48 MB) | 154 ms in total, page geometry read on the import worker; `page_geom` is 21 bytes for each uniform fixture, 161 bytes for the 8-page mixed one |
| Card thumbnails, in-app render cost | text 8 ms / 0.4 MB peak, vector 103 ms / 1.0 MB, transparency 41 ms / 1.7 MB, scan 34 ms / 7.2 MB (cold). Reservation ÷ peak 1.40–15 |
| One viewer tile (512 px), in-app | text 2.6 ms / 1.1 MB, vector 252 ms / 1.1 MB, transparency 218 ms / 5.6 MB, scan 55 ms / 68.9 MB (cold). Reservation ÷ peak ≥ 1.33 |
| Typing 300 characters while a PDF view beside the note renders vector tiles at 3× | input → end of the editor's paint, in process (not pixels on the display: the backing-store flush, WindowServer composition and the display come after) p50 0.51 ms, p95 2.17 ms, p99 2.95 ms, max 3.68 ms (20 tiles rendered meanwhile). To the event-loop turn after the flush: see "Cache size" below. The editor painted 300 times (once per key) and 0 times for PDF results; the PDF view painted 20 times, once per tile, 0 times for keys |
| 200-page document, open at fit width | first viewport sharp in 249 ms |
| Scroll to the end (997 frames of 300 px) | 5,900 tile renders (p50 1.5 ms each), 70 dropped before starting, 3 discarded (2.9 ms of CPU), 0 budget waits. Media cache max 130.9 MB, media use max 134.2 MB |
| Footprint during that scroll | 57.9 MB before opening; sampled max 248.8 MB (rusage peak 263.7 MB), end 234.7 MB; a second pass: max 249.8 MB. A plateau at the baseline plus the budget plus ~60 MB more; it does not grow with pages visited. Of those ~60 MB, ~28 MB is the window's backing store growing from one IOSurface buffer to three, and 13.6 MB the bench's own offscreen canvas (see "Cache size" below) |
| Straight back up | 125 cache hits, 5,808 renders (hit rate 0.02): 200 pages of 27 MB tiles do not fit 128 MiB, only the last ~5 pages hit |
| Scroll frames, on screen (`pdf-onscreen-frames.json`) | intervals between presented viewer paints (display-paced, 60 Hz): down p50 16.67 ms, p95 16.73 ms, p99 16.74 ms, max 16.76 ms, 0 of 997 over 25 ms; up p50 16.67 ms, p99 16.76 ms, 1 of 927 over 25 ms (32.96 ms). The paint work per frame is far smaller: p50 0.2 ms, p95 2.5 ms, p99 3.0 ms in a later, fully painted 997-frame pass of the same scroll (see "Cache size" below). **Correction:** the "viewer paint p50 6.4 ms, p95 8.7 ms" first given here were the memory-pressure phase's 150 frames, the only paint samples the scenario kept |
| Zoom in and out, 6 gestures of 12 steps around page 100 | 0 renders started during the gestures (99 queued tiles dropped as each began); 24 renders after they settled, 108 cache hits (alternate zooms reuse); sharp p50 150 ms, max 240 ms after the last step (the settle is 150 ms); gesture frames p50 16.66 ms, max 16.72 ms |
| 4× zoom, then 60 scroll frames | 444 tiles, 0 dropped or discarded; first viewport 248 ms |
| Close | footprint 220.9 → 90.4 MB, media cache 130.5 MB → 0, documents open 0. Not the viewer's: of the 32 MB above the baseline, the window keeps its extra IOSurface buffers and the allocator the bench's freed canvas (see "Cache size" below) |
| Scan (24 pages, 300 dpi) | first viewport 453 ms; a fling through all pages: 32 renders, 1 discarded (22 ms of CPU), 18 dropped; last viewport 599 ms; tiles p50 31 ms |
| Vector (16 pages, 3000 curves each) | first viewport 3.06 s (tiles p50 161 ms, max 370 ms); fling: 32 renders, 1 discarded (352 ms), last viewport 2.85 s |
| Transparency (16 pages, nested groups) | first viewport 1.54 s (tiles p50 99 ms); fling: 32 renders, 1 discarded (148 ms), last viewport 1.38 s |
| Critical pressure with the viewer open | cache trimmed to 0, footprint 123.3 → 95.9 MB, budget 33.5 MB for 60 s; 150 more scroll frames: 915 tiles at full density (a tile's reservation fits 75 % of the lowered budget), 0 waits, media use max 33.3 MB, last viewport 118 ms |
| Idle, text page (baseline) | 0.15 % of a core, 5.9 wakeups/s (`pdf-onscreen-frames.json`); median 0.10 %, 6 wakeups/s (`pdf.json`, whose mean of 0.44 % comes from one 2-second burst, at seconds 14–15) |
| Idle, viewer open and settled | 0.14 % (median 0.14 %), 5.5 wakeups/s (median 5), 11 threads; 0.17 % and 0.14 % in the two other full runs. No bursts in any run |
| Idle, after closing | median 0.15 %, 5 wakeups/s (`pdf.json`; mean 1.10 % from one burst of 14.6 %, 3.1 % and 2.0 % at seconds 16–18); 0.19 % mean in `pdf-onscreen-frames.json` |

- **Thrown-away renders during a fling.** A render cannot be interrupted,
  so a tile started while the view flies past is wasted. Before the
  viewer deferred heavy pages, a fling through the scan discarded 57 of
  89 renders (1.3 s of CPU) and one through the vector document 30 renders
  (8.7 s). With heavy pages (layers, images of 1 MP or more, content
  streams over 64 KB) requested only once the view slows below a quarter
  viewport per 100 ms for 150 ms: 1 per document. Text pages are never
  deferred: the 200-page scroll keeps up at 16.7 ms frames.
- **Idle bursts.** The one-off bursts above (about 70 ms and 200 ms of
  CPU) appeared on the text page before any PDF was opened, too; with the
  viewer open and settled no run had any. Their cause is not identified;
  the bench window was covered for most of `pdf.json` (see below), and a
  covered window that the system paints again redraws the note, whose card
  thumbnails were trimmed by the pressure phase.
- **The window was covered** for most of `pdf.json` and the heavy-document
  phases of the other runs (`window_exposed_and_active_ratio` 0; see the
  `*_unpainted_frames` keys). Those frames were painted offscreen through
  the same code: counts and memory are valid, their timings are not and
  are left out. In the first full run, whose heavy-document phase the
  system did not paint, transparency tiles rendered ~7× slower than in
  every run where it was painted; `--bench-arg app_nap=0` (now used) holds a
  user-initiated activity so a covered bench window is not throttled.

### Cache size, frame and input timing, repeated close (same machine, 2026-11-06, later)

Raw files and provenance: `bench/results/pdf-cache-2026-11-06/`; compare
runs with `bench/scripts/pdf_cache_compare.py` and break footprints down with
`bench/scripts/footprint_categories.py`. New in the scenario:
`media_budget_mib`/`media_cache_mib`, the share of the visible page area
each frame leaves white or scaled (`*_blank_permille`, `*_scaled_permille`,
painted frames only), the PDF worker's own CPU per phase (`*_render_cpu_s`),
`footprint -p` reports at each checkpoint, a warning-level pressure step,
and phase 8: open → scroll → close of the 200-page document and the scan,
12 times.

**The cache is now capped at 64 MiB inside the 128 MiB budget.** The
clean comparison is the scroll-only pair (`kinds_only=long`, both runs
painted on screen throughout, 2,991 frames each):

| 200-page document at fit width | Budget 128, no cap (before) | Budget 128, cache 64 (now) |
|---|---:|---:|
| Footprint during the scroll, sampled max / rusage peak | 245.7 / 249.5 MB | 187.9 / 201.5 MB |
| Second pass, sampled max | 236.4 MB | 188.1 MB |
| `footprint -p` at the plateau, without the window's IOSurfaces | 164.6 MB | 114.3 MB |
| Media cache max | 131.2 MB | 67.1 MB |
| Renders down / up (cache hits up) | 5,938 / 5,855 (129) | 5,970 / 5,926 (59) |
| PDF worker CPU down / up | 4.59 / 4.52 s | 4.44 / 4.39 s |
| Frames with any white area, down / up / second pass | 20.8 / 18.3 / 19.9 % | 0 / 0 / 0 % |
| Longest run of frames with white area | 16 frames (~270 ms) | 0 |
| Presented intervals down: p99, max, over 25 ms | 16.73, 33.35 ms, 3 | 16.73, 16.79 ms, 0 |
| Viewer paint work down p50 / p95 / p99 | 0.2 / 2.2 / 2.5 ms | 2.0 / 2.4 / 2.6 ms |
| Zoom, 6 gestures: renders / hits, settle → sharp p50 / max | 24 / 108, 150 / 250 ms | 78 / 148, 167 / 283 ms |

- **Why a smaller cache costs no rendering here.** A linear pass through
  200 pages of 27 MB misses the cache either way (hit rate 0.02 at
  128 MiB); only revisits hit: scrolling straight back up found 129 tiles
  at 128 MiB and 59 at 64 MiB. 70 tiles at ~1.5 ms is ~0.1 s, inside the
  run-to-run noise of the worker CPU above. Zoomed back and forth,
  one zoom level's tiles no longer stay beside the other's: 54 more
  renders, 0.06 s more worker CPU, and 17 ms more to sharp at p50.
- **Why the cap ended the white strips.** Prefetch (one viewport ahead)
  runs only while the budget is under half used. A full 128 MiB cache kept
  it off for the rest of any long scroll, so each frame's incoming strip
  was white until rendered. With cached pixels capped at half the budget,
  the worker prefetches whenever it is idle. This depends on the cache
  sitting at or just under the cap. Prefetch stops if the cache is exactly
  full (`used >= limit / 2`).
- **Why the cap, not a smaller budget.** One render may reserve at most 75 %
  of the budget, and a 300 dpi scan's tile reserves ~87 MB. With the whole
  budget at 64 or 48 MiB, scans rendered as whole pages at about half
  density (`scanned_plan` 1197×1549, `scanned_density` 0.52) and photos
  come close (46 of 48 MB). With the cap, scans, vector and transparency
  documents keep 512 px tiles at full density (`pdf-cache64.json`). The
  image page (`images-*.json`) makes the same 29 decodes on the way down,
  at the same 1520×1140, with the cache max 101.7 → 67.0 MB and the peak 221.8 →
  210.9 MB.
- **Reduced-density scans grow the footprint (CoreGraphics).** With the
  budget at 64 MiB, the footprint after each close grew by ~17 MB a cycle
  (136 → 321 MB in 12 cycles; `heap-budget64.*`: 16 live blocks of 8.24 MB,
  132 MB, in `DefaultPurgeableMallocZone` after 6 cycles, non-volatile, kept
  after the document closed). 8.24 MB is the scan's JPEG decoded at half
  size. At 48 MiB (quarter size) it grew 126 → 134 MB, at 128 MiB and with
  the cap not at all. The default configuration never draws scans this way
  except during a pressure cool-down (budget ÷ 4). Cause and fix below.
- **Why: CoreGraphics caches the decode in a recycled block that cannot be
  purgeable.** Raw files: `bench/results/pdf-scan-pin-2026-11-06/`. Drawn
  whole at 0.35–0.5 of its native width (900–1274 px of 2550), the scan's
  JPEG is decoded at half size on ImageIO's hardware path
  (`AppleJPEGReadPlugin::createImageBlockSetWithHardwareDecode`), which
  scales it with vImage through an 8.24 MB malloc buffer, freed at once into
  malloc's large-block cache. CoreGraphics' process-wide image cache
  (`get_cache.image_cache`, filled by `ripc_AcquireRIPImageData`) then keeps
  the decode in an 8.24 MB block of `DefaultPurgeableMallocZone`, and malloc
  serves it from that recycled chunk. `vmmap` shows the block without
  `PURGE=V`, so marking it purgeable does nothing. The entry outlives its
  image and document (`leaks --traceTree` finds only `image_cache` holding
  it) and goes only on memory pressure. A CoreGraphics-only probe
  (`repro/cgprobe.mm`, no Qt) shows the same: +8.5 MB per open → draw →
  close at 900–1274 px, 1.6 → 273 MB in 30 cycles. It stays flat at every
  other width, in 512 px tiles at every width, and with `MallocLargeCache=0`
  (the cache entries are then `PURGE=V`; `probe-sweeps.txt`).
- **Fixed: the lower density is tiled too.** `PdfView::planFor` applies one
  rule at every density: one bitmap only up to 2 MiB, otherwise 512 px tiles.
  At 64 MiB the scan keeps its density (0.52), now as `tiles 1197x1549`. The
  footprint after each close of the scan over 12 cycles was 153.9 → 341.6 MB
  before (`before-budget64.json`). It is now 115.0, then 123.3–128.1 MB
  (`after-budget64.json`). In `footprint -p` after the cycles, MALLOC_LARGE
  went 237 → 21 MB, and the purgeable zone went from 28 × 8.24 MB
  (non-volatile) to 28 × 2.5 MB (volatile). At 128 MiB (full-density tiles,
  unchanged) it was 83.6–100.2 MB; the steps are one more 14.7 MB window
  IOSurface. At 48 MiB the scan now gets the same `tiles 1197x1549`, where it
  used to get whole pages at about a quarter density, and the footprint
  after each close is 82.5–85.6 MB (`after-budget48.json`). The cost: a page at that density is 12
  tiles instead of one bitmap, ~1.6× the CPU in the probe (28 → 45 ms per
  page at 1197 px, measured beside another run). It only applies while one
  of the scan's tiles does not fit the budget.
- **600 dpi scans: 10–15 MB per page drawn in the probe, whole or tiled**
  (fixed below, by the allocator setting, not in Folia's code).
  For each page, CoreGraphics copies the 8 MB JPEG stream into a CFData that
  `realloc` grows to 16 MB (`CGDataProviderCopyData` under
  `CGImageSourceCreateWithDataProvider`). Once freed, the chunk stays dirty
  in malloc's large cache, and the next page does not reuse it. Plain C does
  the same without CoreGraphics (`repro/realloc.c`): growing a buffer by
  doubling `realloc` to 8.3 MB and freeing it, 120 times, raises the
  footprint to ~520 MB, where it levels off. With `MallocLargeCache=0` it
  stays at 0.9 MB, and a plain 8.3 MB `malloc` stays at 9 MB. Folia cannot
  reach those reallocs; the 300 dpi fixtures (2 MB streams) never trigger
  it. Launching with `MallocLargeCache=0` ends both effects in the probes.
- **Fixed: Folia runs without malloc's large cache.** Raw files and
  provenance: `bench/results/malloc-large-cache-2026-11-06/`. On this 8 GB
  M1 every `malloc` above ~32 KB is a "large" block, and the cache keeps
  freed ones dirty and in the footprint; neither `malloc_zone_pressure_relief`
  nor libmalloc's pressure-event handler empties it, so the only lever is
  `MallocLargeCache=0`, which libmalloc reads at process start. `main()`
  therefore sets it and re-executes the binary (same pid) when it is unset,
  covering every launch (Info.plist's `LSEnvironment` would cover only
  LaunchServices launches). `MallocLargeCache=1` opts out; setting it at all
  skips the exec (debuggers stop at an exec). Bench JSON records it as
  `env.malloc_large_cache`. A 600 dpi scan (`makescan` 5100×6600, 6 pages)
  opened, scrolled and closed 12 times with the 200-page document, two runs
  per mode:

  | After each close of the scan | Cache on | Cache off |
  |---|---:|---:|
  | Window 1180 pt (tiles 2296×2971) | 117.8–221.2 MB (medians 121.8, 132.4) | 72.7–74.7 MB |
  | Window 640 pt (`window_width=640`, tiles 1216×1574) | 94.8–113.1 MB (medians 101.8, 103.3) | 52.7–61.2 MB |
  | After importing the five PDFs, before opening the scan | 78.7–99.0 MB | 50.5–70.7 MB |
  | `MALLOC_LARGE` dirty after the cycles (`footprint -p`) | 40–50 MB | 0 |

  In the app the scan does not keep growing per page as in the probe: other
  allocations reuse the cached chunks, so the cache sits at 40–50 MB and
  swings (up to +90 MB between closes in one run). After closing the
  200-page document: 86.8 against 102.2 MB (3 runs each).
  The cost, same machine, modes interleaved (load 2–25 and a pressure
  warning for most runs, so in-process measures only):

  | | Cache on | Cache off |
  |---|---:|---:|
  | PDF worker CPU, 200-page scroll down / up | 7.81 / 9.30 s | 7.22 / 8.87 s |
  | Zoom settle → sharp p50 / p95 | 108 / 217 ms | 107 / 217 ms |
  | Image scroll decodes (54 a run), wall p50 (CPU p50), 2 pairs | 64.6–65.5 (41.2–41.4) ms | 66.4–67.0 (42.0–42.9) ms |
  | Typing, input → end of editor paint p50 / p95 | 2.07 / 8.77 ms | 2.27 / 10.06 ms |
  | Editor scroll, frame work p50 / p95 | 6.55–6.59 / 7.87–7.88 ms | 6.56–6.61 / 7.93 ms |
  | Open a page, uncached → editable p50 | 12.5 ms | 12.3 ms |
  | Launch, process start → editable (median of 5) | 414 ms | 409 ms; 451 ms with the re-exec |

  Typing paid because Qt's raster engine allocated ~41 KB of clip spans
  (`QClipData::initialize`) about 21 times per keystroke's paint: recycled
  blocks with the cache, fresh pages without (`typing-large-allocations.txt`).
  Fixed since (next item): typing now costs the same with the cache off.
  Image decodes after a critical
  pressure trim were 2–2.7× slower in the two runs whose window was covered
  (App Nap; the images scenario has no `app_nap=0`), 0–13 % when painted.
  Decided: the lower footprint after document work (15–59 MB, up to 48 %,
  and no swings) outweighs +0.2 ms per keystroke at p50, 2–4 % per image
  decode and ~40 ms at launch.
- **Fixed: typing no longer rebuilds the clip per text item.** Raw files and
  provenance: `bench/results/painter-clip-2026-11-08/`. QTextEdit clips the
  painter to the paint event's bounding rect (`QWidgetTextControl::drawContents`).
  On a 2× screen `QPainterPrivate::drawTextItem` turns antialiasing on and
  off around every text item, and each change makes the raster engine drop
  the clip and replay it (`renderHintsChanged` → `updateClipping`): a new
  `QClipData`, whose spans the first clipped glyph allocates again. About 1
  in 9 keystroke paints covers the edited line plus a 17 pt strip at the
  viewport's right edge, where the transient scroll bar repaints after a
  height change. Its bounding rect is the whole viewport, so every visible
  block is drawn under a two-rect clip, and each of ~185 text items allocates
  41.7 KB of spans (2 rects × 1,302 rows × 16 B). Since the paint system
  already clips to the update region, `NoteEditor::paintEvent` marks its
  viewport paint and `VirtualLayout::draw` paints with painter clipping off
  (`VirtualLayout::setRedundantClip`); the system clip is built once per
  paint. A test renders the editor through a two-rect region at 2× both
  ways: identical pixels, none outside the region.
  `--bench-arg painter_clip=1` keeps Qt's clip, for A/B runs. Allocations of
  32 KB or more per keystroke (1000- minus 200-key run, interposer): 21.1 →
  0.28. Typing, 1000 keys, modes interleaved, 3–4 runs each:

  | ms, p50 / p95 | Cache on | Cache off |
  |---|---:|---:|
  | Input → end of editor paint, before (`painter_clip=1`) | 2.15 / 8.74 | 2.13 / 9.93 |
  | Input → end of editor paint, now | 2.16 / 8.06 | 2.08 / 7.98 |
  | Editor paint, before / now | 0.47 / 5.77, 0.46 / 5.17 | 0.46 / 7.14, 0.45 / 5.11 |

  With the cache off, typing is now as fast as with it, and p95 is below the
  old cache-on figure (8.74 here, 8.77 on 2026-11-06).
- **Repeated close is flat.** With the cap, the footprint after each close
  of the 200-page document over 12 cycles: 109, 121, 122, 125, then 121–122;
  after the scan: 106–108 (one 121). The allocator returns nothing more
  (`malloc_zone_pressure_relief` frees 0 B at every checkpoint).
- **What stays after closing is mostly not the viewer's.** `footprint -p`
  before opening and after closing: the tiles (untagged mappings) go back
  to 0.4 MB. The window's IOSurfaces grow from one 14.7 MB buffer to three
  once the view updates every frame, and Qt never releases the third
  (docs/RELEASE_REPORT.md). The bench's offscreen canvas (13.6 MB, only
  when the system does not paint the window) stays in `MALLOC_LARGE` after
  it is freed. Without the IOSurfaces, after closing: 34.6–49.3 MB in the
  painted pair against 47–48 MB before opening.
- **Timing endpoints.** "16.7 ms frames" are intervals between presented
  viewer paints: the scenario steps once per frame, waits for the next
  paint and steps again, so a frame over 25 ms is a missed one at 60 Hz.
  The paint work itself is the much smaller `*_view_paint_us`. Typing beside
  a rendering PDF (`pdf-budget128.json`, the editor painted 300 times):
  key event → end of the editor's paint p50 0.52 ms, p95 2.06 ms, p99
  3.30 ms, max 5.66 ms; key event → the event-loop turn after the
  backing-store flush p50 1.45 ms, p95 3.95 ms, p99 5.00 ms, max 5.99 ms.
  Both are in-process endpoints. WindowServer composition and the display's
  refresh come after, typically another frame or more.
- **Pressure.** A warning trims the cache to the 16 MiB floor and halves
  the budget: 27.3 → 16.4 MB cached, footprint −11 MB. Critical behaves as
  before (cache 0, budget 33.6 MB, 915 tiles at full density, 0 waits).
- **Painting.** In the full runs (`pdf-budget*.json`, `pdf-cache64.json`)
  the system painted the window only in some phases. Those runs give
  counts, memory and density. Their timings and white-area figures are
  limited to the frames each run painted (`*_unpainted_frames`). The two
  `scroll-*.json` runs were painted throughout.

## 6c. Split view (the note and a PDF side by side)

`--bench split --bench-arg pdf_dir=<dir>` (fixtures from `folia_mediaprobe
--make-pdf <dir>`), in `src/app/BenchSplit.cpp`: idle with one tab, the
200-page document opened beside a note, idle with two tabs, the divider
dragged at frame rate (`drag_steps` moves of `step_px`, right and back,
16 ms apart) with the viewer's resize settle and without it, typing beside
the open PDF, and closed. Raw files: `bench/results/split-2026-11-06/`.

### Measured (M1 MacBook Air 8 GB, macOS 26.6.2, Release, on battery, 2026-11-06)

Window 1180×780 pt at 2×, default layout; each pane 457 pt wide. Runs whose
window the system did not paint (0 or few paints) were set aside
(`PROVENANCE.md`).

| Divider dragged 120 steps (3.2 s) | Renders on settle (shipped) | Every step renders (off) |
|---|---|---|
| PDF renders started (during + after release) | 5 + 8 | 239 + 1 |
| Jobs dropped queued / discarded after rendering | 0 / 0 | 16 / 8 |
| PDF worker CPU | 0.018 s | 0.23 s |
| Process CPU over the drag | 0.81 s | 1.58 s |
| Peak footprint (lifetime peak after the phase) | 107 MB | 183 MB |
| Main-thread time per divider step, p50 / p90 | 0.21 / 0.27 ms | 0.20 / 0.27 ms |

The 5 renders during the settled drag are whole pages drawn while the
narrowing PDF pane sat at its 260 pt minimum: its width had stopped changing
for 150 ms, though the mouse kept moving.

| Note of 50,000 lines beside the PDF | Per-step estimates (before) | Estimates once settled (shipped) |
|---|---|---|
| Main-thread time per divider step, p50 / p90 / max | 32.5 / 35.9 / 69.0 ms | 0.17 / 0.27 / 0.96 ms |
| Process CPU over the drag | 3.08 s | 0.74 s |

| Other measures (100-line note) | Result |
|---|---|
| Open the PDF beside the note → fully drawn viewport | 107–108 ms |
| Footprint: one tab → two tabs → closed | 70.6 → 78.3 → 74.8 MB (5.6 MB of tiles while open) |
| Idle CPU / wakeups: one tab, two tabs, closed | 0.10 / 0.13 / 0.08 % of a core; 4.6 / 4.5 / 4.2 per s |
| Typing 200 keys beside the open PDF | 0 viewer paints, 0 PDF renders; input to paint p50 0.68 ms |

## 6d. Note tabs

`--bench tabs` (`src/app/BenchTabs.cpp`; arguments in its header comment)
creates 30 ordinary pages (100 lines each, with formulas and code), opens 29
of them in background tabs, idles, switches through all tabs 150 times with
`tab.next` (round-robin, so with 30 tabs and a 16-page document cache every
switch loads its page), types beside the strip, resizes the window 60 times
by 8 pt and closes all tabs but one. `strip=0` runs the same with the
default layout minus its `ui.tabs{}` line: the tabs exist, nothing draws
them. `python3 bench/scripts/tabs_summary.py <json…>` prints the table.

### Measured (M1 MacBook Air 8 GB, macOS 26.6.2, Release, on battery, 2026-11-09)

Raw runs and provenance: `bench/results/tabs-2026-11-09/`. Two runs with
the strip, two without; ranges cover all four unless a column is named.

| Measure | With the strip | Without (`strip=0`) |
|---|---|---|
| Open a background tab (main thread), median / p95 | 13–26 / 31–42 µs | 15 / 31–57 µs |
| Shell renders and pages loaded while opening 29 background tabs | 0 and 0 | 0 and 0 |
| Footprint change from opening 29 background tabs | +0.00–0.03 MB | +0.00–0.02 MB |
| Idle, 1 tab → 30 tabs: footprint | 73.5 → 73.4 MB | 73.4 → 73.4 MB |
| Idle, 1 tab → 30 tabs: CPU | 0.10–0.14 → 0.07–0.08 % of a core | 0.08–0.09 → 0.06 % |
| Idle, 1 tab → 30 tabs: wakeups | 4.7 → 4.2–4.3 per s | 4.6–4.7 → 4.2–4.3 per s |
| Shell renders per switch | 1.0 | 1.0 |
| Switch to an uncached tab: page active (`open_uncached_activate_us`), median / p95 | 2.3–4.5 / 7.1–7.7 ms | 4.2–5.1 / 9.8–11.0 ms |
| Shell render per switch, median | 0.31–0.77 ms | 0.73–0.82 ms |
| Footprint after each round of 30 switches | 75.8–78.6 MB, last round 76.9 / 92.4 MB | 75.7–75.9 MB, last round 75.9 / 78.1 MB |
| Document cache after 150 switches | 16 pages (its cap) | 16 pages |
| Strip paints while typing 200 keys in the note | 0 and 1 | — |
| Window resize step (whole window, synchronous repaint), median | 6.2–16.8 ms | 16.7 ms |
| All but one tab closed: footprint, pages cached | 77.0–77.9 MB, 1 page | 89.9–90.9 MB, 1 page |

Reading it:

* **Background tabs cost nothing measurable**: no page load, no shell
  render, no footprint, and idle with 30 tabs matches idle with one (the
  first idle window starts closer to the page creation, hence its slightly
  higher CPU in every variant).
* **A switch costs what opening a page costs**: one shell render and, for a
  page outside the 16-page cache, a load from SQLite and a session build
  (0.2–0.8 ms median). The strip's own share of the render is within the
  run-to-run spread (the second run with the strip had its window painted
  for most switches and ran faster in every measure, the strip included).
* **The strip adds no paints while typing** (0 and 1, against 200 keys).
* **Paint-based numbers are partial.** macOS painted the click-through bench
  window for 17 and 117 of the 150 switches with the strip, and for 0 and
  13 without, so `tab_switch_to_paint_*` and `switch_strip_paints_per_switch`
  describe only those. `tab_switch_to_editable_*` (16.7 ms median
  everywhere) is bounded by the harness's own wakeup granularity, not by the
  app; `open_uncached_activate_us` is the app's own measurement of the switch.
* The resize step is dominated by the synchronous repaint of the whole
  window (one frame, 16.7 ms); the faster run is the painted one.
* The footprints after closing differ by variant more than within one
  (77–78 against 90–91 MB). Every closed page was freed (one page left in the
  cache).

## 6e. Uploads (Word, Markdown and text)

Two probes:

* `folia_docprobe <file> [--print] [--whole]` (`tools/docprobe.cpp`)
  converts one file in a fresh process and prints its blocks, time and peak
  footprint growth; `--whole` inflates a Word file's `document.xml` whole
  and parses it, to show what streaming saves.
* `--bench import` (`src/app/BenchImport.cpp`) uploads a long Markdown file
  (8 MiB, 83,057 lines) and a long Word file (40,000 paragraphs, written by
  `fixtures::makeUploadSet`), each through `AppController::uploadFiles` as
  the dialog does, and measures time to an editable tab, worker and
  main-thread time, peak footprint growth, idle with the page shown, and
  the footprint after its tab is closed.

### Measured (M1 MacBook Air 8 GB, macOS 26.6.2, Release, on battery, 2026-11-09)

Raw runs and provenance: `bench/results/import-2026-11-09/`; two runs, both
listed.

| Measure | Markdown, 8 MiB (83,057 lines) | Word, 40,000 paragraphs (40,800 lines) |
|---|---|---|
| Upload → the new tab's page editable | 1145 / 1106 ms | 968 / 919 ms |
| Conversion on the import worker | 210 / 212 ms | 560 / 516 ms |
| Main thread: create the page, queue its save | 1.1 / 0.8 ms | 0.4 / 1.2 ms |
| Building the editor's document for the page (any page that size) | 511 / 504 ms | 143 / 139 ms |
| Peak footprint growth, upload → page shown | +183 / +154 MB | +51 / +51 MB |
| Idle with the page shown: footprint | 174 / 144 MB | 118 / 115 MB |
| Idle with the page shown: CPU, wakeups | 0.14 / 0.10 %, 4.4 / 4.5 per s | 0.07 / 0.08 %, 4.3 / 4.3 per s |
| Its tab closed: sessions left, footprint 5–20 s later (not freed: see below, §6f) | 1 (its own, still active); 174–177 / 144–175 MB | 1 (its own); 115–118 MB |

`folia_docprobe` on a Word file written by TextEdit (330 KB, `document.xml`
42 MB inflated, 40,800 paragraphs): **1.26–1.28 s, +19–21 MB peak**
streamed (most of it the 4.4 M characters of the page), against
**+277 MB** to inflate `document.xml` whole and parse it (0.87–0.90 s,
producing nothing). Three runs each.

Reading it:

* **The import itself is cheap on the main thread** (about 1 ms); the
  worker's conversion runs beside typing. What the user waits for after
  that is the editor building the page, the same cost as opening any page
  of that size (the README's 10 MiB stress note).
* **Streaming Word files bounds memory by the output**, not the document
  (19 MB against 277 MB on the same file).
* **In these runs, closing the upload's tab did not free its page.** The
  bench's fresh home left the first tab without a page, and closing a tab
  beside an empty one kept the closed page shown and alive: the one session
  left was its own (`heap` still listed its 83,057 lines after the close,
  §6f), and `malloc_zone_pressure_relief` had nothing to hand back. That was
  a bug in closing tabs, fixed since: the editor now shows no page there and
  the closed page leaves the cache, an upload into an empty workspace fills
  the empty tab, and a closed page whose neighbour was still loading is
  freed once that page is shown. The bench now gives the first tab a page of
  its own. With it, closing the tab frees the page, and the footprint is
  back to about 75 MB within 3 s with the allocator Folia ships (§6f). The
  31 MB rise in one run was the window's IOSurface buffers, not malloc. The
  closed-tab row above predates the fix.

## 6f. Allocator settings, and closing a large page

§6e left a question: after an 8 MiB Markdown upload is shown and its tab
closed, the footprint stayed at 144–177 MB, and
`malloc_zone_pressure_relief` returned 0 bytes. Would a different macOS
allocator setting give that memory back? It turned out the memory was still
in use, and once the page is actually freed, the allocator Folia already
ships returns it within 3 s. The settings below were measured anyway, for
what they change beyond that.

Raw runs, scripts and provenance: `bench/results/malloc-env-2026-11-09/`;
compare with `python3 -I bench/scripts/malloc_env_compare.py <files>`.

### Which allocator, and what each setting changes

macOS 26.6.2 uses **xzone malloc** by default, not the older magazine
allocator: `MallocDebugReport=stderr MallocReportConfig=1 /usr/bin/true`
prints `Enable Xzone Malloc: 1`, `Xzone Enablement Reason: default`. The
names of the variables libmalloc reads were taken from the strings in the
dyld shared cache; `man malloc` documents only the debugging ones. The
report, with `MallocNanoZone` unset (as a Finder or Dock launch has it):

| Launched with | What changes in xzone's configuration |
|---|---|
| nothing set, the system default (`sys` below) | defer tiny/small/large frees, huge cache of 64, thread caching on |
| `MallocLargeCache=0` (Folia's re-exec, `base` below) | no deferred frees, huge cache 0 |
| `MallocNanoZone=0` (`nano`) | thread caching off, small thrash threshold 1 → 0 ms. xzone does not use the nano zone at all (`Enable Nano On Xzone: 0`) |
| `MallocAggressiveMadvise=1` (`aggr`) | nothing (a magazine-allocator setting) |
| `MallocSpaceEfficient=1` (`space`) | what `MallocLargeCache=0` does, plus segment deallocation, no tiny/small batching, no small freelist, one slot and list per size class instead of per CPU cluster, thread caching off |

`MallocXzoneSegmentDeallocate=1`, `MallocXzoneHugeCacheSize=0` and
`MallocSecureAllocator=0` change nothing in the report on their own.

**The Claude Code app exports `MallocNanoZone=0`** to the processes it
starts, so bench runs launched from a Claude session before this one ran
with xzone's thread caching off, unlike a Finder or Dock launch. The runs
here unset it for every setting except `nano`; `env.malloc_env` now records
the allocator variables of every run.

### The memory after the close was in use

`--bench import --bench-arg vm_snapshots=2` writes `heap -s` with the page
shown, 3 s after its tab is closed and 20 s after the relief. In §6e's setup
all three listed the same 677,390 ± 7 live blocks (126.8 MB), among them
**83,057 `folia::BlockData`**, one per line of the imported page, and its
text (2 × 16 MB and 8 MB). The page's session was alive: the "1 session
left" in §6e was the imported page's own. The Word page behaved the same.
Its 40,800 lines stayed live after its close, and the Markdown page's went
only when the Word page pushed it out of the document cache.

The cause is in the bench's setup. A fresh `--home` shows no page, so its
first tab's page is 0 (the import JSON now records
`<kind>_closed_current_tab_page`, `_closed_active_is_imported` and
`_closed_imported_session_alive`). Closing the upload's tab then opens
nothing (`AppController::closeTab` reopens only a page id above 0), the
closed page stays the active one, and `evictIfUnused` skips it
(`s == m_active`). Users could reach that state too: in an empty workspace
(its one tab has no page), and whenever the next tab's page loads
asynchronously, so the closed page is still active when `evictIfUnused`
runs. Both are fixed in `closeTab` since (§6e). The bench now first creates
a small page in the first tab (these runs: `first_page=1`; the option is
gone, as an upload now fills an empty first tab, and `first_page=0` was
§6e's setup). Closing the upload's tab then frees the page, and the
footprint comes back:

| 8 MiB Markdown page, shipped allocator (`base`, 3 runs) | Footprint (MB) | malloc dirty (MB) |
|---|---:|---:|
| Before the upload | 66.4–66.9 | |
| Page shown | 173.4–175.6 | 122.2–124.5 |
| 3 s after its tab is closed (page freed) | 74.3–76.3 | 23.6–25.9 |
| Same, §6e's setup (`import0-base-1`, `first_page=0`: the page still alive and active) | 176.2 | 125.6 |

`malloc_zone_pressure_relief` hands back 0 bytes in every run with every
setting, before and after the page is freed: there is nothing for it to
release. The tens of MB the footprint jumped by in §6e, and in one run here
(the Word page closed at 91.9 MB instead of 75), are the window's IOSurface
buffers (14.7 MB each; 1 → 3 buffers when macOS repaints a window, see
§6b), not malloc: the malloc figure moved by at most 2 MB. All the A/B runs
below had 3 buffers (43.0 MB) in every snapshot except two (that `base` run
and one `sys` run, both after the Word page's close).

### The settings, measured

Interleaved runs (`repro/matrix.sh`); medians, MB = 10⁶ bytes. Import: fresh
home, `--bench import --bench-arg vm_snapshots=1`. Idle: 60 s on the
ordinary page of the 50,000-page fixture. Typing: 1000 edits on the same
page, offscreen (`QT_QPA_PLATFORM=offscreen`, 5 runs each), because macOS
did not paint the floating window for 15 of the 19 windowed typing runs
(`unpainted/`; an alternating `nano`/`base` check showed it was the screen,
not the setting).

| Setting (import / idle runs) | Markdown 8 MiB: shown → 3 s after close | Word 40,000 ¶: shown → 3 s after close | malloc dirty after the Markdown close | Idle footprint (malloc dirty) | Idle CPU, wakeups | Typing CPU per edit, offscreen |
|---|---|---|---:|---|---|---|
| `base`, what ships (3 / 3) | 173.8 → 74.6 | 112.4 → 75.0 | 23.6 | 81.5 (30.6) | 0.19 %, 5.3/s | 0.496 ms |
| `nano` (2 / 2) | 174.1 → 74.7 | 112.7 → 75.3 | 23.6 | 81.5 (30.6) | 0.17 %, 5.4/s | 0.510 ms (+2.8 %) |
| `aggr` (2 / 2) | 174.0 → 74.7 | 112.6 → 75.2 | 24.2 | 81.4 (30.6) | 0.13 %, 5.4/s | 0.499 ms |
| `space` (3 / 3) | 171.0 → 71.2 | 109.9 → 71.8 | 20.6 | 78.8 (27.4) | 0.20 %, 5.4/s | 0.509 ms (+2.6 %) |
| `sys`, system default (2 / –) | 178.0 → 77.7 | 114.4 → 104.2 | 27.1 | – | – | – |

Ranges: `base` closed 74.3–76.3 / 74.9–91.9 (the IOSurface run), idle
81.1–81.6; `space` closed 69.3–73.0 / 69.2–72.7, idle 78.4–79.9; offscreen
typing `base` 0.494–0.509, `nano` 0.499–0.541, `space` 0.502–0.553, `aggr`
0.496–0.501 ms; editor paint p50 0.275 ms in all four. Windowed typing, the
painted runs: `base` 5.04 / 5.32 ms per edit, input → paint p50 1.9 / 2.3 ms;
`aggr` 5.09 / 5.29 ms, 2.0 / 2.1 ms (README: 5.0–5.4 ms). The later
`free=1` phase (the page dropped by `reclaimMemory`) and the relief changed
nothing further (`base` 73.7 MB).

* **`space` (`MallocSpaceEfficient=1`) is the only setting that saves
  memory**: about 3 MB at every point (malloc 30.6 → 27.4 MB at idle,
  23.6 → 20.6 MB after a close), from packing small blocks into fewer
  slots. It costs some CPU: offscreen typing +2.6 % per edit (the runs'
  ranges overlap); the Word page's build 162 against 139 ms (149–181
  against 138–146), its upload → editable 1016 against 887 ms, the Markdown
  conversion on the import worker 208 against 191 ms. The Markdown page's
  build and upload → editable were not slower (517 / 1116 against 524 /
  1136 ms).
* **`nano` (`MallocNanoZone=0`)** changes no memory figure (thread caching
  off) and costs about as much CPU as `space`: +2.8 % per edit (ranges
  overlap), the Word page's build 151 ms.
* **`aggr` (`MallocAggressiveMadvise=1`)** matches `base` within noise, as
  its unchanged configuration predicts.
* **`sys` (`MallocLargeCache=1`)**: after the Word page's close, 97–111 MB
  (malloc 46 MB) for 5–20 s, falling to 77–92 MB. That is the large-block
  cache keeping freed blocks (§6b), which Folia's re-exec already avoids.

### Recommendation

**Keep what ships (`MallocLargeCache=0` through the re-exec) and adopt
none of the others.**

1. With the page freed, the shipped allocator already gives the memory
   back within 3 s (173.8 → 74.6 MB). What held 144–177 MB in §6e was the
   page staying alive, a bug in `AppController::closeTab` (fixed, §6e), not
   the allocator.
2. `MallocSpaceEfficient=1` saves ~3 MB (3–4 %). It costs ~3 % more CPU
   per keystroke and slows some large-page work (the Word page's build
   +17 %, the Markdown conversion +9 %), through an undocumented switch
   Apple can change. Not worth it for a light app that publishes its CPU
   figures too. Revisit only if the 60 MB idle target becomes the
   priority.
3. `MallocNanoZone=0` brings nothing and costs ~3 % CPU per edit;
   `MallocAggressiveMadvise=1` does nothing under xzone.

## 7. Caveats

* `QT_QPA_PLATFORM=offscreen` runs exercise the plumbing only: paint and
  presentation timings there do not include the window server. Threshold
  runs use the normal Cocoa platform on an idle, plugged-in machine
  (`env.thermal_state`, `env.power_mode` are recorded per run).
* The OS file cache is not purged between runs (needs root); "uncached"
  means not in Folia's document cache.
* Input is synthetic (`QApplication::sendEvent`), bypassing the HID/IME path.
* Wakeup counts from inside the app (`idle` scenario) include the driver's
  own 1 s sampling timer; procstat counts come from outside.
* Machine load matters: note the load average with every result set.
* The Claude Code app exports `MallocNanoZone=0` to the processes it
  starts. In a bench launched from a Claude session, xzone malloc's thread
  caching is then off: no memory figure changes, typing CPU is ~3 % higher
  than for a Finder or Dock launch (§6f). `env.malloc_env` records it; unset
  it (`env -u MallocNanoZone`) to measure what users get.
