#include "vulkan/GpuEnumConversions.hpp"

#include "vulkan/volk.hpp"

namespace aether::gpu
{
	// ─────────────────────────────────────────────────────────────────────────
	// Format
	// ─────────────────────────────────────────────────────────────────────────

	// All Format enum values are enumerated explicitly to satisfy
	// -Werror=switch-enum. Anything the engine does not (yet) use maps to
	// VK_FORMAT_UNDEFINED. Extending Format will force a compile error here,
	// prompting the engineer to decide on the VkFormat.
	VkFormat ToVk(Format format) noexcept
	{
		switch (format)
		{
			case Format::Undefined:            return VK_FORMAT_UNDEFINED;
			case Format::R8Unorm:              return VK_FORMAT_R8_UNORM;
			case Format::R8G8B8A8Unorm:        return VK_FORMAT_R8G8B8A8_UNORM;
			case Format::R8G8B8A8Srgb:         return VK_FORMAT_R8G8B8A8_SRGB;
			case Format::B8G8R8A8Unorm:        return VK_FORMAT_B8G8R8A8_UNORM;
			case Format::B8G8R8A8Srgb:         return VK_FORMAT_B8G8R8A8_SRGB;
			case Format::R16G16B16A16Sfloat:   return VK_FORMAT_R16G16B16A16_SFLOAT;
			case Format::R32G32Sfloat:         return VK_FORMAT_R32G32_SFLOAT;
			case Format::R32G32B32Sfloat:      return VK_FORMAT_R32G32B32_SFLOAT;
			case Format::R32G32B32A32Sfloat:   return VK_FORMAT_R32G32B32A32_SFLOAT;
			case Format::D16Unorm:             return VK_FORMAT_D16_UNORM;
			case Format::D32Sfloat:            return VK_FORMAT_D32_SFLOAT;
			case Format::D24UnormS8Uint:       return VK_FORMAT_D24_UNORM_S8_UINT;
			case Format::X8D24UnormPack32:     return VK_FORMAT_X8_D24_UNORM_PACK32;
			case Format::D16UnormS8Uint:       return VK_FORMAT_D16_UNORM_S8_UINT;
			case Format::D32SfloatS8Uint:      return VK_FORMAT_D32_SFLOAT_S8_UINT;
			case Format::BC4UnormBlock:        return VK_FORMAT_BC4_UNORM_BLOCK;
			case Format::BC7UnormBlock:        return VK_FORMAT_BC7_UNORM_BLOCK;
			case Format::BC7SrgbBlock:         return VK_FORMAT_BC7_SRGB_BLOCK;
		}
		return VK_FORMAT_UNDEFINED;
	}

	// VkFormat has ~250 entries and the engine only uses ~20 of them. The
	// project's existing pattern (see GpuDevice.cpp) is to silence -Wswitch-enum
	// for the VkFormat switch and funnel unknown values through Format::Undefined
	// via a default arm. This is preferable to enumerating hundreds of cases
	// that always return Undefined.
	Format FromVk(VkFormat format) noexcept
	{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif
		switch (format)
		{
			case VK_FORMAT_R8_UNORM:               return Format::R8Unorm;
			case VK_FORMAT_R8G8B8A8_UNORM:         return Format::R8G8B8A8Unorm;
			case VK_FORMAT_R8G8B8A8_SRGB:          return Format::R8G8B8A8Srgb;
			case VK_FORMAT_B8G8R8A8_UNORM:         return Format::B8G8R8A8Unorm;
			case VK_FORMAT_B8G8R8A8_SRGB:          return Format::B8G8R8A8Srgb;
			case VK_FORMAT_R16G16B16A16_SFLOAT:    return Format::R16G16B16A16Sfloat;
			case VK_FORMAT_R32G32_SFLOAT:          return Format::R32G32Sfloat;
			case VK_FORMAT_R32G32B32_SFLOAT:       return Format::R32G32B32Sfloat;
			case VK_FORMAT_R32G32B32A32_SFLOAT:    return Format::R32G32B32A32Sfloat;
			case VK_FORMAT_D16_UNORM:              return Format::D16Unorm;
			case VK_FORMAT_D32_SFLOAT:             return Format::D32Sfloat;
			case VK_FORMAT_D24_UNORM_S8_UINT:      return Format::D24UnormS8Uint;
			case VK_FORMAT_X8_D24_UNORM_PACK32:    return Format::X8D24UnormPack32;
			case VK_FORMAT_D16_UNORM_S8_UINT:      return Format::D16UnormS8Uint;
			case VK_FORMAT_D32_SFLOAT_S8_UINT:     return Format::D32SfloatS8Uint;
			case VK_FORMAT_BC4_UNORM_BLOCK:        return Format::BC4UnormBlock;
			case VK_FORMAT_BC7_UNORM_BLOCK:        return Format::BC7UnormBlock;
			case VK_FORMAT_BC7_SRGB_BLOCK:         return Format::BC7SrgbBlock;
			case VK_FORMAT_UNDEFINED:              return Format::Undefined;
			default:                               return Format::Undefined;
		}
#ifdef __clang__
#pragma clang diagnostic pop
#endif
	}

	bool IsDepthFormat(Format format) noexcept
	{
		switch (format)
		{
			case Format::D16Unorm:
			case Format::D32Sfloat:
			case Format::D24UnormS8Uint:
			case Format::X8D24UnormPack32:
			case Format::D16UnormS8Uint:
			case Format::D32SfloatS8Uint:
				return true;
			case Format::Undefined:
			case Format::R8Unorm:
			case Format::R8G8B8A8Unorm:
			case Format::R8G8B8A8Srgb:
			case Format::B8G8R8A8Unorm:
			case Format::B8G8R8A8Srgb:
			case Format::R16G16B16A16Sfloat:
			case Format::R32G32Sfloat:
			case Format::R32G32B32Sfloat:
			case Format::R32G32B32A32Sfloat:
			case Format::BC4UnormBlock:
			case Format::BC7UnormBlock:
			case Format::BC7SrgbBlock:
				return false;
		}
		return false;
	}

	bool IsStencilFormat(Format format) noexcept
	{
		switch (format)
		{
			case Format::D24UnormS8Uint:
			case Format::D16UnormS8Uint:
			case Format::D32SfloatS8Uint:
				return true;
			case Format::Undefined:
			case Format::R8Unorm:
			case Format::R8G8B8A8Unorm:
			case Format::R8G8B8A8Srgb:
			case Format::B8G8R8A8Unorm:
			case Format::B8G8R8A8Srgb:
			case Format::R16G16B16A16Sfloat:
			case Format::R32G32Sfloat:
			case Format::R32G32B32Sfloat:
			case Format::R32G32B32A32Sfloat:
			case Format::D16Unorm:
			case Format::D32Sfloat:
			case Format::X8D24UnormPack32:
			case Format::BC4UnormBlock:
			case Format::BC7UnormBlock:
			case Format::BC7SrgbBlock:
				return false;
		}
		return false;
	}

	bool IsBlockCompressed(Format format) noexcept
	{
		switch (format)
		{
			case Format::BC4UnormBlock:
			case Format::BC7UnormBlock:
			case Format::BC7SrgbBlock:
				return true;
			case Format::Undefined:
			case Format::R8Unorm:
			case Format::R8G8B8A8Unorm:
			case Format::R8G8B8A8Srgb:
			case Format::B8G8R8A8Unorm:
			case Format::B8G8R8A8Srgb:
			case Format::R16G16B16A16Sfloat:
			case Format::R32G32Sfloat:
			case Format::R32G32B32Sfloat:
			case Format::R32G32B32A32Sfloat:
			case Format::D16Unorm:
			case Format::D32Sfloat:
			case Format::D24UnormS8Uint:
			case Format::X8D24UnormPack32:
			case Format::D16UnormS8Uint:
			case Format::D32SfloatS8Uint:
				return false;
		}
		return false;
	}

	std::uint32_t BytesPerPixel(Format format) noexcept
	{
		// Block-compressed formats report 0 here - call sites that need
		// exact block-size accounting must use a dedicated BCn helper.
		switch (format)
		{
			case Format::R8Unorm:                return 1u;
			case Format::R8G8B8A8Unorm:          return 4u;
			case Format::R8G8B8A8Srgb:           return 4u;
			case Format::B8G8R8A8Unorm:          return 4u;
			case Format::B8G8R8A8Srgb:           return 4u;
			case Format::R16G16B16A16Sfloat:     return 8u;
			case Format::R32G32Sfloat:           return 8u;
			case Format::R32G32B32Sfloat:        return 12u;
			case Format::R32G32B32A32Sfloat:     return 16u;
			case Format::D16Unorm:               return 2u;
			case Format::D32Sfloat:              return 4u;
			case Format::D24UnormS8Uint:         return 4u;
			case Format::X8D24UnormPack32:       return 4u;
			case Format::D16UnormS8Uint:         return 4u;
			case Format::D32SfloatS8Uint:        return 8u;
			case Format::BC4UnormBlock:
			case Format::BC7UnormBlock:
			case Format::BC7SrgbBlock:
			case Format::Undefined:
				return 0u;
		}
		return 0u;
	}
} // namespace aether::gpu
