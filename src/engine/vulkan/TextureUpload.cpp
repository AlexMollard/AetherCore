#include "gpu/TextureUpload.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

#include "gpu/OneShotCmd.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether::gpu
{
	namespace
	{
		struct FormatBlock
		{
			std::uint32_t width;
			std::uint32_t height;
			std::uint32_t bytes;
		};

		Expected<FormatBlock> UploadFormatBlock(Format format)
		{
#ifdef __clang__
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Wswitch-enum"
#endif
			switch (format)
			{
				case Format::R8G8B8A8Unorm:
				case Format::R8G8B8A8Srgb:
					return FormatBlock{1, 1, 4};
				case Format::R32G32B32A32Sfloat:
					return FormatBlock{1, 1, 16};
				case Format::BC4UnormBlock:
					return FormatBlock{4, 4, 8};
				case Format::BC7UnormBlock:
				case Format::BC7SrgbBlock:
					return FormatBlock{4, 4, 16};
				default:
					AE_UNEXPECTED(AetherError::Asset("Texture upload format is unsupported."));
			}
#ifdef __clang__
#	pragma clang diagnostic pop
#endif
		}

		void ImageBarrier(VkCommandBuffer cmd, VkImage image, std::uint32_t levels, VkImageLayout oldLayout,
		        VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
		        VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess)
		{
			const VkImageMemoryBarrier2 barrier{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			        .srcStageMask = sourceStage,
			        .srcAccessMask = sourceAccess,
			        .dstStageMask = destinationStage,
			        .dstAccessMask = destinationAccess,
			        .oldLayout = oldLayout,
			        .newLayout = newLayout,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image = image,
			        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1},
			};
			const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
			vkCmdPipelineBarrier2(cmd, &dependency);
		}

		struct StagingBuffer
		{
			BufferHandle handle;
			~StagingBuffer()
			{
				if (handle.IsValid())
				{
					ResourceRegistry::Destroy(handle);
				}
			}
		};
	}

	Expected<void> ValidateTextureUpload(Format format, Extent2D extent, std::uint32_t mipLevels,
	        std::size_t dataBytes, std::span<const ImageUploadRegion> regions)
	{
		AE_TRY(block, UploadFormatBlock(format));
		if (extent.width == 0 || extent.height == 0 || mipLevels == 0 || mipLevels > std::bit_width(std::max(extent.width, extent.height)) || regions.size() != mipLevels)
		{
			AE_UNEXPECTED(AetherError::Asset("Texture upload requires one complete region per mip level."));
		}
		std::uint64_t previousEnd = 0;
		for (std::uint32_t level = 0; level < mipLevels; ++level)
		{
			const auto& region = regions[level];
			const auto width = std::max(1u, extent.width >> level);
			const auto height = std::max(1u, extent.height >> level);
			if (region.mipLevel != level || region.x != 0 || region.y != 0 || region.width != width || region.height != height
			        || region.offset % std::max(4u, block->bytes) != 0 || region.offset < previousEnd || region.offset > dataBytes)
			{
				AE_UNEXPECTED(AetherError::Asset("Texture upload region has invalid bounds or alignment."));
			}
			const std::uint64_t blocksWide = (std::uint64_t(width) + block->width - 1) / block->width;
			const std::uint64_t blocksHigh = (std::uint64_t(height) + block->height - 1) / block->height;
			const auto remaining = dataBytes - static_cast<std::size_t>(region.offset);
			if (blocksWide > remaining / block->bytes / blocksHigh)
			{
				AE_UNEXPECTED(AetherError::Asset("Texture upload source data is truncated."));
			}
			previousEnd = region.offset + blocksWide * blocksHigh * block->bytes;
		}
		return {};
	}

	Expected<void> UploadTexture(Device device, Queue queue, CommandPool pool, TextureHandle texture,
	        std::span<const std::byte> data, std::span<const ImageUploadRegion> regions, bool useHostCopy)
	{
		if (device == nullptr || !texture.IsValid())
		{
			AE_UNEXPECTED(AetherError::Engine("Texture upload requires a device and texture."));
		}
		const auto resolved = ResourceRegistry::ResolveTexture(texture);
		const auto image = static_cast<VkImage>(ResourceRegistry::ResolveTextureImage(texture));
		if (image == VK_NULL_HANDLE || resolved.arrayLayers != 1)
		{
			AE_UNEXPECTED(AetherError::Engine("Texture upload requires a valid 2D texture."));
		}
		AE_TRY_VOID(ValidateTextureUpload(resolved.format, resolved.extent, resolved.mipLevels, data.size(), regions));
		const auto usage = static_cast<std::uint32_t>(resolved.usage);
		if ((usage & static_cast<std::uint32_t>(ImageUsage::Sampled)) == 0 || (usage & static_cast<std::uint32_t>(ImageUsage::TransferDst)) == 0)
		{
			AE_UNEXPECTED(AetherError::Engine("Texture upload requires sampled and transfer-destination usage."));
		}
		const auto vkDevice = static_cast<VkDevice>(device);
		if (useHostCopy)
		{
			if ((usage & static_cast<std::uint32_t>(ImageUsage::HostTransfer)) == 0 || !CanUseHostImageCopy(resolved.format, resolved.extent, resolved.mipLevels)
			        || vkCopyMemoryToImage == nullptr || vkTransitionImageLayout == nullptr)
			{
				AE_UNEXPECTED(AetherError::Engine("Texture does not support host image copy."));
			}
			std::vector<VkMemoryToImageCopy> copies;
			copies.reserve(regions.size());
			for (const auto& region: regions)
			{
				copies.push_back({.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
				        .pHostPointer = data.data() + region.offset,
				        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region.mipLevel, 0, 1},
				        .imageExtent = {region.width, region.height, 1}});
			}
			VkResult result = vkutil::HostTransitionImage(vkDevice, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, resolved.mipLevels);
			if (result != VK_SUCCESS)
			{
				AE_UNEXPECTED(AetherError::Vulkan(result, "Texture host-copy layout transition failed."));
			}
			const VkCopyMemoryToImageInfo copyInfo{.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO, .dstImage = image,
			        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL, .regionCount = static_cast<std::uint32_t>(copies.size()), .pRegions = copies.data()};
			result = vkCopyMemoryToImage(vkDevice, &copyInfo);
			if (result != VK_SUCCESS)
			{
				AE_UNEXPECTED(AetherError::Vulkan(result, "Texture host image copy failed."));
			}
			if (vkutil::SupportsHostImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
			{
				result = vkutil::HostTransitionImage(vkDevice, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, resolved.mipLevels);
				if (result != VK_SUCCESS)
				{
					AE_UNEXPECTED(AetherError::Vulkan(result, "Texture host-copy sampling transition failed."));
				}
				return {};
			}
			if (queue == nullptr || pool == nullptr)
			{
				AE_UNEXPECTED(AetherError::Engine("Texture upload requires a queue and command pool for its sampling transition."));
			}
			OneShotCmd command;
			if (!command.Begin(device, pool))
			{
				AE_UNEXPECTED(AetherError::Engine("Could not begin texture sampling transition."));
			}
			ImageBarrier(static_cast<VkCommandBuffer>(command.CmdList().GetCommandBuffer()), image, resolved.mipLevels,
			        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
			        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
			if (!command.EndAndSubmit(queue))
			{
				AE_UNEXPECTED(AetherError::Engine("Could not submit texture sampling transition."));
			}
			return {};
		}
		if (queue == nullptr || pool == nullptr)
		{
			AE_UNEXPECTED(AetherError::Engine("Staging texture upload requires a queue and command pool."));
		}
		std::vector<VkBufferImageCopy> copies;
		copies.reserve(regions.size());
		for (const auto& region: regions)
		{
			copies.push_back({.bufferOffset = region.offset, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region.mipLevel, 0, 1},
			        .imageExtent = {region.width, region.height, 1}});
		}
		const StagingBuffer staging{ResourceRegistry::CreateMappedBuffer({.size = data.size(), .usage = BufferUsage::TransferSrc,
		        .memoryUsage = MappedMemoryUsage::CpuToGpu, .debugName = "Synchronous texture staging"})};
		if (!staging.handle.IsValid())
		{
			AE_UNEXPECTED(AetherError::Engine("Could not allocate texture staging buffer."));
		}
		const auto mapped = ResourceRegistry::ResolveMappedBuffer(staging.handle);
		if (mapped.mappedPtr == nullptr)
		{
			AE_UNEXPECTED(AetherError::Engine("Could not map texture staging buffer."));
		}
		std::memcpy(mapped.mappedPtr, data.data(), data.size());
		ResourceRegistry::FlushMappedBuffer(staging.handle, 0, data.size());
		OneShotCmd command;
		if (!command.Begin(device, pool))
		{
			AE_UNEXPECTED(AetherError::Engine("Could not begin texture staging upload."));
		}
		const auto buffer = static_cast<VkBuffer>(ResourceRegistry::ResolveBufferVkHandle(staging.handle));
		const auto cmd = static_cast<VkCommandBuffer>(command.CmdList().GetCommandBuffer());
		ImageBarrier(cmd, image, resolved.mipLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(copies.size()), copies.data());
		ImageBarrier(cmd, image, resolved.mipLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		if (!command.EndAndSubmit(queue))
		{
			AE_UNEXPECTED(AetherError::Engine("Could not submit texture staging upload."));
		}
		return {};
	}
}
