# Folia 0.1.0 release report

**Date:** 2026-11-05.

**Primary reference:** MacBook Air M1 (MacBookAir10,1), 8 GB RAM, macOS 26.6.2
(25G83), arm64, built-in Retina display at 60 Hz.

**Build:** Release (`-O3`), Apple clang 21, Qt 6.12.0 (Homebrew), CMake 4.4.5, Ninja 1.13.2.

**Run conditions:** on battery, Low Power Mode off, thermal state nominal or fair
(recorded per run). The benchmark window was 1180×765 points at device pixel
ratio 2.

**Fixture:** seed 42, content hash `fnv1a64:f5ecfb8eecd9058b`, 50,000 pages and
214,616 blocks. The OS file cache was warm; purging it needs root.

**Raw results:** `bench/results/m1-2026-11-04/` holds one JSON file per run with
every sample, the analyzer output (`REPORT.md`, `summary.json`), the fixture
manifest, the recovery results, and `PROVENANCE.md`, which says which run pass
each file came from and why some runs were superseded.

## Verdict

The core release works end to end on the M1:

1. Open a nested research note and type immediately.
2. Edit inline and display equations (source always editable).
3. Copy highlighted code.
4. Close the app and reopen the same content offline, with the caret restored.
5. Redesign the shell in a text editor without recompiling. It reloads live, and a broken layout falls back safely.

Two structurally different presets ship, plus a third customization example.

Results against the performance contract (thresholds unchanged, none relaxed):
- **Pass:** 10 of 12 gates.
- **Miss:** the idle-footprint stretch target (about 80 MB against 60 MB).
- **Formula:** typical formulas meet the 100 ms gate at 33 ms p95. The first
  formula in a process, which includes loading the math font, takes 250 ms and
  is reported separately, as the contract asks.

Not shipped as accepted: Windows and Linux hardware acceptance, and a real
(non-synthetic) IME check. All of these are open and listed below.

Silent data loss, a stale preview replacing current content, and a broken
recovery path were each tested directly; none occurred.

## Achieved / missed / unmeasured

p99 is acceptance-grade only for samples of ≥ 1000 observations across runs;
smaller p99 values are labeled exploratory.

| Scenario | Target | Result on M1 | n | Status |
|---|---|---|---|---|
| Ordinary page, click → first editable, uncached | p95 ≤ 100 ms | p95 **17.9 ms** (p50 11.3, p99 39.5 exploratory) | 360 (3 runs) | **Achieved** |
| Ordinary page, cached | p95 ≤ 30 ms | p95 **13.2 ms** (p50 9.4) | 357 | **Achieved** |
| Create page → editable title | p95 ≤ 30 ms | p95 **0.22 ms**; durable commit, measured separately: p50 53 ms, p95 240 ms | 360 | **Achieved** |
| Expand branch, main-thread work for first visible rows | p95 ≤ 16 ms | p95 **0.91 ms**; rows available, including the reader thread: p95 7.3 ms. Presentation (sidebar repaint, where rows landed on screen): p50 6.5 ms, p95 8–10 ms in unoccluded runs (exploratory, n=91) | 600 | **Achieved** |
| Typing: main-thread work per edit | p95 ≤ 8 ms | p95 **1.04 ms**, p99 4.1 ms | 3225 | **Achieved** |
| Typing: event → presented glyph | p95 ≤ 33 ms | to end of paint: p95 **8.4 ms** (p99 11.5); to backing-store flush: p95 10.7 ms. *Proxy:* excludes WindowServer composition (≤ 1 frame) | 3003 | **Achieved** (proxy) |
| Typing, keyboard-first preset | same | main p95 1.05 ms; paint p95 8.2 ms | 3225 / 3003 | **Achieved** |
| Typing after recovering from an invalid layout | same | main p95 1.05 ms; paint p95 7.9 ms; invalid layout rejected, last valid kept | 2150 / 2002 | **Achieved** |
| Typing during background formula rendering | same | main p95 0.95 ms; paint p95 7.9 ms | 2150 / 2002 | **Achieved** |
| Typing in the 10 MiB stress note | same | main p95 0.78 ms; paint p95 9.4 ms | 1072 / 1000 | **Achieved** |
| Typing in the single 1 MiB code block | same | main p95 0.36 ms; paint p95 4.5 ms | 1072 / 1000 | **Achieved** |
| CPU time per edit, all threads | report | 5.0–5.5 ms (3.1 ms in the code block). Mostly the Qt backing-store blit; see the bottleneck section | 3–13 runs | Reported |
| Large-note scrolling (10 MiB note) | ≥ 95% of frames ≤ 16.7 ms | **100%** of 3600 frames: work p95 6.5 ms, longest 9.3 ms. Presented intervals: 11 of 4344 over 25 ms; longest hitch 57.8 ms | 3600 | **Achieved** |
| Visible short code block → presentation | p95 ≤ 30 ms | p95 **16.7 ms** (one frame) | 300 | **Achieved** |
| Typical formula → presentation | p95 ≤ 100 ms | uncached p95 **33.4 ms**; cache hit p95 33.4 ms (two frames); worker render p50 0.03 ms | 297 + 297 | **Achieved** |
| First formula in a process (renderer and font load) | report separately | 228–250 ms (one run 33 ms) | 3 | Reported (would miss 100 ms if gated) |
| Idle CPU (opaque text workspace, 60 s after settling) | mean < 1% of a core | **0.16–0.27%** mean across 7 runs (default, keyboard, no-math, editor-only, stress note) | 7 × 60 s | **Achieved** |
| Idle footprint (`task_vm_info.phys_footprint`) | ≤ 60 MB (stretch) | **79.0–81.0 MB** (default and keyboard); 76.0 MB editor-only; 114 MB with the 10 MiB note open. Empty Qt window baseline: 47.0 MB | 7 runs | **Missed** |
| Idle wakeups | record | 5.3/s, **including the harness's own 1 Hz sampler and 4 Hz exposure probe**. App-only wakeups were not isolated | 7 runs | Unmeasured (app-only) |
| Repeated navigation, 100 switches × 5 batches | footprint plateaus within cache caps | switches p50 16 ms, p95 38 ms. Settled footprint per batch: 110.5 → 110.7 MB, a plateau. Document cache 3.7 MB estimated, 16 entries (caps 48 MB / 16) | 500 | **Achieved** |
| Stress note (10 MiB, 10,000 blocks) | report | first editable 265–274 ms; jump to end one frame (16.7 ms); select-all across off-screen blocks → paint p50 83 ms; copy 10 MiB 11–13 ms; peak footprint 214 MB | 2 runs, 40 reps | Reported |
| Process launch → editable ordinary page | report | 375–404 ms warm (4 runs); 1406 ms on the first run after a rebuild (cold dyld). `exec→main()` 23–31 ms; shell load about 18 ms; page build about 7 ms | 5 | Reported |
| Max dirty interval under healthy storage | ≤ 1 s | max **967 ms** (p95 945) | 230 commits | **Achieved** |
| Expand, open, typing, idle and navigation on **Windows / Linux reference hardware** | same gates | not run | — | **Unmeasured**; acceptance open |
| Real IME (Kotoeri / Pinyin) through the OS | works | synthetic `QInputMethodEvent` only | — | **Unmeasured**; manual check open |
| Native material (Liquid Glass) compositor/GPU cost | measure separately | no adapter in this build; the opaque fallback is always used | — | Not applicable / unmeasured |

Repeat runs: 3 for typing, open, create, expand, scroll and presentation; 2 for
each typing variant; 7 idle runs; 5 launches.

Run-to-run noise: open p95 varied 16–19 ms across 3 runs, typing p95 8.2–8.4 ms
across presets, and idle footprint ±1 MB.

## Staged cost (same window, same counter, idle 60 s)

| Stage | Footprint (median) | Δ vs previous | Notes |
|---|---|---|---|
| Empty Qt Widgets window (`folia_baseline`) | 47.0 MB | — | 28 MB of it is IOSurface (2 backing buffers) |
| Folia, editor only (minimal Lua layout), no math, ordinary page, 50k-page workspace | 76.0 MB | +29.0 MB | adds a third backing buffer (+14.7 MB), SQLite (2 × 2 MB caps), page and tree data, Lua |
| Default Lua layout (sidebar tree, search, breadcrumbs), no math | 80.8 MB | +4.8 MB | native list views and their models |
| … with the math renderer loaded (ordinary page has display formulas) | 80.1–80.2 MB | within ±1 MB noise | renderer code 161 KiB; font data about 2.6 MB RSS per the math owner's measurement |
| … with the 10 MiB stress note open | 114.2 MB | +34 MB | QTextDocument holding 8.5 M chars plus per-line data |

The first equation's cost is below the noise of this idle comparison.

**Download/install size**, separate from live memory:
- `dist/Folia.app`: 68 MB (Qt frameworks 16 MB; Homebrew ICU data 32 MB).
- `dist/Folia.dmg`: 30 MB.
- Folia's own binary: 2.9 MB.

## How the measurements were taken

All scenarios run the real release app (`Folia --bench <scenario>`, driver in
`src/app/BenchDriver.cpp`) on fresh copies of the same fixture. Raw samples go
to JSON.

```sh
build/tools/folia_fixturegen --out ws.db --seed 42 --manifest manifest.json
python3 -I bench/run_suite.py --app build/src/Folia.app/Contents/MacOS/Folia \
    --fixturegen build/tools/folia_fixturegen --out <dir>
python3 -I bench/scripts/analyze.py <dir>/*.json -o REPORT.md --json summary.json
python3 -I tests/recovery/run_recovery.py --app build/src/Folia.app/Contents/MacOS/Folia --out recovery.json
build/tools/folia_procstat <pid> 1000 60     # external sampler (proc_pid_rusage)
```

Proxies and caveats:
- **Input.** Synthetic Qt events (`QApplication::sendEvent`). They bypass the
  OS HID and IME path.
- **Presented glyph.** Measured to the end of the editor viewport paint, and
  to the next event-loop turn (backing store flushed). WindowServer
  composition is not included.
- **Frame drops.** Main-thread work per frame plus presented-paint intervals,
  with display-paced scrolling at 60 Hz.
- **Occlusion.** macOS stops painting covered windows. Every run records an
  exposure ratio. Bench windows float above other windows and are
  click-through, so a person using the machine cannot perturb a run. Runs
  that were occluded, slept through, or touched by interaction were rerun.
  The originals are kept in `superseded/` with the reasons.

## Dependencies (each justified by a user-visible need and its measured cost)

| Dependency | Why (user-visible need) | Measured cost |
|---|---|---|
| Qt 6.12 Core/Gui/Widgets (Homebrew bottle) | Native windows, text editing with IME/selection/undo/clipboard, accessibility and keyboard on macOS/Windows/Linux from one code base | An empty Qt window is 47.0 MB `phys_footprint`, 28 MB of it IOSurface. Bundled frameworks: QtCore 4.9 + QtGui 5.8 + QtWidgets 5.3 MB. The bottle also pulls ICU (32 + 4.2 MB), OpenSSL (5.2 MB), glib (1.2 MB) and harfbuzz/freetype/zstd |
| SQLite 3.54.1 (vendored) | Durable local storage, transactions, WAL, indexed child and title lookups | 1,354 KiB `__TEXT` (FTS5 compiled in but unused, so it can be dropped); 2 MiB cache cap per connection × 2; commit p50 0.94 ms, p95 8.1 ms on the writer thread |
| Lua 5.5.1 (vendored, one documented patch) | Shell language users edit without recompiling | 234 KiB `__TEXT`; 26–64 KB Lua heap per layout. Default layout: load about 18 ms at launch, render on page switch p50 3.2 ms / p95 5.6 ms |
| Native math renderer (`src/math`) | Display and inline LaTeX with no browser engine or helper process | 161 KiB `__TEXT`; font data about 2.6 MB + 1.3 MB RSS on first use; 11–13 µs per formula p50 |
| Code highlighter (`src/code`) | 19 languages, built per language on first use | 72 KiB `__TEXT`; about 300 MB/s |
| Folia core (`src/core`, `storage`, `editor`, `shell`, `app`) | Everything else | 751 KiB `__TEXT` (includes the bench driver, which is inert unless `--bench` is passed) |

Not used: a browser engine, helper processes, a second implementation
language, a rope library, QtNetwork, QtSql, Qml or QtSvg.

The editor's piece table is Qt's `QTextDocument`. Profiling showed no editing
bottleneck that would justify replacing it.

## Platforms

| Target | Status | Details |
|---|---|---|
| macOS (primary) | **Built, tested, measured** | MacBook Air M1, macOS 26.6.2, arm64. Minimum macOS **14.4** (Homebrew Qt `minos`), **SDK 26.5**. Package: `sh packaging/macos/package.sh` (macdeployqt, ad-hoc codesign, `hdiutil`) |
| Linux | **Built and tested in a container (5/5 suites pass: code, math, math_init_race, core, shell); not measured on hardware** | Debian 13 trixie aarch64 (Docker Desktop VM on the M1), GCC, Qt 6.8, offscreen QPA. See `packaging/linux/README.md`. The container surfaced 3 real bugs, all fixed: `QJsonObject::cbegin`, a missing `fsync` include, and DejaVu's partial MATH table. Reference-hardware acceptance is still open: name a machine and run `bench/run_suite.py` there |
| Windows | **Source and commands only; not built; unmeasured** | `packaging/windows/README.md` (MSVC 2022, Qt 6.8, windeployqt). No Windows machine was available. Acceptance is still open |

## Correctness, recovery and integration evidence

Test suites (macOS, all passing; ASan clean for core and shell):

| Suite | Count | Covers |
|---|---|---|
| `core_tests` | 11 | storage round trip, cycle rejection, order renumbering, session identity, unclosed fences, disk full and retry, IME revision semantics, page links, lazy layout on 20k blocks |
| `code_tests` | 173 | the 19 highlighting languages |
| `math_tests` | 203 + `math_init_race` | the declared LaTeX subset, also on Linux with and without STIX |
| `shell_tests` | 60 | the independent QA suite: invalid layouts, missing or duplicate editor slots, budgets, memory bombs, metamethod and finalizer escapes, shortcut conflicts, widget identity, editor survival across reloads, reload deferral during IME |

Recovery (`tests/recovery/run_recovery.py`, real binary, reopening the database
directly; `integrity_check` = ok in every case):

| Case | Result |
|---|---|
| Writer crash (SIGABRT) before COMMIT | On reopen the DB holds the previous snapshot; only the in-flight batch is lost |
| Writer crash after COMMIT | The DB holds exactly the last snapshot |
| SIGKILL mid-typing | The last committed snapshot survives. Edits after it are lost by design (at most 1 s) |
| Normal close right after typing | Every edit is flushed (stored rev = editor rev) |
| Disk full (fault VFS) then space freed | "Disk full — your edits are kept in memory…" is shown, the text stays editable, newer edits merge into the failing batch, and everything commits once space returns |

Integration rehearsal (`--bench integration`, 13/13 checks):
- Edit during an outstanding save: the stored text and revision equal the
  editor's.
- Switch pages while a formula renders, then edit its source: the preview
  shown has the current source hash.
- 30 rapid edits in a visible formula: only the newest is displayed, and 56
  superseded renders were cancelled or dropped.
- Hot-reload the layout during IME composition: the reload waits for the
  commit, and the preedit, document and caret all survive.

## Largest remaining bottleneck (profiled)

One mechanism explains both the largest measured miss (idle footprint) and most
of the CPU per keystroke: **Qt's macOS backing store** (`QCALayerBackingStore`
in the `qcocoa` plugin).

- **Window buffers dominate memory.** The window is drawn into IOSurface
  buffers of 2360×1560×4 B, about 14.7 MB each. An empty Qt window settles at 2
  buffers (28 MB). As soon as content updates while the WindowServer still
  holds the front buffer, Qt allocates a third and never releases it. An
  editor-only Folia shows 42 MB of IOSurface in `footprint -p`.
- **Each frame copies the whole window.** `sample` during typing: 367 of 747
  main-thread samples in `QWidgetRepaintManager::sync` are
  `QCALayerBackingStore::finalizeBackBuffer → blitBuffer → memmove`, a copy of
  the whole preserved window into the new back buffer on every flush. Folia's
  own paint is about 130 samples. This is why CPU per edit is about 5 ms
  across threads while main-thread edit work is p95 1 ms.

**Smallest credible next fix.** A patch of roughly 50 lines to the Qt Cocoa
backing store, carried with the Qt build Folia ships:

1. Track a damage region per buffer (buffer age), so `finalizeBackBuffer`
   copies only what changed since that buffer was last shown.
2. Mark non-front buffers `kIOSurfacePurgeableVolatile` after about 2 s with
   no flush, so an idle window keeps one resident buffer.

Expected effect, to be verified on this same benchmark rather than assumed:
- about −14.7 MB idle footprint (one buffer);
- the full-window memmove removed from each keystroke.

That alone would not reach 60 MB. The remaining gap (about 47 MB of empty-window
floor plus about 18 MB of Folia's own heap and data) needs either a smaller
default window or heap work, which should be profiled first.

Next candidates, all measured:
- the main-thread save snapshot on the 10 MiB note (full line walk, p95
  11.3 ms once per second while typing there; an incremental dirty index
  would bound it);
- the first-formula font load (250 ms; warm it on the worker when a page with
  math opens);
- the bundle: Qt without ICU, OpenSSL and glib would be about −38 MB of
  download.

## Shell customization proof

- **`presets/default/shell.lua`**: sidebar with jump search and a lazily paged
  tree, top bar with back/forward, breadcrumbs and save status.
- **`presets/keyboard/shell.lua`**: structurally different. No sidebar or
  toolbar; Mod+K opens a page picker overlay and Mod+O an outline overlay, both
  driven entirely from the keyboard.
- **`presets/examples/right-nav/shell.lua`**: navigation moved to the right,
  the toolbar replaced by one keyboard action (Mod+J opens an action strip),
  serif typography. Written without core edits.

Screenshots are in `docs/img/`.

The same typing and idle workflows ran in both presets, as the table above
shows. Language reference: `presets/REFERENCE.md`.

## Supported syntax and languages

- **Math:** `docs/MATH_SUBSET.md`. Fractions, binomials, roots, scripts, primes,
  big operators with limits, stretchy delimiters, matrix environments
  (`pmatrix`, `bmatrix`, `cases`, …), `aligned` and `gathered`, accents, font
  switches, spacing, and inline style with correct baseline. Pathological
  input is bounded (length, depth, nodes, time).
- **Code:** `docs/CODE_LANGUAGES.md`. bash, c, cpp, csharp, css, diff, go,
  html/xml, java, javascript, json, lua, python, rust, sql, swift, toml,
  typescript, yaml. Unknown languages fall back to plain text.

## How it was built

The folder started empty, so there was no prior code to reuse. The six
interfaces (`openPage`, `fetchChildren`, `applyEdit`, `commit`,
`renderPreview`, `dispatchCommand`), stable 64-bit ids and per-page revisions
were frozen first, in `src/core/Api.h` and `src/core/Types.h`. Stubs kept the
integrated build green while four owners worked in parallel:

| Owner | Module | Handed back |
|---|---|---|
| Integrator | build and integration; storage, tree and autosave; editor and layout; shell host; app; bench driver; recovery and integration tests; packaging; docs | the suites and evidence above |
| Math | `src/math` | 203/203 tests on 3 platform configurations; TSan/ASan clean |
| Code | `src/code` | 173/173 tests; ASan/UBSan/TSan clean; about 300 MB/s |
| Performance and QA | fixtures, `procstat`, `analyze.py`, independent shell suite | a deterministic 50k-page workspace in 0.8 s; 60 shell tests |

The independent QA suite found 13 real bugs in the integrator's shell code,
all fixed with regression tests:
- unprotected Lua errors from metamethods and from allocations at the budget;
- a reconcile use-after-free;
- budget errors swallowed by `pcall`;
- C-level pattern backtracking running outside the budget;
- `__gc` bypasses;
- a `ctx.focus` render loop;
- a rejected `ctx.set` leaking state;
- the reserved `editor` id deleting nodes;
- a shared reload dedupe;
- metamethods triggered on ctx writes.

The Linux build found 3 portability bugs, also fixed. One process incident:
a module owner's scratch command overwrote the root `CMakeLists.txt` for about
a minute. The integrator detected it, restored the file and kept a backup.

## Open items

1. Windows build and reference-hardware measurements.
2. Linux reference-hardware measurements (the container build and tests pass).
3. A real-IME manual check (Kotoeri, Pinyin) on macOS, then on the other
   platforms.
4. Idle footprint against the 60 MB stretch target: the Qt backing-store
   patch above, then a heap profile.
5. Optional: a native material adapter (Liquid Glass) with separate
   GPU/compositor measurement; image and media import (a separate milestone,
   not started).
