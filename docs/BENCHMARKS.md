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
`stress`, `presentation`, `idle` (`seconds=60`, `settle_ms`), `navigate`
(`batches=5`, `per_batch=100`), `integration`, `snapshot`, `recovery`,
`settings` (`reps=10`, `window=0|1`, `apply=0|1`, `close_settle_ms`; see below),
`images` (`photos=24`, `idle_seconds=20`, `media_dir`, `step=40`; see §6).
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
`import_typing_input_to_paint_us`, …).

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
