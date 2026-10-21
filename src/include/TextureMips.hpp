#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>
#include <utility>
#include <TextureDilate.hpp>

namespace aether
{
	// Published native BCn textures filter encoded source bytes, including this legacy alpha
	// formula. Keep the Web RGBA chain identical before compression; loose native PNGs use
	// a separate CPU linear-light RGB/plain-alpha filter, not the published texture contract.
	inline void DownsampleBox2x2(const uint8_t* src, int srcW, int srcH, int channels, uint8_t* dst)
	{
		const int dstW = std::max(1, srcW / 2);
		const int dstH = std::max(1, srcH / 2);
		// PS2 cut-out textures carry black/garbage RGB in their fully transparent texels. Averaging
		// RGB unweighted bleeds that black into every mip, so alpha-tested foliage (grass, leaves)
		// darkens toward black with distance. Weight RGB by alpha instead (premultiplied-style);
		// alpha itself stays a plain average so coverage still shrinks.
		const bool hasAlpha = channels == 4;
		for (int y = 0; y < dstH; ++y)
		{
			for (int x = 0; x < dstW; ++x)
			{
				int sums[4] = {0, 0, 0, 0};
				int alphaSum = 0;
				int count = 0;
				for (int dy = 0; dy < 2; ++dy)
				{
					for (int dx = 0; dx < 2; ++dx)
					{
						const int sx = x * 2 + dx;
						const int sy = y * 2 + dy;
						if (sx < srcW && sy < srcH)
						{
							const uint8_t* px = &src[(sy * srcW + sx) * channels];
							const int weight = hasAlpha ? px[3] : 255;
							for (int c = 0; c < channels; ++c)
							{
								sums[c] += px[c] * weight;
							}
							alphaSum += hasAlpha ? px[3] : 255;
							++count;
						}
					}
				}
				uint8_t* out = &dst[(y * dstW + x) * channels];
				if (hasAlpha && alphaSum == 0)
				{
					out[0] = out[1] = out[2] = 0;
					out[3] = 0;
					continue;
				}
				for (int c = 0; c < channels; ++c)
				{
					const int divisor = (hasAlpha && c < 3) ? alphaSum : count * 255;
					out[c] = static_cast<uint8_t>((sums[c] + divisor / 2) / divisor);
				}
			}
		}
	}

	struct TextureMipPlan
	{
		std::uint32_t levelCount = 0;
		std::size_t totalBytes = 0;
	};

	inline std::optional<TextureMipPlan> PlanRgbaTextureMips(std::uint32_t width, std::uint32_t height, bool mipmapped, std::size_t budget)
	{
		if (!width || !height || width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max())
		{
			return std::nullopt;
		}
		// The shared native box filter uses signed int byte indices. Reject oversized
		// source levels before any allocation, even if the caller supplies a larger budget.
		if (std::size_t(std::numeric_limits<int>::max()) / 4 / width < height)
		{
			return std::nullopt;
		}
		TextureMipPlan plan;
		while (true)
		{
			const auto remaining = budget - plan.totalBytes;
			if (remaining / 4 / width < height)
			{
				return std::nullopt;
			}
			plan.totalBytes += std::size_t(width) * height * 4;
			++plan.levelCount;
			if (!mipmapped || (width == 1 && height == 1))
			{
				return plan;
			}
			width = std::max(1u, width / 2);
			height = std::max(1u, height / 2);
		}
	}

	struct RgbaTextureMip
	{
		std::uint32_t width, height;
		std::vector<std::uint8_t> pixels;
	};

	inline std::optional<std::vector<RgbaTextureMip>> BuildRgbaTextureMips(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height, bool mipmapped, std::size_t budget)
	{
		const auto plan = PlanRgbaTextureMips(width, height, mipmapped, budget);
		if (!plan || rgba.size() != std::size_t(width) * height * 4)
		{
			return std::nullopt;
		}
		std::vector<RgbaTextureMip> levels;
		levels.reserve(plan->levelCount);
		levels.push_back({width, height, {rgba.begin(), rgba.end()}});
		for (std::uint32_t index = 0; index < plan->levelCount; ++index)
		{
			auto& level = levels[index];
			if (mipmapped)
			{
				DilateTransparentRgb(level.pixels.data(), static_cast<int>(level.width), static_cast<int>(level.height), 4);
			}
			if (index + 1 < plan->levelCount)
			{
				const auto nextWidth = std::max(1u, level.width / 2), nextHeight = std::max(1u, level.height / 2);
				std::vector<std::uint8_t> next(std::size_t(nextWidth) * nextHeight * 4);
				DownsampleBox2x2(level.pixels.data(), static_cast<int>(level.width), static_cast<int>(level.height), 4, next.data());
				levels.push_back({nextWidth, nextHeight, std::move(next)});
			}
		}
		return levels;
	}
} // namespace aether
