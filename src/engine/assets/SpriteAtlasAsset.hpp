#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "assets/AssetTypes.hpp"

namespace aether
{
	struct SpriteRegion
	{
		AssetObjectId id{};
		std::string name;
		glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f};
		glm::vec2 pixelSize{1.0f};
		glm::vec2 pivot{0.5f};
	};

	struct SpriteAtlasAsset
	{
		std::string texturePath;
		float pixelsPerUnit = 100.0f;
		std::vector<SpriteRegion> sprites;

		[[nodiscard]] static SpriteAtlasAsset WholeTexture(std::string texturePath, glm::vec2 pixelSize, float pixelsPerUnit = 100.0f, std::string_view name = "Sprite")
		{
			SpriteAtlasAsset atlas;
			atlas.texturePath = std::move(texturePath);
			atlas.pixelsPerUnit = pixelsPerUnit > 0.0f ? pixelsPerUnit : 100.0f;
			const AssetId owner = ComputeAssetId(MakeSpriteAtlasSource(atlas.texturePath));
			atlas.sprites.push_back(SpriteRegion{
			        .id = ComputeAssetObjectId(owner, "whole-texture"),
			        .name = std::string(name),
			        .uvRect = {0.0f, 0.0f, 1.0f, 1.0f},
			        .pixelSize = glm::max(pixelSize, glm::vec2(1.0f)),
			        .pivot = {0.5f, 0.5f},
			});
			return atlas;
		}

		[[nodiscard]] const SpriteRegion* Find(AssetObjectId id) const noexcept
		{
			for (const SpriteRegion& sprite: sprites)
			{
				if (sprite.id == id)
				{
					return &sprite;
				}
			}
			return nullptr;
		}
	};
} // namespace aether
