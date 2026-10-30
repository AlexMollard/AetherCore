#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/GltfAsset.hpp"
#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "assets/TileMapAsset.hpp"
#include "assets/TileSetAsset.hpp"
#include "material/MaterialSerializer.hpp"
#include "material/Texture.hpp"
#include "ui/FontAsset.hpp"
#include "utils/Expected.hpp"

namespace aether::assets
{
	struct StreamingPreparedTexture
	{
		std::string path;
		TextureColorSpace colorSpace = TextureColorSpace::Srgb;
		Texture::Prepared texture;
	};

	struct StreamingDecodeResult
	{
		std::optional<GltfAsset> model;
		std::optional<MaterialPresetSpec> material;
		std::optional<SpriteAtlasAsset> atlas;
		std::optional<SpriteAnimationAsset> animation;
		std::optional<TileSetAsset> tileSet;
		std::optional<TileMapAsset> tileMap;
		std::optional<ui::FontAsset> font;
		std::vector<StreamingPreparedTexture> textures;
		std::vector<std::byte> bytes;
		std::vector<std::string> dependencies;
		std::size_t byteSize = 0;
	};

	// Each successful reservation belongs to the caller, including on failure.
	[[nodiscard]] Expected<StreamingDecodeResult> DecodeStreamingAsset(std::string_view path, const std::function<bool(std::size_t)>& reserve, const std::function<bool()>& cancelled = {});
} // namespace aether::assets
