#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "assets/TileMapAsset.hpp"
#include "assets/TileSetAsset.hpp"

namespace aether
{
	// Cached tileset/tilemap loading, mirroring SpriteAssetStore. MutableTileMap
	// exposes the loaded asset for editor painting; SetCell bumps chunk
	// revisions, so render/collision caches converge without invalidation.
	class TileAssetStore
	{
	public:
		[[nodiscard]] Expected<const TileSetAsset*> LoadTileSet(std::string_view path);
		[[nodiscard]] Expected<const TileMapAsset*> LoadTileMap(std::string_view path);

		// Editor access to a loaded (or lazily loaded) map for cell edits.
		[[nodiscard]] TileMapAsset* MutableTileMap(std::string_view path);
		[[nodiscard]] TileSetAsset* MutableTileSet(std::string_view path);

		[[nodiscard]] Expected<void> SaveTileSet(std::string path, TileSetAsset tileSet);
		[[nodiscard]] Expected<void> SaveTileMap(std::string path, TileMapAsset tileMap);

		void Invalidate(std::string_view path);
		void Clear();

	private:
		std::unordered_map<std::string, TileSetAsset> m_tileSets;
		std::unordered_map<std::string, TileMapAsset> m_tileMaps;
	};
} // namespace aether
