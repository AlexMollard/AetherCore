// 2D authoring control methods: tile painting, tileset/atlas/animation
// creation, editor camera control, and project asset discovery. These drive
// the same stores the editor panels use, so chunk revisions self-heal the
// live render/collision caches and MCP paints land in the Tile Palette's
// undo stack.
#include "editor/ControlMethods.hpp"
#include "editor/ControlSchema.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "PlayState.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "assets/TileAssetStore.hpp"
#include "assets/TileMapAsset.hpp"
#include "assets/TileSetAsset.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "debug/TilePaintingState.hpp"
#include "debug/EditorCommand.hpp"
#include "debug/UndoStack.hpp"
#include "io/FileGlobOptions.hpp"
#include "io/FileSystem.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	using nlohmann::json;

	namespace
	{
		constexpr std::size_t kMaxPaintCells = 10'000;
		constexpr std::size_t kMaxReadCells = 10'000;
		constexpr std::size_t kMaxListedAssets = 2'000;
// Slicing a 1536x1024 texture at 4x4 makes 98,304 sprites, which echoed back as a single
// 7.5 MB response - enough to swallow an agent's whole context. The atlas on disk still gets
// every sprite; only what is echoed is capped, the way read_tiles and list_assets already do.
constexpr std::size_t kMaxListedSprites = 2'000;

		// Everything the tile methods need, resolved from an entity id. `error`
		// is non-null when resolution failed.
		// Reflected catalog name of TileMapComponent (see Physics2D/CoreComponents reflect).
		const std::string kTileMapTypeName{"Tile Map"};

		struct TileContext
		{
			json error;
			World* world = nullptr;
			Entity entity{};
			TileMapComponent* component = nullptr;
			TileAssetStore* tiles = nullptr;
			TileMapAsset* map = nullptr;
			const TileSetAsset* tileSet = nullptr;
		};

		// Largest cell coordinate the engine can fully support. Past Box2D's 100000-unit vertex
	// limit a chunk's tile collision is skipped (Physics2DSystem warns and carries on, rather
	// than asserting the editor dead as it used to), so accepting a coordinate out there would
	// quietly hand back a map with holes in its collision. Refuse it the way a bad tile index
	// is refused.
	[[nodiscard]] std::int32_t MaxAuthorableCell(float cellSize)
	{
		return static_cast<std::int32_t>(100000.0f / std::max(cellSize, 0.0001f));
	}

	[[nodiscard]] nlohmann::json CellRangeError(glm::ivec2 cell, std::int32_t limit)
	{
		return nlohmann::json{{"error",
		        "cell (" + std::to_string(cell.x) + ", " + std::to_string(cell.y) + ") is outside the supported range of +/-"
		                + std::to_string(limit) + " cells for this map's cell size; a tile that far out cannot carry collision"}};
	}

	TileContext ResolveTileContext(const json& p, MethodContext& ctx, bool needAssets = true)
		{
			TileContext result;
			auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			if (scenes == nullptr)
			{
				result.error = json{{"error", "no scene loaded"}};
				return result;
			}
			result.world = &scenes->GetWorld();
			result.entity = Entity{p.value("id", std::uint32_t{0})};
			if (!result.world->GetRegistry().valid(World::ToEntt(result.entity)))
			{
				result.error = json{{"error", "entity not found"}};
				return result;
			}
			result.component = result.world->TryGet<TileMapComponent>(result.entity);
			if (result.component == nullptr)
			{
				result.error = json{{"error", "entity has no Tile Map component (add_component with type 'Tile Map' first)"}};
				return result;
			}
			result.tiles = ctx.services.TryGet<TileAssetStore>();
			if (result.tiles == nullptr)
			{
				result.error = json{{"error", "tile asset store unavailable"}};
				return result;
			}
			if (!needAssets)
			{
				return result;
			}
			if (result.component->tilemapPath.empty())
			{
				result.error = json{{"error", "Tile Map component has no tilemap asset (call create_tile_assets first)"}};
				return result;
			}
			result.map = result.tiles->MutableTileMap(result.component->tilemapPath);
			if (result.map == nullptr)
			{
				result.error = json{{"error", "failed to load tilemap '" + result.component->tilemapPath + "'"}};
				return result;
			}
			const auto tileSet = result.tiles->LoadTileSet(result.map->tileSetPath);
			if (!tileSet.has_value())
			{
				result.error = json{{"error", "failed to load tileset '" + result.map->tileSetPath + "': " + tileSet.error().ToString()}};
				return result;
			}
			result.tileSet = *tileSet;
			return result;
		}

		// Tileset tile index -> encoded cell value, appending to the palette on
		// first use. Returns nullopt (with `error` filled) for a bad index.
		std::optional<std::uint32_t> EncodeCell(TileMapAsset& map, const TileSetAsset& tileSet, const json& tile, bool flipX, bool flipY, json& error)
		{
			if (tile.is_null())
			{
				return tilecell::kEmpty;
			}
			if (!tile.is_number_integer() || tile.get<std::int64_t>() < 0 || tile.get<std::size_t>() >= tileSet.tiles.size())
			{
				error = json{{"error", "'tile' must be a tileset tile index in [0, " + std::to_string(tileSet.tiles.size()) + ") - call get_tile_map for the list"}};
				return std::nullopt;
			}
			const std::uint16_t palette = map.PaletteIndexFor(tileSet.tiles[tile.get<std::size_t>()].id);
			return tilecell::Make(palette, flipX, flipY);
		}

		// Palette index -> tileset tile index (or -1 when the palette entry no
		// longer resolves in the tileset).
		std::int32_t TileSetIndexOf(const TileMapAsset& map, const TileSetAsset& tileSet, std::uint16_t paletteIndex)
		{
			if (paletteIndex >= map.tilePalette.size())
			{
				return -1;
			}
			const TileDefinition* tile = tileSet.Find(map.tilePalette[paletteIndex]);
			return tile != nullptr ? static_cast<std::int32_t>(tile - tileSet.tiles.data()) : -1;
		}

		// Persist the live map and clear the palette panel's dirty flag; a
		// skipped save marks it dirty instead so the editor shows the star.
		json FinishTileEdit(TileContext& tc, MethodContext& ctx, bool save, json result)
		{
			auto* paintState = ctx.services.TryGet<TilePaintingState>();
			if (save)
			{
				TileMapAsset copy = *tc.map;
				if (const auto saved = tc.tiles->SaveTileMap(tc.component->tilemapPath, std::move(copy)); !saved.has_value())
				{
					result["saved"] = false;
					result["saveError"] = saved.error().ToString();
					return result;
				}
				result["saved"] = true;
				if (paintState != nullptr)
				{
					paintState->mapDirty = false;
				}
			}
			else
			{
				result["saved"] = false;
				if (paintState != nullptr)
				{
					paintState->mapDirty = true;
				}
			}
			return result;
		}

		std::optional<SpriteAnimationLoopMode> ParseLoopMode(std::string_view name)
		{
			if (name == "loop")
			{
				return SpriteAnimationLoopMode::Loop;
			}
			if (name == "once")
			{
				return SpriteAnimationLoopMode::Once;
			}
			if (name == "pingpong")
			{
				return SpriteAnimationLoopMode::PingPong;
			}
			if (name == "hold")
			{
				return SpriteAnimationLoopMode::Hold;
			}
			return std::nullopt;
		}

		// Sprite reference by atlas index or name -> region pointer.
		const SpriteRegion* ResolveSprite(const SpriteAtlasAsset& atlas, const json& reference)
		{
			if (reference.is_number_integer())
			{
				const auto index = reference.get<std::int64_t>();
				return index >= 0 && static_cast<std::size_t>(index) < atlas.sprites.size() ? &atlas.sprites[static_cast<std::size_t>(index)] : nullptr;
			}
			if (reference.is_string())
			{
				const std::string name = reference.get<std::string>();
				for (const SpriteRegion& region: atlas.sprites)
				{
					if (region.name == name)
					{
						return &region;
					}
				}
			}
			return nullptr;
		}

		std::string AtlasStem(std::string_view atlasPath)
		{
			std::string stem = std::filesystem::path(atlasPath).filename().generic_string();
			if (const std::size_t suffix = stem.find(".spriteatlas.toml"); suffix != std::string::npos)
			{
				stem.resize(suffix);
			}
			return stem.empty() ? std::string{"tiles"} : stem;
		}
	} // namespace

	void Append2DAuthoringMethods(std::vector<ControlMethod>& methods)
	{
		methods.push_back({"tiles.create",
		        "create_tile_assets",
		        "Create a tileset (every atlas sprite becomes a paintable tile) plus an empty tilemap, and bind the tilemap to an entity's Tile Map component when 'id' is given. 'cellSize' is in world units and must be greater than 0. Returns tileset/tilemap paths and the paintable tile list.",
		        true,
		        Obj({{"atlas", StrProp()}, {"id", IntProp()}, {"name", StrProp()}, {"directory", StrProp()}, {"cellSize", NumProp()}, {"solid", json{{"type", "boolean"}, {"description", "give every tile Full collision (default false)"}}}},
		                {"atlas"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* sprites = ctx.services.TryGet<SpriteAssetStore>();
			        auto* tiles = ctx.services.TryGet<TileAssetStore>();
			        if (sprites == nullptr || tiles == nullptr)
			        {
				        return json{{"error", "asset stores unavailable"}};
			        }
			        const std::string atlasPath = p.value("atlas", std::string{});
			        // Checked before the atlas is loaded: the name becomes a filename, and a colon
			        // or a separator does not fail loudly - it writes somewhere else and reports
			        // success. Cheap string check before the expensive load.
			        const std::string name = p.value("name", AtlasStem(atlasPath));
			        if (!app::scene::IsValidAssetName(name))
			        {
				        return json{{"error", "'" + name + "' is not a usable tileset name"}};
			        }
			        const auto atlas = sprites->LoadAtlas(atlasPath);
			        if (!atlas.has_value())
			        {
				        return json{{"error", "failed to load atlas '" + atlasPath + "': " + atlas.error().ToString()}};
			        }
			        std::string directory = p.value("directory", std::string{"project://assets/tilemaps/"});
			        if (!directory.empty() && directory.back() != '/')
			        {
				        directory.push_back('/');
			        }
			        // Clamping to an epsilon rather than refusing writes a tileset whose cells are
			        // a thousandth of a unit - a broken asset from a typo, with no complaint.
			        const float cellSize = p.value("cellSize", 1.0f);
			        if (cellSize <= 0.0f)
			        {
				        return json{{"error", "cellSize must be greater than 0 (got " + std::to_string(cellSize) + ")"}};
			        }
			        const bool solid = p.value("solid", false);

			        // An atlas that loads but holds no sprites produced an empty tileset and
			        // still reported success, leaving two asset files, a Tile Map component
			        // bound to the entity, and a map nothing could ever be painted on because
			        // every tile index is out of range. The usual cause is passing a
			        // .tileset.toml where a .spriteatlas.toml belongs - it parses, it just
			        // describes no sprites. Refused before anything is written.
			        if ((*atlas)->sprites.empty())
			        {
				        return json{{"error", "atlas '" + atlasPath + "' defines no sprites, so there would be nothing to paint with (a .tileset.toml is not a .spriteatlas.toml - call atlas.slice to make one from a texture)"}};
			        }

			        TileSetAsset tileSet;
			        tileSet.name = name;
			        tileSet.cellSize = cellSize;
			        json tileList = json::array();
			        for (const SpriteRegion& region: (*atlas)->sprites)
			        {
				        TileDefinition& tile = tileSet.AddTile(atlasPath, region.id, region.name);
				        tile.collision = solid ? TileCollisionKind::Full : TileCollisionKind::None;
				        tileList.push_back(json{{"index", tileList.size()}, {"name", region.name}});
			        }

			        TileMapAsset map;
			        const std::string setPath = directory + name + ".tileset.toml";
			        const std::string mapPath = directory + name + ".tilemap";
			        map.tileSetPath = setPath;
			        map.cellSize = cellSize;
			        map.layers.emplace_back();
			        if (!tiles->SaveTileSet(setPath, std::move(tileSet)).has_value() || !tiles->SaveTileMap(mapPath, std::move(map)).has_value())
			        {
				        return json{{"error", "failed to write tile assets under " + directory}};
			        }

			        json result{{"tileset", setPath}, {"tilemap", mapPath}, {"tiles", std::move(tileList)}, {"cellSize", cellSize}};
			        if (p.contains("id"))
			        {
				        // Give the entity a Tile Map first if it has none. This method is handed an
				        // entity id precisely so it can bind what it just created; refusing because
				        // the component is absent made the obvious first call fail, and reported it
				        // as a bindError inside an otherwise success-shaped response - so a caller
				        // checking for an "error" key saw success and got an unbound tilemap.
				        if (auto* scenes = ctx.services.TryGet<SceneSubsystem>())
				        {
					        World& world = scenes->GetWorld();
					        const Entity target{p.value("id", std::uint32_t{0})};
					        if (world.GetRegistry().valid(World::ToEntt(target)) && !world.Has<TileMapComponent>(target))
					        {
						        world.Emplace<TileMapComponent>(target, TileMapComponent{});
						        result["addedTileMapComponent"] = true;
					        }
				        }

				        TileContext tc = ResolveTileContext(p, ctx, false);
				        if (!tc.error.is_null())
				        {
					        result["bindError"] = tc.error["error"];
				        }
				        else
				        {
					        // Binding the map is the only scene mutation here (the tile
					        // assets themselves are files), so it records as a field edit.
					        nlohmann::json before;
					        bool isReflected = false;
					        CaptureComponentFields(*tc.world, tc.entity, kTileMapTypeName, ctx.services, before, isReflected);
					        tc.component->tilemapPath = mapPath;
					        if (auto* undo = ctx.services.TryGet<UndoStack>())
					        {
						        nlohmann::json after;
						        bool afterReflected = false;
						        CaptureComponentFields(*tc.world, tc.entity, kTileMapTypeName, ctx.services, after, afterReflected);
						        if (!before.empty() && before != after)
						        {
							        undo->Record(std::make_unique<SetComponentCommand>(tc.entity.id, kTileMapTypeName, std::move(before), std::move(after), isReflected));
						        }
					        }
					        result["boundTo"] = tc.entity.id;
				        }
			        }
			        return result;
		        }});

		methods.push_back({"tiles.info",
		        "get_tile_map",
		        "Inspect an entity's tilemap: paintable tiles (tileset indices used by paint_tiles), layers, palette usage, painted-cell bounds, and cell count.",
		        false,
		        Obj({{"id", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        TileContext tc = ResolveTileContext(p, ctx);
			        if (!tc.error.is_null())
			        {
				        return tc.error;
			        }
			        json tiles = json::array();
			        for (std::size_t i = 0; i < tc.tileSet->tiles.size(); ++i)
			        {
				        const TileDefinition& tile = tc.tileSet->tiles[i];
				        tiles.push_back(json{{"index", i},
				                {"name", tile.name},
				                {"collision",
				                        tile.collision == TileCollisionKind::Full   ? "full"
				                        : tile.collision == TileCollisionKind::Rect ? "rect"
				                                                                    : "none"},
				                {"oneWay",
				                        tile.collision == TileCollisionKind::None ? "none"
				                        : tile.oneWay == TileOneWay::Up           ? "up"
				                        : tile.oneWay == TileOneWay::Down         ? "down"
				                        : tile.oneWay == TileOneWay::Left         ? "left"
				                        : tile.oneWay == TileOneWay::Right        ? "right"
				                                                                  : "none"},
				                {"animated", !tile.animationFrames.empty()}});
			        }
			        json layers = json::array();
			        for (std::size_t i = 0; i < tc.map->layers.size(); ++i)
			        {
				        const TileMapLayer& layer = tc.map->layers[i];
				        layers.push_back(json{{"index", i}, {"name", layer.name}, {"visible", layer.visible}, {"collision", layer.collision}, {"opacity", layer.opacity}, {"sortingLayer", layer.sortingLayer}, {"orderInLayer", layer.orderInLayer}});
			        }
			        glm::ivec2 minCell{std::numeric_limits<std::int32_t>::max()};
			        glm::ivec2 maxCell{std::numeric_limits<std::int32_t>::lowest()};
			        std::size_t cellCount = 0;
			        for (const TileMapLayer& layer: tc.map->layers)
			        {
				        for (const auto& [key, chunk]: layer.chunks)
				        {
					        for (std::int32_t localY = 0; localY < kTileChunkSize; ++localY)
					        {
						        for (std::int32_t localX = 0; localX < kTileChunkSize; ++localX)
						        {
							        if (tilecell::Empty(chunk.cells[static_cast<std::size_t>(localY) * kTileChunkSize + localX]))
							        {
								        continue;
							        }
							        ++cellCount;
							        const glm::ivec2 cell{key.x * kTileChunkSize + localX, key.y * kTileChunkSize + localY};
							        minCell = glm::min(minCell, cell);
							        maxCell = glm::max(maxCell, cell);
						        }
					        }
				        }
			        }
			        json result{{"tilemap", tc.component->tilemapPath}, {"tileset", tc.map->tileSetPath}, {"cellSize", tc.map->cellSize}, {"cellCount", cellCount}, {"tiles", std::move(tiles)}, {"layers", std::move(layers)}};
			        if (cellCount > 0)
			        {
				        result["bounds"] = json{{"min", json::array({minCell.x, minCell.y})}, {"max", json::array({maxCell.x, maxCell.y})}};
			        }
			        return result;
		        }});

		methods.push_back({"tiles.paint",
		        "paint_tiles",
		        "Paint tilemap cells. Each cell is {x, y, tile?, flipX?, flipY?} where 'tile' is a tileset tile index (get_tile_map lists them); omit 'tile' to erase. Cells beyond what the physics engine can carry collision for are refused, and every cell is checked before any is painted. Edits join the editor's undo stack. 'save' (default true) persists the tilemap "
		        "asset.",
		        true,
		        Obj({{"id", IntProp()},
		                    {"layer", IntProp()},
		                    {"cells", json{{"type", "array"}, {"minItems", 1}, {"maxItems", kMaxPaintCells}, {"items", Obj({{"x", IntProp()}, {"y", IntProp()}, {"tile", IntProp()}, {"flipX", BoolProp()}, {"flipY", BoolProp()}}, {"x", "y"})}}},
		                    {"save", BoolProp()}},
		                {"id", "cells"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        TileContext tc = ResolveTileContext(p, ctx);
			        if (!tc.error.is_null())
			        {
				        return tc.error;
			        }
			        const auto layer = p.value("layer", std::size_t{0});
			        if (layer >= tc.map->layers.size())
			        {
				        return json{{"error", "layer out of range"}};
			        }
			        const json& cells = p.value("cells", json::array());
			        if (!cells.is_array() || cells.empty() || cells.size() > kMaxPaintCells)
			        {
				        return json{{"error", "'cells' must contain between 1 and " + std::to_string(kMaxPaintCells) + " entries"}};
			        }

			        // Checked before anything is painted: the loop below returns on the first
			        // bad cell, so validating inside it would leave the earlier cells applied.
			        const std::int32_t cellLimit = MaxAuthorableCell(tc.map->cellSize);
			        for (const json& cell: cells)
			        {
				        const glm::ivec2 at{cell.value("x", 0), cell.value("y", 0)};
				        if (std::abs(at.x) >= cellLimit || std::abs(at.y) >= cellLimit)
				        {
					        return CellRangeError(at, cellLimit);
				        }
			        }

			        TilePaintStroke stroke;
			        stroke.tilemapPath = tc.component->tilemapPath;
			        std::size_t painted = 0;
			        for (const json& cell: cells)
			        {
				        json error;
				        const auto value = EncodeCell(*tc.map, *tc.tileSet, cell.contains("tile") ? cell["tile"] : json{}, cell.value("flipX", false), cell.value("flipY", false), error);
				        if (!value.has_value())
				        {
					        return error;
				        }
				        const glm::ivec2 at{cell.value("x", 0), cell.value("y", 0)};
				        const std::uint32_t before = tc.map->GetCell(layer, at);
				        if (before == *value)
				        {
					        continue;
				        }
				        tc.map->SetCell(layer, at, *value);
				        stroke.edits.push_back(TilePaintEdit{.layer = layer, .cell = at, .before = before, .after = *value});
				        ++painted;
			        }
			        if (auto* undo = ctx.services.TryGet<UndoStack>(); undo != nullptr && !stroke.edits.empty())
			        {
				        undo->Record(std::make_unique<TileStrokeCommand>(std::move(stroke.tilemapPath), std::move(stroke.edits)));
			        }
			        if (auto* paintState = ctx.services.TryGet<TilePaintingState>())
			        {
				        paintState->mapDirty = true;
			        }
			        return FinishTileEdit(tc, ctx, p.value("save", true), json{{"painted", painted}, {"cellCount", tc.map->TotalCellCount()}});
		        }});

		methods.push_back({"tiles.fill",
		        "fill_tiles",
		        "Fill (or erase, when 'tile' is omitted) an inclusive cell rect [x0, y0, x1, y1] on a tilemap layer without sending every cell. Edits join the editor's undo stack. Cells beyond what the physics engine can carry collision for are refused; the error names the limit.",
		        true,
		        Obj({{"id", IntProp()}, {"layer", IntProp()}, {"rect", RectProp()}, {"tile", IntProp()}, {"flipX", BoolProp()}, {"flipY", BoolProp()}, {"save", BoolProp()}}, {"id", "rect"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        TileContext tc = ResolveTileContext(p, ctx);
			        if (!tc.error.is_null())
			        {
				        return tc.error;
			        }
			        const auto layer = p.value("layer", std::size_t{0});
			        if (layer >= tc.map->layers.size())
			        {
				        return json{{"error", "layer out of range"}};
			        }
			        const json& rect = p["rect"];
			        if (!rect.is_array() || rect.size() != 4)
			        {
				        return json{{"error", "'rect' must be [x0, y0, x1, y1]"}};
			        }
			        const glm::ivec2 lo{std::min(rect[0].get<std::int32_t>(), rect[2].get<std::int32_t>()), std::min(rect[1].get<std::int32_t>(), rect[3].get<std::int32_t>())};
			        const glm::ivec2 hi{std::max(rect[0].get<std::int32_t>(), rect[2].get<std::int32_t>()), std::max(rect[1].get<std::int32_t>(), rect[3].get<std::int32_t>())};
			        const std::int32_t cellLimit = MaxAuthorableCell(tc.map->cellSize);
			        for (const glm::ivec2 corner: {lo, hi})
			        {
			        	if (std::abs(corner.x) >= cellLimit || std::abs(corner.y) >= cellLimit)
			        	{
			        		return CellRangeError(corner, cellLimit);
			        	}
			        }
			        const std::size_t area = static_cast<std::size_t>(hi.x - lo.x + 1) * static_cast<std::size_t>(hi.y - lo.y + 1);
			        if (area > 1'000'000)
			        {
				        return json{{"error", "rect covers more than 1,000,000 cells"}};
			        }
			        json error;
			        const auto value = EncodeCell(*tc.map, *tc.tileSet, p.contains("tile") ? p["tile"] : json{}, p.value("flipX", false), p.value("flipY", false), error);
			        if (!value.has_value())
			        {
				        return error;
			        }

			        TilePaintStroke stroke;
			        stroke.tilemapPath = tc.component->tilemapPath;
			        std::size_t painted = 0;
			        for (std::int32_t y = lo.y; y <= hi.y; ++y)
			        {
				        for (std::int32_t x = lo.x; x <= hi.x; ++x)
				        {
					        const glm::ivec2 at{x, y};
					        const std::uint32_t before = tc.map->GetCell(layer, at);
					        if (before == *value)
					        {
						        continue;
					        }
					        tc.map->SetCell(layer, at, *value);
					        stroke.edits.push_back(TilePaintEdit{.layer = layer, .cell = at, .before = before, .after = *value});
					        ++painted;
				        }
			        }
			        if (auto* undo = ctx.services.TryGet<UndoStack>(); undo != nullptr && !stroke.edits.empty())
			        {
				        undo->Record(std::make_unique<TileStrokeCommand>(std::move(stroke.tilemapPath), std::move(stroke.edits)));
			        }
			        if (auto* paintState = ctx.services.TryGet<TilePaintingState>())
			        {
				        paintState->mapDirty = true;
			        }
			        return FinishTileEdit(tc, ctx, p.value("save", true), json{{"painted", painted}, {"cellCount", tc.map->TotalCellCount()}});
		        }});

		methods.push_back({"tiles.read",
		        "read_tiles",
		        "Read the non-empty cells of an inclusive cell rect [x0, y0, x1, y1] back as {x, y, tile, flipX, flipY} with 'tile' as a tileset index. Large rects are capped - check 'truncated'.",
		        false,
		        Obj({{"id", IntProp()}, {"layer", IntProp()}, {"rect", RectProp()}}, {"id", "rect"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        TileContext tc = ResolveTileContext(p, ctx);
			        if (!tc.error.is_null())
			        {
				        return tc.error;
			        }
			        const auto layer = p.value("layer", std::size_t{0});
			        if (layer >= tc.map->layers.size())
			        {
				        return json{{"error", "layer out of range"}};
			        }
			        const json& rect = p["rect"];
			        if (!rect.is_array() || rect.size() != 4)
			        {
				        return json{{"error", "'rect' must be [x0, y0, x1, y1]"}};
			        }
			        const glm::ivec2 lo{std::min(rect[0].get<std::int32_t>(), rect[2].get<std::int32_t>()), std::min(rect[1].get<std::int32_t>(), rect[3].get<std::int32_t>())};
			        const glm::ivec2 hi{std::max(rect[0].get<std::int32_t>(), rect[2].get<std::int32_t>()), std::max(rect[1].get<std::int32_t>(), rect[3].get<std::int32_t>())};
			        const std::int32_t cellLimit = MaxAuthorableCell(tc.map->cellSize);
			        for (const glm::ivec2 corner: {lo, hi})
			        {
			        	if (std::abs(corner.x) >= cellLimit || std::abs(corner.y) >= cellLimit)
			        	{
			        		return CellRangeError(corner, cellLimit);
			        	}
			        }
			        json cells = json::array();
			        bool truncated = false;
			        for (std::int32_t y = lo.y; y <= hi.y && !truncated; ++y)
			        {
				        for (std::int32_t x = lo.x; x <= hi.x; ++x)
				        {
					        const std::uint32_t cell = tc.map->GetCell(layer, {x, y});
					        if (tilecell::Empty(cell))
					        {
						        continue;
					        }
					        if (cells.size() >= kMaxReadCells)
					        {
						        truncated = true;
						        break;
					        }
					        cells.push_back(json{{"x", x}, {"y", y}, {"tile", TileSetIndexOf(*tc.map, *tc.tileSet, tilecell::PaletteIndex(cell))}, {"flipX", (cell & tilecell::kFlipX) != 0u}, {"flipY", (cell & tilecell::kFlipY) != 0u}});
				        }
			        }
			        return json{{"cells", std::move(cells)}, {"truncated", truncated}};
		        }});

		methods.push_back({"tiles.add_layer",
		        "add_tile_layer",
		        "Append a layer to an entity's tilemap and return its index (paint_tiles' 'layer' parameter).",
		        true,
		        Obj({{"id", IntProp()}, {"name", StrProp()}, {"collision", BoolProp()}, {"opacity", NumProp()}, {"sortingLayer", IntProp()}, {"orderInLayer", IntProp()}, {"save", BoolProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        TileContext tc = ResolveTileContext(p, ctx);
			        if (!tc.error.is_null())
			        {
				        return tc.error;
			        }
			        TileMapLayer& layer = tc.map->layers.emplace_back();
			        layer.name = p.value("name", "Layer " + std::to_string(tc.map->layers.size()));
			        layer.collision = p.value("collision", true);
			        layer.opacity = std::clamp(p.value("opacity", 1.0f), 0.0f, 1.0f);
			        layer.sortingLayer = p.value("sortingLayer", 0);
			        layer.orderInLayer = p.value("orderInLayer", 0);
			        if (auto* undo = ctx.services.TryGet<UndoStack>())
			        {
				        undo->Record(std::make_unique<AddTileLayerCommand>(tc.component->tilemapPath, tc.map->layers.size() - 1, layer));
			        }
			        return FinishTileEdit(tc, ctx, p.value("save", true), json{{"index", tc.map->layers.size() - 1}, {"name", layer.name}});
		        }});

		methods.push_back({"atlas.slice",
		        "slice_atlas",
		        "Grid-slice a texture into a sprite atlas asset (the same operation as the Sprite Slicer panel). Re-slicing an existing atlas keeps stable sprite ids where regions still correspond. cellWidth/cellHeight are pixels and must be at least 1. Returns the atlas path, spriteCount, and the first 2,000 sprites - check 'truncated', because the atlas on disk holds them all.",
		        true,
		        Obj({{"texture", StrProp()},
		                    {"cellWidth", IntProp()},
		                    {"cellHeight", IntProp()},
		                    {"paddingX", IntProp()},
		                    {"paddingY", IntProp()},
		                    {"spacingX", IntProp()},
		                    {"spacingY", IntProp()},
		                    {"trimAlpha", BoolProp()},
		                    {"pixelsPerUnit", NumProp()},
		                    {"out", StrProp()}},
		                {"texture", "cellWidth", "cellHeight"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* sprites = ctx.services.TryGet<SpriteAssetStore>();
			        if (sprites == nullptr)
			        {
				        return json{{"error", "sprite asset store unavailable"}};
			        }
			        const std::string texture = p.value("texture", std::string{});
			        const auto image = DecodeSpriteSourceImage(texture);
			        if (!image.has_value())
			        {
				        return json{{"error", "failed to decode '" + texture + "': " + image.error().ToString()}};
			        }
			        // Clamping a nonsense cell size to 1 instead of refusing it produced an atlas
			        // of one-pixel sprites - 400 of them for a 20x20 texture, a million for a
			        // 1024x1024 - and wrote it to disk. Say no, the way a bad tile index does.
			        const int cellWidth = p.value("cellWidth", 32);
			        const int cellHeight = p.value("cellHeight", 32);
			        if (cellWidth < 1 || cellHeight < 1)
			        {
				        return json{{"error",
				                "cellWidth and cellHeight must be at least 1 pixel (got " + std::to_string(cellWidth) + "x" + std::to_string(cellHeight)
				                        + "); slicing by a smaller cell would make one sprite per pixel"}};
			        }
			        SpriteSliceSettings settings;
			        settings.cellWidth = cellWidth;
			        settings.cellHeight = cellHeight;
			        settings.paddingX = std::max(p.value("paddingX", 0), 0);
			        settings.paddingY = std::max(p.value("paddingY", 0), 0);
			        settings.spacingX = std::max(p.value("spacingX", 0), 0);
			        settings.spacingY = std::max(p.value("spacingY", 0), 0);
			        settings.trimAlpha = p.value("trimAlpha", false);

			        std::string out = p.value("out", std::string{});
			        if (out.empty())
			        {
				        out = texture;
				        if (const std::size_t dot = out.find_last_of('.'); dot != std::string::npos && dot > out.find_last_of('/'))
				        {
					        out.resize(dot);
				        }
				        out += ".spriteatlas.toml";
			        }
			        const SpriteAtlasAsset* previous = nullptr;
			        if (const auto existing = sprites->LoadAtlas(out); existing.has_value())
			        {
				        previous = *existing;
			        }
			        SpriteAtlasAsset atlas = SpriteAtlasAsset::SliceGrid(texture, image->width, image->height, settings, previous, image->rgbaPixels);
			        if (p.contains("pixelsPerUnit"))
			        {
				        const float pixelsPerUnit = p.value("pixelsPerUnit", 100.0f);
				        if (pixelsPerUnit <= 0.0f)
				        {
					        return json{{"error", "pixelsPerUnit must be greater than 0 (got " + std::to_string(pixelsPerUnit) + ")"}};
				        }
				        atlas.pixelsPerUnit = pixelsPerUnit;
			        }
			        json spriteList = json::array();
			        // Captured before SaveAtlas below moves the atlas out - reading it afterwards
			        // reports 0 sprites from the moved-from object.
			        const std::size_t spriteCount = atlas.sprites.size();
			        const std::size_t listed = std::min(spriteCount, kMaxListedSprites);
			        for (std::size_t i = 0; i < listed; ++i)
			        {
				        const SpriteRegion& region = atlas.sprites[i];
				        spriteList.push_back(json{{"index", i}, {"name", region.name}, {"x", region.pixelRect.x}, {"y", region.pixelRect.y}, {"width", region.pixelRect.width}, {"height", region.pixelRect.height}});
			        }
			        if (const auto saved = sprites->SaveAtlas(out, std::move(atlas)); !saved.has_value())
			        {
				        return json{{"error", "failed to save atlas '" + out + "': " + saved.error().ToString()}};
			        }
			        return json{{"atlas", out},
			                {"textureWidth", image->width},
			                {"textureHeight", image->height},
			                {"spriteCount", spriteCount},
			                {"truncated", spriteCount > listed},
			                {"sprites", std::move(spriteList)}};
		        }});

		methods.push_back({"atlas.info",
		        "get_atlas",
		        "List a sprite atlas' regions: index, name, and pixel rect. Indices/names feed create_sprite_animation; names identify sprites for Sprite Renderer components. Returns spriteCount and the first 2,000 regions - check 'truncated'.",
		        false,
		        Obj({{"path", StrProp()}}, {"path"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* sprites = ctx.services.TryGet<SpriteAssetStore>();
			        if (sprites == nullptr)
			        {
				        return json{{"error", "sprite asset store unavailable"}};
			        }
			        const std::string path = p.value("path", std::string{});
			        const auto atlas = sprites->LoadAtlas(path);
			        if (!atlas.has_value())
			        {
				        return json{{"error", "failed to load atlas '" + path + "': " + atlas.error().ToString()}};
			        }
			        json spriteList = json::array();
			        const std::size_t spriteCount = (*atlas)->sprites.size();
			        const std::size_t listed = std::min(spriteCount, kMaxListedSprites);
			        for (std::size_t i = 0; i < listed; ++i)
			        {
				        const SpriteRegion& region = (*atlas)->sprites[i];
				        spriteList.push_back(json{{"index", i}, {"name", region.name}, {"x", region.pixelRect.x}, {"y", region.pixelRect.y}, {"width", region.pixelRect.width}, {"height", region.pixelRect.height}});
			        }
			        return json{
			                {"atlas", path}, {"texture", (*atlas)->texturePath}, {"textureWidth", (*atlas)->textureWidth}, {"textureHeight", (*atlas)->textureHeight}, {"pixelsPerUnit", (*atlas)->pixelsPerUnit}, {"spriteCount", spriteCount}, {"truncated", spriteCount > listed}, {"sprites", std::move(spriteList)}};
		        }});

		methods.push_back({"animation.create",
		        "create_sprite_animation",
		        "Create a sprite animation asset from atlas frames. 'frames' entries are sprite indices, sprite names, or {sprite, duration} objects; 'fps' sets the default frame duration and must be greater than 0. 'loop' is one of loop (restart, the default), once (stop on the last frame), pingpong (forwards then backwards) or hold (stop, keeping the last frame shown). Assign the result to a Sprite Animator component to play "
		        "it.",
		        true,
		        Obj({{"atlas", StrProp()},
		                    {"name", StrProp()},
		                    {"frames", json{{"type", "array"}, {"minItems", 1}, {"maxItems", 1024}}},
		                    {"fps", NumProp()},
		                    {"loop", json{{"type", "string"}, {"enum", json::array({"loop", "once", "pingpong", "hold"})}}},
		                    {"out", StrProp()}},
		                {"atlas", "name", "frames"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* sprites = ctx.services.TryGet<SpriteAssetStore>();
			        if (sprites == nullptr)
			        {
				        return json{{"error", "sprite asset store unavailable"}};
			        }
			        const std::string atlasPath = p.value("atlas", std::string{});
			        const auto atlas = sprites->LoadAtlas(atlasPath);
			        if (!atlas.has_value())
			        {
				        return json{{"error", "failed to load atlas '" + atlasPath + "': " + atlas.error().ToString()}};
			        }
			        const auto loopMode = ParseLoopMode(p.value("loop", std::string{"loop"}));
			        if (!loopMode.has_value())
			        {
				        return json{{"error", "loop must be loop, once, pingpong, or hold"}};
			        }
			        const float fps = p.value("fps", 10.0f);
			        if (fps <= 0.0f)
			        {
				        return json{{"error", "fps must be greater than 0 (got " + std::to_string(fps) + "); a clamped 0 would play one frame every 1000 seconds"}};
			        }

			        SpriteAnimationAsset animation;
			        animation.name = p.value("name", std::string{"Animation"});
			        // The name becomes the filename when 'out' is omitted, so it needs the same
			        // guard scenes, prefabs and tilesets have: a colon writes into an NTFS
			        // stream and a separator writes outside the animations folder.
			        if (!p.contains("out") && !app::scene::IsValidAssetName(animation.name))
			        {
				        return json{{"error", "'" + animation.name + "' is not a usable animation name; pass an explicit 'out' path or use a name without <>:\"/\|?*"}};
			        }
			        animation.atlasPath = atlasPath;
			        animation.loopMode = *loopMode;
			        for (const json& frame: p.value("frames", json::array()))
			        {
				        const json& reference = frame.is_object() ? frame["sprite"] : frame;
				        const SpriteRegion* region = ResolveSprite(**atlas, reference);
				        if (region == nullptr)
				        {
					        return json{{"error", "frame '" + reference.dump() + "' matches no sprite in the atlas (call get_atlas for the list)"}};
				        }
				        animation.frames.push_back(SpriteAnimationFrame{.spriteId = region->id, .durationSeconds = frame.is_object() ? std::max(frame.value("duration", 1.0f / fps), 0.001f) : 1.0f / fps});
			        }
			        // An empty frame list wrote a real .spriteanim.toml that reported success
			        // with a zero duration - an animation asset that can never show anything,
			        // and one nothing downstream refuses either. Checked before the write so a
			        // refused call leaves no file behind.
			        if (animation.frames.empty())
			        {
				        return json{{"error", "'frames' is empty, so the animation would have nothing to play (call atlas.info for the sprites this atlas offers)"}};
			        }

			        std::string out = p.value("out", std::string{});
			        if (out.empty())
			        {
				        out = "project://assets/animations/" + animation.name + ".spriteanim.toml";
			        }
			        const float duration = animation.DurationSeconds();
			        if (const auto saved = sprites->SaveAnimation(out, std::move(animation)); !saved.has_value())
			        {
				        return json{{"error", "failed to save animation '" + out + "': " + saved.error().ToString()}};
			        }
			        return json{{"animation", out}, {"durationSeconds", duration}};
		        }});

		methods.push_back({"editor.camera",
		        "set_editor_camera",
		        "Move the edit-mode viewport camera: 'position' ([x, y] keeps z, [x, y, z] sets it), 'height' sets the orthographic view height, and 'frame' centres on an entity. Call it with no arguments to read the camera without moving it - it returns the current position and orthographic height either way. 'height' only lands in a 2D scene; a 3D edit camera is perspective and keeps the value it had. Edit mode only - the play camera belongs to the game.",
		        true,
		        Obj({{"position", json{{"type", "array"}, {"items", NumProp()}, {"minItems", 2}, {"maxItems", 3}}}, {"height", NumProp()}, {"frame", json{{"type", "integer"}, {"description", "entity id to centre on"}}}}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        const auto* playState = ctx.services.TryGet<app::PlayState>();
			        if (playState != nullptr && playState->IsPlaying())
			        {
				        return json{{"error", "editor camera control is edit-mode only (stop play first)"}};
			        }
			        auto* cams = ctx.services.TryGet<CameraManager>();
			        Camera* cam = cams != nullptr ? cams->TryGet(cams->GetMainCamera()) : nullptr;
			        if (cam == nullptr)
			        {
				        return json{{"error", "no active editor camera"}};
			        }
			        glm::vec3 position = cam->GetPosition();
			        if (p.contains("frame"))
			        {
				        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
				        if (scenes == nullptr)
				        {
					        return json{{"error", "no scene loaded"}};
				        }
				        World& world = scenes->GetWorld();
				        const Entity entity{p.value("frame", std::uint32_t{0})};
				        const auto* transform = world.GetRegistry().valid(World::ToEntt(entity)) ? world.TryGet<TransformComponent>(entity) : nullptr;
				        if (transform == nullptr)
				        {
					        return json{{"error", "frame target entity not found"}};
				        }
				        const glm::vec3 target = glm::vec3(transform->localToWorld[3]);
				        position = {target.x, target.y, position.z};
			        }
			        if (p.contains("position"))
			        {
				        const json& array = p["position"];
				        if (!array.is_array() || array.size() < 2)
				        {
					        return json{{"error", "'position' must be [x, y] or [x, y, z]"}};
				        }
				        position.x = array[0].get<float>();
				        position.y = array[1].get<float>();
				        if (array.size() > 2)
				        {
					        position.z = array[2].get<float>();
				        }
			        }
			        cam->SetPosition(position);
			        if (p.contains("height") && cam->GetProjection() == CameraProjection::Orthographic)
			        {
				        cam->SetOrthographic(std::max(p.value("height", 10.0f), 0.01f), cam->GetNearPlane(), cam->GetFarPlane());
			        }
			        return json{{"position", json::array({position.x, position.y, position.z})}, {"orthographicHeight", cam->GetOrthographicHeight()}};
		        }});

		methods.push_back({"scene.add_script",
		        "add_script",
		        "Attach a C# script to an entity by type name (the class name in the project's scripts folder, e.g. 'PlayerController'). Optional 'properties' seeds public fields: numbers, bools, strings, or [x, y, z] arrays.",
		        true,
		        Obj({{"id", IntProp()}, {"type", StrProp()}, {"properties", json{{"type", "object"}}}}, {"id", "type"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return json{{"error", "no scene loaded"}};
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity{p.value("id", std::uint32_t{0})};
			        if (!world.GetRegistry().valid(World::ToEntt(entity)))
			        {
				        return json{{"error", "entity not found"}};
			        }
			        const std::string type = p.value("type", std::string{});
			        if (type.empty())
			        {
				        return json{{"error", "'type' must be a script class name"}};
			        }
			        // Reject a name that is not a loaded script type. Attaching one used to
			        // succeed and only fail on Play, by which point the dead entry is in the
			        // saved scene. Skipped while the assembly is unloaded (no project built
			        // yet) so this cannot reject every name just because nothing is loaded.
			        if (const auto* csharp = ctx.services.TryGet<app::scripting::CSharpScriptingSubsystem>())
			        {
				        const std::vector<std::string>& known = csharp->GetScriptTypeNames();
				        if (!known.empty() && std::find(known.begin(), known.end(), type) == known.end())
				        {
					        std::string message = "unknown script type '" + type + "'; loaded types: ";
					        for (std::size_t i = 0; i < known.size(); ++i)
					        {
						        message += (i > 0 ? ", " : "") + known[i];
					        }
					        return json{{"error", std::move(message)}};
				        }
			        }
			        auto* existing = world.TryGet<ScriptComponent>(entity);
			        ScriptComponent& sc = existing != nullptr ? *existing : world.Emplace<ScriptComponent>(entity);
			        ScriptEntry& script = sc.scripts.emplace_back();
			        script.path = type;
			        script.attached = false;
			        if (p.contains("properties") && p["properties"].is_object())
			        {
				        for (const auto& [name, value]: p["properties"].items())
				        {
					        ScriptPropertyValue property;
					        if (value.is_boolean())
					        {
						        property.type = ScriptPropertyValue::Type::Bool;
						        property.i64 = value.get<bool>() ? 1 : 0;
					        }
					        else if (value.is_number_integer())
					        {
						        property.type = ScriptPropertyValue::Type::Int;
						        property.i64 = value.get<std::int64_t>();
					        }
					        else if (value.is_number())
					        {
						        property.type = ScriptPropertyValue::Type::Float;
						        property.f4[0] = value.get<float>();
					        }
					        else if (value.is_string())
					        {
						        property.type = ScriptPropertyValue::Type::String;
						        property.str = value.get<std::string>();
					        }
					        else if (value.is_array() && value.size() == 3)
					        {
						        property.type = ScriptPropertyValue::Type::Vector3;
						        for (std::size_t axis = 0; axis < 3; ++axis)
						        {
							        property.f4[axis] = value[axis].get<float>();
						        }
					        }
					        else
					        {
						        return json{{"error", "property '" + name + "' must be a number, bool, string, or [x, y, z] array"}};
					        }
					        script.properties[name] = std::move(property);
				        }
			        }
			        if (auto* undo = ctx.services.TryGet<UndoStack>())
			        {
				        undo->Record(std::make_unique<AddScriptCommand>(entity.id, type, script));
			        }
			        return json{{"id", entity.id}, {"type", type}, {"scriptCount", sc.scripts.size()}};
		        }});

		methods.push_back({"scene.scripts",
		        "list_scripts",
		        "List the C# scripts attached to an entity: each one's type, whether it is attached yet, and its property values in the same shape add_script takes, so a value read here can be written straight back.",
		        false,
		        Obj({{"id", IntProp()}}, {"id"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return json{{"error", "no scene loaded"}};
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity{p.value("id", std::uint32_t{0})};
			        if (!world.GetRegistry().valid(World::ToEntt(entity)))
			        {
				        return json{{"error", "entity not found"}};
			        }
			        json scripts = json::array();
			        if (auto* sc = world.TryGet<ScriptComponent>(entity))
			        {
				        // Read-through-to-live: sc->scripts holds each ScriptEntry's cache,
				        // which only refreshes from the real C# instance at scene-save time
				        // (ScriptSerde.cpp's CaptureScripts) or when add_script explicitly
				        // seeds it - so a field a script itself mutated at runtime (Lever.Out
				        // flipping on Interact(), say) never reached this cache before. Same
				        // fix, same call, as CaptureScripts: pull each entry's live values
				        // from its actual instance immediately before reading, so a caller
				        // asserting on state right after an action sees what really happened,
				        // not what the scene was authored with.
				        auto* runner = static_cast<app::ScriptComponentSystem*>(world.FindSystem("ScriptComponentSystem"));
				        for (std::size_t i = 0; i < sc->scripts.size(); ++i)
				        {
					        ScriptEntry& script = sc->scripts[i];
					        if (runner != nullptr && !script.path.empty())
					        {
						        runner->SyncPropertiesFromLiveInstance(entity, static_cast<std::uint32_t>(i), script);
					        }
					        // add_script accepts property values but nothing reported them
					        // back, so a caller could set one and had no way to confirm it
					        // took. Written as the inverse of the conversion add_script does,
					        // so a value read here can be passed straight back to it.
					        json properties = json::object();
					        for (const auto& [name, value]: script.properties)
					        {
						        switch (value.type)
						        {
							        case ScriptPropertyValue::Type::Bool:
								        properties[name] = value.i64 != 0;
								        break;
							        case ScriptPropertyValue::Type::Int:
							        case ScriptPropertyValue::Type::Enum:
								        properties[name] = value.i64;
								        break;
							        case ScriptPropertyValue::Type::Entity:
								        properties[name] = value.i64;
								        break;
							        case ScriptPropertyValue::Type::Float:
								        properties[name] = value.f4[0];
								        break;
							        case ScriptPropertyValue::Type::Vector3:
								        properties[name] = json::array({value.f4[0], value.f4[1], value.f4[2]});
								        break;
							        case ScriptPropertyValue::Type::String:
							        case ScriptPropertyValue::Type::Component:
								        properties[name] = value.str;
								        break;
							        case ScriptPropertyValue::Type::None:
								        break;
						        }
					        }
					        scripts.push_back(json{{"type", script.path}, {"attached", script.attached}, {"properties", std::move(properties)}});
				        }
			        }
			        return json{{"id", entity.id}, {"scripts", std::move(scripts)}};
		        }});

		methods.push_back({"scene.script_types",
		        "list_script_types",
		        "List the C# script type names add_script accepts, as loaded from the project's built assembly. Empty while no assembly is loaded (a project that has not been built yet), which is also when add_script stops checking the name.",
		        false,
		        Obj(),
		        [](const json&, MethodContext& ctx) -> json
		        {
			        // The names were only reachable by getting one wrong: add_script listed
			        // them in its refusal and nothing else would say them.
			        auto* csharp = ctx.services.TryGet<app::scripting::CSharpScriptingSubsystem>();
			        if (csharp == nullptr)
			        {
				        return json{{"error", "no C# scripting subsystem"}};
			        }
			        const std::vector<std::string>& known = csharp->GetScriptTypeNames();
			        return json{{"types", known}, {"count", known.size()}, {"assemblyLoaded", !known.empty()}};
		        }});

		methods.push_back({"scene.remove_script",
		        "remove_script",
		        "Detach a C# script from an entity by type name.",
		        true,
		        Obj({{"id", IntProp()}, {"type", StrProp()}}, {"id", "type"}),
		        [](const json& p, MethodContext& ctx) -> json
		        {
			        auto* scenes = ctx.services.TryGet<SceneSubsystem>();
			        if (scenes == nullptr)
			        {
				        return json{{"error", "no scene loaded"}};
			        }
			        World& world = scenes->GetWorld();
			        const Entity entity{p.value("id", std::uint32_t{0})};
			        if (!world.GetRegistry().valid(World::ToEntt(entity)))
			        {
				        return json{{"error", "entity not found"}};
			        }
			        const std::string type = p.value("type", std::string{});
			        auto* sc = world.TryGet<ScriptComponent>(entity);
			        const std::size_t before = sc != nullptr ? sc->scripts.size() : 0;
			        // Snapshot the first matching script before erasing, for undo.
			        ScriptEntry captured;
			        bool hasCaptured = false;
			        if (sc != nullptr)
			        {
				        for (const ScriptEntry& s: sc->scripts)
				        {
					        if (s.path == type)
					        {
						        captured = s;
						        hasCaptured = true;
						        break;
					        }
				        }
			        }
			        if (sc != nullptr)
			        {
				        std::erase_if(sc->scripts, [&](const ScriptEntry& script) { return script.path == type; });
			        }
			        if (hasCaptured)
			        {
				        if (auto* undo = ctx.services.TryGet<UndoStack>())
				        {
					        undo->Record(std::make_unique<RemoveScriptCommand>(entity.id, type, std::move(captured)));
				        }
			        }
			        const std::size_t removed = sc != nullptr ? before - sc->scripts.size() : 0;
			        return json{{"id", entity.id}, {"removed", removed}};
		        }});

		methods.push_back({"assets.list",
		        "list_assets",
		        "Glob project files by virtual path, e.g. 'project://assets/**/*.png' or 'project://assets/**/*.spriteatlas.toml'. Feeds texture/atlas parameters on the other 2D authoring methods. "
		        "'limit' caps how many paths come back while 'matched' and 'truncated' still describe the full result, so a small limit is enough to count what a glob hits.",
		        false,
		        Obj({{"glob", StrProp()}, {"limit", IntProp()}}),
		        [](const json& p, MethodContext&) -> json
		        {
			        const std::string pattern = p.value("glob", std::string{"project://assets/**"});
			        const auto limit = std::clamp<std::size_t>(p.value("limit", kMaxListedAssets), 1, kMaxListedAssets);
			        const auto files = io::FileSystem::Glob(pattern);
			        if (!files.has_value())
			        {
				        return json{{"error", "glob failed: " + files.error().ToString()}};
			        }
			        // Glob returns mount-relative paths; re-prefix with the pattern's
			        // mount so results feed straight back into other methods.
			        std::string mount;
			        if (const std::size_t scheme = pattern.find("://"); scheme != std::string::npos)
			        {
				        mount = pattern.substr(0, scheme + 3);
			        }
			        json list = json::array();
			        for (const std::string& file: *files)
			        {
				        if (list.size() >= limit)
				        {
					        break;
				        }
				        list.push_back(file.starts_with(mount) ? file : mount + file);
			        }
			        return json{{"files", std::move(list)}, {"matched", files->size()}, {"truncated", files->size() > limit}};
		        }});
	}
} // namespace aether::editor
