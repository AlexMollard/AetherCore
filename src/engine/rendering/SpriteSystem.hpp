#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "material/TextureHandle.hpp"

namespace aether
{
	class TextureRegistry;
	class SpriteAssetStore;
	class World;
	struct Render2DFrameData;

	class SpriteSystem
	{
	public:
		void Initialize(TextureRegistry& textures, SpriteAssetStore& assets);
		void Shutdown();
		void Extract(World& world, Render2DFrameData& output);

	private:
		[[nodiscard]] TextureHandle ResolveTexture(const std::string& path);

		TextureRegistry* m_textures = nullptr;
		SpriteAssetStore* m_assets = nullptr;
		struct CachedTexture { TextureHandle handle; std::uint64_t lastUsed = 0; };
		std::unordered_map<std::string, CachedTexture> m_textureCache;
		std::uint64_t m_extractCount = 0;
	};
} // namespace aether
