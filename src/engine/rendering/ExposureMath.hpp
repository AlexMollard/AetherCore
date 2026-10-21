#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace aether
{
	inline float AdaptHistogramExposure(std::span<const std::uint32_t, 256> bins, float currentExposure, float key, float speed)
	{
		double total = 0.0;
		for (std::uint32_t i = 1; i < 256; ++i)
		{
			total += bins[i];
		}
		if (total < 1.0)
		{
			return currentExposure;
		}
		const double lowCount = total * 0.30f;
		const double highCount = total * 0.95f;
		double seen = 0.0, weighted = 0.0, weight = 0.0;
		for (std::uint32_t i = 1; i < 256; ++i)
		{
			const double count = bins[i];
			if (count <= 0.0)
			{
				continue;
			}
			const double start = seen;
			seen += count;
			const double lo = std::max(start, lowCount);
			const double hi = std::min(seen, highCount);
			if (hi <= lo)
			{
				continue;
			}
			const float logLuminance = -10.0f + (static_cast<float>(i) / 255.0f) * 20.0f;
			weighted += logLuminance * (hi - lo);
			weight += hi - lo;
		}
		if (weight <= 0.0)
		{
			return currentExposure;
		}
		const float luminance = std::exp2(static_cast<float>(weighted / weight));
		const float target = std::clamp(key / std::max(luminance, 1e-4f), 0.03f, 30.0f);
		const float blend = 1.0f - std::exp(-speed * (1.0f / 60.0f));
		const float current = std::log2(std::max(currentExposure, 1e-4f));
		return std::exp2(current + (std::log2(target) - current) * blend);
	}
} // namespace aether
