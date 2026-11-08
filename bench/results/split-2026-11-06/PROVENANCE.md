# Split view runs, 2026-11-06

MacBook Air M1 (8 GB), macOS 26.6.2, Release build of branch `split-panes`,
on battery (93 %), thermal state nominal, user working in other apps. Each
run: a fresh scratch `--home`, `--no-watch`, fixtures written beforehand by
`folia_mediaprobe --make-pdf`, wrapped in `caffeinate -d -i`. The bench
window is click-through and floats on top (bench mode).

| File | Code | Note length | Painted? | Used for |
|---|---|---|---|---|
| `normal-1.json` | PDF resize settle; editor heights still estimated per step | 100 lines | yes (135 / 91 paints per drag) | cross-check of `normal-2` |
| `normal-2.json` | same | 100 lines | yes (136 / 91) | §6c tables (drag, open, footprint, idle, typing) |
| `lines50k-per-step-estimates.json` | same (before the editor change) | 50,000 lines | yes (136 / 92) | §6c "before" column |
| `lines50k-settled-estimates.json` | editor heights estimated once settled (shipped) | 50,000 lines | yes (70 editor / 91 viewer paints) | §6c "shipped" column |

The editor change only matters once a note is long (re-estimating 100
lines costs microseconds), so the 100-line runs stand for the shipped code.

Set aside (`set-aside/`): `normal-unpainted.json` (the system painted no
frame: 0 paints, no viewport ever drawn), `normal-partly-painted-a.json`
(22 paints per drag) and `normal-partly-painted-b.json` (49 / 62 paints).
Their main-thread step times agree with the clean runs; their render and
paint counts do not describe a visible window.

`*_peak_footprint_bytes` is the process's lifetime peak at the end of that
phase. The settle drag ran first, so the no-settle figure includes it and
the difference (+76 MB) is what the no-settle drag added.
