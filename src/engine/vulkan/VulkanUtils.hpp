#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "vulkan/volk.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::vkutil
{
	// Host-image-copy destination layouts the device supports, cached once at context
	// init (vkTransitionImageLayout may only target layouts from this list).
	inline std::vector<VkImageLayout> g_hostImageCopyDstLayouts;

	inline void SetHostImageCopyDstLayouts(std::vector<VkImageLayout> layouts)
	{
		g_hostImageCopyDstLayouts = std::move(layouts);
	}

	inline bool SupportsHostImageLayout(VkImageLayout layout)
	{
		return std::find(g_hostImageCopyDstLayouts.begin(), g_hostImageCopyDstLayouts.end(), layout) != g_hostImageCopyDstLayouts.end();
	}

	// Host-side layout transition (no queue, no command buffer, returns complete).
	inline VkResult HostTransitionImage(VkDevice device, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
	{
		const VkHostImageLayoutTransitionInfo transition{
		        .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
		        .image = image,
		        .oldLayout = oldLayout,
		        .newLayout = newLayout,
		        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
		};
		return vkTransitionImageLayout(device, 1, &transition);
	}

	// Finish a host-copied texture on the CPU: GENERAL -> SHADER_READ_ONLY_OPTIMAL with
	// no queue submission. Only valid when SupportsHostImageLayout(SHADER_READ_ONLY).
	inline std::int32_t HostTransitionImageToShaderRead(gpu::Device device, gpu::ImageView image)
	{
		return static_cast<std::int32_t>(HostTransitionImage(static_cast<VkDevice>(device), static_cast<VkImage>(image), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	}

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
		const VkResult result = vkTransitionImageLayout(device, 1, &transition);
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

	inline std::int32_t HostCopyToImage(gpu::Device device, gpu::ImageView dstImage, const void* hostData, uint32_t width, uint32_t height)
	{
		return static_cast<std::int32_t>(HostCopyToImage(static_cast<VkDevice>(device), static_cast<VkImage>(dstImage), hostData, width, height));
	}

	// Thread-local storage for the debug-utils function pointer.
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
