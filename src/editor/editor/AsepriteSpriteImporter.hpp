#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "utils/Expected.hpp"

namespace aether::editor
{
	struct AsepriteImportResult
	{
		SpriteAtlasAsset atlas;
		std::vector<SpriteAnimationAsset> animations;
		std::vector<std::string> diagnostics;
	};

	[[nodiscard]] Expected<AsepriteImportResult> ImportAsepriteSpriteMetadata(const std::filesystem::path& jsonPath, std::string texturePath, const SpriteAtlasAsset* previous = nullptr);
} // namespace aether::editor
