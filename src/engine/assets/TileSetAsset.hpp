#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "assets/AssetTypes.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	enum class TileCollisionKind : std::uint8_t
	{
		None = 0,
		Full, // solid unit cell; merged into chunk chain outlines
		// Solid sub-rectangle of the cell (collisionRect) - for art that does
		// not fill its cell, e.g. thin platforms. Contiguous same-rect runs
		// merge into one box per row.
		Rect,
	};

	// One paintable tile: visuals come from a sprite atlas region (stable ids on
	// both sides so re-slicing an atlas never silently redirects tiles).
	struct TileDefinition
	{
		AssetObjectId id{}; // stable across tileset edits
		std::string name;
		std::string atlasPath;
		AssetObjectId spriteId{};
		TileCollisionKind collision = TileCollisionKind::None;
		// Rect collision only: (x, y, w, h) in cell fractions, y-up from the
		// cell's bottom-left. Default = the full cell.
		glm::vec4 collisionRect{0.0f, 0.0f, 1.0f, 1.0f};
		// Animated tiles: atlas region ids played at animationFps; empty = static.
		std::vector<AssetObjectId> animationFrames;
		float animationFps = 8.0f;
		std::map<std::string, std::string> properties; // custom authored key/values
	};

	struct TileSetAsset
	{
		static constexpr std::uint32_t kSchemaVersion = 1;

		std::uint32_t schemaVersion = kSchemaVersion;
		std::string name = "Tile Set";
		float cellSize = 1.0f; // world units per painted cell
		std::vector<TileDefinition> tiles;

		// Appends a tile with a stable id derived from the tileset name, atlas
		// path, region id, and tile name.
		TileDefinition& AddTile(std::string atlasPath, AssetObjectId spriteId, std::string tileName);

		[[nodiscard]] const TileDefinition* Find(AssetObjectId id) const noexcept;
		[[nodiscard]] TileDefinition* Find(AssetObjectId id) noexcept;

		[[nodiscard]] Expected<void> Save(const std::filesystem::path& path) const;
		[[nodiscard]] static Expected<TileSetAsset> Load(const std::filesystem::path& path);
	};
} // namespace aether
