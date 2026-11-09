# Folia shell language reference (shell API version 1)

A shell file is a Lua 5.5 chunk that **returns one table**. Folia loads it at
startup and again whenever the file is saved. If it fails to load, the last
valid layout stays on screen with an error banner; **Shell ▸ Reset to Default
Layout** (Mod+Alt+Shift+R) or launching with **Shift held** / `--safe-mode`
always gets you back to the built-in default.

```lua
return {
  version = 1,                 -- required
  name = "My layout",
  theme = { ... },             -- optional, see Theme
  state = { sidebar = true },  -- local interaction state (any Lua values)
  keys = { ["Mod+K"] = ... },  -- key bindings, see Keys
  focus_order = { "jump", "title", "editor" },   -- Tab order by id
  view = function(state, ctx) return ui.row{ ... } end,  -- required
}
```

`view(state, ctx)` returns a tree of nodes. It runs on load, after a handler
changes state, when the current page changes and when the system switches
between light and dark — **never** while typing, scrolling or painting. The tree must contain **exactly one `ui.editor{}`**.

## Nodes

Every node is a table; children are its array part. `nil`/`false` children are
skipped, so `cond and ui.text{...} or nil` works.

| Constructor | Purpose | Main fields |
|---|---|---|
| `ui.row{...}` / `ui.column{...}` | horizontal / vertical container | `gap`, children |
| `ui.overlay{base, popup...}` | first child fills; later children float centered | popup `style.width`, `style.top` |
| `ui.text{}` | label | `text`, or `bind = "status" \| "title" \| "path"` (updated natively), `wrap` |
| `ui.button{}` | push button | `text` and/or `icon` (a Lucide name, see Icons), `command = "page.new"` or `on_click = function(ctx) end` |
| `ui.input{}` | single-line text field | `placeholder`, `value`, `list = "<list id>"`, `on_change(ctx, text)`, `on_submit(ctx, text)`, `on_cancel(ctx)` |
| `ui.list{}` | virtualized page list | `source`, `row`, `activate = "page.open"` or `on_activate(ctx, item)`, `orientation = "horizontal"`, `click_activates`, `select_first`, `follow_current` |
| `ui.editor{}` | the page title + body editor (one instance, re-parented, never recreated); while a PDF is open it shares this slot with the PDF pane, side by side | `style` |
| `ui.tabs{}` | the note tabs, like a browser's: click to switch, × or middle-click to close, drag to reorder, "+" for a new page in a new tab, right-click for Close Others / Close to the Right. One native widget (at most one per layout), parked when a layout leaves it out | `auto_hide` (shown only with 2+ tabs), `tab_min_width` (72), `tab_max_width` (220); `style.bg` (`"sidebar"`), `style.active_bg` (`"background"`), `style.fg`, `style.dim`, `style.hover_bg`, `style.border`, `style.height` (36) |
| `ui.spacer{}` | flexible space | |
| `ui.divider{}` | 1 px line | `vertical = true` |

Tabs: each tab shows one page and has its own back/forward history. Only the
shown tab's page is loaded; the others cost their title. In lists, Mod-click,
middle-click or Mod+Enter on a row opens it in a new tab in the background
(add Shift to show it). A list with `activate = "page.open"` does this
natively; `on_activate(ctx, item)` gets `item.new_tab = true` and
`item.background` and decides itself.

Common fields: `id` (stable identity: nodes with the same id keep their native
widget across renders), `visible`, `autofocus`, `focusable`, `tooltip`,
`label` (accessible name), `style`.

`style`: `bg`, `fg`, `border` (colors: `#rrggbb` or a theme color name such as
`"sidebar"`, `"accent"`, `"dim"`, `"panel"`, `"border"`), `font`, `size`,
`weight` (`"bold"`, `"medium"`, `"light"` or 100–900), `padding`, `gap`,
`radius`, `width`, `height`, `min_width`, `max_width`, `grow` (stretch factor),
`align` (`"left" | "center" | "right"`, text), `icon_size` (button icons, px;
default 16).

### Icons

Button icons are [Lucide](https://lucide.dev/icons) icons drawn natively in the
button's `fg` color. An icon-only button should set `label` (its accessible
name) and usually `tooltip`:

```lua
ui.button{ icon = "trash-2", label = "Delete page", tooltip = "Delete page",
           on_click = function(ctx) ctx.run("page.delete", { confirm = true }) end }
```

Available: `check`, `chevron-down`, `chevron-left`, `chevron-right`, `copy`,
`external-link`, `file-plus`, `file-text`, `globe`, `link`, `panel-left-close`,
`panel-left-open`, `pencil`, `plus`, `rotate-ccw`, `settings`, `trash-2`,
`unlink`, `upload`, `x`. An unknown name
is a layout error (the message lists the available names).

An input with `list = "results"` forwards ↑/↓/PgUp/PgDn/Enter to that list
and feeds its typed text to a `data.search{}` source natively.

### Components

```lua
ui.component("Sidebar", function(props) return ui.column{ ... } end)
-- then: ui.Sidebar{ state = state }
```

## Data sources (read-only, paged, native)

| Source | Rows |
|---|---|
| `data.tree{ root = 0 }` | the page outline; lazily expanded (→/← or click the triangle), paged 200 rows at a time |
| `data.search{ query = "...", limit = 50 }` | title matches (prefix matches first); empty query = recent pages |
| `data.recent{ limit = 20 }` | recently edited pages |
| `data.children{}` | children of the current page |
| `data.ancestors{}` | root … current page (breadcrumbs; use `orientation = "horizontal"`) |

`row` template (declarative, no per-row Lua): `height`, `indent`,
`disclosure`, `icon`, `detail = "count"`, `separator` (horizontal lists),
`selected_bg`, `selected_fg`, `hover_bg`, `dim`, `actions`.

`actions` are icon buttons shown on the row under the mouse (like Notion's
sidebar), drawn natively. Each runs a command with the row's page id passed as
`id_arg` (default `"id"`) plus any fixed `args`:

```lua
row = { actions = {
  { icon = "trash-2", command = "page.delete", args = { confirm = true }, tooltip = "Delete page" },
  { icon = "plus", command = "page.new", id_arg = "parent", tooltip = "Add a page inside" },
} }
```

Unknown icons, unknown commands and invalid arguments are layout errors.

`on_activate(ctx, item)` receives `item = { id, title, depth, row }`.

Tree lists also handle, natively: Mod+Alt+↑/↓ reorder, Mod+Alt+→/← indent/outdent.

## ctx

| Field | Meaning |
|---|---|
| `ctx.state` | the live state table |
| `ctx.set{ k = v }` | merge into state and re-render (handlers only) |
| `ctx.toggle("k")` | flip a boolean state key and re-render |
| `ctx.run(name, args)` | run a command; returns `true` or `false, message` |
| `ctx.focus(id)` | focus a node by id, or `"editor"` / `"title"` |
| `ctx.commands()` | list of command names |
| `ctx.page` | `{ id, title, parent, depth }` of the current page |
| `ctx.platform` | `"macos"`, `"windows"` or `"linux"` |
| `ctx.prefers` | `{ dark, reduced_motion, reduced_transparency }`; `dark` is the appearance Folia shows: the OS's with `mode = "system"`, else the theme's mode |

## Keys

```lua
keys = {
  ["Mod+N"] = "page.new",                         -- one command
  ["Mod+Shift+N"] = { "page.new_child", "title.focus" },   -- composed actions
  ["Mod+O"] = { command = "page.open", args = { id = 1 } },
  ["Mod+\\"] = function(ctx) ctx.toggle("hide_sidebar") end,
}
```

Modifiers: `Mod` (Cmd on macOS, Ctrl elsewhere), `Ctrl` (the physical Control
key), `Cmd`/`Super`, `Alt`/`Option`, `Shift`. Reserved (rejected with a
warning so the editor stays usable): Mod+Z, Mod+Shift+Z, Mod+X/C/V/A, Mod+Q,
Mod+Alt+R (reload), Mod+Alt+Shift+R (reset), Mod+, (Settings). Two bindings that resolve to the
same shortcut are reported; the first one wins.

## Commands

`page.open{id}`, `page.new{parent?, title?}`, `page.new_child{title?}`,
`page.new_sibling`, `page.move{id, parent, index?}`, `page.move_up{id?}`,
`page.move_down{id?}`, `page.indent{id?}`, `page.outdent{id?}`,
`page.delete{id?, confirm?}` (moves to trash; `confirm = true` asks first),
`page.reveal{id?}`, `nav.back`, `nav.forward` (the current tab's own
history), `nav.parent`, `title.focus`,
`editor.focus`, `editor.undo`, `editor.redo`, `code.copy_block`, `link.edit`
(the Text + Link form for the selection or the link at the caret),
`pdf.close` (the PDF pane), `pane.swap` (the PDF pane to the other side of the
note), `pane.focus_other` (between the note and the PDF pane),
`tab.new` (a new page in a new tab), `tab.open{id, background?}`,
`tab.close{index?}`, `tab.select{index}` (1-based; `-1` is the last tab),
`tab.next`, `tab.prev`, `tab.reopen` (the last closed tab), `file.upload`
(the Upload Files dialog: PDFs go into the current note, Markdown and text
files become new pages in tabs; layouts cannot name a file to read). Tab commands with
nothing to do, such as closing the only tab, succeed quietly.
`shell.reload`, `shell.reset`,
`shell.use{name}`, `shell.edit`, `shell.open_folder`, `app.settings` (shows
Settings in the main window).
Arguments are validated: unknown commands/arguments and wrong types fail.

## Theme

```lua
theme = {
  mode = "light",                      -- "light" | "dark" | "system" (follows the OS live)
  font = { family = "Georgia", size = 17 },
  mono = { family = "Menlo", size = 13 },
  editor = { max_width = 720, line_height = 1.1, paragraph_spacing = 8, margin = 48 },
  colors = { accent = "#2f6fdf", background = "#ffffff", text = "#1c1c1e", dim = "#8e8e93",
             sidebar = "#f6f6f8", panel = "#ffffff", border = "#dedee2", selection = "#b3d7ff",
             code_bg = "#f5f5f7", math = "#7846a0" },
  material = "opaque",                 -- "glass" is accepted; opaque fallback is used when unavailable
}
```

Settings (Mod+,) edits `mode`, `colors.accent`, `font`, `mono` and `editor`
without touching this file: its choices are stored in the workspace and
layered over whichever layout is active, key by key. A layout's other values
still apply, and "Use Layout's …" in Settings removes the overrides.

`mode` also sets the app's color scheme, so native controls, menus and
dialogs match it: `"light"` and `"dark"` pin it, `"system"` follows the OS.
Explicit `colors` are kept in either mode; a layout written for light
colors should leave them out (or check `ctx.prefers.dark`) to work in dark.

## Sandbox and limits

Available: `string` (no `dump`), `table`, `math`, `utf8`, `coroutine`, base
functions (no `load`, `loadfile`, `dofile`, `require`, `collectgarbage`),
`print` (goes to the app log). No `io`, `os`, `package` or `debug`, so shell
code cannot touch files or the network.

Limits per invocation: 5 M VM instructions, 250 ms, 24 MiB of Lua memory and
20 000 host calls. Trees are limited to 2 000 nodes and 40 levels.

- Exceeding a limit aborts that call and keeps the previous layout active.
- `pcall`, `xpcall` and `coroutine.resume` cannot catch a budget error.
- A single pattern match (`string.find`, `match`, `gmatch`, `gsub`) is capped
  at 4 M matcher steps.
- `setmetatable` refuses metatables with `__gc`.
- `ctx.set`, `ctx.toggle`, `ctx.run` and `ctx.focus` are errors inside
  `view()`; use `autofocus = true` to focus on render.
- The ids `"editor"` and `"title"` are reserved.
