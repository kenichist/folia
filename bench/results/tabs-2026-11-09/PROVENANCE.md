# Note tab runs, 2026-11-09

MacBook Air M1 (8 GB), macOS 26.6.2, Release build of branch
`worktree-note-tabs` (Folia 0.2.0 plus tabs and uploads), Qt 6.12.0, on
battery, Low Power Mode off, thermal state nominal in every run, the user
working in other apps. Each run: a fresh scratch `--home` (the welcome pages
plus the 30 bench pages), wrapped in `caffeinate -d -i`. The bench window is
click-through and floats on top (bench mode); `window_exposed_and_active_ratio`
is 0 in every run because it is never the active window.

| File | Variant | Switches painted | Used for |
|---|---|---|---|
| `tabs.strip1.0.json` | default layout (with `ui.tabs{}`) | 17 of 150 | docs/BENCHMARKS.md §6d |
| `tabs.strip1.1.json` | same | 117 of 150 | §6d |
| `tabs.strip0.0.json` | `--bench-arg strip=0` (default layout minus `ui.tabs{}`) | 0 of 150 | §6d |
| `tabs.strip0.1.json` | same | 13 of 150 | §6d |

Set aside (`superseded/`): `tabs.strip0.0.aborted.json`, a first `strip=0`
run that stopped at "first page did not open": the bench then waited for a
paint of the first page, which macOS did not deliver. The bench now waits
for the page to be editable, and `tabs.strip0.0.json` is its rerun.

Earlier runs of the same day were not kept: they waited for a paint after
every switch (most never came, so most samples were empty) and closed tabs
from inside the bench's nested event loops (where `deleteLater()` waits for
the bench to return, so closed pages were not yet freed when measured).

Summary: `python3 bench/scripts/tabs_summary.py bench/results/tabs-2026-11-09/tabs.*.json`.
