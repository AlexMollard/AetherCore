#pragma once

#include <cstdint>

namespace aether
{
	// (never a bindless heap slot); generation guards against use-after-release.
	struct TextureHandle
	{
		static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;
		// IsValid() (so packing routes it through ResolveSlot -> the magenta default,
		static constexpr std::uint32_t kBrokenIndex = 0xFFFFFFFEu;

		std::uint32_t index = kInvalidIndex;
		std::uint32_t generation = 0u;

		[[nodiscard]] bool IsValid() const
		{
			return index != kInvalidIndex;
		}

		[[nodiscard]] static TextureHandle Broken()
		{
			return TextureHandle{.index = kBrokenIndex, .generation = 0u};
		}

		friend bool operator==(const TextureHandle& a, const TextureHandle& b)
		{
			return a.index == b.index && a.generation == b.generation;
		}
	};
} // namespace aether
