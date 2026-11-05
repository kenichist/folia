-- Minimal shell: only the editor (used as the "editor with a real note"
-- stage in staged memory/launch measurements).
return { version = 1, name = "Minimal", view = function() return ui.column{ ui.editor{} } end }
