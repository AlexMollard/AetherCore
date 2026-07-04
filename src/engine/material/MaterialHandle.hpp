#pragma once

#include <cstdint>

namespace aether
{
	// Opaque, ref-counted material reference. index = GPU slot; generation guards
	// against use-after-release. Default-constructed handles are invalid.
	struct MaterialHandle
	{
		static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

		std::uint32_t index = kInvalidIndex;
		std::uint32_t generation = 0u;

		[[nodiscard]] bool IsValid() const { return index != kInvalidIndex; }

		friend bool operator==(const MaterialHandle& a, const MaterialHandle& b)
		{
			return a.index == b.index && a.generation == b.generation;
		}
	};
} // namespace aether
