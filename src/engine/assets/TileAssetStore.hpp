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

		// Persist every cached tilemap whose in-memory cells diverge from disk (edited
		// via MutableTileMap/SetCell but not yet saved), clearing each map's dirty flag.
		// Scene save and publish call this so the on-disk .tiles the project pak ships
		// matches the editor. Returns the first write error (remaining maps stay dirty).
		[[nodiscard]] Expected<void> FlushDirtyTileMaps();
		[[nodiscard]] bool AnyTileMapDirty() const;

		// Deep copy / restore of the whole tilemap cache. Used to snapshot tile state
		// at Play-enter and revert script-driven tile edits on Stop, mirroring the ECS
		// scene snapshot. Restore replaces the cache wholesale: maps edited during play
		// revert, maps loaded during play are dropped (reload clean on next access), and
		// pre-play unsaved editor edits are preserved (their dirty flag is in the copy).
		// Chunk render/collision caches key on revision with !=, so they rebuild even
		// when a restore moves a revision backward.
		[[nodiscard]] std::unordered_map<std::string, TileMapAsset> SnapshotTileMaps() const;
		void RestoreTileMaps(std::unordered_map<std::string, TileMapAsset> snapshot);

		void Invalidate(std::string_view path);
		void Clear();

		// Monotonic per-tileset edit counter: bumped on save/invalidate and by
		// editors after mutating via MutableTileSet. Collision caches key on it
		// so a tile's collision edit rebuilds bodies without touching chunks.
		[[nodiscard]] std::uint32_t TileSetGeneration(std::string_view path) const;
		void BumpTileSetGeneration(std::string_view path);

	private:
		std::unordered_map<std::string, TileSetAsset> m_tileSets;
		std::unordered_map<std::string, TileMapAsset> m_tileMaps;
		std::unordered_map<std::string, std::uint32_t> m_tileSetGenerations;
	};
} // namespace aether
