#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAtlasAsset.hpp"

namespace aether
{
	class SpriteAssetStore
	{
	public:
		[[nodiscard]] Expected<const SpriteAtlasAsset*> LoadAtlas(std::string_view path);
		[[nodiscard]] Expected<const SpriteAnimationAsset*> LoadAnimation(std::string_view path);

		[[nodiscard]] Expected<SpriteAtlasReimportDiagnostics> SaveAtlas(std::string path, SpriteAtlasAsset atlas);
		[[nodiscard]] Expected<void> SaveAnimation(std::string path, SpriteAnimationAsset animation);

		void Invalidate(std::string_view path);
		void Clear();

	private:
		std::unordered_map<std::string, SpriteAtlasAsset> m_atlases;
		std::unordered_map<std::string, SpriteAnimationAsset> m_animations;
	};
} // namespace aether
