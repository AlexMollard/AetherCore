#include <doctest/doctest.h>

#include "gpu/TextureUpload.hpp"
#include "vulkan/HostImageCopy.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace
{
	int s_imageQueries = 0;
	int s_formatQueries = 0;
	VkResult s_imageResult = VK_SUCCESS;
	bool s_formatSupportsHostCopy = true;
	VkPhysicalDeviceImageFormatInfo2 s_lastImageInfo{};

	VKAPI_ATTR void VKAPI_CALL QueryFormat(VkPhysicalDevice, VkFormat, VkFormatProperties2* properties)
	{
		++s_formatQueries;
		auto* properties3 = static_cast<VkFormatProperties3*>(properties->pNext);
		properties3->optimalTilingFeatures = s_formatSupportsHostCopy ? VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT : 0;
	}

	VKAPI_ATTR VkResult VKAPI_CALL QueryImage(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2* info, VkImageFormatProperties2* properties)
	{
		++s_imageQueries;
		s_lastImageInfo = *info;
		properties->imageFormatProperties = {.maxExtent = {1024, 1024, 1}, .maxMipLevels = 11, .maxArrayLayers = 1, .sampleCounts = VK_SAMPLE_COUNT_1_BIT};
		return s_imageResult;
	}

	VKAPI_ATTR VkResult VKAPI_CALL CopyMemory(VkDevice, const VkCopyMemoryToImageInfo*) { return VK_SUCCESS; }
	VKAPI_ATTR VkResult VKAPI_CALL Transition(VkDevice, uint32_t, const VkHostImageLayoutTransitionInfo*) { return VK_SUCCESS; }

	struct HostCopyStubs
	{
		PFN_vkGetPhysicalDeviceFormatProperties2 format = vkGetPhysicalDeviceFormatProperties2;
		PFN_vkGetPhysicalDeviceImageFormatProperties2 image = vkGetPhysicalDeviceImageFormatProperties2;
		PFN_vkCopyMemoryToImage copy = vkCopyMemoryToImage;
		PFN_vkTransitionImageLayout transition = vkTransitionImageLayout;
		std::vector<VkImageLayout> layouts = aether::vkutil::g_hostImageCopyDstLayouts;

		HostCopyStubs()
		{
			vkGetPhysicalDeviceFormatProperties2 = &QueryFormat;
			vkGetPhysicalDeviceImageFormatProperties2 = &QueryImage;
			vkCopyMemoryToImage = &CopyMemory;
			vkTransitionImageLayout = &Transition;
			s_imageQueries = 0;
			s_formatQueries = 0;
			s_imageResult = VK_SUCCESS;
			s_formatSupportsHostCopy = true;
			aether::vkutil::SetHostImageCopyDstLayouts({VK_IMAGE_LAYOUT_GENERAL});
			aether::vulkan::SetHostImageCopySupport(reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{1}), true);
		}

		~HostCopyStubs()
		{
			aether::vulkan::SetHostImageCopySupport(VK_NULL_HANDLE, false);
			aether::vkutil::SetHostImageCopyDstLayouts(std::move(layouts));
			vkGetPhysicalDeviceFormatProperties2 = format;
			vkGetPhysicalDeviceImageFormatProperties2 = image;
			vkCopyMemoryToImage = copy;
			vkTransitionImageLayout = transition;
		}
	};
}

TEST_CASE("Host image copy disabled capability performs no format queries")
{
	const HostCopyStubs stubs;
	aether::vulkan::SetHostImageCopySupport(reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{1}), false);
	CHECK_FALSE(aether::gpu::CanUseHostImageCopy(aether::gpu::Format::R8G8B8A8Unorm, {64, 64}, 7));
	CHECK(s_imageQueries == 0);
	CHECK(s_formatQueries == 0);
}

TEST_CASE("Host image copy requires format support and the exact image usage")
{
	const HostCopyStubs stubs;
	CHECK(aether::gpu::CanUseHostImageCopy(aether::gpu::Format::R8G8B8A8Unorm, {64, 64}, 7));
	CHECK(s_lastImageInfo.usage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT));
	CHECK(s_lastImageInfo.tiling == VK_IMAGE_TILING_OPTIMAL);
	s_formatSupportsHostCopy = false;
	CHECK_FALSE(aether::gpu::CanUseHostImageCopy(aether::gpu::Format::R32G32B32A32Sfloat, {64, 64}, 1));
	s_formatSupportsHostCopy = true;
	s_imageResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
	CHECK_FALSE(aether::gpu::CanUseHostImageCopy(aether::gpu::Format::BC7UnormBlock, {64, 64}, 7));
}

TEST_CASE("Host image copy rejects unsupported dimensions layouts and entry points")
{
	const HostCopyStubs stubs;
	CHECK_FALSE(aether::gpu::CanUseHostImageCopy(aether::gpu::Format::R8G8B8A8Unorm, {2048, 64}, 1));
	CHECK_FALSE(aether::gpu::CanUseHostImageCopy(aether::gpu::Format::R8G8B8A8Unorm, {64, 64}, 12));
	CHECK_FALSE(aether::gpu::CanUseHostImageCopy(aether::gpu::Format::R8G8B8A8Unorm, {64, 64}, 8));
	aether::vkutil::SetHostImageCopyDstLayouts({VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
	CHECK_FALSE(aether::gpu::CanUseHostImageCopy(aether::gpu::Format::R8G8B8A8Unorm, {64, 64}, 1));
	aether::vkutil::SetHostImageCopyDstLayouts({VK_IMAGE_LAYOUT_GENERAL});
	vkCopyMemoryToImage = nullptr;
	CHECK_FALSE(aether::gpu::CanUseHostImageCopy(aether::gpu::Format::R8G8B8A8Unorm, {64, 64}, 1));
}
