#include "vulkan/HostImageCopy.hpp"

#include <cstdlib>
#include <string_view>
#include <algorithm>
#include <bit>

#include "gpu/TextureUpload.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether::vulkan
{
	namespace
	{
		VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
		bool g_enabled = false;
	}

	void SetHostImageCopySupport(VkPhysicalDevice physicalDevice, bool enabled) noexcept
	{
		g_physicalDevice = physicalDevice;
		g_enabled = enabled;
	}

	bool HostImageCopyDisabledByEnvironment() noexcept
	{
#if defined(_WIN32)
		char* value = nullptr;
		std::size_t length = 0;
		if (_dupenv_s(&value, &length, "AETHER_DISABLE_HOST_IMAGE_COPY") != 0 || value == nullptr)
		{
			return false;
		}
		const bool disabled = std::string_view(value) == "1";
		std::free(value);
		return disabled;
#else
		const char* value = std::getenv("AETHER_DISABLE_HOST_IMAGE_COPY");
		return value != nullptr && std::string_view(value) == "1";
#endif
	}
}

namespace aether::gpu
{
	bool CanUseHostImageCopy(Format format, Extent2D extent, std::uint32_t mipLevels) noexcept
	{
		if (!vulkan::g_enabled || vulkan::g_physicalDevice == VK_NULL_HANDLE || vkCopyMemoryToImage == nullptr || vkTransitionImageLayout == nullptr
		        || vkGetPhysicalDeviceFormatProperties2 == nullptr || vkGetPhysicalDeviceImageFormatProperties2 == nullptr
		        || !vkutil::SupportsHostImageLayout(VK_IMAGE_LAYOUT_GENERAL) || extent.width == 0 || extent.height == 0 || mipLevels == 0)
		{
			return false;
		}
		if (mipLevels > std::bit_width(std::max(extent.width, extent.height)))
		{
			return false;
		}
		const VkFormat vkFormat = ToVk(format);
		if (vkFormat == VK_FORMAT_UNDEFINED)
		{
			return false;
		}
		VkFormatProperties3 formatProperties3{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
		VkFormatProperties2 formatProperties2{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &formatProperties3};
		vkGetPhysicalDeviceFormatProperties2(vulkan::g_physicalDevice, vkFormat, &formatProperties2);
		if ((formatProperties3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT) == 0)
		{
			return false;
		}
		const VkPhysicalDeviceImageFormatInfo2 imageInfo{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		        .format = vkFormat,
		        .type = VK_IMAGE_TYPE_2D,
		        .tiling = VK_IMAGE_TILING_OPTIMAL,
		        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT,
		};
		VkImageFormatProperties2 imageProperties{.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
		if (vkGetPhysicalDeviceImageFormatProperties2(vulkan::g_physicalDevice, &imageInfo, &imageProperties) != VK_SUCCESS)
		{
			return false;
		}
		const auto& limits = imageProperties.imageFormatProperties;
		return extent.width <= limits.maxExtent.width && extent.height <= limits.maxExtent.height && limits.maxExtent.depth >= 1
		        && mipLevels <= limits.maxMipLevels && limits.maxArrayLayers >= 1 && (limits.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
	}
}
