#include "assets/TileSetAsset.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <sstream>

#include "io/FileSystem.hpp"
#include "utils/TomlConfig.hpp"

namespace aether
{
	namespace
	{
		[[nodiscard]] std::string TileKey(std::size_t index, std::string_view field)
		{
			return std::format("tile.{}.{}", index, field);
		}

		[[nodiscard]] std::string FrameKey(std::size_t tileIndex, std::size_t frameIndex)
		{
			return std::format("tile.{}.frame.{}", tileIndex, frameIndex);
		}

		[[nodiscard]] std::string PropertyKey(std::size_t tileIndex, std::size_t propertyIndex, std::string_view field)
		{
			return std::format("tile.{}.property.{}.{}", tileIndex, propertyIndex, field);
		}

		[[nodiscard]] std::string IdToString(AssetObjectId id)
		{
			return std::format("{:016x}", id.value);
		}

		[[nodiscard]] AssetObjectId ParseId(std::string_view value)
		{
			AssetObjectId id{};
			const auto result = std::from_chars(value.data(), value.data() + value.size(), id.value, 16);
			return result.ec == std::errc{} ? id : AssetObjectId{};
		}

		[[nodiscard]] const char* CollisionName(TileCollisionKind kind)
		{
			return kind == TileCollisionKind::Full ? "full" : "none";
		}

		[[nodiscard]] TileCollisionKind CollisionFromName(std::string_view name)
		{
			return name == "full" ? TileCollisionKind::Full : TileCollisionKind::None;
		}
	} // namespace

	TileDefinition& TileSetAsset::AddTile(std::string atlasPath, AssetObjectId spriteId, std::string tileName)
	{
		const AssetId owner = ComputeAssetId(MakeTileSetSource(name));
		TileDefinition tile;
		tile.id = ComputeAssetObjectId(owner, atlasPath + "#" + IdToString(spriteId) + "#" + tileName);
		tile.name = std::move(tileName);
		tile.atlasPath = std::move(atlasPath);
		tile.spriteId = spriteId;
		tiles.push_back(std::move(tile));
		return tiles.back();
	}

	const TileDefinition* TileSetAsset::Find(AssetObjectId id) const noexcept
	{
		const auto it = std::ranges::find(tiles, id, &TileDefinition::id);
		return it != tiles.end() ? &*it : nullptr;
	}

	TileDefinition* TileSetAsset::Find(AssetObjectId id) noexcept
	{
		const auto it = std::ranges::find(tiles, id, &TileDefinition::id);
		return it != tiles.end() ? &*it : nullptr;
	}

	Expected<void> TileSetAsset::Save(const std::filesystem::path& path) const
	{
		TomlConfig config;
		config.Set("tileset.schema_version", static_cast<float>(kSchemaVersion));
		config.Set("tileset.name", name);
		config.Set("tileset.cell_size", cellSize);
		config.Set("tileset.tile_count", static_cast<float>(tiles.size()));
		for (std::size_t index = 0; index < tiles.size(); ++index)
		{
			const TileDefinition& tile = tiles[index];
			config.Set(TileKey(index, "id"), IdToString(tile.id));
			config.Set(TileKey(index, "name"), tile.name);
			config.Set(TileKey(index, "atlas"), tile.atlasPath);
			config.Set(TileKey(index, "sprite_id"), IdToString(tile.spriteId));
			config.Set(TileKey(index, "collision"), std::string_view{CollisionName(tile.collision)});
			config.Set(TileKey(index, "animation_fps"), tile.animationFps);
			config.Set(TileKey(index, "frame_count"), static_cast<float>(tile.animationFrames.size()));
			for (std::size_t frameIndex = 0; frameIndex < tile.animationFrames.size(); ++frameIndex)
			{
				config.Set(FrameKey(index, frameIndex), IdToString(tile.animationFrames[frameIndex]));
			}
			config.Set(TileKey(index, "property_count"), static_cast<float>(tile.properties.size()));
			std::size_t propertyIndex = 0;
			for (const auto& [key, value]: tile.properties)
			{
				config.Set(PropertyKey(index, propertyIndex, "key"), key);
				config.Set(PropertyKey(index, propertyIndex, "value"), value);
				++propertyIndex;
			}
		}

		const std::string pathString = path.generic_string();
		if (pathString.contains("://"))
		{
			std::ostringstream stream;
			config.Save(stream, "AetherCore tile set");
			AE_TRY_VOID(io::FileSystem::WriteFileText(pathString, stream.str()));
		}
		else if (!config.SaveToPath(path, "AetherCore tile set"))
		{
			AE_UNEXPECTED(AetherError::Asset("Failed to save tile set '" + path.string() + "'."));
		}
		return {};
	}

	Expected<TileSetAsset> TileSetAsset::Load(const std::filesystem::path& path)
	{
		TomlConfig config;
		const std::string pathString = path.generic_string();
		if (pathString.contains("://"))
		{
			AE_TRY(text, io::FileSystem::ReadFileText(pathString));
			config.Load(*text);
		}
		else if (!config.LoadFromPath(path))
		{
			AE_UNEXPECTED(AetherError::Asset("Failed to load tile set '" + path.string() + "'."));
		}

		TileSetAsset tileSet;
		tileSet.schemaVersion = static_cast<std::uint32_t>(config.GetFloat("tileset.schema_version", 1.0f));
		tileSet.name = config.GetString("tileset.name", "Tile Set");
		tileSet.cellSize = std::max(config.GetFloat("tileset.cell_size", 1.0f), 0.001f);
		const auto tileCount = static_cast<std::size_t>(config.GetFloat("tileset.tile_count", 0.0f));
		tileSet.tiles.reserve(tileCount);
		for (std::size_t index = 0; index < tileCount; ++index)
		{
			TileDefinition tile;
			tile.id = ParseId(config.GetString(TileKey(index, "id")));
			tile.name = config.GetString(TileKey(index, "name"));
			tile.atlasPath = config.GetString(TileKey(index, "atlas"));
			tile.spriteId = ParseId(config.GetString(TileKey(index, "sprite_id")));
			tile.collision = CollisionFromName(config.GetString(TileKey(index, "collision"), "none"));
			tile.animationFps = std::max(config.GetFloat(TileKey(index, "animation_fps"), 8.0f), 0.001f);
			const auto frameCount = static_cast<std::size_t>(config.GetFloat(TileKey(index, "frame_count"), 0.0f));
			tile.animationFrames.reserve(frameCount);
			for (std::size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
			{
				tile.animationFrames.push_back(ParseId(config.GetString(FrameKey(index, frameIndex))));
			}
			const auto propertyCount = static_cast<std::size_t>(config.GetFloat(TileKey(index, "property_count"), 0.0f));
			for (std::size_t propertyIndex = 0; propertyIndex < propertyCount; ++propertyIndex)
			{
				tile.properties.emplace(config.GetString(PropertyKey(index, propertyIndex, "key")), config.GetString(PropertyKey(index, propertyIndex, "value")));
			}
			if (!tile.id.IsValid())
			{
				continue; // corrupt entry: skip rather than poison lookups with id 0
			}
			tileSet.tiles.push_back(std::move(tile));
		}
		return tileSet;
	}
} // namespace aether
