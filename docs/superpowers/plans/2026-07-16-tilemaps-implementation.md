# Tilemaps (Roadmap Phase 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Author and run large chunked tile worlds: `TileSetAsset`/`TileMapAsset`/`TileMapComponent`, sparse 32x32 chunks with zstd compression and dirty tracking, camera-culled rendering through the existing sprite instance pipeline, palette/painting/layer tools with chunk-local undo, and incrementally rebuilt Box2D collision.

**Architecture:** Tile cell data lives in a binary `.tilemap` asset (never scene TOML); the scene stores a `TileMapComponent` referencing it. A `TileMapSystem` keeps per-(entity, layer, chunk) caches of `SpriteRenderInstance` arrays, rebuilds only chunks whose asset `revision` changed, culls by chunk bounds against the orthographic camera, and appends into `Render2DFrameData::sprites` before a shared sort — zero new GPU code, tiles and sprites interleave correctly by sort key. Collision mirrors the same dirty tracking: `Physics2DSystem` maintains one static Box2D body per solid chunk built from greedy-merged rectangles.

**Tech Stack:** zstd (already vendored) for chunk compression; toml++ for the tileset; Box2D via the existing Physics2DSystem; ImGui palette panel + viewport painting; doctest.

**Key facts (verified in-code, 2026-07-16):**
- `Render2DFrameData` already has `tileInstances` (unused; this plan appends into `sprites` instead and removes `tileInstances`).
- `SpriteSystem::Extract` clears `output.sprites`, fills, then stable-sorts by `sortKey` — the sort moves to a shared `Finalize2DFrame` so tile extraction can append between fill and sort (`AetherCore.cpp:610` is the call site).
- Sort key layout (`SpriteSystem.cpp:28`): `(biasedLayer << 48) | (biasedOrder << 32) | entityId`. Tiles reuse `MakeSortKey`-equivalent math with the tilemap entity id.
- Engine sources are globbed; only test CMake needs edits for new reflect TUs.
- Feature gating precedent (Phase 3): new domain = flag default for Scene2D + scene-format migration + `RequiredFeatures` on the system + `RequiresFeature` on components + menus filter on ACTIVE features.

---

## File Map

| File | Responsibility |
|---|---|
| `src/engine/assets/TileSetAsset.hpp/.cpp` (create) | Tile definitions (stable ids, atlas region refs, collision flag, animation frames, properties); TOML save/load |
| `src/engine/assets/TileMapAsset.hpp/.cpp` (create) | Sparse chunked cell storage, palette of tile ids, layers, per-chunk revision, binary+zstd save/load, Set/GetCell |
| `src/engine/assets/TileAssetStore.hpp/.cpp` (create) | Cached load/save of tilesets + tilemaps (mirrors SpriteAssetStore) |
| `src/engine/assets/AssetSubsystem.hpp/.cpp` (modify) | Own + register `TileAssetStore`; initialize `TileMapSystem` |
| `src/engine/scene/Components.hpp` (modify) | `TileMapComponent` (path, tint, sorting offsets, layer visibility mask) |
| `src/engine/rendering/TileMapSystem.hpp/.cpp` (create) | Per-chunk instance caches, dirty tracking, camera culling, extraction |
| `src/engine/rendering/SpriteSystem.cpp` + `RenderFramePacket.hpp` + `AetherCore.cpp` (modify) | Lift the sort into `Finalize2DFrame`; camera bounds to tile extract; drop `tileInstances` |
| `src/engine/physics2d/TileMapCollision.hpp/.cpp` (create) | Greedy rect merge (pure fn) + chunk body bookkeeping types |
| `src/engine/physics2d/Physics2DSystem.hpp/.cpp` (modify) | `SyncTileMapCollision(world)` — per-chunk static bodies, dirty-only rebuild |
| `src/app/scene/reflection/MoreComponents.reflect.cpp` (modify) | `TileMapComponent` reflection (`RequiresFeature(Tilemaps)`) |
| `src/app/scene/SceneSerializer.*` (modify) | `tile_map` record (capture/write/read/apply/reset); v15 migration grants `tilemaps` to 2D scenes |
| `src/engine/scene/SceneKind.hpp` (modify) | `DefaultSceneFeatures(Scene2D) += Tilemaps` |
| `src/app/debug/TilePalettePanel.hpp/.cpp` (create) | Tileset browser, tool + layer UI, stroke undo/redo, save |
| `src/app/debug/TilePaintingState.hpp` (create) | Shared painting state service (tool, tile, layer, undo stack) |
| `src/app/debug/ViewportPanel.*` (modify) | `HandleTilePainting` — world->cell mapping, pencil/rect/fill/erase/picker |
| `src/app/debug/HierarchyPanel.cpp` (modify) | 2D create menu gains "Tile Map" |
| `src/app/layers/DebugLayer.cpp` (modify) | Register the palette panel + painting state |
| `tests/assets/TileAssetTests.cpp` (create) | Asset round-trips, cells, chunks, revisions |
| `tests/rendering/TileMapExtractTests.cpp` (create) | Culling + dirty-only rebuild + 100k-tile gate |
| `tests/physics2d/TileCollisionTests.cpp` (create) | Rect merge + incremental collision |

Execution is sliced so every task ends green and pushed. Follow Phase 3 conventions throughout (Expected<> error style, `AE_WARN` categories, tabs, no editor deps in engine).

---

### Task 1: TileSetAsset

**Files:** create `src/engine/assets/TileSetAsset.hpp/.cpp`, test `tests/assets/TileAssetTests.cpp`.

- [ ] **Step 1: Header**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "assets/AssetTypes.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	enum class TileCollisionKind : std::uint8_t
	{
		None = 0,
		Full, // solid unit cell; merged into chunk rectangles
	};

	struct TileDefinition
	{
		AssetObjectId id{};       // stable across tileset edits
		std::string name;
		std::string atlasPath;    // sprite atlas supplying the visuals
		AssetObjectId spriteId{}; // region within the atlas
		TileCollisionKind collision = TileCollisionKind::None;
		// Animated tiles: atlas region ids played at fps; empty = static.
		std::vector<AssetObjectId> animationFrames;
		float animationFps = 8.0f;
		std::map<std::string, std::string> properties; // custom key/values
	};

	struct TileSetAsset
	{
		static constexpr std::uint32_t kSchemaVersion = 1;

		std::uint32_t schemaVersion = kSchemaVersion;
		std::string name;
		float cellSize = 1.0f; // world units per cell
		std::vector<TileDefinition> tiles;

		TileDefinition& AddTile(std::string atlasPath, AssetObjectId spriteId, std::string tileName);
		[[nodiscard]] const TileDefinition* Find(AssetObjectId id) const noexcept;

		[[nodiscard]] Expected<void> Save(const std::filesystem::path& path) const;
		[[nodiscard]] static Expected<TileSetAsset> Load(const std::filesystem::path& path);
	};
} // namespace aether
```

`AddTile` computes the stable id via `ComputeAssetObjectId(ComputeAssetId(MakeTileSetSource(name)), atlasPath + "#" + std::to_string(spriteId.value) + "#" + tileName)` — add `MakeTileSetSource`/`MakeTileMapSource` helpers to `AssetTypes.hpp` beside `MakeSpriteAtlasSource`.

- [ ] **Step 2: TOML save/load** — mirror `SpriteAtlasAsset.cpp` (toml++ tables, VFS-independent `std::filesystem` I/O like the atlas uses, warn-and-default on unknown enum strings). Tables: `[tileset]` header (schema/name/cell_size), `[[tiles]]` array with `id` (int64), `name`, `atlas`, `sprite_id`, `collision` ("none"/"full"), `animation_frames` (int64 array), `animation_fps`, `[tiles.properties]` inline table.
- [ ] **Step 3: Tests** — build a tileset with two tiles (one animated, one solid with properties), Save to a temp dir, Load, CHECK every field including stable ids; `Find` hit/miss; loading a missing file returns an error.
- [ ] **Step 4: Full suite, commit, push** — `Add TileSetAsset with stable tile ids and TOML round-trip`

### Task 2: TileMapAsset (chunked binary storage)

**Files:** create `src/engine/assets/TileMapAsset.hpp/.cpp`, extend tests.

- [ ] **Step 1: Header**

```cpp
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/AssetTypes.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	// Cell encoding: bits 0..15 = palette index + 1 (0 == empty cell),
	// bit 16 = flip X, bit 17 = flip Y. Remaining bits reserved.
	namespace tilecell
	{
		inline constexpr std::uint32_t kEmpty = 0;
		inline constexpr std::uint32_t kIndexMask = 0xFFFFu;
		inline constexpr std::uint32_t kFlipX = 1u << 16u;
		inline constexpr std::uint32_t kFlipY = 1u << 17u;

		[[nodiscard]] constexpr std::uint32_t Make(std::uint16_t paletteIndex, bool flipX = false, bool flipY = false) noexcept
		{
			return (static_cast<std::uint32_t>(paletteIndex) + 1u) | (flipX ? kFlipX : 0u) | (flipY ? kFlipY : 0u);
		}
		[[nodiscard]] constexpr bool Empty(std::uint32_t cell) noexcept { return (cell & kIndexMask) == 0; }
		[[nodiscard]] constexpr std::uint16_t PaletteIndex(std::uint32_t cell) noexcept { return static_cast<std::uint16_t>((cell & kIndexMask) - 1u); }
	} // namespace tilecell

	inline constexpr std::int32_t kTileChunkSize = 32;

	struct TileChunkKey
	{
		std::int32_t x = 0;
		std::int32_t y = 0;
		bool operator==(const TileChunkKey&) const = default;
	};
	struct TileChunkKeyHash
	{
		std::size_t operator()(const TileChunkKey& k) const noexcept
		{
			return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.x)) << 32u) | static_cast<std::uint32_t>(k.y));
		}
	};

	struct TileChunk
	{
		std::array<std::uint32_t, kTileChunkSize * kTileChunkSize> cells{}; // row-major, all kEmpty by default
		std::uint32_t revision = 1; // bumped on every edit; render/collision caches compare
		[[nodiscard]] bool IsEmpty() const noexcept;
	};

	struct TileMapLayer
	{
		std::string name = "Layer";
		bool visible = true;
		bool collision = true;
		float opacity = 1.0f;
		glm::vec4 tint{1.0f};
		std::int32_t sortingLayer = 0;
		std::int32_t orderInLayer = 0;
		std::unordered_map<TileChunkKey, TileChunk, TileChunkKeyHash> chunks; // empty chunks omitted
	};

	struct TileMapAsset
	{
		static constexpr std::uint32_t kFormatVersion = 1;

		std::string tileSetPath;
		float cellSize = 1.0f; // copied from the tileset at bind time
		std::vector<AssetObjectId> tilePalette; // cell indices point here
		std::vector<TileMapLayer> layers;

		// Palette maintenance: returns the palette index for a tile id, appending it if new.
		[[nodiscard]] std::uint16_t PaletteIndexFor(AssetObjectId tileId);

		// World-cell accessors: chunk-sparse, auto-create on write, auto-prune
		// chunks that become empty. SetCell bumps the chunk revision.
		void SetCell(std::size_t layer, glm::ivec2 cell, std::uint32_t value);
		[[nodiscard]] std::uint32_t GetCell(std::size_t layer, glm::ivec2 cell) const noexcept;
		[[nodiscard]] std::size_t TotalCellCount() const noexcept; // non-empty cells

		[[nodiscard]] Expected<void> Save(const std::filesystem::path& path) const;
		[[nodiscard]] static Expected<TileMapAsset> Load(const std::filesystem::path& path);
	};

	// cell -> chunk mapping helpers (floor division for negatives).
	[[nodiscard]] constexpr TileChunkKey ChunkKeyFor(glm::ivec2 cell) noexcept;
	[[nodiscard]] constexpr std::size_t CellIndexInChunk(glm::ivec2 cell) noexcept;
} // namespace aether
```

- [ ] **Step 2: Binary save/load with zstd.** Format: magic `"ATLM"`, u32 version, length-prefixed `tileSetPath`, f32 cellSize, u32 palette count + u64 ids, u32 layer count; per layer: length-prefixed name, u8 visible, u8 collision, f32 opacity, 4x f32 tint, i32 sortingLayer, i32 orderInLayer, u32 chunk count; per chunk: i32 x, i32 y, u32 compressedSize, then `ZSTD_compress` of the 4096-byte cell array (level 3, same as the pak pipeline default). Load validates magic/version and `ZSTD_decompress` sizes; corrupt chunks return an error naming the chunk. Chunk revisions reset to 1 on load (in-memory only, never serialized).
- [ ] **Step 3: Tests** — Set/Get round-trip across chunk borders and negative cells (floor-division mapping pinned: cell {-1,-1} -> chunk {-1,-1} index 33*32-ish, assert exact values); auto-prune (set then clear a lone cell removes its chunk); revision bumps on set, not on identical set; palette dedupe; binary save/load equality for a 3-layer map with flips + negative chunks; `TotalCellCount`.
- [ ] **Step 4: Full suite, commit, push** — `Add TileMapAsset: sparse zstd chunks, palette cells, revisions`

### Task 3: TileAssetStore + AssetSubsystem wiring

**Files:** create `src/engine/assets/TileAssetStore.hpp/.cpp`; modify `AssetSubsystem.hpp/.cpp`.

- [ ] **Step 1:** Store mirrors `SpriteAssetStore` exactly: `LoadTileSet/LoadTileMap` (cached, `Expected<const T*>`), `SaveTileSet/SaveTileMap`, `MutableTileMap(path)` (non-const access for the editor/painting; returns nullptr when not loaded), `Invalidate`, `Clear`. Paths resolve through the same VFS/absolute conventions the sprite store uses (copy its path handling verbatim).
- [ ] **Step 2:** `AssetSubsystem` owns a `TileAssetStore m_tileStore` + `GetTileAssetStore()`, registers it as a service beside `SpriteAssetStore` (find `services.Register<SpriteAssetStore>` and mirror).
- [ ] **Step 3:** Cache-behaviour test (load twice returns the same pointer; Invalidate reloads). Full suite, commit, push — `Add TileAssetStore and register it with the asset subsystem`

### Task 4: TileMapComponent + reflection + serialization + feature gating

**Files:** modify `Components.hpp`, `MoreComponents.reflect.cpp`, `SceneSerializer.hpp/.cpp`, `SceneKind.hpp`; tests in `SceneSerializerTests.cpp`.

- [ ] **Step 1: Component** (in `Components.hpp` near the sprite components):

```cpp
	struct TileMapComponent
	{
		std::string tilemapPath;
		glm::vec4 tint{1.0f};
		std::int32_t sortingLayer = 0;  // added to each layer's own sorting
		std::int32_t orderInLayer = 0;
		std::uint32_t visibleLayerMask = 0xFFFFFFFFu;
		bool visible = true;
	};
```

- [ ] **Step 2: Reflection** (`MoreComponents.reflect.cpp`): `AE_COMPONENT(TileMapComponent, "Tile Map", "Rendering", ICON_FA_TABLE_CELLS)` — verify the icon exists in `Icons.hpp`, fall back to `ICON_FA_IMAGE`; fields `tilemap` (String), `tint` (Color4), `sorting_layer`/`order_in_layer` (Int), `visible_layers` (UInt), `visible` (Bool); `b.RequiresFeature(SceneFeatureFlags::Tilemaps);`.
- [ ] **Step 3: Serializer** — EntityRecord `std::optional<TileMapComponent> tileMap;` + the five integration points (capture/write "tile_map" via `WriteReflectedToToml`/read/apply/reset), copying the Day Night pattern verbatim. Apply implies `SceneFeatureFlags::Tilemaps` (add to the implied-features reconcile beside sprites).
- [ ] **Step 4: Defaults + migration** — `DefaultSceneFeatures(Scene2D)` gains `Tilemaps`; scene format v15 (`kSceneFormatVersion = 15`) with `MigrateTilemapsFeatureToV15` granting `tilemaps` to 2D scenes (copy the v14 migration shape); update the v14 test expectations that assert Scene2D defaults (`Sprites|Physics2D` -> `Sprites|Physics2D|Tilemaps` — the Blank 2D template test, kind-aware default tests, and `resources/scenes/default2d.scene.toml` version/features).
- [ ] **Step 5: Round-trip test** (all authored fields), migration test (v14 2D scene gains tilemaps), full suite, commit, push — `Add TileMapComponent with reflection, serialization, and Tilemaps gating`

### Task 5: TileMapSystem extraction (culling + dirty caches)

**Files:** create `src/engine/rendering/TileMapSystem.hpp/.cpp`; modify `RenderFramePacket.hpp` (remove `tileInstances`), `SpriteSystem.cpp` (drop the trailing sort), `AetherCore.cpp` (finalize call + camera bounds), `AssetSubsystem` (own + initialize the system); test `tests/rendering/TileMapExtractTests.cpp`.

- [ ] **Step 1: Shared finalize.** Add to `RenderFramePacket.hpp`: `void Finalize2DFrame(Render2DFrameData& frame);` (implemented in `SpriteSystem.cpp`: the existing stable sort). `SpriteSystem::Extract` no longer sorts. `AetherCore::PrepareFrame` calls sprite extract, then tile extract, then `Finalize2DFrame(packet.render2D)`. Update `Sprite2DTests.cpp` call sites to finalize after extract (they assert sorted order today).
- [ ] **Step 2: System interface**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "material/TextureHandle.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class SpriteAssetStore;
	class TileAssetStore;
	class TextureRegistry;
	class World;

	// Camera-visible world rect on the XY plane; invalid when no ortho camera.
	struct View2DBounds
	{
		glm::vec2 min{0.0f};
		glm::vec2 max{0.0f};
		bool valid = false;
	};

	// Extracts visible tile chunks into sprite-compatible instances. Caches one
	// instance array per (entity, layer, chunk) keyed by the chunk's revision +
	// the entity transform, so only edited chunks rebuild. Animated tiles patch
	// their UVs during the copy, never the cache.
	class TileMapSystem
	{
	public:
		void Initialize(TextureRegistry& textures, SpriteAssetStore& sprites, TileAssetStore& tiles);
		void Shutdown();
		void Extract(World& world, const View2DBounds& view, float elapsedSeconds, Render2DFrameData& output);

		// Cache eviction for editor invalidation (asset reload, entity destroyed).
		void InvalidateEntity(Entity entity);
		void InvalidateAll();

		// Test/diagnostic: chunk cache rebuilds since construction.
		[[nodiscard]] std::uint64_t RebuildCount() const noexcept { return m_rebuildCount; }

	private:
		struct ChunkCacheKey
		{
			std::uint32_t entity;
			std::uint32_t layer;
			std::int32_t chunkX;
			std::int32_t chunkY;
			bool operator==(const ChunkCacheKey&) const = default;
		};
		struct ChunkCacheKeyHash
		{
			std::size_t operator()(const ChunkCacheKey& k) const noexcept;
		};
		struct AnimatedSlot
		{
			std::uint32_t instanceIndex; // within the cached array
			std::uint32_t tileIndex;     // into the tileset's tiles
		};
		struct ChunkCache
		{
			std::vector<SpriteRenderInstance> instances;
			std::vector<AnimatedSlot> animated;
			std::uint32_t builtRevision = 0;
			glm::mat4 builtTransform{1.0f};
		};

		TextureRegistry* m_textures = nullptr;
		SpriteAssetStore* m_sprites = nullptr;
		TileAssetStore* m_tiles = nullptr;
		std::unordered_map<ChunkCacheKey, ChunkCache, ChunkCacheKeyHash> m_chunks;
		std::unordered_map<std::string, TextureHandle> m_textureCache;
		std::uint64_t m_rebuildCount = 0;
	};
} // namespace aether
```

- [ ] **Step 3: Extract implementation.** For each `TileMapComponent`+`TransformComponent` (excluding `DisabledComponent`, honoring `visible`): load map + tileset; compute the entity's world matrix; for each visible layer (mask + `layer.visible`): for each chunk, compute its world AABB (chunk origin `chunkKey * kTileChunkSize * cellSize` transformed by the entity matrix — conservative AABB of the 4 corners) and skip when disjoint from `view` (when `view.valid`); rebuild the cache when `builtRevision != chunk.revision || builtTransform != entityMatrix`: one `SpriteRenderInstance` per non-empty cell — world matrix = entity matrix translated to the cell centre, `sizeAndPivot = {cellSize, cellSize, 0.5, 0.5}`, uv from the tile's atlas region (inset by the same texel-centre helper the sprite path uses — export `InsetAtlasUvRect` from `SpriteSystem.cpp` into a small shared header or duplicate the 12 lines with a comment), flips from cell bits, `color = layer.tint * component.tint * opacity`, sortKey from biased `(layer.sortingLayer + component.sortingLayer, layer.orderInLayer + component.orderInLayer, entity.id)` (reuse the bias math), `entityId = entity.id` (picking selects the tilemap entity), blend = Alpha. Animated tiles record `AnimatedSlot`; on append, current frame = `frames[size_t(elapsed * fps) % frames.size()]`, patch the copied instance's uv only. Append = `output.sprites.insert(end, cache.instances...)` then patch animated in place.
- [ ] **Step 4: AetherCore wiring.** Compute `View2DBounds` in `PrepareFrame` from the main camera when orthographic (`position +- {height*aspect, height}/2`), else invalid (no culling — 3D/mixed views still render tiles). Call order: sprite extract -> `assetsSub.GetTileMapSystem().Extract(world, bounds, elapsed, packet.render2D)` -> `Finalize2DFrame`.
- [ ] **Step 5: Tests** (headless — FakeTextureSink like the sprite tests): build a tileset+map in a temp dir with a real atlas TOML; scene with one tilemap entity; CHECKs: instance count == non-empty visible cells; a view covering one chunk extracts only that chunk's cells; `SetCell` + re-extract bumps `RebuildCount` by exactly 1 (dirty-only); moving the entity transform rebuilds; flip flags produce flipped instance flags; layer tint multiplies; sort keys interleave with a sprite of the same layer (extract both, finalize, assert order). **100k gate test:** 320x320 filled map (102,400 cells), full-view extract count == 102,400 once, then a second extract with zero edits does zero rebuilds, and a 1-chunk view appends <= 1,024 instances.
- [ ] **Step 6: Full suite, commit, push** — `Add TileMapSystem: chunk-culled, dirty-cached tile extraction`

### Task 6: Tile collision (greedy merge + incremental chunk bodies)

**Files:** create `src/engine/physics2d/TileMapCollision.hpp/.cpp`; modify `Physics2DSystem.hpp/.cpp`; test `tests/physics2d/TileCollisionTests.cpp`.

- [ ] **Step 1: Pure merge function**

```cpp
// TileMapCollision.hpp
struct TileRect
{
	std::int32_t x = 0, y = 0, w = 0, h = 0; // cell units within the chunk
	bool operator==(const TileRect&) const = default;
};
// Greedy row-expand-then-column-expand merge of solid cells (classic greedy
// meshing): deterministic, covers every solid cell exactly once.
[[nodiscard]] std::vector<TileRect> MergeSolidCells(const std::array<std::uint32_t, kTileChunkSize * kTileChunkSize>& cells, const std::function<bool(std::uint32_t cell)>& isSolid);
```

Tests first: full chunk -> 1 rect {0,0,32,32}; one row -> 1 rect; L-shape -> 2 rects; checkerboard 4x4 region -> 8 unit rects; empty -> none.

- [ ] **Step 2: Physics2DSystem integration.** New private member `std::unordered_map<TileBodyKey, TileBodyEntry, ...>` where the key is (entity, layer, chunkX, chunkY) and the entry stores the packed `b2BodyId` + built revision. `void SyncTileMapCollision(World& world)` called from `Update` (before stepping) and `FlushPendingOnly`: for each tilemap entity (+Transform), skip layers with `collision == false`; per chunk: if revision matches the entry, keep; else destroy the old body, run `MergeSolidCells` (solid = palette tile's `TileCollisionKind::Full` from the tileset), create one static body at the chunk origin (entity transform applied: position + Z rotation; the chunk-local rects become `b2MakeOffsetBox` shapes in cell units * cellSize * entity scale). Remove entries whose entity/chunk vanished. The system needs the `TileAssetStore`: add `void SetTileAssets(TileAssetStore* tiles)` wired in `Application.cpp` after the asset subsystem registers (or pull via a `World`-independent pointer set from `AssetSubsystem::Initialize` — pick whichever the existing sprite-animation system uses to reach `SpriteAssetStore` and copy it).
- [ ] **Step 3: Behaviour tests:** a dropped dynamic box rests on a painted tile floor; `SetCell` clearing the cell under it + `FlushPendingOnly` lets a fresh box fall through (old body destroyed); editing one chunk leaves the other chunk's body untouched (body count via `OverlapAabb` probes); merged rect count matches `MergeSolidCells` (probe corners).
- [ ] **Step 4: Full suite, commit, push** — `Add incremental tile collision: greedy-merged static chunk bodies`

### Task 7: Editor — palette panel, painting tools, chunk-local undo

**Files:** create `src/app/debug/TilePalettePanel.hpp/.cpp`, `src/app/debug/TilePaintingState.hpp`; modify `ViewportPanel.hpp/.cpp`, `DebugLayer.cpp`, `HierarchyPanel.cpp`.

- [ ] **Step 1: Painting state service** (plain struct registered by DebugLayer):

```cpp
// TilePaintingState.hpp
enum class TileTool : std::uint8_t { None, Pencil, Rectangle, Fill, Erase, Picker };
struct TilePaintEdit { std::size_t layer; glm::ivec2 cell; std::uint32_t before, after; };
struct TilePaintStroke { std::string tilemapPath; std::vector<TilePaintEdit> edits; };
struct TilePaintingState
{
	TileTool tool = TileTool::None;
	AssetObjectId selectedTile{};
	bool flipX = false, flipY = false;
	std::size_t activeLayer = 0;
	std::vector<TilePaintStroke> undo;   // chunk-local diffs, never map snapshots
	std::vector<TilePaintStroke> redo;
	bool mapDirty = false;               // unsaved asset edits
};
```

- [ ] **Step 2: TilePalettePanel.** Selected-entity driven (needs `TileMapComponent`): tileset path row with a "New Tile Set from Atlas" helper (creates a tileset containing every region of a chosen atlas — reuse the File Explorer asset-selection pattern the Sprite Slicer uses for atlas picking); tile grid rendered with `ImGui::ImageButton` using the atlas texture + region UVs (copy the Sprite Slicer's texture-preview acquisition); tool buttons (pencil/rect/fill/erase/picker) writing `TilePaintingState`; per-tile collision + animation editing (collision combo, frame list add/remove); layer list (add/rename/remove/reorder/visibility/opacity/tint/collision toggles) editing the loaded `TileMapAsset` via `TileAssetStore::MutableTileMap`; Undo/Redo buttons + Ctrl+Z/Ctrl+Y while the panel or viewport is focused and a tool is active (apply stroke inverse via `SetCell`, which bumps revisions so render+collision self-heal); Save button (store `SaveTileMap`/`SaveTileSet`, clears `mapDirty`); dirty indicator in the title.
- [ ] **Step 3: Viewport painting.** `ViewportPanel::HandleTilePainting(context, imageMin, imageSize, renderAspect)` called before picking; active only when: edit mode, Scene2D, ortho camera, a tool selected, selected entity has `TileMapComponent`. Mouse world position -> entity-local -> cell (`floor(local / cellSize)` with the entity's inverse transform). Pencil/Erase: paint on down+drag (record before/after per touched cell once per stroke); Rectangle: click-drag preview outline (reuse the collider-handle line drawing), commit on release filling the rect; Fill: flood fill bounded to the touched chunk plus 8 neighbours with a 64x64 hard cap (log when clamped — no silent caps); Picker: sets `selectedTile` + flips from the clicked cell. Strokes push onto `state.undo`, clear `redo`, set `mapDirty`. Painting captures the mouse exactly like the collider handles (suppress picking; reuse `m_collider2DMouseCapture`-style member `m_tilePaintCapture`). Draw a cell-highlight preview under the cursor and chunk boundary overlay when a tool is active.
- [ ] **Step 4: Creation flow.** Hierarchy 2D submenu gains "Tile Map" (visible with `Tilemaps` feature active): creates an entity with `TransformComponent` + `TileMapComponent`, and writes fresh sibling assets `project://assets/tilemaps/<Name>.tileset.toml` + `<Name>.tilemap` (one default layer), pointing the component at them.
- [ ] **Step 5: Manual verification via MCP** (scene load/save round-trip of a painted map; play mode collision on painted floor) + screenshot; commit, push — `Add tile palette panel, viewport painting tools, chunk-local undo`

### Task 8: Phase close-out

- [ ] Stress verify the exit gate end-to-end in the editor (MCP): generate a 100k-tile map via a temp script or the fill tool, screenshot, confirm interactive FPS + single-chunk edits.
- [ ] Tick roadmap Phase 4 checkboxes that shipped; annotate "terrain/autotile rules" (explicitly a later-phase item per the roadmap's own tile-editor section — implement only `properties` now) and "external interchange" (deferred until the native format has soaked).
- [ ] Update the roadmap status paragraph; full suite; `Complete 2D engine phase 4 tilemaps` commit; push.

---

## Self-Review Notes

- **Spec coverage:** assets (T1-T3), chunk storage/compression/versioning/dirty (T2, revisions), culled sprite-compatible rendering (T5), palette/layers/painting/fill/selection-tools/undo (T7 — "selection/move/copy/paste" from the roadmap's tile-editor wishlist is consciously deferred with terrain rules; the Phase 4 checkbox items themselves are covered), incremental collision (T6), animated tiles + per-layer sorting/tint (T1/T5), custom tile properties (T1), external interchange deferred by the roadmap's own sequencing rule.
- **Streaming hooks:** chunk sparsity + per-chunk load boundaries ARE the hook; no background streaming this phase (roadmap: no background threads before ownership contracts exist).
- **Type consistency:** `TileChunkKey/TileChunk/TileMapLayer/TileMapAsset` (T2) used by T5/T6/T7; `tilecell::` helpers shared; `MergeSolidCells` consumed by T6 tests and sync; `TilePaintingState` shared by T7 panel+viewport.
- **Risk:** ImGui atlas preview needs a texture id for `ImageButton` — the Sprite Slicer already solves editor texture preview; copy that path exactly rather than inventing one.
