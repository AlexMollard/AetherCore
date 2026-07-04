#pragma once

#include <cstdint>

namespace aether
{
	// Opaque, ref-counted texture reference. index = TextureRegistry ENTRY index
	// (never a bindless heap slot); generation guards against use-after-release.
	// Default-constructed handles are invalid (the "optional map not set" case).
	struct TextureHandle
	{
		static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;
		// A "broken" handle: a texture was REQUESTED but could not be loaded. It is
		// IsValid() (so packing routes it through ResolveSlot -> the magenta default,
		// a VISIBLE error) yet points at no real entry, so AddRef/Release are no-ops
		// and it never touches a live refcount. Distinct from the invalid handle,
		// which means "no texture requested" (-> kNoTexture, base colour).
		static constexpr std::uint32_t kBrokenIndex = 0xFFFFFFFEu;

		std::uint32_t index = kInvalidIndex;
		std::uint32_t generation = 0u;

		[[nodiscard]] bool IsValid() const
		{
			return index != kInvalidIndex;
		}

		[[nodiscard]] static TextureHandle Broken()
		{
			return TextureHandle{kBrokenIndex, 0u};
		}

		friend bool operator==(const TextureHandle& a, const TextureHandle& b)
		{
			return a.index == b.index && a.generation == b.generation;
		}
	};
} // namespace aether
