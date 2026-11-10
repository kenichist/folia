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
        if item.new_tab then   -- Mod-click, middle-click or Mod+Enter
          ctx.run("tab.open", { id = item.id, background = item.background })
        else
          ctx.run("page.open", { id = item.id })
        end
        ctx.focus("editor")
      end,
      style = { bg = "sidebar", grow = 1 },
    },

    ui.list{
      id = "outline",
      visible = not searching,
      source = data.tree{},                  -- lazily paged, expand with Right/click
      row = {
        height = 27, indent = true, disclosure = true, icon = false, detail = "count",
        -- Shown on the row under the mouse (drawn natively, no Lua per row).
        -- The row's page id is passed as `id_arg` (default "id").
        actions = {
          { icon = "trash-2", command = "page.delete", args = { confirm = true }, tooltip = "Delete page" },
          { icon = "plus", command = "page.new", id_arg = "parent", tooltip = "Add a page inside" },
        },
      },
      activate = "page.open",
      style = { bg = "sidebar", grow = 1, size = 13 },
    },

    ui.divider{},
    ui.row{
      style = { padding = 6, gap = 2 },
      ui.button{ icon = "plus", text = "New page", command = "page.new", tooltip = "New page (Mod+N)",
                 style = { fg = "dim", size = 12, icon_size = 14, padding = 5 } },
      ui.spacer{},
      ui.button{ id = "open_settings", icon = "settings", label = "Settings", command = "app.settings",
                 tooltip = "Settings (Mod+,)", style = { fg = "dim", width = 30, height = 26, icon_size = 15 } },
    },
  }
end)

-- Icons are Lucide names (https://lucide.dev/icons); see REFERENCE.md.
local icon_button = { fg = "dim", width = 30, height = 28 }

ui.component("TopBar", function(p)
  local hidden = p.state.hide_sidebar
  return ui.row{
    id = "topbar",
    style = { padding = 6, gap = 2, height = 40 },
    ui.button{
      id = "toggle_sidebar",
      icon = hidden and "panel-left-open" or "panel-left-close",
      label = hidden and "Show sidebar" or "Hide sidebar",
      tooltip = (hidden and "Show sidebar" or "Hide sidebar") .. " (Mod+\\)",
      on_click = function(ctx) ctx.toggle("hide_sidebar") end,
      style = icon_button,
    },
    ui.button{ icon = "chevron-left", label = "Back", command = "nav.back", tooltip = "Back (Mod+[)", style = icon_button },
    ui.button{ icon = "chevron-right", label = "Forward", command = "nav.forward", tooltip = "Forward (Mod+])", style = icon_button },
    ui.list{
      id = "breadcrumbs",
      orientation = "horizontal",
      source = data.ancestors{},
      row = { height = 26, separator = "  ›" },
      activate = "page.open",
      style = { grow = 1, size = 13 },
    },
    ui.text{ id = "status", bind = "status", style = { fg = "dim", size = 12, padding = 6 } },
    ui.button{ id = "upload", icon = "upload", label = "Upload files", command = "file.upload",
               tooltip = "Upload PDF, Word or Markdown files (Mod+O)", style = icon_button },
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
    ["Mod+K"]        = "link.edit",                -- insert/edit a link (Text + Link)
    ["Mod+Shift+C"]  = "code.copy_block",
    ["Mod+Alt+Up"]   = "page.move_up",
    ["Mod+Alt+Down"] = "page.move_down",
    ["Mod+Alt+Right"] = "page.indent",
    ["Mod+Alt+Left"] = "page.outdent",
    ["Mod+Shift+O"]  = function(ctx) ctx.set{ hide_sidebar = false }; ctx.focus("outline") end,
    ["Mod+O"]          = "file.upload",               -- PDFs into this note; Markdown becomes a page
    -- Tabs (one page each; background tabs load only when shown).
    ["Mod+T"]          = "tab.new",
    ["Mod+W"]          = "tab.close",
    ["Mod+Shift+T"]    = "tab.reopen",
    ["Ctrl+Tab"]       = "tab.next",
    ["Ctrl+Shift+Tab"] = "tab.prev",
    ["Mod+1"] = { command = "tab.select", args = { index = 1 } },
    ["Mod+2"] = { command = "tab.select", args = { index = 2 } },
    ["Mod+3"] = { command = "tab.select", args = { index = 3 } },
    ["Mod+4"] = { command = "tab.select", args = { index = 4 } },
    ["Mod+5"] = { command = "tab.select", args = { index = 5 } },
    ["Mod+6"] = { command = "tab.select", args = { index = 6 } },
    ["Mod+7"] = { command = "tab.select", args = { index = 7 } },
    ["Mod+8"] = { command = "tab.select", args = { index = 8 } },
    ["Mod+9"] = { command = "tab.select", args = { index = -1 } },   -- the last tab
  },

  focus_order = { "jump", "outline", "title", "editor" },

  view = function(state, ctx)
    return ui.row{
      id = "root",
      ui.Sidebar{ state = state, visible = not state.hide_sidebar },
      ui.divider{ vertical = true, visible = not state.hide_sidebar },
      ui.column{
        id = "main",
        style = { grow = 1, bg = "background" },
        ui.tabs{},                 -- the note tabs, like a browser's (one native widget)
        ui.TopBar{ state = state },
        ui.editor{},
      },
    }
  end,
}
