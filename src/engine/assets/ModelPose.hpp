#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "utils/Expected.hpp"

namespace aether::assets
{
	struct GltfAsset;

	struct ModelPose
	{
		std::vector<glm::mat4> nodeWorld;
		std::vector<std::vector<glm::mat4>> skins;
	};

	// Model-space node transforms and global-joint * inverse-bind skin palettes.
	[[nodiscard]] Expected<ModelPose> EvaluateModelPose(const GltfAsset& asset, std::optional<std::uint32_t> clip = {}, float time = 0.0f, bool looping = true);
} // namespace aether::assets
