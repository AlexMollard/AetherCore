#include "assets/TileAssetStore.hpp"

#include <filesystem>
#include <utility>

namespace aether
{
	Expected<const TileSetAsset*> TileAssetStore::LoadTileSet(std::string_view path)
	{
		if (path.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Tile set path is empty."));
		}
		const std::string key(path);
		if (const auto it = m_tileSets.find(key); it != m_tileSets.end())
		{
			return &it->second;
		}
		AE_TRY(loaded, TileSetAsset::Load(std::filesystem::path(key)));
		const auto [it, inserted] = m_tileSets.emplace(key, std::move(*loaded));
		(void) inserted;
		return &it->second;
	}

	Expected<const TileMapAsset*> TileAssetStore::LoadTileMap(std::string_view path)
	{
		if (path.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Tile map path is empty."));
		}
		const std::string key(path);
		if (const auto it = m_tileMaps.find(key); it != m_tileMaps.end())
		{
			return &it->second;
		}
		AE_TRY(loaded, TileMapAsset::Load(std::filesystem::path(key)));
		const auto [it, inserted] = m_tileMaps.emplace(key, std::move(*loaded));
		(void) inserted;
		return &it->second;
	}

	TileMapAsset* TileAssetStore::MutableTileMap(std::string_view path)
	{
		if (const auto it = m_tileMaps.find(std::string(path)); it != m_tileMaps.end())
		{
			return &it->second;
		}
		if (const auto loaded = LoadTileMap(path); loaded.has_value())
		{
			return const_cast<TileMapAsset*>(*loaded);
		}
		return nullptr;
	}

	TileSetAsset* TileAssetStore::MutableTileSet(std::string_view path)
	{
		if (const auto it = m_tileSets.find(std::string(path)); it != m_tileSets.end())
		{
			return &it->second;
		}
		if (const auto loaded = LoadTileSet(path); loaded.has_value())
		{
			return const_cast<TileSetAsset*>(*loaded);
		}
		return nullptr;
	}

	Expected<void> TileAssetStore::SaveTileSet(std::string path, TileSetAsset tileSet)
	{
		AE_TRY_VOID(tileSet.Save(path));
		BumpTileSetGeneration(path);
		m_tileSets.insert_or_assign(std::move(path), std::move(tileSet));
		return {};
	}

	Expected<void> TileAssetStore::SaveTileMap(std::string path, TileMapAsset tileMap)
	{
		AE_TRY_VOID(tileMap.Save(path));
		m_tileMaps.insert_or_assign(std::move(path), std::move(tileMap));
		return {};
	}

	void TileAssetStore::Invalidate(std::string_view path)
	{
		m_tileSets.erase(std::string(path));
		m_tileMaps.erase(std::string(path));
		BumpTileSetGeneration(path);
	}

	std::uint32_t TileAssetStore::TileSetGeneration(std::string_view path) const
	{
		const auto it = m_tileSetGenerations.find(std::string(path));
		return it != m_tileSetGenerations.end() ? it->second : 0;
	}

	void TileAssetStore::BumpTileSetGeneration(std::string_view path)
	{
		++m_tileSetGenerations[std::string(path)];
	}

	void TileAssetStore::Clear()
	{
		m_tileSets.clear();
		m_tileMaps.clear();
	}
} // namespace aether
