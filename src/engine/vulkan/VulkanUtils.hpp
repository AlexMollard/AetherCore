#pragma once

#include <cstdint>

#include "vulkan/volk.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::vkutil
{
	inline void TransitionImage(VkCommandBuffer cmd,
	        VkImage image,
	        VkImageLayout oldLayout,
	        VkImageLayout newLayout,
	        VkPipelineStageFlags2 srcStage,
	        VkAccessFlags2 srcAccess,
	        VkPipelineStageFlags2 dstStage,
	        VkAccessFlags2 dstAccess,
	        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
	{
		const VkImageMemoryBarrier2 barrier{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		        .srcStageMask = srcStage,
		        .srcAccessMask = srcAccess,
		        .dstStageMask = dstStage,
		        .dstAccessMask = dstAccess,
		        .oldLayout = oldLayout,
		        .newLayout = newLayout,
		        .image = image,
		        .subresourceRange = {.aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
		};
		const VkDependencyInfo dep{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .imageMemoryBarrierCount = 1,
		        .pImageMemoryBarriers = &barrier,
		};
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	inline void TransitionImages(VkCommandBuffer cmd, const VkImageMemoryBarrier2* barriers, uint32_t count)
	{
		if (count == 0)
		{
			return;
		}
		const VkDependencyInfo dep{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .imageMemoryBarrierCount = count,
		        .pImageMemoryBarriers = barriers,
		};
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	inline VkResult HostCopyToImage(VkDevice device, VkImage dstImage, const void* hostData, uint32_t width, uint32_t height)
	{
		const VkHostImageLayoutTransitionInfo transition{
		        .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
		        .image = dstImage,
		        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
		};
		VkResult result = vkTransitionImageLayout(device, 1, &transition);
		if (result != VK_SUCCESS)
		{
			return result;
		}

		const VkMemoryToImageCopy region{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
		        .pHostPointer = hostData,
		        .memoryRowLength = 0,
		        .memoryImageHeight = 0,
		        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
		        .imageOffset = {.x = 0, .y = 0, .z = 0},
		        .imageExtent = {.width = width, .height = height, .depth = 1},
		};
		const VkCopyMemoryToImageInfo copyInfo{
		        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
		        .dstImage = dstImage,
		        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
		        .regionCount = 1,
		        .pRegions = &region,
		};
		return vkCopyMemoryToImage(device, &copyInfo);
	}

	// Engine-side overload: opaque gpu::Device + gpu::ImageView. Translates
	// at the seam so engine callers don't need to mention Vk* types.
	// Returns the raw VkResult so callers can still throw AetherError::Vulkan.
	inline std::int32_t HostCopyToImage(gpu::Device device, gpu::ImageView dstImage, const void* hostData, uint32_t width, uint32_t height)
	{
		return static_cast<std::int32_t>(HostCopyToImage(static_cast<VkDevice>(device), static_cast<VkImage>(dstImage), hostData, width, height));
	}

	// -- Debug object naming -----------------------------------------------------

	// Thread-local storage for the debug-utils function pointer.
	// Set once at engine init via SetObjectNameFunction.
	inline PFN_vkSetDebugUtilsObjectNameEXT g_setObjectNameFn = nullptr;

	inline void SetObjectNameFunction(PFN_vkSetDebugUtilsObjectNameEXT fn)
	{
		g_setObjectNameFn = fn;
	}

	inline void SetObjectName(VkDevice device, std::uint64_t handle, VkObjectType type, const char* name)
	{
		if (g_setObjectNameFn == nullptr || device == VK_NULL_HANDLE || handle == 0 || name == nullptr)
		{
			return;
		}
		const VkDebugUtilsObjectNameInfoEXT info{
		        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		        .objectType = type,
		        .objectHandle = handle,
		        .pObjectName = name,
		};
		g_setObjectNameFn(device, &info);
	}
} // namespace aether::vkutil
