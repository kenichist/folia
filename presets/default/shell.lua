-- Folia default shell (shell API version 1).
--
-- This file is the whole window layout. It is plain Lua using the public API
-- documented in REFERENCE.md: copy it, edit it in any text editor and save —
-- Folia reloads it live. If it breaks, the last valid layout stays active and
-- Shell ▸ Reset to Default Layout (Mod+Alt+Shift+R) always restores this file.
--
-- Liquid Glass: on macOS 26 and later, nodes with style.material = "glass"
-- (the sidebar and the toolbar buttons) float on native glass, and the title
-- bar is merged into the layout, with the window buttons inside the sidebar.
-- Where glass is unavailable the same nodes draw their `bg` instead, so the
-- layout looks the same apart from the glass. Glass shows only where nothing
-- above it paints a background (here, nothing but the editor does).

ui.component("Sidebar", function(p)
  local state, ctx = p.state, p.ctx
  local searching = state.query ~= nil and state.query ~= ""
  -- Room for the window buttons when the title bar is merged (0 otherwise).
  local header = math.max(0, ctx.titlebar.height - 8)
  return ui.column{
    id = "sidebar",
    visible = p.visible ~= false,
    style = { width = 280, padding = 8 },      -- the panel floats 8 px inside the window

    ui.column{
      id = "sidebar_panel",
      style = { material = "glass", bg = "sidebar", radius = 18, grow = 1 },

      ui.row{ visible = header > 0, style = { height = header } },

      ui.row{
        style = { padding = 10 },
        ui.input{
          id = "jump",
          placeholder = "Jump to page…   Mod+P",
          list = "jump_results",               -- Up/Down/Enter drive that list natively
          on_change = function(ctx, text) ctx.set{ query = text } end,
          on_cancel = function(ctx) ctx.set{ query = "" }; ctx.focus("editor") end,
          style = { bg = "fill", radius = 15, height = 30, padding = 6, size = 13 },
        },
      },

      ui.column{
        style = { padding = 6, grow = 1 },     -- keeps the row pills off the glass edge

        -- Title jump results replace the outline while a query is typed.
        ui.list{
          id = "jump_results",
          visible = searching,
          source = data.search{ query = state.query or "", limit = 60 },
          row = { height = 30, radius = 8, icon = true, disclosure = false, indent = false },
          on_activate = function(ctx, item)
            ctx.set{ query = "" }
            ctx.run("page.open", { id = item.id })
            ctx.focus("editor")
          end,
          style = { bg = "transparent", grow = 1, size = 13 },
        },

        ui.list{
          id = "outline",
          visible = not searching,
          source = data.tree{},                -- lazily paged, expand with Right/click
          row = {
            height = 28, radius = 8, indent = true, disclosure = true, icon = false, detail = "count",
            -- Shown on the row under the mouse (drawn natively, no Lua per row).
            -- The row's page id is passed as `id_arg` (default "id").
            actions = {
              { icon = "trash-2", command = "page.delete", args = { confirm = true }, tooltip = "Delete page" },
              { icon = "plus", command = "page.new", id_arg = "parent", tooltip = "Add a page inside" },
            },
          },
          activate = "page.open",
          style = { bg = "transparent", grow = 1, size = 13 },
        },
      },

      ui.row{
        style = { padding = 8, gap = 2 },
        ui.button{ icon = "plus", text = "New page", command = "page.new", tooltip = "New page (Mod+N)",
                   style = { fg = "dim", size = 12, icon_size = 14, padding = 6, radius = 9 } },
        ui.spacer{},
        ui.button{ id = "open_settings", icon = "settings", label = "Settings", command = "app.settings",
                   tooltip = "Settings (Mod+,)",
                   style = { fg = "dim", width = 30, height = 30, icon_size = 15, radius = 15 } },
      },
    },
  }
end)

-- Icons are Lucide names (https://lucide.dev/icons); see REFERENCE.md.
-- Toolbar buttons are glass capsules, 36 px tall like the system's.
local round_glass = { material = "glass", bg = "fill", fg = "text", radius = 18, width = 36, height = 36, icon_size = 17 }
local in_capsule = { fg = "text", radius = 16, width = 36, height = 32, icon_size = 17 }

ui.component("TopBar", function(p)
  local state, ctx = p.state, p.ctx
  local hidden = state.hide_sidebar
  return ui.row{
    id = "topbar",
    style = { padding = 8, gap = 8, height = math.max(52, ctx.titlebar.height) },
    -- With the sidebar hidden, the window buttons sit at the bar's left.
    ui.spacer{ visible = hidden and ctx.titlebar.inset > 0,
               style = { width = math.max(0, ctx.titlebar.inset - 16), grow = 0 } },
    ui.button{
      id = "toggle_sidebar",
      icon = hidden and "panel-left-open" or "panel-left-close",
      label = hidden and "Show sidebar" or "Hide sidebar",
      tooltip = (hidden and "Show sidebar" or "Hide sidebar") .. " (Mod+\\)",
      on_click = function(ctx) ctx.toggle("hide_sidebar") end,
      style = round_glass,
    },
    ui.row{
      id = "history",
      style = { material = "glass", bg = "fill", radius = 18, height = 36, padding = 2 },
      ui.button{ icon = "chevron-left", label = "Back", command = "nav.back", tooltip = "Back (Mod+[)", style = in_capsule },
      ui.button{ icon = "chevron-right", label = "Forward", command = "nav.forward", tooltip = "Forward (Mod+])", style = in_capsule },
    },
    ui.list{
      id = "breadcrumbs",
      orientation = "horizontal",
      source = data.ancestors{},
      row = { height = 34, radius = 8, separator = "  ›" },
      activate = "page.open",
      style = { bg = "transparent", grow = 1, size = 13 },
    },
    ui.text{ id = "status", bind = "status", style = { fg = "dim", size = 12, padding = 6 } },
  }
end)

return {
  version = 1,
  name = "Default",

  theme = {
    mode = "light",            -- "light" | "dark" | "system"
    font = { size = 15 },      -- family defaults to the system UI font
    mono = { size = 13 },
    editor = { max_width = 760, line_height = 1.0, paragraph_spacing = 7, margin = 40 },
    colors = { accent = "#2f6fdf" },
    material = "glass",        -- Liquid Glass where the system has it (macOS 26+); "opaque" opts out
    titlebar = "transparent",  -- macOS: the layout extends under the title bar
  },

  state = { query = "", hide_sidebar = false },

  keys = {
    ["Mod+P"]        = function(ctx) ctx.set{ hide_sidebar = false }; ctx.focus("jump") end,
    ["Mod+\\"]       = function(ctx) ctx.toggle("hide_sidebar") end,
    ["Mod+N"]        = "page.new",
    ["Mod+Shift+N"]  = { "page.new_child" },
    ["Mod+["]        = "nav.back",
    ["Mod+]"]        = "nav.forward",
    ["Mod+Shift+Up"] = "nav.parent",
    ["Mod+L"]        = "title.focus",
    ["Mod+K"]        = "link.edit",                -- insert/edit a link (Text + Link)
    ["Mod+Shift+C"]  = "code.copy_block",
    ["Mod+Alt+Up"]   = "page.move_up",
    ["Mod+Alt+Down"] = "page.move_down",
    ["Mod+Alt+Right"] = "page.indent",
    ["Mod+Alt+Left"] = "page.outdent",
    ["Mod+Shift+O"]  = function(ctx) ctx.set{ hide_sidebar = false }; ctx.focus("outline") end,
  },

  focus_order = { "jump", "outline", "title", "editor" },

  view = function(state, ctx)
    return ui.row{
      id = "root",
      ui.Sidebar{ state = state, ctx = ctx, visible = not state.hide_sidebar },
      ui.column{
        id = "main",
        style = { grow = 1 },    -- no background: the toolbar's glass shows through
        ui.TopBar{ state = state, ctx = ctx },
        ui.editor{},
      },
    }
  end,
}
