# Allocator settings A/B, 2026-11-09 (provenance)

Whether a different macOS allocator setting lets Folia give back memory after
a very large page is closed, and what each setting costs elsewhere. Results
and recommendation: docs/BENCHMARKS.md §6f.

Machine: MacBook Air M1 (MacBookAir10,1) 8 GB, macOS 26.6.2 (25G83), Release
build of branch `claude/wizardly-shamir-b53c0d` (branch `worktree-note-tabs`,
Folia 0.2.0 plus tabs and uploads, plus this branch's bench changes), Qt
6.12.0, on battery (67 % → 55 %), Low Power Mode off, thermal state
nominal, the user working in other apps. The system was at memory pressure
level 2 (warning) for nearly all runs (`*.pressure-level.log`, sampled every
5 s: 655 samples at level 2, 4 at level 1), load average 1.5–7. Runs
12:17–13:12; no run had a sleep gap. Window 1180×759 pt at 2×, floating and
click-through (bench mode), each run in a fresh `--home` under
`caffeinate -d -i`.

## Modes (in file names)

Every mode first unsets `MallocNanoZone`, which the Claude Code app exports
as `0` to every process it starts (a Finder or Dock launch has it unset).
Unless `MallocLargeCache` is set, Folia re-executes itself with
`MallocLargeCache=0` (src/app/main.cpp), so every mode except `sys` runs with
it.

| Mode | Launched with | In the run's `env.malloc_env` |
|---|---|---|
| `base` | nothing (what ships) | `MallocLargeCache=0` |
| `nano` | `MallocNanoZone=0` | `MallocLargeCache=0, MallocNanoZone=0` |
| `aggr` | `MallocAggressiveMadvise=1` | `MallocAggressiveMadvise=1, MallocLargeCache=0` |
| `space` | `MallocSpaceEfficient=1` | `MallocLargeCache=0, MallocSpaceEfficient=1` |
| `sys` | `MallocLargeCache=1` (the system default, no re-exec) | `MallocLargeCache=1` |

`allocator-config.txt` holds `MallocReportConfig=1` for each (what xzone
malloc actually changes).

## Runs

Order (`repro/matrix.sh`), modes interleaved so drift hits all: rounds 1–2
run base, space, nano, aggr (import, idle, typing each) and sys (import);
round 3 runs base and space; then `import0-base-1`.

| Files | Scenario and arguments |
|---|---|
| `import-<mode>-<n>.json` | `--bench import --bench-arg vm_snapshots=1` (defaults `md_mib=8`, `docx_paragraphs=40000`, `idle_seconds=10`, `first_page=1`, `free=1`), no workspace (fresh home); `footprint -p` at `<kind>-shown`, `-closed`, `-after-relief`, `-freed`, `-freed-after-relief` |
| `import0-base-1.json` | same with `first_page=0`: §6e's setup (the first tab without a page) |
| `typing-base-1`, `-2`, `typing-aggr-1`, `-2` | `--bench typing --bench-arg doc=ordinary --bench-arg n=1000` on the same fixture: the windowed typing runs macOS painted for every edit (see below) |
| `offscreen/typing-<mode>-<1..5>.json` | `--bench typing --bench-arg doc=ordinary --bench-arg n=1000` with `QT_QPA_PLATFORM=offscreen`, modes interleaved, 5 rounds: the editor paints every edit into an offscreen buffer, so CPU per edit does not depend on the screen. `typingoff-base-0` was the trial run |
| `idle-<mode>-<n>.json` | `--bench idle --bench-arg seconds=60 --bench-arg vm_snapshots=1` on a copy of the 50,000-page fixture (`folia_fixturegen --seed 42`, `fnv1a64:f5ecfb8eecd9058b`), ordinary page; `footprint -p` at the end |
| `diag/` | the two first runs, `vm_snapshots=2` (`heap -s` too), before the bench had `first_page`/`free`: they show the closed page still alive (83,057 `folia::BlockData`). Both inherited `MallocNanoZone=0` from the Claude app, so `diag/import-base-diag` is the `nano` configuration |

Windowed typing runs are only valid when macOS painted the window for every
edit (1000 `input_to_paint_us` samples): otherwise each edit waits out its
50 ms paint timeout and costs less CPU. Only 4 of the matrix's 10 typing runs
were painted (`base` 1–2, `aggr` 1–2). The unpainted ones are in
`unpainted/`: `typing-base-3`
(87 of 1000), `typing-space-1` to `-3` (674, 40, 23), `typing-nano-1`, `-2`
(41, 33): five of the six were `nano`/`space` runs. To check that the
setting was not the cause, `unpainted/typingab-{nano,base}-{1,2,3}` ran
them alternately right after the matrix: all six were unpainted (21–118), and
so were `typing-space-4`, `typing-nano-4` and `typing-base-4` (100, 85, 23)
after that. The window was not on screen, whatever the setting. Typing CPU
is therefore compared offscreen (`offscreen/`).

Discarded before these runs (not kept): two batches started with an older
build of the bench, one of them still inheriting `MallocNanoZone=0` in
`base`; they were stopped when the closed page turned out to be still alive
(`diag/`), and the bench gained `first_page` and `free`.

## Scripts (`repro/`, as run; paths point at the session's scratch folder)

`run.sh <label> <mode> <scenario> [bench args]` (one run; a gap over 15 s in
the pressure log means the Mac slept, and the run is moved to `invalid/` and
repeated) and `matrix.sh`. The offscreen and alternating typing runs used
`run.sh` with `QT_QPA_PLATFORM=offscreen` (offscreen only) and `OUT` set to
their folder.

`diag/` keeps `heap -s` reports (`vm_snapshots=2`) of `import-base-diag`
only; `import-space-diag` keeps its JSON and `footprint -p` reports.

Later: the tab-close fix (`AppController::closeTab`, docs/BENCHMARKS.md
§6e) made `first_page=0`'s setup unreachable, since an upload now fills an
empty first tab, so the option was removed and the bench always creates
the first page. These runs are unchanged by that.
