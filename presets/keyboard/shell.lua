-- Folia keyboard-first shell (shell API version 1).
--
-- Structurally different from the default: there is no persistent sidebar or
-- toolbar. The editor fills the window; navigation is a page picker (Mod+K)
-- and an outline overlay (Mod+O), both driven entirely from the keyboard.
-- Everything here uses the same public API as the default layout.

local function close_overlays(ctx)
  ctx.set{ picker = false, outline = false, query = "" }
  ctx.focus("editor")
end

ui.component("Picker", function(p)
  return ui.column{
    id = "picker",
    visible = p.open,
    style = { bg = "panel", radius = 10, border = "border", padding = 8, width = 620, top = 70 },
    ui.input{
      id = "picker_input",
      placeholder = "Go to page…  (↑↓ to choose, Enter to open, Esc to close)",
      list = "picker_results",
      autofocus = true,
      on_change = function(ctx, text) ctx.set{ query = text } end,
      on_cancel = close_overlays,
      style = { size = 16, padding = 8, bg = "panel" },
    },
    ui.divider{},
    ui.list{
      id = "picker_results",
      source = data.search{ query = p.query, limit = 40 },
      row = { height = 30, icon = true, indent = false, disclosure = false },
      select_first = true,
      on_activate = function(ctx, item)
        ctx.run("page.open", { id = item.id })
        close_overlays(ctx)
      end,
      style = { height = 330, bg = "panel" },
    },
  }
end)

ui.component("Outline", function(p)
  return ui.column{
    id = "outline_panel",
    visible = p.open,
    style = { bg = "panel", radius = 10, border = "border", padding = 8, width = 460, top = 70 },
    ui.text{ text = "Outline — ←/→ collapse/expand · Mod+Alt+↑/↓ reorder · Enter open · Esc close",
             style = { fg = "dim", size = 11, padding = 4 } },
    ui.list{
      id = "outline",
      source = data.tree{},
      row = { height = 27, indent = true, disclosure = true, detail = "count" },
      click_activates = true,
      autofocus = true,
      on_activate = function(ctx, item)
        ctx.run("page.open", { id = item.id })
        close_overlays(ctx)
      end,
      style = { height = 420, bg = "panel" },
    },
  }
end)

return {
  version = 1,
  name = "Keyboard",

  theme = {
    mode = "light",
    font = { size = 16 },
    mono = { size = 13.5 },
    editor = { max_width = 720, line_height = 1.06, paragraph_spacing = 8, margin = 56 },
    colors = { accent = "#8a3ffc", panel = "#ffffff" },
  },

  state = { picker = false, outline = false, query = "" },

  keys = {
    ["Mod+K"]       = function(ctx) ctx.set{ picker = true, outline = false, query = "" }; ctx.focus("picker_input") end,
    ["Mod+P"]       = function(ctx) ctx.set{ picker = true, outline = false, query = "" }; ctx.focus("picker_input") end,
    ["Mod+O"]       = function(ctx) ctx.set{ outline = true, picker = false }; ctx.focus("outline") end,
    ["Escape"]      = close_overlays,
    ["Mod+N"]       = { "page.new_child" },        -- keyboard-first: new pages nest under the current one
    ["Mod+Shift+N"] = "page.new",
    ["Mod+["]       = "nav.back",
    ["Mod+]"]       = "nav.forward",
    ["Mod+Shift+Up"] = "nav.parent",
    ["Mod+L"]       = "title.focus",
    ["Mod+Shift+C"] = "code.copy_block",
    ["Mod+Shift+K"] = "link.edit",                -- Mod+K is the page picker here
  },

  focus_order = { "title", "editor" },

  view = function(state, ctx)
    return ui.overlay{
      id = "root",
      ui.column{
        id = "page",
        style = { bg = "background" },
        ui.row{
          id = "topline",
          style = { padding = 8, height = 30 },
          ui.text{ bind = "path", style = { fg = "dim", size = 12 } },
          ui.spacer{},
          ui.text{ bind = "status", style = { fg = "dim", size = 12 } },
          ui.text{ text = "   Mod+K go to · Mod+O outline", style = { fg = "dim", size = 12 } },
        },
        ui.editor{},
      },
      ui.Picker{ open = state.picker, query = state.query },
      ui.Outline{ open = state.outline },
    }
  end,
}
