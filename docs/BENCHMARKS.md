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
  the fixture carries exactly the app's migrations, `PRAGMA user_version=1`).
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
`settings` (`reps=10`, `window=0|1`, `apply=0|1`, `close_settle_ms`; see below).
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

### `settings`: the Settings window

Each rep opens Settings (`showSettings()` → its next paint), applies two
text-size and two light/dark changes (main-thread time, and time until the
editor repaints), restores the overrides and closes the window the way its
title-bar button does. Footprint is sampled before, 500 ms after each open
and after each close; at the end the font lists are filled once.
`window=0` measures theme changes alone, `apply=0` the window alone.

M1, Release, seeded welcome workspace (not the 50k fixture), 2026-11-05,
30 reps:

| Metric | Result |
|---|---|
| First open (creates the window) | 130 ms; `settings_create_us` 9–10 ms |
| Reopen | p50 58 ms, p95 62 ms |
| Footprint: before → first open → after close | 70.0 → 87.4 → 84.0 MB; then flat at 88.5 MB closed / 92.1 MB open for 30 reps |
| Same with theme changes each rep | levels off at 98.6 MB closed (theme changes alone: +6 MB, then flat) |
| Text-size change | main thread p50 3.0 ms (p95 5.7); to editor paint p50 32 ms (p95 58) |
| Light/dark change | main thread p50 2.4 ms (p95 4.6); to editor paint p50 29 ms (p95 59) |
| Font lists, first popup | 6.5 ms, +0.08 MB |

Two findings shaped the window. On macOS 26 with Qt 6.12, a top-level
window that was activated and then destroyed by Qt (deleted, or closed
with an accepted close event) keeps its native `QNSWindow` alive; `heap`
showed one more per cycle, and footprint grew 3.7–3.9 MB per open/close.
Settings is therefore created once, and its close button, Mod+W and Esc
hide it. Asking every font family whether it is fixed-pitch cost 300 ms
and 4 MB, so the code-font list orders families by name instead. Benchmark
loops run inside nested event loops, where `deleteLater()` is deferred:
flush with `QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete)`
before measuring a deletion.

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

## 6. Caveats

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
