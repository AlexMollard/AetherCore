#pragma once

#include <string>
#include <unordered_map>

#include "material/TextureHandle.hpp"

namespace aether
{
	class TextureRegistry;
	class World;
	struct Render2DFrameData;

	class SpriteSystem
	{
	public:
		void Initialize(TextureRegistry& textures);
		void Shutdown();
		void Extract(World& world, Render2DFrameData& output);

	private:
		[[nodiscard]] TextureHandle ResolveTexture(const std::string& path);

		TextureRegistry* m_textures = nullptr;
		std::unordered_map<std::string, TextureHandle> m_textureCache;
	};
} // namespace aether
