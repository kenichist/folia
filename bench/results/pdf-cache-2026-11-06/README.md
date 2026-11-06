# PDF cache size, timing endpoints and repeated close, 2026-11-06 (provenance)

All runs: MacBook Air M1 (MacBookAir10,1) 8 GB, macOS 26.6.2, Release
builds of branch `pdf-cache-budget`, Qt 6.12.0, battery power, Low Power
Mode off, thermal state nominal. Window 1180×759 pt at 2×, floating and
click-through (bench mode), `--bench-arg app_nap=0`. Fixtures from
`folia_mediaprobe --make-pdf <dir>` and `--make <dir> 24`, written out of
process. Results in docs/BENCHMARKS.md §6b, "Cache size, frame and input
timing, repeated close".

| File | Run | Build | Used for |
|---|---|---|---|
| `scroll-budget128.json` | `--bench pdf --bench-arg kinds_only=long --bench-arg media_cache_mib=128` | final | the clean A/B, before: painted on screen throughout |
| `scroll-cache64.json` | `--bench pdf --bench-arg kinds_only=long` (cache cap 64 MiB, the new default) | final | the clean A/B, after: painted on screen throughout |
| `pdf-budget128.json` | full scenario, budget 128 MiB, no cache cap | before the cap | control; typing beside a PDF (editor painted 300 times); first scroll painted |
| `pdf-budget64.json`, `pdf-budget48.json` | full scenario, `media_budget_mib=64` / `48`, no cap | before the cap | lowering the whole budget instead: reduced-density scans, footprint growth |
| `pdf-cache64.json` | full scenario, budget 128 MiB, cache cap 64 MiB | cap, without the per-phase worker CPU keys | scans, vector and transparency at full density with the cap; cycles; pressure |
| `images-budget128.json`, `images-cache64.json` | `--bench images`, `media_cache_mib=128` / default | cap | the image page with and without the cap |
| `heap-budget64.*` | `kinds_only=scanned,cycles cycles=6 vm_snapshots=2 media_budget_mib=64`, `MallocStackLogging=lite` | cap | what holds the growing footprint (`heap -s -z`: CoreGraphics' purgeable zone) |
| `heap-cache64.*` | the same at the default budget and cap | cap | the same checkpoints where nothing grows |

`*.footprint-<checkpoint>.txt` are `footprint -p` reports written by the
scenario; `*.heap-zones-*.txt` the zone summary of `heap -s -z -H` (the
full reports and `malloc_history -allBySize`, ~94 MB each, are not kept).

The system painted the window only in some phases of the full runs
(`*_unpainted_frames`). Frames it did not paint were painted offscreen
through the same code after a 100 ms wait, so counts, memory and render
density are valid there, but frame timings and white-area figures are
taken from painted frames only. "Before the cap" builds differ from the
final one only in that they have no cache cap and bench keys added later
(`*_render_cpu_s`, the `scanned_plan`/`*_density` keys, the before-open
snapshot taken before the viewer opens rather than just after).

Superseded (not kept): a first 128 MiB run and part of a 64 MiB run whose
white-area figures mixed painted and offscreen frames. Those runs led to
keeping the two apart.
