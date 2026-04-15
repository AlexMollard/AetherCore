#include "Texture.hpp"

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

// stb_image — single-header image loader.
// STB_IMAGE_IMPLEMENTATION must be defined in exactly one compilation unit.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "BindlessManager.hpp"
#include "FileSystem.hpp"
#include "UniqueBuffer.hpp"

namespace aether
{
	namespace
	{
		// Allocate + begin a one-time command buffer from the supplied pool.
		VkCommandBuffer BeginOneTimeBuffer(VkDevice device, VkCommandPool pool)
		{
			const VkCommandBufferAllocateInfo allocInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			};
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			vkAllocateCommandBuffers(device, &allocInfo, &cmd);

			const VkCommandBufferBeginInfo beginInfo{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			vkBeginCommandBuffer(cmd, &beginInfo);
			return cmd;
		}

		// Submit a command buffer and block until the queue is idle, then free the
		// buffer.
		void EndAndSubmitOneTimeBuffer(VkDevice device, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd)
		{
			vkEndCommandBuffer(cmd);

			const VkSubmitInfo submitInfo{
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.commandBufferCount = 1,
				.pCommandBuffers = &cmd,
			};
			vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
			vkQueueWaitIdle(queue);

			vkFreeCommandBuffers(device, pool, 1, &cmd);
		}

		// Transition an image between two layouts using a pipeline barrier.
		void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
		{
			const VkImageMemoryBarrier2 barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = srcStage,
      .srcAccessMask = srcAccess,
      .dstStageMask = dstStage,
      .dstAccessMask = dstAccess,
      .oldLayout = oldLayout,
      .newLayout = newLayout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };

			const VkDependencyInfo depInfo{
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &barrier,
			};
			vkCmdPipelineBarrier2(cmd, &depInfo);
		}

		UniqueImage UploadRgbaToGpuImage(const stbi_uc* pixels, int width, int height, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless)
		{
			const VkDeviceSize imageBytes = static_cast<VkDeviceSize>(width) * height * 4;

			const VkBufferCreateInfo stagingBufInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = imageBytes,
				.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			};
			const VmaAllocationCreateInfo stagingAllocInfo{
				.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
				.usage = VMA_MEMORY_USAGE_AUTO,
			};
			UniqueBuffer staging = UniqueBuffer::Create(allocator, device, stagingBufInfo, stagingAllocInfo);

			std::memcpy(staging.GetAllocationInfo().pMappedData, pixels, imageBytes);

			UniqueImage image = UniqueImage::Create(device,
			        allocator,
			        {
			                .extent = { static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) },
			                .format = VK_FORMAT_R8G8B8A8_SRGB,
			                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            });

			VkCommandBuffer cmd = BeginOneTimeBuffer(device, uploadPool);

			TransitionImageLayout(cmd, image.Get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

			const VkBufferImageCopy copyRegion{
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .mipLevel = 0,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
      .imageOffset = {0, 0, 0},
      .imageExtent =
          {
              static_cast<std::uint32_t>(width),
              static_cast<std::uint32_t>(height),
              1,
          },
  };
			vkCmdCopyBufferToImage(cmd, staging.Get(), image.Get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

			TransitionImageLayout(cmd, image.Get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

			EndAndSubmitOneTimeBuffer(device, uploadPool, uploadQueue, cmd);

			image.EnsureBindlessSampled(bindless, device, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			return image;
		}
	} // namespace

	Texture Texture::LoadFromFile(std::string_view path, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless)
	{
		// ── 1. Decode the image on the CPU ─────────────────────────────────
		int width = 0;
		int height = 0;
		int channels = 0;

		// Resolve via the VFS so callers can use virtual paths like
		// "assets://textures/..."
		const std::vector<std::byte> fileData = io::FileSystem::ReadFile(path);

		// Force 4-channel RGBA output regardless of source format.
		stbi_uc* const pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fileData.data()), static_cast<int>(fileData.size()), &width, &height, &channels, STBI_rgb_alpha);

		if (pixels == nullptr)
		{
			throw std::runtime_error("Texture::LoadFromFile: failed to load '" + std::string(path) + "': " + stbi_failure_reason());
		}

		Texture texture;
		texture.m_image = UploadRgbaToGpuImage(pixels, width, height, device, allocator, uploadQueue, uploadPool, bindless);
		stbi_image_free(pixels);

		return texture;
	}

	Texture Texture::LoadFromDiskPath(const std::filesystem::path& path, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless)
	{
		int width = 0;
		int height = 0;
		int channels = 0;

		stbi_uc* const pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);

		if (pixels == nullptr)
		{
			throw std::runtime_error("Texture::LoadFromDiskPath: failed to load '" + path.string() + "': " + stbi_failure_reason());
		}

		Texture texture;
		texture.m_image = UploadRgbaToGpuImage(pixels, width, height, device, allocator, uploadQueue, uploadPool, bindless);

		stbi_image_free(pixels);
		return texture;
	}

	void Texture::Destroy()
	{
		m_image.Reset();
	}

	std::uint32_t Texture::GetBindlessSlot() const
	{
		return m_image.GetBindlessSampledSlot();
	}
} // namespace aether
