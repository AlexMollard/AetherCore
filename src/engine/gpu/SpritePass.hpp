#pragma once

#include <algorithm>
#include <cstddef>
#include <expected>
#include <string>
#include "rendering/Render2DFrameData.hpp"

namespace aether::gpu
{
	struct alignas(16) SpriteGpuInstance
	{
		glm::vec4 world0, world1, world2, world3;
		glm::vec4 uvRect, color, sizeAndPivot;
		glm::uvec4 metadata; // flags, entity identity, reserved, reserved
	};

	static_assert(sizeof(SpriteGpuInstance) == 128 && offsetof(SpriteGpuInstance, color) == 80);

	struct SpriteBatch
	{
		std::uint32_t first, count, texture, blend;
		bool displaySpace = false;
	};

	struct SpritePass
	{
		std::vector<SpriteGpuInstance> instances;
		std::vector<SpriteBatch> batches;
	};

	// Transparent ordering is preserved; capacity and texture changes split draws.
	inline std::expected<SpritePass, std::string> PlanSpritePass(const Render2DFrameData& frame, std::uint32_t batchCapacity, std::uint32_t frameCapacity)
	{
		if (batchCapacity == 0 || frame.sprites.size() > frameCapacity)
		{
			return std::unexpected("Sprite pass exceeds the supported instance budget");
		}
		std::vector<const SpriteRenderInstance*> ordered;
		ordered.reserve(frame.sprites.size());
		for (const auto& sprite: frame.sprites)
		{
			ordered.push_back(&sprite);
		}
		std::stable_sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->sortKey < b->sortKey; });
		SpritePass pass;
		pass.instances.reserve(ordered.size());
		for (const auto* sprite: ordered)
		{
			if (sprite->blendMode > 3)
			{
				return std::unexpected("Unsupported sprite blend mode");
			}
			const bool displaySpace = (static_cast<std::uint32_t>(sprite->flags) & static_cast<std::uint32_t>(SpriteInstanceFlags::DisplaySpace)) != 0;
			if (pass.batches.empty() || pass.batches.back().count == batchCapacity || pass.batches.back().texture != sprite->textureIndex || pass.batches.back().blend != sprite->blendMode || pass.batches.back().displaySpace != displaySpace)
			{
				pass.batches.push_back({static_cast<std::uint32_t>(pass.instances.size()), 0, sprite->textureIndex, sprite->blendMode, displaySpace});
			}
			++pass.batches.back().count;
			pass.instances.push_back({sprite->world[0], sprite->world[1], sprite->world[2], sprite->world[3], sprite->uvRect, sprite->color, sprite->sizeAndPivot, {static_cast<std::uint32_t>(sprite->flags), sprite->entityId, 0, 0}});
		}
		return pass;
	}
} // namespace aether::gpu
