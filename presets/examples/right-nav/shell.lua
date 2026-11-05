-- Customization example (shell API version 1), written without core edits:
--   1. navigation moves to the RIGHT side of the window,
--   2. the toolbar is gone — its actions live behind one keyboard action:
--      Mod+J opens an action strip with focus on its first button
--      (Tab/Shift+Tab move, Space activates, Mod+J or Esc closes),
--   3. typography changes: a serif body face, larger size, airier lines.

local actions = {
  { label = "New page",  command = "page.new" },
  { label = "New child", command = "page.new_child" },
  { label = "Back",      command = "nav.back" },
  { label = "Parent",    command = "nav.parent" },
  { label = "Delete",    command = "page.delete", args = { confirm = true } },
}

ui.component("ActionStrip", function(p)
  local row = { id = "actions", visible = p.open, style = { bg = "sidebar", padding = 6, gap = 8 } }
  for i, a in ipairs(actions) do
    row[#row + 1] = ui.button{
      id = "act_" .. i,
      text = a.label,
      autofocus = (i == 1),
      focusable = true,
      on_click = function(ctx) ctx.set{ actions = false }; ctx.run(a.command, a.args) end,
      style = { fg = "accent", size = 13 },
    }
  end
  row[#row + 1] = ui.spacer{}
  return ui.row(row)
end)

return {
  version = 1,
  name = "Right navigation (example)",

  theme = {
    mode = "light",
    font = { family = "Georgia", size = 17 },
    mono = { family = "Menlo", size = 13 },
    editor = { max_width = 680, line_height = 1.18, paragraph_spacing = 10, margin = 48 },
    colors = { accent = "#b0541b", sidebar = "#f7f3ee", background = "#fffdf9", text = "#2b2420" },
  },

  state = { actions = false },

  keys = {
    ["Mod+J"] = function(ctx) ctx.toggle("actions") end,   -- replaces the toolbar
    ["Escape"] = function(ctx) ctx.set{ actions = false }; ctx.focus("editor") end,
    ["Mod+N"] = "page.new",
    ["Mod+Shift+N"] = "page.new_child",
    ["Mod+["] = "nav.back",
    ["Mod+Shift+Up"] = "nav.parent",
    ["Mod+K"] = "link.edit",
  },

  view = function(state, ctx)
    return ui.row{
      id = "root",
      ui.column{
        id = "main",
        style = { grow = 1, bg = "background" },
        ui.ActionStrip{ open = state.actions },
        ui.editor{},
      },
      ui.divider{ vertical = true },
      ui.column{
        id = "nav",
        style = { bg = "sidebar", width = 250 },
        ui.text{ bind = "path", wrap = true, style = { fg = "dim", size = 12, padding = 10 } },
        ui.list{ id = "outline", source = data.tree{}, row = { height = 28, indent = true, disclosure = true },
                 activate = "page.open", style = { grow = 1, bg = "sidebar", size = 14 } },
        ui.text{ bind = "status", style = { fg = "dim", size = 11, padding = 8 } },
      },
    }
  end,
}
