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

	// Outgoing clip of a crossfade (SkinnedMeshComponent fadeClipIndex/fadeTime/fadeWeight/fadeLooping).
	struct PoseFade
	{
		std::uint32_t clip = 0;
		float time = 0.0f;
		bool looping = true;
		float weight = 0.0f;
	};

	// Model-space node transforms and global-joint * inverse-bind skin palettes.
	[[nodiscard]] Expected<ModelPose> EvaluateModelPose(const GltfAsset& asset, std::optional<std::uint32_t> clip = {}, float time = 0.0f, bool looping = true, const PoseFade* fade = nullptr);
} // namespace aether::assets
