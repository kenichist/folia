# Folia

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
  equations render below their source; inline `$…$` shows a preview while the
  caret is inside it. Source always stays editable.
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

## Customize the shell

Shell ▸ **Edit Layout File** opens the active layout; saving it reloads it
live. **Use “keyboard” Layout** switches presets. If a layout has an error,
the last valid layout stays and a banner explains the problem. Shell ▸
**Reset to Default Layout** (Mod+Alt+Shift+R) always works.

## Documentation

- `docs/ARCHITECTURE.md`: interfaces, identities and revisions, durability, threads, caches
- `presets/REFERENCE.md`: shell language reference
- `docs/BENCHMARKS.md`: fixtures and benchmark harness
- `docs/RELEASE_REPORT.md`: results, achieved/missed/unmeasured targets, next fix
