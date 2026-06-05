#include "material/Texture.hpp"

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "vulkan/volk.hpp"

// stb_image - single-header image loader.
// STB_IMAGE_IMPLEMENTATION must be defined in exactly one compilation unit.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "gpu/BindlessManager.hpp"
#include "io/FileSystem.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/UniqueBuffer.hpp"

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
			const VkResult allocResult = vkAllocateCommandBuffers(device, &allocInfo, &cmd);
			if (allocResult != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(static_cast<int32_t>(allocResult), "Texture: failed to allocate upload command buffer"));
			}

			const VkCommandBufferBeginInfo beginInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			const VkResult beginResult = vkBeginCommandBuffer(cmd, &beginInfo);
			if (beginResult != VK_SUCCESS)
			{
				vkFreeCommandBuffers(device, pool, 1, &cmd);
				Throw(AetherError::Vulkan(static_cast<int32_t>(beginResult), "Texture: failed to begin upload command buffer"));
			}
			return cmd;
		}

		// Submit a command buffer and block until the queue is idle, then free the buffer.
		void EndAndSubmitOneTimeBuffer(VkDevice device, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd)
		{
			const VkResult endResult = vkEndCommandBuffer(cmd);
			if (endResult != VK_SUCCESS)
			{
				vkFreeCommandBuffers(device, pool, 1, &cmd);
				Throw(AetherError::Vulkan(static_cast<int32_t>(endResult), "Texture: failed to end upload command buffer"));
			}

			const VkCommandBufferSubmitInfo cbInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			        .commandBuffer = cmd,
			};
			const VkSubmitInfo2 submitInfo{
			        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			        .commandBufferInfoCount = 1,
			        .pCommandBufferInfos = &cbInfo,
			};
			const VkResult submitResult = vkQueueSubmit2(queue, 1, &submitInfo, VK_NULL_HANDLE);
			if (submitResult != VK_SUCCESS)
			{
				vkFreeCommandBuffers(device, pool, 1, &cmd);
				Throw(AetherError::Vulkan(static_cast<int32_t>(submitResult), "Texture: failed to submit upload command buffer"));
			}
			const VkResult idleResult = vkQueueWaitIdle(queue);
			if (idleResult != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(static_cast<int32_t>(idleResult), "Texture: failed to wait idle after texture upload"));
			}

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

		UniqueImage UploadRgbaToGpuImage(
		        const stbi_uc* pixels, int width, int height, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless, TextureFilter filter, const char* debugName = nullptr)
		{
			AE_EXPECT_OR_THROW(image,
			        UniqueImage::Create(device,
			                allocator,
			                {
			                        .extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
			                        .format = VK_FORMAT_R8G8B8A8_SRGB,
			                        .usage = VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT | VK_IMAGE_USAGE_SAMPLED_BIT,
			                        .debugName = debugName,
			                }));

			{
				const VkMemoryToImageCopyEXT region{
				        .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT,
				        .pHostPointer = pixels,
				        .memoryRowLength = 0,
				        .memoryImageHeight = 0,
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
				const VkCopyMemoryToImageInfoEXT copyInfo{
				        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT,
				        .dstImage = image.Get(),
				        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
				        .regionCount = 1,
				        .pRegions = &region,
				};
			const VkResult copyResult = vkCopyMemoryToImageEXT(device, &copyInfo);
			if (copyResult != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(static_cast<int32_t>(copyResult), "UploadRgbaToGpuImage: vkCopyMemoryToImageEXT failed"));
			}
			}

			VkCommandBuffer cmd = BeginOneTimeBuffer(device, uploadPool);

			TransitionImageLayout(cmd, image.Get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

			EndAndSubmitOneTimeBuffer(device, uploadPool, uploadQueue, cmd);

			AE_EXPECT_OR_THROW_VOID(image.EnsureBindlessSampled(bindless, device, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, filter));

			return image;
		}
	} // namespace

	namespace
	{
		// -------------------------------------------------------------------------
		// DDS / BCn upload path
		// -------------------------------------------------------------------------

		constexpr uint32_t DDS_MAGIC = 0x20534444u;
		constexpr uint32_t FOURCC_DX10 = 0x30315844u;
		constexpr uint32_t DXGI_BC4_UNORM = 80u;
		constexpr uint32_t DXGI_BC7_UNORM = 98u;
		constexpr uint32_t DXGI_BC7_UNORM_SRGB = 99u;

#pragma pack(push, 1)

		struct DdsPixelFormat
		{
			uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
		};

		struct DdsHeader
		{
			uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
			uint32_t reserved1[11];
			DdsPixelFormat ddspf;
			uint32_t caps, caps2, caps3, caps4, reserved2;
		};

		struct DdsDx10Header
		{
			uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
		};

#pragma pack(pop)

		VkFormat DxgiToVkFormat(uint32_t dxgi)
		{
			switch (dxgi)
			{
				case DXGI_BC4_UNORM:
					return VK_FORMAT_BC4_UNORM_BLOCK;
				case DXGI_BC7_UNORM:
					return VK_FORMAT_BC7_UNORM_BLOCK;
				case DXGI_BC7_UNORM_SRGB:
					return VK_FORMAT_BC7_SRGB_BLOCK;
				default:
					return VK_FORMAT_UNDEFINED;
			}
		}

		Expected<UniqueImage> UploadBcnDds(std::span<const std::byte> fileData, std::string_view debugPath, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
		{
			constexpr std::size_t kMinSize = sizeof(uint32_t) + sizeof(DdsHeader) + sizeof(DdsDx10Header);
			if (fileData.size() < kMinSize)
			{
				AE_UNEXPECTED(AetherError::Asset("DDS file too small: " + std::string(debugPath)));
			}

			const std::byte* p = fileData.data();
			p += sizeof(uint32_t); // skip magic

			const auto& hdr = *reinterpret_cast<const DdsHeader*>(p);
			p += sizeof(DdsHeader);
			const auto& dx10 = *reinterpret_cast<const DdsDx10Header*>(p);
			p += sizeof(DdsDx10Header);

			if (hdr.ddspf.fourCC != FOURCC_DX10)
			{
				AE_UNEXPECTED(AetherError::Asset("only DX10-extended DDS files are supported: " + std::string(debugPath)));
			}

			const VkFormat vkFmt = DxgiToVkFormat(dx10.dxgiFormat);
			if (vkFmt == VK_FORMAT_UNDEFINED)
			{
				AE_UNEXPECTED(AetherError::Vulkan(0, "unsupported DXGI format " + std::to_string(dx10.dxgiFormat) + " in: " + std::string(debugPath)));
			}

			const uint32_t width = hdr.width;
			const uint32_t height = hdr.height;
			const std::size_t blockDataSize = static_cast<std::size_t>(fileData.size()) - sizeof(uint32_t) - sizeof(DdsHeader) - sizeof(DdsDx10Header);

			AE_TRY(image,
			        UniqueImage::Create(device,
			                allocator,
			                {
			                        .extent = {width, height},
			                        .format = vkFmt,
			                        .usage = VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT | VK_IMAGE_USAGE_SAMPLED_BIT,
			                }));

			{
				const VkMemoryToImageCopyEXT region{
				        .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT,
				        .pHostPointer = p,
				        .memoryRowLength = 0,
				        .memoryImageHeight = 0,
				        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
				        .imageOffset = {0, 0, 0},
				        .imageExtent = {width, height, 1},
				};
				const VkCopyMemoryToImageInfoEXT copyInfo{
				        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT,
				        .dstImage = image->Get(),
				        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
				        .regionCount = 1,
				        .pRegions = &region,
				};
			const VkResult copyResult = vkCopyMemoryToImageEXT(device, &copyInfo);
			if (copyResult != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(static_cast<int32_t>(copyResult), "UploadBcnDds: vkCopyMemoryToImageEXT failed"));
			}
			}

			VkCommandBuffer cmd = BeginOneTimeBuffer(device, uploadPool);

			TransitionImageLayout(cmd, image->Get(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

			EndAndSubmitOneTimeBuffer(device, uploadPool, uploadQueue, cmd);

			AE_EXPECT_OR_THROW_VOID(image->EnsureBindlessSampled(bindless, device, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, filter));
			return image;
		}
	} // namespace

	Expected<Texture> Texture::LoadFromFileData(std::span<const std::byte> fileData, std::string_view debugPath, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
	{
		if (fileData.size() < 4)
		{
			int width = 0, height = 0, channels = 0;
			stbi_uc* const pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fileData.data()), static_cast<int>(fileData.size()), &width, &height, &channels, STBI_rgb_alpha);
			if (!pixels)
			{
				AE_UNEXPECTED(AetherError::Asset("failed to decode '" + std::string(debugPath) + "': " + stbi_failure_reason()));
			}
			Texture texture;
			texture.m_image = UploadRgbaToGpuImage(pixels, width, height, device, allocator, uploadQueue, uploadPool, bindless, filter, std::string(debugPath).c_str());
			stbi_image_free(pixels);
			return texture;
		}

		uint32_t magic = 0;
		std::memcpy(&magic, fileData.data(), 4);
		if (magic == DDS_MAGIC)
		{
			Texture texture;
			AE_TRY(image, UploadBcnDds(fileData, debugPath, device, allocator, uploadQueue, uploadPool, bindless, filter));
			texture.m_image = std::move(*image);
			return texture;
		}

		int width = 0, height = 0, channels = 0;
		stbi_uc* const pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fileData.data()), static_cast<int>(fileData.size()), &width, &height, &channels, STBI_rgb_alpha);
		if (!pixels)
		{
			AE_UNEXPECTED(AetherError::Asset("failed to decode '" + std::string(debugPath) + "': " + stbi_failure_reason()));
		}

		Texture texture;
		texture.m_image = UploadRgbaToGpuImage(pixels, width, height, device, allocator, uploadQueue, uploadPool, bindless, filter, std::string(debugPath).c_str());
		stbi_image_free(pixels);
		return texture;
	}

	Expected<Texture> Texture::LoadFromFile(std::string_view path, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
	{
		AE_PROFILE_ZONE_N("Texture::LoadFromFile");
		AE_PROFILE_SET_ZONE_NAME(path.data());

		// Try the pre-transcoded .texture (DDS) version first, then fall back to
		// the original path for assets not processed by AssetPacker.
		auto TryLoad = [&](std::string_view tryPath) -> std::vector<std::byte>
		{
			if (io::FileSystem::Exists(tryPath))
			{
				if (auto result = io::FileSystem::ReadFile(tryPath); result.has_value())
				{
					return std::move(*result);
				}
			}
			return {};
		};

		// Derive the .texture sibling path (e.g. "assets://foo/bar.png" -> "…/bar.texture")
		const std::string pathStr(path);
		std::string texturePath;
		{
			const std::filesystem::path fp(pathStr);
			const std::filesystem::path stem = fp.parent_path() / fp.stem();
			texturePath = stem.generic_string() + ".texture";
			// Preserve the VFS mount (everything before the first '/')
			const std::size_t slashSlash = pathStr.find("://");
			if (slashSlash != std::string::npos)
			{
				const std::string mount = pathStr.substr(0, slashSlash);
				const std::filesystem::path rel = std::filesystem::path(pathStr.substr(slashSlash + 3));
				texturePath = mount + "://" + (rel.parent_path() / rel.stem()).generic_string() + ".texture";
			}
		}

		std::vector<std::byte> fileData = TryLoad(texturePath);
		if (fileData.empty())
		{
			AE_TRY(data, io::FileSystem::ReadFile(path));
			fileData = std::move(*data);
		}

		return LoadFromFileData(fileData, pathStr, device, allocator, uploadQueue, uploadPool, bindless, filter);
	}

	Expected<Texture> Texture::LoadFromDiskPath(const std::filesystem::path& path, VkDevice device, VmaAllocator allocator, VkQueue uploadQueue, VkCommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
	{
		int width = 0;
		int height = 0;
		int channels = 0;

		stbi_uc* const pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);

		if (pixels == nullptr)
		{
			AE_UNEXPECTED(AetherError::Asset("failed to load '" + path.string() + "': " + stbi_failure_reason()));
		}

		Texture texture;
		texture.m_image = UploadRgbaToGpuImage(pixels, width, height, device, allocator, uploadQueue, uploadPool, bindless, filter, path.string().c_str());

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
