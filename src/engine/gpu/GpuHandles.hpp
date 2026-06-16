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
	// Each handle pairs a 24-bit slot index with an 8-bit generation counter
	// incremented on slot reuse. The generation check makes stale-handle use
	// fail loudly in IsValid() / Resolve*() instead of silently re-allocating
	// over a live resource (a classic source of "the wrong texture is
	// showing" / "buffer got freed mid-frame" bugs).
	//
	// Phase 2 of the GPU refactor introduces these types. Consumer migration
	// happens in Phase 3 (gpu::CommandList) and Phase 5 (RenderGraph barriers).
	// Until then, raw VkImage/VkBuffer/VkPipeline continue to be used at the
	// engine boundary - the handle types are simply *available* for new code
	// and for the registry that the backend will populate in a follow-up.
	//
	// The handle struct is 32 bits so handles fit cleanly in a single
	// push-constant / draw-contract slot. The high 8 bits hold the
	// generation; the low 24 bits hold the slot index. This caps total
	// tracked resources per type at 16,777,216, which is far more than any
	// practical engine needs.
	namespace detail
	{
		inline constexpr std::uint32_t kInvalidIndex = 0x0000FFFFu;
		inline constexpr std::uint32_t kInvalidGeneration = 0u;
	} // namespace detail

	struct TextureHandle
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

		bool operator==(const TextureHandle& other) const noexcept
		{
			return bits == other.bits;
		}

		bool operator!=(const TextureHandle& other) const noexcept
		{
			return !(*this == other);
		}

		[[nodiscard]] static TextureHandle Make(std::uint32_t index, std::uint32_t generation) noexcept
		{
			AE_ASSERT(index <= detail::kInvalidIndex, "TextureHandle index out of range");
			AE_ASSERT(generation <= 0xFFFFu, "TextureHandle generation out of range");
			TextureHandle h{};
			h.bits = (index & 0x0000FFFFu) | ((generation & 0xFFFFu) << 16);
			return h;
		}
	};

	struct BufferHandle
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

		bool operator==(const BufferHandle& other) const noexcept
		{
			return bits == other.bits;
		}

		bool operator!=(const BufferHandle& other) const noexcept
		{
			return !(*this == other);
		}

		[[nodiscard]] static BufferHandle Make(std::uint32_t index, std::uint32_t generation) noexcept
		{
			AE_ASSERT(index <= detail::kInvalidIndex, "BufferHandle index out of range");
			AE_ASSERT(generation <= 0xFFFFu, "BufferHandle generation out of range");
			BufferHandle h{};
			h.bits = (index & 0x0000FFFFu) | ((generation & 0xFFFFu) << 16);
			return h;
		}
	};

	struct PipelineHandle
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

		bool operator==(const PipelineHandle& other) const noexcept
		{
			return bits == other.bits;
		}

		bool operator!=(const PipelineHandle& other) const noexcept
		{
			return !(*this == other);
		}

		[[nodiscard]] static PipelineHandle Make(std::uint32_t index, std::uint32_t generation) noexcept
		{
			AE_ASSERT(index <= detail::kInvalidIndex, "PipelineHandle index out of range");
			AE_ASSERT(generation <= 0xFFFFu, "PipelineHandle generation out of range");
			PipelineHandle h{};
			h.bits = (index & 0x0000FFFFu) | ((generation & 0xFFFFu) << 16);
			return h;
		}
	};

	struct SamplerHandle
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

		bool operator==(const SamplerHandle& other) const noexcept
		{
			return bits == other.bits;
		}

		bool operator!=(const SamplerHandle& other) const noexcept
		{
			return !(*this == other);
		}

		[[nodiscard]] static SamplerHandle Make(std::uint32_t index, std::uint32_t generation) noexcept
		{
			AE_ASSERT(index <= detail::kInvalidIndex, "SamplerHandle index out of range");
			AE_ASSERT(generation <= 0xFFFFu, "SamplerHandle generation out of range");
			SamplerHandle h{};
			h.bits = (index & 0x0000FFFFu) | ((generation & 0xFFFFu) << 16);
			return h;
		}
	};
} // namespace aether::gpu
