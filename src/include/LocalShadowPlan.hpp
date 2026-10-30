#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace aether
{
	inline constexpr std::uint32_t kLocalShadowCandidateLimit = 256;
	inline constexpr std::uint32_t kLocalShadowEntryLimit = 48;
	inline constexpr std::uint32_t kLocalPointShadowResolution = 384;
	inline constexpr std::uint32_t kLocalSpotShadowResolution = 512;

	struct LocalShadowCandidate
	{
		glm::vec3 position;
		float radius, sourceRadius;
		std::uint32_t lightIndex, lightType;
		float distanceSq;
	};

	template<typename Points, typename Spots>
	std::vector<LocalShadowCandidate> LocalShadowCandidates(const Points& points, const Spots& spots, glm::vec3 camera)
	{
		std::vector<LocalShadowCandidate> candidates;
		auto add = [&](const auto& lights, std::uint32_t type)
		{
			for (std::size_t i = 0; i < lights.size() && candidates.size() < kLocalShadowCandidateLimit; ++i)
			{
				const auto& light = lights[i];
				if (!light.castsShadow)
				{
					continue;
				}
				const auto delta = light.position - camera;
				candidates.push_back({light.position, light.radius, light.sourceRadius, static_cast<std::uint32_t>(i), type, glm::dot(delta, delta)});
			}
		};
		add(points, 0);
		add(spots, 1);
		std::sort(candidates.begin(),
		        candidates.end(),
		        [](const auto& a, const auto& b)
		        {
			        if (std::abs(a.distanceSq - b.distanceSq) > 1e-4f)
			        {
				        return a.distanceSq < b.distanceSq;
			        }
			        if (a.lightType != b.lightType)
			        {
				        return a.lightType < b.lightType;
			        }
			        return a.lightIndex < b.lightIndex;
		        });
		return candidates;
	}

	inline std::vector<LocalShadowCandidate> SelectLocalShadows(std::vector<LocalShadowCandidate> candidates)
	{
		std::uint32_t entries = 0;
		auto end = std::remove_if(candidates.begin(),
		        candidates.end(),
		        [&](const auto& candidate)
		        {
			        const auto count = candidate.lightType == 0 ? 6u : 1u;
			        if (entries + count > kLocalShadowEntryLimit)
			        {
				        return true;
			        }
			        entries += count;
			        return false;
		        });
		candidates.erase(end, candidates.end());
		return candidates;
	}

	struct ShadowShelf
	{
		unsigned y, height, used;
	};

	inline glm::uvec4 AllocateShadowShelf(std::vector<ShadowShelf>& shelves, unsigned atlasWidth, unsigned atlasHeight, unsigned width, unsigned height)
	{
		if (!width || !height || width > atlasWidth || height > atlasHeight)
		{
			return {};
		}
		auto shelf = std::find_if(shelves.begin(), shelves.end(), [&](const ShadowShelf& s) { return s.height >= height && s.used + width <= atlasWidth; });
		if (shelf == shelves.end())
		{
			const unsigned y = shelves.empty() ? 0 : shelves.back().y + shelves.back().height;
			if (y + height > atlasHeight)
			{
				return {};
			}
			shelves.push_back({y, height, 0});
			shelf = std::prev(shelves.end());
		}
		const glm::uvec4 region(shelf->used, shelf->y, width, height);
		shelf->used += width;
		return region;
	}

	struct PackedShadowAtlas
	{
		std::vector<glm::vec4> regions;
		unsigned width = 1, height = 1;
	};

	inline PackedShadowAtlas PackShadowAtlas(const std::vector<unsigned>& sizes, unsigned maxDimension)
	{
		PackedShadowAtlas result;
		if (sizes.empty())
		{
			return result;
		}
		const unsigned minimum = *std::max_element(sizes.begin(), sizes.end());
		std::uint64_t bestArea = UINT64_MAX;
		for (unsigned width = minimum; width <= maxDimension;)
		{
			std::vector<ShadowShelf> shelves;
			std::vector<glm::vec4> regions;
			unsigned height = 0, usedWidth = 0;
			for (const auto size: sizes)
			{
				const auto region = AllocateShadowShelf(shelves, width, maxDimension, size, size);
				if (!region.z)
				{
					regions.clear();
					break;
				}
				regions.emplace_back(region);
				height = std::max(height, region.y + region.w);
				usedWidth = std::max(usedWidth, region.x + region.z);
			}
			const auto area = std::uint64_t(usedWidth) * height;
			if (regions.size() == sizes.size() && area < bestArea)
			{
				bestArea = area;
				result = {std::move(regions), usedWidth, height};
			}
			if (width == maxDimension)
			{
				break;
			}
			width = width > maxDimension / 2 ? maxDimension : width * 2;
		}
		return result;
	}
} // namespace aether
