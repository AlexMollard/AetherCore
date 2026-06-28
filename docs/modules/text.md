# `text/` - Removed UI text renderer

The old `src/engine/text` font atlas and text renderer were only used by the deleted custom UI stack and have been removed.

Dear ImGui owns its own font atlas through `ImguiSubsystem`. Future runtime text should be implemented as part of the replacement runtime UI stack rather than reviving the old `TextRenderer`.
