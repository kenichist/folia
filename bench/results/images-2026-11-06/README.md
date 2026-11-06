# Image benchmarks, 2026-11-06 (provenance)

All runs: MacBook Air M1 (MacBookAir10,1) 8 GB, macOS 26.6.2, Release build
of this branch (uncommitted working tree on top of `5c0f719`), Qt 6.12.0,
battery power, Low Power Mode off, thermal state nominal. The window was
1180×759 pt at 2× and floated on top, click-through (bench mode). The
fixtures came from `folia_mediaprobe --make <dir> 24`, written out of
process: 24 JPEG photos 4032×3024 (5.3 MB each), one HEIC (4.6 MB) and one
WebP (2.1 MB) of the same size, three 2880×1800 UI-like PNG screenshots, a
1000×40000 PNG scrolling screenshot (0.25 MB) and a 6000×4000 noisy PNG
(36 MB).

| File | What | Used for |
|---|---|---|
| `mediaprobe.json` | `bench/scripts/mediaprobe_matrix.py`: each format at 400–6000 px outputs, 3 fresh processes per point, worst peak kept | the decode-cost table and `MediaService::costFor` |
| `images.json` | `--bench images` run 4, final code (bitmaps in their own mappings) | memory, decodes, cache hits, idle, pressure, import |
| `images-onscreen-frames.json` | run 3: same scenario, the window was painted by the system throughout | frame timings only |

In run 4 the system did not paint the window during the two scroll passes
(it was covered or on another Space). Those frames were painted through
the same code into an offscreen image (9.6 MB, counted in the footprint),
so decodes and memory are valid but frame timings are not; they come from
run 3.

Superseded runs (not kept):

- Run 1: the window was not painted during the scroll passes, and the
  scenario had no offscreen fallback yet, so nothing was decoded while
  "scrolling". This led to the unpainted-frame detection and fallback.
- Run 2: before PNG was recalibrated to in-app peaks (the in-app
  2880×1800 PNG peaked at 62 MB against a 49 MB estimate) and before
  bitmaps moved to their own mappings.
- Run 3 (kept for frame timings): before bitmaps moved to their own
  mappings. Trimming the cache to 0 under pressure left the footprint
  unchanged (184.3 MB before and after), which is how that problem was
  found.
