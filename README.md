<p align="center"><img src="packaging/brand/folia-banner.svg" alt="Folia: programmable, lightweight local notes" width="100%"></p>

A lightweight, local notes app with nested pages, LaTeX math and code blocks.
Its whole window layout is a Lua file you can redesign in any text editor,
without recompiling. C++17 · Qt 6 Widgets · embedded Lua 5.5 · SQLite 3.54 ·
no browser engine.

- **Pages**: nested pages, titles, child-page links inside the parent,
  breadcrumbs, collapse/expand, move/reorder/indent, title jump, back/forward.
  Hovering a page in the sidebar shows Delete (asks first) and Add a page
  inside, as in Notion. Icons are [Lucide](https://lucide.dev), drawn natively.
- **Editing**: native text editing with IME, undo/redo, plain-text clipboard
  and keyboard navigation. Local autosave commits within 1 s; "Saved" is shown
  only once that revision is committed.
- **Math**: native TeX-subset renderer (`docs/MATH_SUBSET.md`). Display
  equations render below their source. Inline `$…$` renders in place; the
  formula the caret touches shows its source (with a preview bubble), as in
  Obsidian. Source always stays editable.
- **Links**: bare URLs are clickable, and named links work like Google Docs:
  select text and press Mod+K (Text + Link), or paste an address onto a
  selection. Clicking a link shows its address with Open / Copy / Edit /
  Remove; Mod-click opens it. Stored as Markdown `[text](url)`.
- **Code**: fenced code blocks with lazy highlighting for 19 languages
  (`docs/CODE_LANGUAGES.md`), a Copy button and `Mod+Shift+C`.
- **Shell**: `presets/default` (sidebar), `presets/keyboard` (keyboard-first:
  no sidebar, picker and outline overlays) and `presets/examples/right-nav`
  (customization example). Language reference: `presets/REFERENCE.md`.

## Build and run (macOS, tested on an M1 MacBook Air with macOS 26.6.2)

```sh
brew install cmake ninja qtbase
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qtbase
cmake --build build
open build/src/Folia.app            # or: build/src/Folia.app/Contents/MacOS/Folia
ctest --test-dir build --output-on-failure
sh packaging/macos/package.sh       # self-contained dist/Folia.app + dist/Folia.dmg
```

The minimum macOS is 14.4, set by the Homebrew Qt 6.12 frameworks (minos
14.4, SDK 26.5). For Linux and Windows, see `packaging/linux/README.md` and
`packaging/windows/README.md`.

Data lives in `~/Library/Application Support/Folia/`: `workspace.db`, plus the
`shell/` folder with editable copies of the presets. Useful flags:

| Flag | Effect |
|---|---|
| `--preset keyboard` | use a different preset |
| `--shell file.lua` | use a specific layout file |
| `--safe-mode` (or hold Shift at launch) | built-in default layout |
| `--no-math` | do not load the formula renderer |
| `--home dir` | separate data folder |

## Settings

Folia ▸ **Settings…** (File ▸ Settings… on Linux and Windows; Mod+, or the
gear at the bottom of the sidebar) opens Settings inside the main window, in
place of your notes; **‹ Notes**, Esc or Mod+W goes back, and so does opening
any page. Its sections:

- **General**: reopen the last page at startup; where your data lives.
- **Appearance**: Light / Dark / Match System, accent color, text and code
  fonts and sizes.
- **Editor**: text width, line height, paragraph spacing, side margin; math
  rendering on/off (applies at the next launch).
- **Layout**: switch layouts, edit or reload the layout file, live reload,
  the shell language reference, reset to the default layout.

Changes apply immediately and are stored in the workspace, layered over the
active layout's `theme` — the Lua file is never rewritten. "Use Layout's …"
buttons remove them. Launch flags (`--no-math`, `--no-watch`) win for that
run. Light, Dark and Match System also set the app's native controls,
menus and dialogs to match. Cost (M1, `--bench settings`): nothing while
closed; first open about 80 ms, reopening 36 ms; +3.5 MB while open and
+6.6 MB kept after the first open, with no growth over 30 opens; a text-size
change takes under 1 ms of main-thread work (~32 ms to the screen), a
light/dark switch about 17–21 ms (~48 ms to the screen).

## Customize the shell

Shell ▸ **Edit Layout File** opens the active layout; saving it reloads it
live. **Use “keyboard” Layout** switches presets. If a layout has an error,
the last valid layout stays and a banner explains the problem. Shell ▸
**Reset to Default Layout** (Mod+Alt+Shift+R) always works.

## Resource usage

Latest valid run (`bench/results/m1-2026-11-04/`): MacBook Air M1 (8 GB),
macOS 26.6.2 (25G83), Release build, Qt 6.12.0, on battery with Low Power
Mode off, thermal state nominal unless noted. The window was 1180×765 pt at
2×, and the fixture was a 50,000-page workspace (seed 42). The "ordinary
page" has 100 blocks (about 25 KiB) with formulas and code. Counters cover
Folia's own process only, so WindowServer is not included; MB = 10⁶ bytes.
Runs disturbed by a covered window, a sleeping Mac or user input were rerun,
and the originals are set aside (`PROVENANCE.md`). These runs predate the
first commit (`environment.json` says "uncommitted working tree"), so they
measure the 0.1.0 code. Links, icons, in-place inline math and Settings came
later and were not re-run; Settings has its own numbers above.

| Measure | Result | Counter | Date |
|---|---|---|---|
| Idle footprint, 60 s after a 5 s settle | median 80.2 MB with the default layout, 79.2 MB with keyboard, 76.0 MB with an editor-only layout and `--no-math`; 114.2 MB with the 10 MiB stress note open. Misses the 60 MB stretch target | `task_vm_info.phys_footprint`, 1 s samples | 2026-11-05 |
| Idle CPU, same runs | mean 0.16–0.27% of one core (7 runs; target < 1%) | `task_power_info` user + system time | 2026-11-05 |
| Idle wakeups, same runs | 5.2–5.7/s mean, 5/s median. Includes the bench driver's own 1 Hz sampler and 4 Hz window probe; app-only wakeups are not isolated | `task_power_info` interrupt + platform-idle wakeups | 2026-11-05 |
| Typing CPU, 1000 edits in 16.8 s on the ordinary page | 5.0–5.4 ms per edit, all threads (3 runs); 3.0 ms in a 1 MiB code block | `task_power_info` user + system ÷ edits | 2026-11-04 |
| Typing wakeups, same runs | 18–37 per run; interrupt wakeups are not isolated | `task_platform_idle_wakeups` | 2026-11-04 |
| Peak footprint | 118.9–119.1 MB over a typing run (launch, open, 1000 edits); 214 MB in the 10 MiB stress-note test (open, jump to end and back, select all, copy) | `task_vm_info.ledger_phys_footprint_peak` | 2026-11-04 |
| Launch → editable ordinary page | 375–404 ms (4 warm launches); 1406 ms for the first launch after a rebuild. Footprint 2 s later: 80.0–80.5 MB | kernel process start (`p_starttime`) to end of the first editor paint; `phys_footprint` | 2026-11-05 |
| 500 page switches in 5 batches of 100 | footprint after each batch: 107.2 → 110.0 → 110.5 → 110.6 → 110.7 MB, a plateau; document cache 3.7 MB of its 48 MiB cap. Thermal state fair | `phys_footprint` after settling | 2026-11-04 |

For scale, an empty Qt Widgets window idles at 47.0 MB on the same counter.
On disk, the 0.1.0 `dist/Folia.app` is 68 MB and `dist/Folia.dmg` is 30 MB.
Windows and Linux reference hardware are not measured yet: Linux builds and
passes its tests in a container, and Windows is not built. For per-run tables,
proxies and open items, see [`docs/RELEASE_REPORT.md`](docs/RELEASE_REPORT.md),
[`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) and
[`bench/results/m1-2026-11-04/REPORT.md`](bench/results/m1-2026-11-04/REPORT.md).

## Documentation

- `docs/ARCHITECTURE.md`: interfaces, identities and revisions, durability, threads, caches
- `presets/REFERENCE.md`: shell language reference
- `docs/BENCHMARKS.md`: fixtures and benchmark harness
- `docs/RELEASE_REPORT.md`: results, achieved/missed/unmeasured targets, next fix
- `packaging/brand/`: app icon and banner, drawn from hand-placed vertices;
  `swift packaging/brand/make_brand.swift` regenerates them
