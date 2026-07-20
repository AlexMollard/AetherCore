# 2D Authoring MCP Methods Implementation Plan

> **For agentic workers:** Executed inline (superpowers:executing-plans). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give MCP agents first-class 2D scene-authoring powers — tile painting, tileset/atlas/animation creation, editor camera control, and asset discovery — with every capability mirrored by polished editor UI.

**Architecture:** New `src/app/editor/ControlMethods2D.cpp` appends an `authoring 2D` method group to `BuildControlMethods()` (declared in ControlMethods.hpp). Methods drive the same engine stores the editor panels use (`TileAssetStore::MutableTileMap`, `SpriteAssetStore`, `SpriteAnimationAsset`), so revisions self-heal rendering/collision live and MCP paints land in the Tile Palette's undo stack. UI parity work upgrades the Tile Palette create flow to the standard `spriteui::DrawAssetSlot` picker.

**Tech Stack:** nlohmann::json control methods, existing asset stores, `io::FileSystem::Glob`, `CameraManager`.

---

### Task 1: Control methods (`ControlMethods2D.cpp`)

**Files:**
- Create: `src/app/editor/ControlMethods2D.cpp`
- Modify: `src/app/editor/ControlMethods.hpp` (declare `Append2DAuthoringMethods`)
- Modify: `src/app/editor/ControlMethods.cpp` (call it at the end of `BuildControlMethods`)

Methods (name → MCP tool):

| Method | Tool | Purpose |
|---|---|---|
| `tiles.create` | `create_tile_assets` | Tileset + tilemap from an atlas; optionally bind to an entity's Tile Map component |
| `tiles.info` | `get_tile_map` | Layers, paintable tiles (tileset indices), palette, bounds, cell count |
| `tiles.paint` | `paint_tiles` | Batch cell edits `{x, y, tile?, flipX?, flipY?}`; `tile` = tileset index, omitted = erase; undo-integrated; `save` default true |
| `tiles.fill` | `fill_tiles` | Rect fill/erase without a huge payload |
| `tiles.read` | `read_tiles` | Read cells back (bounded) |
| `tiles.add_layer` | `add_tile_layer` | Append a named layer with collision/sorting settings |
| `atlas.slice` | `slice_atlas` | Grid-slice a texture into a sprite atlas (stable ids on re-slice) |
| `atlas.info` | `get_atlas` | Sprite ids/names/rects for an atlas |
| `animation.create` | `create_sprite_animation` | SpriteAnimationAsset from atlas frame names/indices |
| `editor.camera` | `set_editor_camera` | Position/ortho-height/frame-entity for the edit-mode camera |
| `assets.list` | `list_assets` | `FileSystem::Glob` over `project://` |

- [x] Implement all methods with schemas + mutates flags
- [x] MCP paints push a `TilePaintStroke` onto `TilePaintingState::undo` so Ctrl+Z works on agent edits
- [x] Reconfigure CMake (CONFIGURE_DEPENDS glob) and build

### Task 2: UI parity polish (Tile Palette)

**Files:**
- Modify: `src/app/debug/TilePalettePanel.cpp`

- [x] Replace the typed-path create flow with `spriteui::DrawAssetSlot` (drag-drop + browse + Use Selected)
- [x] Use `spriteui::DrawStatus` for status messages
- [x] Add a Redo button beside Undo

### Task 3: Verification

- [x] Full test suite green
- [x] Live: use only the new methods via aether-ctl to author a demo scene (slice atlas → tileset/tilemap → paint level → camera frame → screenshot)
- [x] Commit + push per task
