#pragma once

#include <cstdint>

#include "utils/Assert.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
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
