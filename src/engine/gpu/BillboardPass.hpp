#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "rendering/Render2DFrameData.hpp"

namespace aether::gpu
{
	inline constexpr std::size_t kWebBillboardParticleLimit = 262144;
	inline constexpr std::size_t kWebBillboardDrawLimit = 8192;
	static_assert(sizeof(BillboardParticleInstance) == 64);

	[[nodiscard]] inline std::expected<std::vector<BillboardParticleInstance>, std::string> PlanBillboardPass(
	        std::span<const BillboardParticleInstance> source, const glm::mat4& view, std::size_t textureCount, std::size_t particleLimit = kWebBillboardParticleLimit, std::size_t drawLimit = kWebBillboardDrawLimit)
	{
		if (source.size() > particleLimit)
		{
			return std::unexpected("Web billboard particle frame exceeds the instance budget");
		}
		const auto finite = [](glm::vec4 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		};
		for (unsigned column = 0; column < 4; ++column)
		{
			if (!finite(view[column]))
			{
				return std::unexpected("Nonfinite billboard camera view");
			}
		}
		if (!std::isfinite(glm::determinant(view)) || std::abs(glm::determinant(view)) < 1e-12f)
		{
			return std::unexpected("Singular billboard camera view");
		}
		for (const auto& particle: source)
		{
			if (!finite(particle.positionSize) || !finite(particle.color) || !finite(particle.uvRect) || !std::isfinite(particle.rotationDegrees) || !finite(view * glm::vec4(glm::vec3(particle.positionSize), 1)))
			{
				return std::unexpected("Nonfinite billboard particle data or camera depth");
			}
			if (particle.positionSize.w < 0 || (particle.blendMode & ~(kBillboardAdditive | kBillboardDisplaySpace | kBillboardLit)) != 0)
			{
				return std::unexpected("Invalid billboard particle size or blend flags");
			}
			if (particle.textureIndex != UINT32_MAX && particle.textureIndex >= textureCount)
			{
				return std::unexpected("Billboard particle references missing Web texture");
			}
		}
		std::vector<BillboardParticleInstance> particles(source.begin(), source.end());
		std::stable_sort(particles.begin(), particles.end(), [&](const auto& a, const auto& b) { return (view * glm::vec4(glm::vec3(a.positionSize), 1)).z < (view * glm::vec4(glm::vec3(b.positionSize), 1)).z; });
		std::array<std::uint64_t, 2> previous{UINT64_MAX, UINT64_MAX};
		std::size_t draws = 0;
		for (const auto& particle: particles)
		{
			const auto phase = (particle.blendMode & kBillboardDisplaySpace) != 0 ? 1 : 0;
			const auto key = (std::uint64_t(particle.textureIndex) << 32) | particle.blendMode;
			if (previous[phase] != key)
			{
				if (++draws > drawLimit)
				{
					return std::unexpected("Web billboard frame exceeds 8192 sorted texture/blend draw batches");
				}
				previous[phase] = key;
			}
		}
		return particles;
	}
} // namespace aether::gpu
