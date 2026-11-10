# Upload runs, 2026-11-09

MacBook Air M1 (8 GB), macOS 26.6.2, Release build of branch
`worktree-note-tabs` (Folia 0.2.0 plus tabs and uploads), Qt 6.12.0, on
battery, Low Power Mode off, thermal state nominal, the user working in other
apps. Each run: a fresh scratch `--home`, the default arguments
(`md_mib=8`, `docx_paragraphs=40000`, `idle_seconds=10`), wrapped in
`caffeinate -d -i`; the fixtures are written by `fixtures::makeUploadSet`
at the start of the run.

| File | Used for |
|---|---|
| `import.0.json` | docs/BENCHMARKS.md §6e |
| `import.1.json` | §6e |

Each file is uploaded through `AppController::uploadFiles`, as the dialog
and drops do. After its tab is closed (from the event loop, as a click on ×
is), the run records the sessions left, the footprint 0.5–3 s later, what
`malloc_zone_pressure_relief` hands back (0 bytes in both runs), and the
footprint 5, 10, 15 and 20 s after that.

The `folia_docprobe` figures in §6e (TextEdit's Word file, streamed against
`--whole`) were taken the same day on the same machine, three runs each, in
a fresh process per run; the file was written with
`textutil -convert docx` from a generated HTML page of 40,000 paragraphs.
