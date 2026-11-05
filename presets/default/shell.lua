-- Folia default shell (shell API version 1).
--
-- This file is the whole window layout. It is plain Lua using the public API
-- documented in REFERENCE.md: copy it, edit it in any text editor and save —
-- Folia reloads it live. If it breaks, the last valid layout stays active and
-- Shell ▸ Reset to Default Layout (Mod+Alt+Shift+R) always restores this file.

ui.component("Sidebar", function(p)
  local state = p.state
  local searching = state.query ~= nil and state.query ~= ""
  return ui.column{
    id = "sidebar",
    visible = p.visible ~= false,
    style = { bg = "sidebar", width = 264 },

    ui.row{
      style = { padding = 10 },
      ui.input{
        id = "jump",
        placeholder = "Jump to page…   Mod+P",
        list = "jump_results",               -- Up/Down/Enter drive that list natively
        on_change = function(ctx, text) ctx.set{ query = text } end,
        on_cancel = function(ctx) ctx.set{ query = "" }; ctx.focus("editor") end,
        style = { bg = "panel", radius = 7, padding = 6, border = "border", size = 13 },
      },
    },

    -- Title jump results replace the outline while a query is typed.
    ui.list{
      id = "jump_results",
      visible = searching,
      source = data.search{ query = state.query or "", limit = 60 },
      row = { height = 28, icon = true, disclosure = false, indent = false },
      on_activate = function(ctx, item)
        ctx.set{ query = "" }
        ctx.run("page.open", { id = item.id })
        ctx.focus("editor")
      end,
      style = { bg = "sidebar", grow = 1 },
    },

    ui.list{
      id = "outline",
      visible = not searching,
      source = data.tree{},                  -- lazily paged, expand with Right/click
      row = { height = 27, indent = true, disclosure = true, icon = false, detail = "count" },
      activate = "page.open",
      style = { bg = "sidebar", grow = 1, size = 13 },
    },

    ui.divider{},
    ui.row{
      style = { padding = 6, gap = 2 },
      ui.button{ text = "＋ New page", command = "page.new", tooltip = "Mod+N", style = { fg = "dim", size = 12 } },
      ui.spacer{},
      ui.button{ text = "＋ Child", command = "page.new_child", tooltip = "Mod+Shift+N", style = { fg = "dim", size = 12 } },
    },
  }
end)

ui.component("TopBar", function(p)
  return ui.row{
    id = "topbar",
    style = { padding = 6, gap = 4, height = 40 },
    ui.button{ text = "‹", command = "nav.back", tooltip = "Back (Mod+[)", style = { fg = "dim", size = 16, width = 28 } },
    ui.button{ text = "›", command = "nav.forward", tooltip = "Forward (Mod+])", style = { fg = "dim", size = 16, width = 28 } },
    ui.list{
      id = "breadcrumbs",
      orientation = "horizontal",
      source = data.ancestors{},
      row = { height = 26, separator = "  ›" },
      activate = "page.open",
      style = { grow = 1, size = 13 },
    },
    ui.text{ id = "status", bind = "status", style = { fg = "dim", size = 12 } },
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
    material = "opaque",       -- native materials are optional; opaque always works
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
      ui.Sidebar{ state = state, visible = not state.hide_sidebar },
      not state.hide_sidebar and ui.divider{ vertical = true } or nil,
      ui.column{
        id = "main",
        style = { grow = 1, bg = "background" },
        ui.TopBar{},
        ui.editor{},
      },
    }
  end,
}
