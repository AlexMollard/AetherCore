#include "material/Texture.hpp"

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanUtils.hpp"

#include "gpu/OneShotCmd.hpp"
#include "gpu/CommandList.hpp"

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

		// Submit a command buffer, wait for the submission fence, then free the buffer.
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
			const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			VkFence fence = VK_NULL_HANDLE;
			if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
			{
				vkFreeCommandBuffers(device, pool, 1, &cmd);
				Throw(AetherError::Vulkan(0, "Texture: failed to create upload fence"));
			}
			const VkResult submitResult = vkQueueSubmit2(queue, 1, &submitInfo, fence);
			if (submitResult != VK_SUCCESS)
			{
				vkDestroyFence(device, fence, nullptr);
				vkFreeCommandBuffers(device, pool, 1, &cmd);
				Throw(AetherError::Vulkan(static_cast<int32_t>(submitResult), "Texture: failed to submit upload command buffer"));
			}
			(void) vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
			vkDestroyFence(device, fence, nullptr);
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
		        const stbi_uc* pixels, int width, int height, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter, const char* debugName = nullptr)
		{
			// TODO(phase5b): remove static_casts once UniqueImage moves to gpu handles.
			auto* vkDevice = static_cast<VkDevice>(device);
			auto* vmaAllocator = static_cast<VmaAllocator>(allocator);
			AE_EXPECT_OR_THROW(image,
			        UniqueImage::Create(vkDevice,
			                vmaAllocator,
			                {
			                        .extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
			                        .format = gpu::Format::R8G8B8A8Srgb,
			                        .usage = VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT | VK_IMAGE_USAGE_SAMPLED_BIT,
			                        .debugName = debugName,
			                }));

			{
				const VkResult copyResult = vkutil::HostCopyToImage(vkDevice, image.Get(), pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
				if (copyResult != VK_SUCCESS)
				{
					Throw(AetherError::Vulkan(static_cast<int32_t>(copyResult), "UploadRgbaToGpuImage: HostCopyToImage failed"));
				}
			}

			gpu::OneShotCmd cmd;
			if (!cmd.Begin(device, uploadPool))
			{
				Throw(AetherError::Vulkan(0, "UploadRgbaToGpuImage: failed to begin OneShotCmd"));
			}
			cmd.CmdList().ImageMemoryBarrier(
			        image.Get(), gpu::ImageLayout::General, gpu::ImageLayout::ShaderReadOnly, gpu::ImageAspect::Color, gpu::PipelineStage::AllCommands, gpu::AccessFlags::None, gpu::PipelineStage::FragmentShader, gpu::AccessFlags::ShaderRead);
			if (!cmd.EndAndSubmit(uploadQueue))
			{
				Throw(AetherError::Vulkan(0, "UploadRgbaToGpuImage: failed to submit OneShotCmd"));
			}

			AE_EXPECT_OR_THROW_VOID(image.EnsureBindlessSampled(bindless, vkDevice, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, filter));

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

		Expected<UniqueImage> UploadBcnDds(
		        std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
		{
			// TODO(phase5b): remove static_casts once UniqueImage moves to gpu handles.
			auto* vkDevice = static_cast<VkDevice>(device);
			auto* vmaAllocator = static_cast<VmaAllocator>(allocator);
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
			AE_TRY(image,
			        UniqueImage::Create(vkDevice,
			                vmaAllocator,
			                {
			                        .extent = {width, height},
			                        .format = gpu::FromVk(vkFmt),
			                        .usage = VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT | VK_IMAGE_USAGE_SAMPLED_BIT,
			                }));

			{
				const VkResult copyResult = vkutil::HostCopyToImage(vkDevice, image->Get(), p, width, height);
				if (copyResult != VK_SUCCESS)
				{
					Throw(AetherError::Vulkan(static_cast<int32_t>(copyResult), "UploadBcnDds: HostCopyToImage failed"));
				}
			}

			gpu::OneShotCmd cmd;
			if (!cmd.Begin(device, uploadPool))
			{
				Throw(AetherError::Vulkan(0, "UploadBcnDds: failed to begin OneShotCmd"));
			}
			cmd.CmdList().ImageMemoryBarrier(
			        image->Get(), gpu::ImageLayout::General, gpu::ImageLayout::ShaderReadOnly, gpu::ImageAspect::Color, gpu::PipelineStage::AllCommands, gpu::AccessFlags::None, gpu::PipelineStage::FragmentShader, gpu::AccessFlags::ShaderRead);
			if (!cmd.EndAndSubmit(uploadQueue))
			{
				Throw(AetherError::Vulkan(0, "UploadBcnDds: failed to submit OneShotCmd"));
			}

			AE_EXPECT_OR_THROW_VOID(image->EnsureBindlessSampled(bindless, vkDevice, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, filter));
			return image;
		}
	} // namespace

	Expected<Texture> Texture::LoadFromFileData(
	        std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
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

	Expected<Texture> Texture::LoadFromFile(std::string_view path, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
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

		// Derive the .texture sibling path (e.g. "assets://foo/bar.png" -> ".../bar.texture")
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

	Expected<Texture> Texture::LoadFromDiskPath(const std::filesystem::path& path, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
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
