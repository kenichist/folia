# Provenance of the runs in this folder

All runs: MacBook Air M1 8 GB, macOS 26.6.2, Release build, fixture seed 42
(`fnv1a64:f5ecfb8eecd9058b`), 1180×765 pt window @2x, battery power, Low Power Mode off.

| Runs | Pass | Window condition |
|---|---|---|
| typing*, scroll, stress, presentation | 1st pass (2026-11-04 17:00–17:10) | visible + active the whole run (exposure ratio 1.00) |
| create, navigate | 2nd pass (always-on-top) | visible (on top) |
| open, integration | 3rd pass (2026-11-05, always-on-top) | visible + active (ratio 1.00 for open) |
| expand | 4th pass (corrected presentation probe) | on top; main-thread metric is CPU-only. Presentation samples only where new rows were painted |
| idle_* | 5th pass (on top + click-through) | visible, not interactable (no user hover or clicks could reach it) |
| launch | 6th pass (probe timestamps the first editable paint) | visible |

Superseded (kept for transparency, not used for verdicts):
- `superseded/sleep-or-noWatch/`: open/expand from a pass during which the Mac
  slept (timeouts of minutes), plus an integration run with live reload disabled
  (`--no-watch`), which invalidated one check.
- `superseded/idle-pass2/`: idle runs where the floating window could still be
  hovered or clicked by the person using the machine. One run even ended in a
  different layout; CPU spikes up to 61% came from interaction.
- `superseded/launch-probe-v1/`: the launch probe included the bench driver's own
  startup waits (about 3.1 s, not a product measurement).
- Expand runs in the first pass had a presentation probe that missed the paint
  (it snapshotted after the repaint).
