# malloc's large cache off (`MallocLargeCache=0`), 2026-11-06 (provenance)

Whether Folia should run without malloc's large-block cache, measured in the
app. Results and decision: docs/BENCHMARKS.md §6b, "Fixed: Folia runs
without malloc's large cache". Background (the 600 dpi realloc chunks, the
probes): `../pdf-scan-pin-2026-11-06/`.

Machine: MacBook Air M1 (MacBookAir10,1) 8 GB, macOS 26.6.2 (25G83), Release
builds, Qt 6.12.0, battery power, thermal state nominal. Window 1180×759 pt
at 2× (or 640 pt wide, `window_width=640`), floating and click-through
(bench mode), `--bench-arg app_nap=0` for PDFs, under `caffeinate -d -i`, a
fresh `--home` per run. The system was at memory pressure level 2 (warning)
for nearly all runs, and its load average was 9–25 (Spotlight indexing,
Chrome, other sessions' work), so timings are noisy; runs of the two modes
are interleaved (A B A B) so drift hits both. `*.pressure-level.log` samples
`kern.memorystatus_vm_pressure_level` every 5 s; a gap over 15 s means the
Mac slept (see "Discarded runs").

**Decision: ship it.** Folia re-executes itself with `MallocLargeCache=0`
when the variable is unset (`src/app/main.cpp`). After PDF work the
footprint is 15–59 MB lower and flat; PDF rendering, page opening and
editor scrolling are not slower; typing costs +0.2 ms at p50 and +1.3 ms at
p95 in process, image decodes 2–4 %, launch ~40 ms.

Modes, in file names: `default` = `MallocLargeCache=1` (the system default;
any value other than `1` disables the cache), `off` = `MallocLargeCache=0`,
`reexec` = unset, so the new build re-executes itself with `0`. Compare with
`python3 -I bench/scripts/malloc_cache_compare.py <files>`.

## Memory: a 600 dpi scan opened, scrolled and closed (`scan600*`)

Fixtures: `folia_mediaprobe --make-pdf <dir>`, with `scanned-24.pdf` replaced
by `makescan scanned-24.pdf 5100 6600 6` (`../pdf-scan-pin-2026-11-06/repro/`):
6 Letter pages, one 5100×6600 JPEG each (8.3 MB streams, 49.7 MB). The name
is kept so the bench picks it up as the scanned document.

`Folia --home <fresh> --no-watch --bench pdf --bench-arg pdf_dir=<dir>
--bench-arg app_nap=0 --bench-arg kinds_only=scanned,cycles --bench-arg cycles=12
[--bench-arg window_width=640]`: import the five PDFs, open the scan, scroll
through it, close (`scanned_*`), then 12 cycles of open → scroll → close of the
200-page document and the scan. Built from `dfcbb5c` plus the `window_width`
argument (no re-exec), so `default` here is the unset variable.

| Run | Scan plan | After import (MB) | After each close of the scan (MB) | Peak while open (MB) | `MALLOC_LARGE` dirty after the cycles |
|---|---|---:|---|---:|---:|
| `scan600-default-1` | tiles 2296×2971 | 98.8 | 117.8–132.4 (median 121.8) | 223.0 | 43 MB |
| `scan600-default-2` | tiles 2296×2971 | 99.0 | 129.6–221.2 (median 132.4) | 230.8 | 40 MB |
| `scan600-off-1` | tiles 2296×2971 | 70.7 | 72.7–74.3 (median 73.6) | 222.0 | 0 |
| `scan600-off-2` | tiles 2296×2971 | 70.0 | 72.9–74.7 (median 73.7) | 221.5 | 0 |
| `scan600w640-default-1` | tiles 1216×1574 | 78.9 | 94.8–112.1 (median 101.8) | 161.3 | 40 MB |
| `scan600w640-default-2` | tiles 1216×1574 | 78.7 | 101.5–113.1 (median 103.3) | 162.7 | 50 MB |
| `scan600w640-off-1` | tiles 1216×1574 | 50.5 | 52.7–61.2 (median 53.2) | 133.9 | 0 |
| `scan600w640-off-2` | tiles 1216×1574 | 50.6 | 53.3–53.9 (median 53.5) | 139.3 | 0 |

- With the cache off, everything after a close is ~48–59 MB lower, and flat
  (±1 MB over 12 cycles); with it, the figure wanders by up to 90 MB
  (`scan600-default-2`, cycles 4–6). The 28 MB difference already after the
  import is freed import and thumbnail memory.
- In the app the scan does not grow 10–15 MB per page drawn, as the
  CoreGraphics-only probe does: the cache stays at 40–50 MB, so other
  allocations reuse its chunks. It is a constant cost, not a leak.
- The window IOSurfaces (22 MB at 640 pt, 41–55 MB at 1180 pt) are the same in
  both modes; `footprint -p` reports (`*.footprint-*.txt`) differ only in
  `MALLOC_LARGE`.
- Painting: the system did not paint some phases of some runs
  (`*_unpainted_frames`), in both modes; counts and memory are valid, the
  bench's 13.6 MB offscreen canvas is then freed into the large cache too
  (`default`) or unmapped (`off`).

## Standalone probes (`probes.txt`, `repro/allocbench.c`)

`cgprobe` on the 600 dpi scan (512 px tiles at 1197 px, 3 pages per document,
8 cycles): 44 → 266 MB with the cache, 5.2 → 5.6 MB without, 161 ms of CPU per
cycle either way. `allocbench` gives the price: every `malloc` above ~32 KB is
a "large" allocation on this machine, and without the cache each one maps
and faults in fresh pages: 3 µs at 33 KB, 37 µs at 0.5 MB, ~1 ms at 8.3 MB,
~10 ms at 48 MB (against ≤ 0.1 ms when a cached block is reused).

## Speed (the new build; `default` = `MallocLargeCache=1`, `off` = `0`)

Built from this commit's code (re-exec in `main()`); `default` and `off` set
the variable, so neither re-executes. Medians over runs [min–max]; frame
intervals only for frames the system painted.

**200-page PDF** (`long-*`, 3 pairs): `--bench pdf --bench-arg
pdf_dir=<folia_mediaprobe --make-pdf> --bench-arg app_nap=0 --bench-arg
kinds_only=long`. The system painted few scroll frames in most runs, so the
worker's own CPU is the measure.

| | `default` | `off` |
|---|---:|---:|
| PDF worker CPU, scroll down / up (s) | 7.81 [7.43–9.62] / 9.30 [7.32–9.62] | 7.22 [7.21–8.69] / 8.87 [8.15–9.00] |
| One render (wall), p50 / p95 (ms; the 4× zoom scroll, ~480 tiles) | 1.41 / 2.58 | 0.91 / 1.66 |
| Zoom: settle → sharp p50 / p95 (ms) | 108 / 217 | 107 / 217 |
| Footprint during the scroll, sampled max / rusage peak (MB) | 188.1 / 201.8 | 182.4 / 196.7 |
| Footprint after closing (MB) | 102.2 [90.7–102.5] | 86.8 [72.8–87.0] |

**Images** (`images-*` 2 pairs, `images2-*` 3 pairs; `images2` adds
`images_scroll_decode_ms`, every decode of the scroll and resize phases):
`--bench images --bench-arg media_dir=<folia_mediaprobe --make <dir> 24>`.

| | `default` | `off` |
|---|---:|---:|
| Scroll decodes (54 a run), wall p50 / p95, `images2` pairs 1 and 2 (ms) | 64.6–65.5 / 204–213 | 66.4–67.0 / 207–211 |
| Same, thread CPU p50 (ms) | 41.2–41.4 | 42.0–42.9 |
| All scroll decodes, total wall: pair 1 (painted) / pair 2 (`off` partly unpainted) / pair 3 (unpainted) (s) | 4.09 / 4.05 / 10.88 | 4.11 / 5.19 / 12.68 |
| Format probe, JPEG 4032×3024 p50, all 5 pairs (ms) | 42.1 | 44.6 |
| Cool-down decodes after critical pressure, p50, the three mostly painted pairs (ms) | 52.7–60.4 | 54.6–67.6 |
| Same, the two unpainted pairs `images-*-1`, `images2-*-3` (ms) | 55.3, 51.5 | 111.7, 140.7 |
| Footprint: scroll peak, idle with images, idle after (MB) | no difference beyond run-to-run spread (painted runs carry two more window IOSurfaces) | |

The images scenario does not take `app_nap=0`: in the unpainted pairs the
covered window's app was likely in App Nap, where faulting in fresh pages
after the pressure trim cost 2–2.7× (the scroll decodes of pair 3 were not
slower, 159.5 vs 145.8 ms p50). In the other pairs the cost is 0–13 %
there, and 1–4 % on scroll decodes (pair 1 painted throughout; in pair 2,
552 scroll frames of `images2-off-2` were not).

**Editor** (fixture `folia_fixturegen --seed 42`, 50,000 pages; window painted):

| | `default` | `off` |
|---|---:|---:|
| Typing 1000 keys, input → end of editor paint p50 / p95 (ms; 2 pairs) | 2.07 / 8.77 | 2.27 / 10.06 |
| Typing, input → event-loop turn after the flush p50 / p95 (ms) | 3.40 / 11.27 | 3.38 / 12.45 |
| Typing, editor paint p50 / p95 (ms) | 0.43 / 5.91 | 0.46 / 7.19 |
| Scroll 1200 frames, frame work p50 / p95 (ms; `scroll-*-1`, `-3`) | 6.55–6.59 / 7.87–7.88 | 6.56–6.61 / 7.93 |
| Scroll, presented interval p50 / p95 (ms) | 16.3 / 21.5 | 16.3 / 21.0–22.2 |
| Open a page, uncached → editable p50 (ms; 2 pairs) | 12.5 | 12.3 |
| Launch, process start → editable (ms; 5 each; `reexec`: 451 [430–821]) | 414 [403–439] | 409 [402–440] |
| Launch, process start → `main()` (ms; `reexec`: 52.9 [50.5–137]) | 28.0 | 30.1 |
| Footprint 2 s after launch (MB) | 81.1 | 78.9 |

`scroll-off-2` is left out of the scroll row: the system did not paint for
31.6 s of it (76 s instead of ~23 s); `scroll-*-3` replaced it.

**Where the typing cost comes from** (`typing-large-allocations.txt`, from
`repro/bigalloc.c`, a `DYLD_INSERT_LIBRARIES` interposer that counts
`malloc`/`calloc`/`realloc`/`malloc_zone_*` calls of 32 KB or more by stack):
a 1000-key run makes 19,329 such allocations, a 200-key run 2,469, so about
21 per key (~0.9 MB). Nearly all are `QClipData::initialize()` in Qt's raster
engine, ~41 KB of per-scanline clip spans, allocated whenever glyphs or
backgrounds are drawn under a new non-rectangular clip during the editor's
paint (`VirtualLayout::draw` → `QTextLayout::draw`). With the cache, each is a
recycled block; without it, fresh pages (`allocbench`: ~3–7 µs each plus page
faults). Folia sets no clip there itself.

## Discarded runs (`invalid/`)

`long-default-1` (first attempt) spanned 17:53–19:06 while the Mac slept
(gaps of 15–18 min in its pressure log); `long-off-1` (first attempt) was
killed when the batch was restarted. `repro/run.sh` now moves any run with a
gap over 15 s aside and the batch repeats it; none needed it afterwards.

## Scripts (`repro/`, as run; paths point at the session's scratch folder)

`run.sh <label> <default|off|reexec> <scenario> [bench args]` (one run),
`speed.sh` (PDF, images, editor and launch batches), `images2.sh`,
`allocbench.c`, `bigalloc.c`.
