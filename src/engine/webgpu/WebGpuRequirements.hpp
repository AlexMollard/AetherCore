#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace aether::webgpu
{
	struct AdapterLimitRequirement
	{
		std::string_view name;
		std::uint64_t minimum;
	};

	inline constexpr auto kAdapter3DLimits = std::to_array<AdapterLimitRequirement>({
#define AE_WEBGPU_LIMIT(name, value) {#name, value},
#define AE_WEBGPU_FEATURE(name)
#define AE_WEBGPU_FORMAT(name)
#include "WebGpuRequirements.inc"
#undef AE_WEBGPU_LIMIT
#undef AE_WEBGPU_FEATURE
#undef AE_WEBGPU_FORMAT
	});

	// The getter returns the adapter's supported value for the named limit.
	template<typename Getter>
	std::string UnmetAdapter3DLimits(Getter get)
	{
		std::string result;
		for (const auto& requirement: kAdapter3DLimits)
		{
			const auto supported = get(requirement.name);
			if (supported < requirement.minimum)
			{
				if (!result.empty()) { result += "; "; }
				result += requirement.name;
				result += " (adapter=" + std::to_string(supported) + ", required=" + std::to_string(requirement.minimum) + ")";
			}
		}
		return result;
	}
}
