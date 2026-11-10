# Repaint regions, idle window buffers, launch: raw runs (2026-11-10)

MacBook Air M1 8 GB, macOS 26.6.2, Qt 6.12.0, Release, on battery. Fixture
seed 42 (`fixture-manifest.json`, `fnv1a64:f5ecfb8eecd9058b`), a fresh copy of
the workspace per run, window 1180×759 pt at 2×. Summary: `summary.txt`
(`python3 -I bench/scripts/repaint_compare.py <this folder>`).

The machine was busy throughout (load average 4–12, other sessions building),
so "before" and "after" runs were interleaved; compare pairs and counts
(paints, lines painted), not single wall-clock figures. The user worked in
full-screen Spaces, where macOS never paints a plain window: every run but the
launches used `--bench-arg overlay=0.04` (`platform::showOverAllSpaces`).

| Files | Command (plus `--no-watch --bench-arg manifest=…`) |
|---|---|
| `typing-top-*` | `--bench typing n=400 caret=top`, before: `wide_repaints=1` |
| `typing-bottom-*` | `--bench typing n=400` (the caret on the viewport's last line), before: `wide_repaints=1` |
| `typing-formula-*` | `--bench typing n=400 caret=top formula=1`, before: `wide_repaints=1` |
| `scroll-code-after-*`, `-shaped-*`, `-wide-*` | `--bench scroll stress_page=<code page> frames=500`; `code_shaping=1`; `wide_repaints=1` |
| `scroll-stress-after-*` | `--bench scroll frames=600` |
| `scroll-*-98090ec-*` | the same scrolls on a build of the parent commit 98090ec (with the overlay window patched in), for the height estimates and the second repaint, which `wide_repaints=1` does not bring back in full |
| `wake-*` | `--bench wake gap_ms=3000 rounds=8`, before: `buffer_trim_ms=0`, after: `buffer_trim_ms=1500` |
| `idle-*` | `--bench idle page=2 seconds=25 settle_ms=3000`, before: `buffer_trim_ms=0`, after: the default (10 s) |
| `launch-before-*`, `-prefetch-*`, `-after-*` | `--bench launch start_page=2` with `MallocLargeCache=0` set (no re-exec): `start_prefetch=0 font_warmup=0`; `font_warmup=0`; defaults. 18 interleaved runs each; the slowest third of each set are load spikes |
