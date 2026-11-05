# Folia architecture

Folia is a local notes app: nested pages, LaTeX math and code blocks, with a
window layout written in Lua. Stack: C++17, Qt 6 Widgets (Core/Gui/Widgets
only, no browser engine), embedded Lua 5.5, SQLite 3.54. Everything runs in
one process. Background threads: one SQLite writer, one SQLite reader, and one
math worker that exits when idle.

```
           ┌────────────── main thread ─────────────────────────────────────┐
 keys/IME ─► NoteEditor (QTextEdit) ── QTextDocument (page source lines)    │
           │      │                         │ contentsChange                │
           │      │ paint                   ▼                               │
           │      ▼                    PageSession ── takeSaveBatch() ──────┼──► Store writer thread ─► SQLite (WAL)
           │  VirtualLayout ── preview request ───────────────────────────────► PreviewService worker ─► math::render
           │      ▲   (lazy per-block layout, estimated heights, eviction)  │
           │      └── code::highlightLine (visible lines only)              │
           │                                                                │
           │  ShellHost (Lua) ─ reconcile ─► native widgets (lists, buttons, inputs, editor slot)
           │      ▲ view() on load / state change / page change only        │
           │  CommandRegistry ◄── keys, buttons, menus, ctx.run()           │
           │  DataContext ◄── page metadata ◄── Store reader thread ◄───────┼──  SQLite (read-only connection)
           └────────────────────────────────────────────────────────────────┘
```

## 1. Frozen v1 interfaces (`src/core/Api.h`, `src/core/Types.h`)

| Call | Owner | Contract |
|---|---|---|
| `openPage(id, ctx, done)` | Store (reader) | One page's blocks in order, plus title, revision and persisted selection. Descendants are never loaded. |
| `fetchChildren(parent, offset, limit, ctx, done(rows, total))` | Store (reader) | One page of child metadata in sibling order. `total` sizes scroll bars without loading rows. |
| `applyEdit(Edit)` | PageSession | Programmatic edit on the open page. Returns the new revision. User typing goes through the native control and uses the same revision counter. |
| `commit(SaveBatch)` | Store (writer) | Make a batch durable. Batches still queued for the same page are coalesced (newer wins). |
| `renderPreview(req, ctx, done)` | PreviewService | Source plus revision in, display list out. Supersedes any outstanding request for that block, and only the newest request's callback runs. Optional additions: `lookup` (cache probe) and `renderNow` (render on the calling thread when the renderer is warm and within a time limit). |
| `dispatchCommand(name, args)` | CommandRegistry | Validated: unknown names, unknown arguments, missing or wrong-typed arguments are all rejected. Used by Lua, menus, buttons and keys alike. |

Callbacks are delivered on the context object's thread. If the context has been
destroyed by then, the callback is dropped. No callback ever runs Lua.

## 2. Identity and revisions

- **PageId / BlockId**: positive 64-bit integers. They are allocated in-process
  from `max(id)+1` with atomic counters, so a new page can become editable
  without a database round-trip. They are never reused. A lock file
  (`workspace.db.lock`) enforces one writer process.
- **Lines vs blocks.** The editor holds a page as plain source lines in one
  `QTextDocument`. Each line carries a `BlockData` with an id. A stored block
  is a *segment*:
  - a paragraph is one line;
  - a code block runs from its ```` ```lang ```` line to its ```` ``` ```` line;
  - display math runs from its `$$` line to its `$$` line, or is a single `$$…$$` line;
  - a child-page link is one line holding a U+FFFC object.

  A segment takes the id of its first line.
- **Stable through edits.** Splitting a line keeps the id on the first part
  (Qt keeps the user data on the original block). Joining keeps the first
  line's id. An unclosed fence is stored as plain text lines, so it
  round-trips byte for byte.
- **Roles are derived from text** by a fence state machine kept in
  `QTextBlock::userState`. After an edit, roles are recomputed forward only
  until a line's role is unchanged ("propagate until it stabilizes"). The
  syntax-highlight carry state works the same way, limited to 4096 lines per
  edit; any lines beyond that are recomputed lazily when they become visible.
- **Revision**: a per-page counter that strictly increases.
  - Every text edit or title change bumps it.
  - IME preedit updates do not.
  - Storage persists the last committed revision, so the counter keeps
    increasing across restarts.
  - "Saved" is shown only when `committedRevision == revision` and nothing is
    left unsnapshotted.
- **Selection** is stored as (segment id, offset) for both anchor and position,
  plus scroll position. It is restored on reopen.
- **Previews** are keyed by a hash of the source and the point size. A result is
  applied only if the hash still equals the hash of the block's current source.
  A stale preview can never replace current content.

## 3. Save pipeline

1. **Edit.** `contentsChange` marks the touched lines dirty and bumps the
   revision. This costs O(touched lines) and copies no text.
2. **Coalesce.** A 300 ms idle timer and a 900 ms deadline timer. The
   deadline starts at the first dirty edit and is not restarted, so no edit
   stays dirty longer than about 1 s under healthy storage. The measured
   maximum is 950 ms.
3. **Snapshot** (`takeSaveBatch`, main thread). Walks the page's lines
   (O(lines), with no layout). Only dirty segments have their text extracted.
   The batch's order list is included only when the structure changed. Pages
   being left are snapshotted at navigation time.
4. **Writer thread.** One transaction per job (`BEGIN IMMEDIATE … COMMIT`),
   strictly FIFO.
   - Upserts write text only.
   - Order is applied with sparse `ord` keys (gap 65536). The longest
     increasing subsequence keeps its keys and the rest are placed into gaps,
     so inserting one block touches about one row. Siblings are renumbered
     only when a gap is exhausted.
5. **Report.** `committed(page, rev)` is emitted, or `commitFailed`. On
   failure the job, with its data, stays at the head of the queue and is
   retried with backoff from 250 ms to 4 s. Newer batches for the same page
   are merged into it, so a full disk never drops pending edits. The UI shows
   the error text, and the editor keeps the text.
6. **Close.** Everything is flushed. If edits cannot be written, a dialog
   offers Retry, Keep Folia Open (the text stays recoverable) or Quit.

### Durability configuration

| Setting | Value | Why |
|---|---|---|
| `journal_mode` | WAL | Readers never block the writer. |
| `synchronous` (writer) | FULL | The WAL is fsync'd on every commit, so a committed revision survives an OS crash. At about one commit per second the cost is under 1 ms on the background thread (commit p50 about 0.3 ms). |
| `checkpoint_fullfsync` | ON (macOS) | Checkpoints use `F_FULLFSYNC` (survives power loss). Per-commit `F_FULLFSYNC` is not used: on macOS, `fsync` without it is not power-loss-proof for the last commits. This is documented as accepted risk. |
| `journal_size_limit` | 4 MiB | Bounds WAL growth after checkpoints. |
| `cache_size` | 2 MiB per connection | Memory cap. |
| `mmap_size` | 0 | Keeps footprint accounting simple. |

No separate application journal is used: no requirement needed one.

## 4. Editor and layout

- `QTextEdit` provides IME, selection, undo/redo, the clipboard (plain text
  only) and keyboard traversal. `VirtualLayout` replaces Qt's document
  layout:
  - It lays out only the blocks that are painted, hit-tested or asked for
    geometry.
  - Every other block has an estimated height.
  - It keeps a lazily extended prefix sum of block heights.
  - It evicts laid-out blocks more than about 150 blocks away from the
    viewport once more than 600 blocks are laid out, keeping their measured
    heights.
  - The caret block and an active IME preedit are never evicted.
- Fonts and colors for headings, code, math and inline markup are applied as
  `QTextLayout` additional formats at layout time, so the document stays
  plain source. IME preedit formats are preserved by filtering on a marker
  property.
- `QTextEdit::setDocument` sends a whole-document relayout request. With an
  unchanged width and block count it is ignored, so warm page switches keep
  their layouts.
- Display math renders below its source. Inline `$…$` renders in place,
  except the formula the caret touches, which shows its source (plus a
  floating preview). Source always stays editable, and errors are shown
  under the source (an inline formula with an error stays as source).
- **Inline formulas in place.** The source characters stay in the line, so
  positions, hit-testing, selection, undo and copy are unchanged. They are
  drawn transparent with letter spacing that makes the span exactly as wide
  as the rendered formula, which `draw()` paints over it; a line holding a
  tall formula gets extra room above/below, and a formula is never split
  across lines. Per-line state is one pointer, allocated only for lines with
  formulas and freed on layout eviction; an unchanged formula keeps its
  result and measurement across relayouts (typing elsewhere on the line).
  Results come from the preview cache; on a miss a warm renderer renders on
  the main thread within a 3 ms budget per paint (a formula takes about
  12 µs), so it appears in the same frame. A cold renderer or an
  over-budget formula goes to the worker, and arrivals are shown in one
  repaint per burst, 40 ms later (an immediate second frame made macOS keep
  an extra full-window buffer).

## 5. Programmable shell

See `presets/REFERENCE.md` (the language reference shipped with the presets).

- The layout is a versioned Lua table with a `view(state, ctx)` function that
  returns a node tree: row, column, overlay, text, button, input, list,
  editor, spacer, divider.
- Nodes are reconciled by stable key (explicit `id`, or a path), so widgets
  survive re-renders.
- There is exactly one `EditorPane`. It is re-parented, never recreated.
- Lists render native, paged data sources (tree, search, recent, children,
  ancestors) from a declarative row template. There are no per-row or
  per-frame Lua callbacks.
- Lua runs only on: load, `ctx.set` from handlers, page changes, system
  light/dark switches (a `mode = "system"` theme is re-applied first), and bound
  user actions such as buttons, keys and input changes in shell inputs. It never
  runs while typing in the editor, scrolling or painting.
- **Sandbox.**
  - Libraries loaded: base, string, table, math, utf8, coroutine.
  - Removed: `load`, `loadfile`, `dofile`, `require`, `collectgarbage`,
    `string.dump`.
  - No `io`, `os`, `package` or `debug`.
  - Budgets per invocation: 24 MiB through the allocator, 5 M instructions,
    250 ms and 20 000 host calls.
  - Trees are limited to 2000 nodes and 40 levels.
- **Recovery.**
  - Any load, render or validation error keeps the last valid layout and
    shows a native banner with a Reset button.
  - A broken layout at startup falls back to the built-in default.
  - Shift-at-launch or `--safe-mode` loads the built-in default.
  - Shell ▸ Reset to Default Layout (Mod+Alt+Shift+R) is native and always
    available.
  - Live reload (file watch) is deferred while the editor has an IME preedit.
- **Shortcuts.** Logical names (`Mod` = Cmd on macOS, Ctrl elsewhere) map to
  platform conventions.
  - Editing and recovery keys are reserved.
  - Conflicts are reported and the first binding wins.
  - Native menus carry no rebindable shortcuts, so a layout owns its key
    bindings.

## 6. Threads, caches and memory

| Thread | Work |
|---|---|
| main | input, layout, paint, Lua, snapshots |
| writer | all SQLite writes |
| reader | queries; superseded title searches are skipped |
| math worker | `QThreadPool` with 1 thread; exits after 2 s idle |

| Cache | Cap |
|---|---|
| Document cache (open pages) | 16 pages / 48 MB (estimated). Pages not yet committed are pinned. LRU eviction. |
| Preview display lists | 4 MiB LRU (byte-sized by `MathDisplayList::byteSize`). |
| Laid-out blocks per document | 600 (evicts glyph data far from the viewport). |
| Title cache | 20 000 entries. |
| SQLite | 2 MiB per connection. |

On macOS memory pressure (dispatch source): clean cached pages are dropped,
the preview cache is trimmed (to 0 when critical), and the layout cap is
lowered. The caret stops blinking after 5 s idle, so an idle window has no
repaint loop.

## 7. Platform layer

`src/app/Platform.h`:

- **macOS** (`platform/mac/MacPlatform.mm`): Mach `task_vm_info.phys_footprint`,
  `task_power_info` CPU and wakeups, thermal state, Reduce Motion and Reduce
  Transparency, memory-pressure source.
- **Linux and Windows** (`app/PlatformPortable.cpp`): RSS or PrivateUsage
  counters, with no-op hooks.

Native materials (Liquid Glass, macOS 26+): with `theme.material = "glass"`
the main window becomes translucent and `platform::applyWindowChrome` places
one `NSGlassEffectView` per `style.material` node, plus a backdrop view in the
theme's `window` color, in the window's frame view *below* Qt's content view.
Qt keeps its single raster backing store and paints every control on top; the
glass shows wherever the layout paints no background. `ShellHost` tracks the
glass nodes' window rects (event filters on them and their ancestors) and
re-places the views on every move, resize or visibility change. Elsewhere,
before macOS 26, offscreen, or with `FOLIA_NO_GLASS` set, nodes draw their
`bg` instead. `theme.titlebar = "transparent"` sets
`Qt::ExpandedClientAreaHint` and an empty unified `NSToolbar` (for its 52 pt
height); presses the layout ignores in that strip move the window
(`QWindow::startSystemMove`). The glass's compositor cost has not been
benchmarked yet.
