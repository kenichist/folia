# PDF benchmarks, 2026-11-06 (provenance)

All runs: MacBook Air M1 (MacBookAir10,1) 8 GB, macOS 26.6.2, Release
build of branch `claude/kind-ptolemy-d29804`, Qt 6.12.0, battery power,
Low Power Mode off, thermal state nominal. App window 1180×759 pt at 2×,
floating and click-through (bench mode), `--bench-arg app_nap=0` unless
noted. Fixtures from `folia_mediaprobe --make-pdf <dir>`, written out of
process (see docs/BENCHMARKS.md §6b for what each contains).

| File | What | Build | Used for |
|---|---|---|---|
| `pdfprobe-tiles.json` | `bench/scripts/pdfprobe_matrix.py`: page 3 of each fixture at 600–6000 px, whole page vs 512 and 1024 px tiles, a fresh process per point | first build (renderer and probe as final) | tile size and whole-page threshold; "left after close" |
| `pdfprobe-estimate.json` | the same at 300–4000 px, whole page and 512 px tiles, with the final `MediaService::pdfEstimate` | final estimate | reservation ÷ peak |
| `pdf.json` | `--bench pdf`, full scenario | final code except the 150 ms fling settle (`0c1ad92`), which only changes the heavy-document phase | memory, renders, zoom, pressure, close, idle |
| `pdf-heavy-and-typing.json` | `--bench pdf --bench-arg kinds_only=beside,scanned,vector,transparency` | final (`0c1ad92`) | heavy documents; typing beside a rendering PDF (the window was painted while typing) |
| `pdf-onscreen-frames.json` | full scenario, run 1 | before the fling deferral, the orphan-render fix and the merge of main; App Nap allowed | frame timings of the 200-page scroll (painted on screen; text pages are never deferred, so the later changes do not touch them) and idle |

The window was covered for most of `pdf.json` and for the heavy-document
phases (`window_exposed_and_active_ratio` 0); those frames were painted
offscreen through the same code, so their timings are excluded. In run 1
the system painted the 200-page scroll but not the heavy documents, whose
transparency tiles then took ~7× longer than in every painted run; the
later runs hold a user-initiated activity (`app_nap=0`) so a covered
window is not throttled.

Superseded runs (not kept): run 2 (full, before the orphan-render fix and
the fling threshold changes; the window was covered throughout, so the
typing phase recorded no paints) and three short `kinds_only` runs used
to compare App Nap on/off (no difference with a painted window) and the
fling thresholds (half viewport/80 ms, quarter/80 ms, quarter/150 ms).
