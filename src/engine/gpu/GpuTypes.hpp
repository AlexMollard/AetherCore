#pragma once

#include <cstdint>

namespace aether
{
	enum class GpuFormat : std::uint32_t
	{
		Undefined = 0,
		R8G8B8A8Unorm,
		B8G8R8A8Srgb,
		R16G16B16A16Sfloat,
		D32Sfloat,
		D24UnormS8Uint,
	};

	inline constexpr std::uint32_t kMaxFramesInFlight = 3;

	struct GpuExtent2D
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;

		GpuExtent2D() = default;

		GpuExtent2D(std::uint32_t w, std::uint32_t h)
		      : width(w), height(h)
		{
		}

		template<typename VkExtent>
		explicit GpuExtent2D(const VkExtent& ext)
		      : width(ext.width), height(ext.height)
		{
		}
	};
} // namespace aether
