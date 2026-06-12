#pragma once

#include <cstdint>

namespace aether::gpu
{
	// Engine-facing, Vulkan-free texture/image format enum.
	// Superset of formats referenced anywhere outside src/engine/vulkan/.
	// Translation to/from VkFormat lives in exactly one TU:
	// src/engine/vulkan/GpuEnumConversions.cpp.
	enum class Format : std::uint32_t
	{
		Undefined = 0,

		// 8-bit color
		R8Unorm,
		R8G8B8A8Unorm,
		R8G8B8A8Srgb,
		B8G8R8A8Unorm,
		B8G8R8A8Srgb,

		// 16-bit float color
		R16G16B16A16Sfloat,

		// 32-bit float
		R32G32Sfloat,
		R32G32B32Sfloat,
		R32G32B32A32Sfloat,

		// Depth / depth-stencil
		D16Unorm,
		D32Sfloat,
		D24UnormS8Uint,
		X8D24UnormPack32, // VK_FORMAT_X8_D24_UNORM_PACK32 - packed depth, no sampling
		D16UnormS8Uint,
		D32SfloatS8Uint,

		// Block-compressed (sampled only, no render-target use)
		BC4UnormBlock,
		BC7UnormBlock,
		BC7SrgbBlock,
	};

	inline constexpr std::uint32_t kFormatCount = static_cast<std::uint32_t>(Format::BC7SrgbBlock) + 1u;
} // namespace aether::gpu

// Short-form alias for engine-facing format enum.
// aether::GpuFormat and aether::gpu::Format are the same type; prefer
// aether::gpu::Format in new code to avoid the legacy alias.
namespace aether
{
	using GpuFormat = gpu::Format;
}
