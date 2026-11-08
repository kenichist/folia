# Reduced-density scans and the footprint, 2026-11-06 (provenance)

Why the footprint grew after every close of a scanned PDF drawn at a lower
density, and the fix (`PdfView::planFor` tiles that density too). Results
in docs/BENCHMARKS.md §6b, "Reduced-density scans grow the footprint" and
the bullets after it.

Machine: MacBook Air M1 (MacBookAir10,1) 8 GB, macOS 26.6.2 (25G83), Release
builds, Qt 6.12.0, battery power. Window 1180×759 pt at 2×, floating and
click-through (bench mode), `--bench-arg app_nap=0`, under `caffeinate -d -i`.
Fixtures from `folia_mediaprobe --make-pdf <dir>`.

## App runs

All: `Folia --home <fresh dir> --bench pdf --bench-arg pdf_dir=<dir>
--bench-arg app_nap=0 --bench-arg kinds_only=scanned,cycles --bench-arg cycles=12`
with the budget given.

| File | Build | Budget | Scan plan | After each close of the scan (MB) | Notes |
|---|---|---|---|---|---|
| `before-budget64.*` | `62dc1c1` (branch `pdf-cache-budget`) | 64 MiB | page 1197×1549 | 153.9 → 341.6, +17 a cycle | `vm_snapshots=2`; painted on screen throughout |
| `after-budget64.*` | the fix on `62dc1c1` | 64 MiB | tiles 1197×1549 | 115.0, then 123.3–128.1 | `vm_snapshots=2`; 1,060 of 1,249 cycle frames not painted by the system (painted offscreen after 100 ms; memory valid, timings not), one more window IOSurface (+14.7 MB) than before |
| `after-budget48.*` | the fix on `62dc1c1` | 48 MiB | tiles 1197×1549 | 82.5–85.6 | `vm_snapshots=1`; painted; `*.pressure-level.log`: the system was at pressure level 2 (warning) for the first 37 s, which the app did not see as a transition |
| `after-budget48-warning.*` | the fix on `62dc1c1` | 48 MiB, halved | page 623×806 | 81.4–84.4 | first 48 MiB run: its scan plan only fits a budget halved by a pressure warning (24 MiB), so the scanned phase ran in a cool-down; kept as the small whole-page case (2 MiB bitmap) |
| `after-budget64-main.*` | the fix rebased on `main` 35bcce9 | 64 MiB | tiles 1197×1549 | 99.0, then 82.9–113.2, no trend | `vm_snapshots=1`; 342 of the cycle frames painted offscreen; system at pressure level 2 for 28 of 41 samples (`*.pressure-level.log`), so caches may have been trimmed mid-run; the steps are window IOSurfaces and trims |
| `after-budget128.*` | the fix on `62dc1c1` | 128 MiB | tiles 2296×2971 (full density, path unchanged) | 83.6–100.2 | `vm_snapshots=1`; painted; the steps are one more 14.7 MB window IOSurface |

`*.footprint-<checkpoint>.txt` are `footprint -p` reports;
`*.heap-zones-<checkpoint>.txt` the zone part of `heap -s -z -H` (the
per-type tables are cut); `*.heaplarge-*.txt` live blocks of 256 KB or more.
In `before-budget64.heap-zones-after-cycles.txt`, `DefaultPurgeableMallocZone`
holds 28 × 8,240 KB and `footprint -p` shows 237 MB of `MALLOC_LARGE`. After
the fix, 28 × ~2,560 KB, volatile, and 21 MB of `MALLOC_LARGE`.

## Standalone probes (`repro/`, no Qt)

Built with
`clang++ -std=c++17 -O2 -fobjc-arc cgprobe.mm -framework CoreGraphics -framework Foundation -framework ImageIO -o cgprobe`
(likewise `makescan.mm`; `clang -O2 realloc.c -o realloc`).

- `cgprobe <pdf> <pageWidthPx> <cycles> [tile] [pagesPerCycle]`: open the
  PDF, draw page(s) as `platform::renderPdfPage` does (whole when `tile` is
  0, else in tile×tile regions), release the document, and print the
  footprint and the purgeable zone after each cycle. `BANDS=n` draws a whole
  page as n clipped bands, `RELIEF=1` calls `malloc_zone_pressure_relief`
  after each cycle, and `HOLD=s` sleeps at the end for `vmmap`/`heap`/`leaks`.
- `makescan <out.pdf> <w> <h> <pages> [gray]` writes Letter pages with one
  JPEG scan each (DCT kept), as the fixture does: `scan150` 1275×1650,
  `scan200` 1700×2200, `scan600` 5100×6600, `scan300g` grayscale.
- `realloc <MB> <grow> [cycles]` grows a buffer by doubling `realloc`
  (`grow` 1) or mallocs it at once (0), touches and frees it.
- `sweep.py <pdf> <cycles> <tile> <w,w,...>` runs `cgprobe` for each width.
  Its output is in `probe-sweeps.txt`. The growth is from cycle 3 to the
  last, and the CPU per page was measured while another bench ran.

What the probes found (session notes; commands as above):
- 300 dpi scan drawn whole: +8.5 MB per cycle at 900–1274 px. Flat at
  ≤ 880 px and ≥ 1275 px, in 512 px tiles at every width, in 2 bands, and
  with `MallocLargeCache=0`. Interpolation quality makes no difference.
  `malloc_zone_pressure_relief` frees 0.
- Allocation stack of the pinned block (`MallocStackLogging=1`,
  `malloc_history`): `CGContextDrawPDFPage` → `CGContextDrawImage` →
  `ripc_AcquireRIPImageData` → `IIOImageProviderInfo::CopyImageBlockSetWithOptions`
  → `AppleJPEGReadPlugin::createImageBlockSetWithHardwareDecode` →
  `createImageBlockFromIOSurface` → `vImageScale_ARGB8888` → `malloc`.
  `leaks --traceTree` roots it only in CoreGraphics' `get_cache.image_cache`
  (a `cache_create` cache with a memory-pressure source). `vmmap -v`: the
  zone's blocks have no `PURGE=V` with the default allocator, and are
  `PURGE=V` with `MallocLargeCache=0`.
- 600 dpi scan: +10–15 MB per page drawn at most widths, whole or tiled.
  Single widths vary between runs (0.9–14 MB at 1197 px), since the cache
  sometimes reuses a chunk. It is per page, not per draw: three pages per document grow it three times,
  and redrawing pages of one open document does not. The memory is freed
  16 MB chunks (`MALLOC_LARGE (empty)`) from `CGDataProviderCopyData`
  growing a CFData by `realloc`. `realloc.c` shows the same without
  CoreGraphics; it levels off at ~520 MB.
