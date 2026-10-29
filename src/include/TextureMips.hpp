#pragma once

#include <algorithm>
#include <array>
#include <cmath>
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
	// sRGB colour uses alpha coverage; linear data channels are averaged independently.
	inline void DownsampleBox2x2(const uint8_t* src, int srcW, int srcH, int channels, uint8_t* dst, bool srgb)
	{
		static const std::array<float, 256> toLinear = []
		{
			std::array<float, 256> table{};
			for (std::size_t i = 0; i < table.size(); ++i)
			{
				const float c = static_cast<float>(i) / 255.0f;
				table[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
			}
			return table;
		}();
		const int dstW = std::max(1, srcW / 2);
		const int dstH = std::max(1, srcH / 2);
		for (int y = 0; y < dstH; ++y)
		{
			const int y0 = std::min(y * 2, srcH - 1);
			const int y1 = std::min(y0 + 1, srcH - 1);
			for (int x = 0; x < dstW; ++x)
			{
				const int x0 = std::min(x * 2, srcW - 1);
				const int x1 = std::min(x0 + 1, srcW - 1);
				const std::size_t taps[] = {
				        (std::size_t(y0) * srcW + x0) * channels,
				        (std::size_t(y0) * srcW + x1) * channels,
				        (std::size_t(y1) * srcW + x0) * channels,
				        (std::size_t(y1) * srcW + x1) * channels,
				};
				auto* out = dst + (std::size_t(y) * dstW + x) * channels;
				int alphaSum = 0;
				if (srgb && channels == 4)
				{
					for (const auto tap: taps) { alphaSum += src[tap + 3]; }
				}
				for (int c = 0; c < channels; ++c)
				{
					const bool decode = srgb && c < 3;
					const bool weighted = decode && channels == 4 && alphaSum != 0;
					float sum = 0.0f;
					for (const auto tap: taps)
					{
						const float value = decode ? toLinear[src[tap + c]] : static_cast<float>(src[tap + c]);
						sum += value * (weighted ? src[tap + 3] : 1);
					}
					float value = sum / (weighted ? static_cast<float>(alphaSum) : 4.0f);
					if (decode)
					{
						value = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
						value = std::clamp(value, 0.0f, 1.0f) * 255.0f;
					}
					out[c] = static_cast<uint8_t>(std::lround(value));
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

	inline std::optional<std::vector<RgbaTextureMip>> BuildRgbaTextureMips(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height, bool mipmapped, std::size_t budget, bool srgb)
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
			if (mipmapped && srgb)
			{
				DilateTransparentRgb(level.pixels.data(), static_cast<int>(level.width), static_cast<int>(level.height), 4);
			}
			if (index + 1 < plan->levelCount)
			{
				const auto nextWidth = std::max(1u, level.width / 2), nextHeight = std::max(1u, level.height / 2);
				std::vector<std::uint8_t> next(std::size_t(nextWidth) * nextHeight * 4);
				DownsampleBox2x2(level.pixels.data(), static_cast<int>(level.width), static_cast<int>(level.height), 4, next.data(), srgb);
				levels.push_back({nextWidth, nextHeight, std::move(next)});
			}
		}
		return levels;
	}
} // namespace aether
