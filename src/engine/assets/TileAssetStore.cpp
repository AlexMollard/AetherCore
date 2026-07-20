#include "assets/TileAssetStore.hpp"

#include <filesystem>
#include <format>
#include <optional>
#include <utility>

#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

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
		tileMap.dirty = false;
		AE_TRY_VOID(tileMap.Save(path));
		m_tileMaps.insert_or_assign(std::move(path), std::move(tileMap));
		return {};
	}

	Expected<void> TileAssetStore::FlushDirtyTileMaps()
	{
		std::optional<AetherError> firstError;
		int flushed = 0;
		int failed = 0;
		for (auto& [path, tileMap]: m_tileMaps)
		{
			if (!tileMap.dirty)
			{
				continue;
			}
			if (path.empty())
			{
				// A cached map with no path cannot be persisted; clear the flag so it
				// does not block every future flush, and warn (this should never happen).
				AE_WARN(LogCategory::Asset, "Discarding dirty flag on a tilemap with no path - edits cannot be saved.");
				tileMap.dirty = false;
				continue;
			}
			if (auto saved = tileMap.Save(path); saved.has_value())
			{
				tileMap.dirty = false; // in sync with disk
				++flushed;
			}
			else
			{
				// Leave dirty = true so a later flush (or Save As) can retry, and keep
				// the first failure to report; still attempt the remaining maps.
				++failed;
				AE_WARN(LogCategory::Asset, "Failed to save edited tilemap '{}': {}", path, saved.error().message);
				if (!firstError.has_value())
				{
					firstError = std::move(saved.error());
				}
			}
		}
		if (flushed > 0)
		{
			AE_INFO(LogCategory::Asset, "Flushed {} edited tilemap(s) to disk{}.", flushed, failed > 0 ? std::format(" ({} failed)", failed) : std::string{});
		}
		if (firstError.has_value())
		{
			return std::unexpected(std::move(*firstError));
		}
		return {};
	}

	bool TileAssetStore::AnyTileMapDirty() const
	{
		for (const auto& [path, tileMap]: m_tileMaps)
		{
			if (tileMap.dirty)
			{
				return true;
			}
		}
		return false;
	}

	std::unordered_map<std::string, TileMapAsset> TileAssetStore::SnapshotTileMaps() const
	{
		return m_tileMaps;
	}

	void TileAssetStore::RestoreTileMaps(std::unordered_map<std::string, TileMapAsset> snapshot)
	{
		m_tileMaps = std::move(snapshot);
	}

	void TileAssetStore::Invalidate(std::string_view path)
	{
		m_tileSets.erase(std::string(path));
		if (const auto it = m_tileMaps.find(std::string(path)); it != m_tileMaps.end())
		{
			if (it->second.dirty)
			{
				// Reloading from disk drops unsaved cell edits - surface it rather than
				// losing painting silently (callers should flush first if intended).
				AE_WARN(LogCategory::Asset, "Invalidating tilemap '{}' with unsaved edits - in-memory changes are discarded.", path);
			}
			m_tileMaps.erase(it);
		}
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
