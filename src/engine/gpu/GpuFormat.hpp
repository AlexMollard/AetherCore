#pragma once

#include <cstdint>

namespace aether::gpu
{
	enum class Format : std::uint32_t
	{
		Undefined = 0,

		R8Unorm,
		R8G8B8A8Unorm,
		R8G8B8A8Srgb,
		B8G8R8A8Unorm,
		B8G8R8A8Srgb,

		R16G16B16A16Sfloat,

		R32Sfloat,
		R32G32Sfloat,
		R32G32B32Sfloat,
		R32G32B32A32Sfloat,

		D16Unorm,
		D32Sfloat,
		D24UnormS8Uint,
		X8D24UnormPack32,
		D16UnormS8Uint,
		D32SfloatS8Uint,

		BC4UnormBlock,
		BC7UnormBlock,
		BC7SrgbBlock,
	};

	inline constexpr std::uint32_t kFormatCount = static_cast<std::uint32_t>(Format::BC7SrgbBlock) + 1u;
} // namespace aether::gpu

namespace aether
{
	using GpuFormat = gpu::Format;
}
