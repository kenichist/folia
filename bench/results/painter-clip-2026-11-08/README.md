# Editor paints without QTextEdit's painter clip, 2026-11-08 (provenance)

Why each keystroke's editor paint allocated ~21 blocks of ~41 KB in Qt's raster
engine, and the fix. Results and decision: docs/BENCHMARKS.md §6b, "Fixed:
typing no longer rebuilds the clip per text item". Background (why those
allocations cost more with `MallocLargeCache=0`): `../malloc-large-cache-2026-11-06/`.

Machine: MacBook Air M1 (MacBookAir10,1) 8 GB, macOS 26.6.2, Release build
(`5428076` plus this change), Qt 6.12.0 (Homebrew bottle), battery power.
Window 1180×759 pt at 2×, floating and click-through (bench mode), under
`caffeinate -d -i`, a fresh `--home` with a copy of the `folia_fixturegen
--seed 42` workspace (`fnv1a64:f5ecfb8eecd9058b`, the same as the 2026-11-06
runs) per run. Memory pressure level 1–2 and load average 3–9 during the runs
(`*.pressure-level.log`, `*.log`).

## Cause

1. **Which clip.** `QWidgetTextControl::drawContents` clips the painter to
   the paint event's bounding rect (`setClipRect(e->rect(), IntersectClip)`);
   the raster engine intersects it with the system clip (the update region).
   Folia sets no clip of its own in `VirtualLayout::draw`.
2. **Why a new `QClipData` per text item.** On a 2× screen the painter's
   matrix includes the device-pixel-ratio scale, so
   `QPainterPrivate::drawTextItem` (qpainter.cpp) turns `Antialiasing` on
   before each text item ("antialias decoration with transformed text") and
   back off after it. Each change reaches `QRasterPaintEngine::renderHintsChanged`,
   which calls `updateClipping()`: drop the clip and replay the painter's clip
   operations (`QPaintEngineExPrivate::replayClipOperations`), i.e. build a new
   `QClipData` without spans. The first glyph blit or fill that needs clipping
   then calls `QClipData::initialize()`, which allocates the spans again.
   `cliptrace-n40.txt` (lldb on the unfixed build, `repro/cliptrace.py`):
   3,208 replays from `drawTextItem` under `VirtualLayout::draw` in a 40-key run.
3. **Why 41 KB.** The allocation is `initialize()`'s region branch (return
   address `+204` follows the `malloc` after `QRegion::rectCount`):
   `max(height, (ymax − ymin) × rects) × 16 B`. `paint-regions-n300.txt` (a
   temporary `fprintf` of `e->region()` in `NoteEditor::paintEvent`, not
   committed) lists the editor's update regions over 300 keys: 200 paints of
   the edited line only (one rect), 66 of the whole viewport (one rect, after
   Return or a height change), and 34 of the line **plus a 17 pt strip at the
   viewport's right edge** (two rects). The strip is the transient scroll bar
   repainting over the viewport when the document height changes. Two rects
   spanning 1,302 device rows give 2,604 spans = 41.7 KB, above the ~32 KB
   large-block threshold; a one-rect clip's spans are 1,518 × 16 B = 24 KB
   and only needed for glyphs that cross its edge.
4. **Why so many.** In a two-rect paint the bounding rect is the whole
   viewport, so the layout draws every visible block, and every text item
   (each run of a format, on each line) replays the clip and allocates once:
   ~185 per such paint, 21 per keystroke on average.

## Fix

The painter clip set by `drawContents` is the bounding rect of the update
region, and the system clip is the region itself, so the clip adds nothing.
`NoteEditor::paintEvent` marks its viewport paint (`VirtualLayout::setRedundantClip`),
and `VirtualLayout::draw` paints with `painter->setClipping(false)` (restored
at the end). `updateClipping()` returns early when clipping is disabled, so
there are no replays; glyphs are clipped by the system clip, whose spans are
built once per paint. The guard also requires a non-empty system clip.
Clips set inside the draw (selections, `drawText` in a rect) intersect with
the system clip as before. `tests/core/test_core.cpp`
`textPaintsSameWithoutPainterClip` renders the editor through a two-rect
region into a 2× image both ways: identical, and no pixel outside the region.

`--bench-arg painter_clip=1` keeps Qt's clip (the behaviour before), for A/B
runs from one binary; bench JSON records it as `editor_painter_clip`.

## Allocations of 32 KB or more per keystroke (`alloc/`)

`repro/allocrun.sh <label> <app> <n>`: the typing bench (`doc=ordinary`,
`MallocLargeCache=1`) with `../malloc-large-cache-2026-11-06/repro/bigalloc.c`
injected (`DYLD_INSERT_LIBRARIES`; `caffeinate -w <pid>` beside it, since a
SIP binary in between would strip the variable). Per keystroke = (1000-key
run − 200-key run) / 800. Every run painted ~1 editor frame per key.

| Build | 200 keys | 1000 keys | Per keystroke |
|---|---:|---:|---:|
| `5428076` (before, `base-*`) | 2,468 (157.7 MB) | 19,321 (863.4 MB) | 21.1 (0.88 MB) |
| This change, `painter_clip=1` (`fixclip-*`) | 2,452 (146.0 MB) | 19,329 (866.3 MB) | 21.1 (0.90 MB) |
| This change (`fix-*`) | 290 (55.6 MB) | 513 (79.9 MB) | 0.28 (30 KB) |

What is left in `fix-*` (difference of the two runs' stacks, 800 keys): 109
from `QScrollBar::paintEvent` (`QMacStyle` → CoreGraphics transparency
layer, Qt's style), 111 from the system clip's spans, built once in each
two-rect paint by its first clipped glyph (67), block decoration (35) or
text background (9), and 3 from writing the bench JSON.

## Typing timing (`typing-*.json`)

`repro/speed.sh` (with `repro/run.sh`): `Folia --bench typing --bench-arg
doc=ordinary --bench-arg n=1000`, four configurations interleaved per round,
four rounds. In file names, `fix` = this change, `clip` = `painter_clip=1`;
`default` = `MallocLargeCache=1` (cache on), `off` = `MallocLargeCache=0`
(what Folia now runs with). Medians over runs [range], from
`python3 -I bench/scripts/malloc_cache_compare.py typing-fix-*.json` (and
`typing-clip-*.json`):

| ms, p50 / p95 | Before, cache on | Before, cache off | Now, cache on | Now, cache off |
|---|---:|---:|---:|---:|
| Input → end of editor paint | 2.15 / 8.74 | 2.13 / 9.93 | 2.16 / 8.06 | 2.08 / 7.98 |
| p95 range over runs | 8.41–8.75 | 9.68–9.97 | 7.76–8.22 | 7.88–8.22 |
| Editor paint | 0.47 / 5.77 | 0.46 / 7.14 | 0.46 / 5.17 | 0.45 / 5.11 |
| Input → event-loop turn after the flush | 3.60 / 11.03 | 3.45 / 12.05 | 3.46 / 10.48 | 3.57 / 10.46 |
| CPU per edit, all threads | 5.43 | 5.48 | 5.29 | 5.27 |
| Runs | 3 | 4 | 4 | 3 |

- Before, the cache-off cost shows at p95 (+1.2 ms input → paint, +1.4 ms
  editor paint), as on 2026-11-06 (8.77 → 10.06). The p50 difference seen
  then (2.07 → 2.27) is not reproduced here; p50 moves by ±0.15 ms between
  runs of one configuration.
- Now the two modes are within noise of each other, and both are below the
  old cache-on p95: the paints that draw every visible block (whole viewport,
  or the edited line plus the scroll bar strip, ~1 in 3) no longer rebuild a
  clip per text item.

## Discarded runs (`invalid/`)

`typing-clip-default-1` (64 editor paints in 67 s) and `typing-fix-off-3`
(622 paints in 37 s): the system stopped painting the window for most of
the run, so the bench's per-key wait for a paint timed out. Round 4 of all
four configurations was added afterwards. No run had a sleep gap.

## Scripts (`repro/`, as run; paths point at the session's scratch folder)

`run.sh <label> <default|off|reexec> <scenario> [bench args]` (one run, as in
`../malloc-large-cache-2026-11-06/repro/`), `speed.sh` (the typing batch;
`REPS="4"` for round 4), `allocrun.sh` (interposer runs), `summ.py` (per-run
p50/p95 and paint counts), `cliptrace.py` (lldb: `command script import
cliptrace.py`, launch, then `cliptrace_dump`; records `QClipData::setClipRect`,
`setClipRegion`, `initialize` with no spans and `systemStateChanged` with
their stacks).
