# `text/` - Font atlas and text renderer

`text/` provides FreeType-backed font rasterization and a small text primitive renderer. Used by the in-engine UI and by debug overlays.

## Files

| File | Role |
|---|---|
| `FontAtlas.hpp` / `FontAtlas.cpp` | FreeType-driven font atlas. |
| `TextRenderer.hpp` / `TextRenderer.cpp` | Draws text using a quad primitive. |

## `FontAtlas`

`src/engine/text/FontAtlas.hpp`. Loads a TTF/OTF font and rasterizes a fixed set of glyphs at a chosen size into a single texture atlas. The atlas is registered as a bindless sampled image and addressed by glyph index.

The atlas is built once at `UISubsystem` init time and re-built if the font or size changes.

## `TextRenderer`

`src/engine/text/TextRenderer.hpp`. Issues per-glyph quad draws into a `CommandRecorder`. Used by:

- `UISubsystem` for UI text.
- `DebugLayer` for debug overlays.

## See also

- [`modules/ui.md`](ui.md) - UI text.
- [`modules/gpu.md`](gpu.md) - bindless textures.
