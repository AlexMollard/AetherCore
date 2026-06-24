#pragma once

#include <cstdint>

#include "utils/Assert.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	// -------------------------------------------------------------------------
	// Opaque resource handles
	// -------------------------------------------------------------------------
	// Engine-facing replacements for raw Vk* handles at the public API.
	// Each handle pairs a 16-bit slot index with a 16-bit generation counter
	// incremented on slot reuse. The generation check makes stale-handle use
	// fail loudly in IsValid() / Resolve*() instead of silently re-allocating
	// over a live resource (a classic source of "the wrong texture is
	// showing" / "buffer got freed mid-frame" bugs).
	//
	// The handle struct is 32 bits so handles fit cleanly in a single
	// push-constant / draw-contract slot. The high 16 bits hold the
	// generation; the low 16 bits hold the slot index. This caps total
	// tracked resources per type at 65,535, which is far more than any
	// practical engine needs.
	namespace detail
	{
		inline constexpr std::uint32_t kInvalidIndex = 0x0000FFFFu;
		inline constexpr std::uint32_t kInvalidGeneration = 0u;
	} // namespace detail

	template<typename Tag>
	struct TypedHandle
	{
		std::uint32_t bits = 0u;

		[[nodiscard]] bool IsValid() const noexcept
		{
			return GetGeneration() != detail::kInvalidGeneration && GetIndex() != detail::kInvalidIndex;
		}

		[[nodiscard]] std::uint32_t GetIndex() const noexcept
		{
			return bits & 0x0000FFFFu;
		}

		[[nodiscard]] std::uint32_t GetGeneration() const noexcept
		{
			return (bits >> 16) & 0xFFFFu;
		}

		bool operator==(const TypedHandle& other) const noexcept
		{
			return bits == other.bits;
		}

		bool operator!=(const TypedHandle& other) const noexcept
		{
			return !(*this == other);
		}

		[[nodiscard]] static TypedHandle Make(std::uint32_t index, std::uint32_t generation) noexcept
		{
			AE_ASSERT(index <= detail::kInvalidIndex, "TypedHandle index out of range");
			AE_ASSERT(generation <= 0xFFFFu, "TypedHandle generation out of range");
			TypedHandle h{};
			h.bits = (index & 0x0000FFFFu) | ((generation & 0xFFFFu) << 16);
			return h;
		}
	};

	using TextureHandle = TypedHandle<struct TextureTag>;
	using BufferHandle = TypedHandle<struct BufferTag>;
	using PipelineHandle = TypedHandle<struct PipelineTag>;
} // namespace aether::gpu
