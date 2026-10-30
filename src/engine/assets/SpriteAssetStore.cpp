#include "assets/SpriteAssetStore.hpp"

#include <filesystem>
#include <utility>
#include "io/FileSystem.hpp"

namespace aether
{
	namespace
	{
		std::string CacheKey(std::string_view path)
		{
			if (path.starts_with("uid://")) return io::FileSystem::ResolveAssetPath(path);
			return std::string(path);
		}
	}
	Expected<const SpriteAtlasAsset*> SpriteAssetStore::LoadAtlas(std::string_view path)
	{
		if (path.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Sprite atlas path is empty."));
		}
		const std::string key = CacheKey(path);
		if (const auto it = m_atlases.find(key); it != m_atlases.end())
		{
			return &it->second;
		}
		AE_TRY(loaded, SpriteAtlasAsset::Load(std::filesystem::path(key)));
		const auto [it, inserted] = m_atlases.emplace(key, std::move(*loaded));
		(void) inserted;
		return &it->second;
	}

	Expected<const SpriteAnimationAsset*> SpriteAssetStore::LoadAnimation(std::string_view path)
	{
		if (path.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Sprite animation path is empty."));
		}
		const std::string key = CacheKey(path);
		if (const auto it = m_animations.find(key); it != m_animations.end())
		{
			return &it->second;
		}
		AE_TRY(loaded, SpriteAnimationAsset::Load(std::filesystem::path(key)));
		const auto [it, inserted] = m_animations.emplace(key, std::move(*loaded));
		(void) inserted;
		return &it->second;
	}

	bool SpriteAssetStore::PublishAtlas(std::string path, SpriteAtlasAsset atlas)
	{
		return m_atlases.emplace(std::move(path), std::move(atlas)).second;
	}

	bool SpriteAssetStore::PublishAnimation(std::string path, SpriteAnimationAsset animation)
	{
		return m_animations.emplace(std::move(path), std::move(animation)).second;
	}

	Expected<SpriteAtlasReimportDiagnostics> SpriteAssetStore::SaveAtlas(std::string path, SpriteAtlasAsset atlas)
	{
		SpriteAtlasReimportDiagnostics diagnostics;
		if (const auto existing = m_atlases.find(path); existing != m_atlases.end())
		{
			diagnostics = CompareSpriteAtlasReimport(existing->second, atlas);
		}
		else if (std::filesystem::exists(path))
		{
			if (auto previous = SpriteAtlasAsset::Load(path); previous.has_value())
			{
				diagnostics = CompareSpriteAtlasReimport(*previous, atlas);
			}
		}
		AE_TRY_VOID(atlas.Save(path));
		m_atlases.insert_or_assign(std::move(path), std::move(atlas));
		return diagnostics;
	}

	Expected<void> SpriteAssetStore::SaveAnimation(std::string path, SpriteAnimationAsset animation)
	{
		AE_TRY_VOID(animation.Save(path));
		m_animations.insert_or_assign(std::move(path), std::move(animation));
		return {};
	}

	void SpriteAssetStore::Invalidate(std::string_view path)
	{
		m_atlases.erase(CacheKey(path));
		m_animations.erase(CacheKey(path));
	}

	void SpriteAssetStore::Clear()
	{
		m_atlases.clear();
		m_animations.clear();
	}
} // namespace aether
