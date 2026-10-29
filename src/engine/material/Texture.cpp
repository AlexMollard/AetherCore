#include "material/Texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpu/OneShotCmd.hpp"
#include "gpu/CommandList.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "gpu/ResourceRegistry.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <TextureMips.hpp>

#include "io/FileSystem.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"

#define INVALID_BINDLESS_SLOT 0xFFFFFFFFu

namespace aether
{
	namespace
	{
		std::uint32_t RegisterTextureBindless(gpu::TextureHandle handle, gpu::ImageLayout layout)
		{
			gpu::ResourceRegistry::EnsureBindlessSampled(handle, gpu::ImageAspect::Color, layout);
			const std::uint32_t slot = gpu::ResourceRegistry::GetBindlessSampledSlot(handle);
			if (slot == INVALID_BINDLESS_SLOT)
			{
				Throw(AetherError::Engine("Texture: EnsureBindlessSampled failed"));
			}

			return slot;
		}

		// Destroys the image if any later step Throws. A failed upload must not
		// leak the VkImage, its VMA allocation and its bindless descriptor slot;
		// ResourceRegistry::Destroy defers the actual free to the destruction
		// ring, so releasing here is as frame-safe as a Texture destructor.
		struct ScopedTexture
		{
			gpu::TextureHandle handle{};
			bool released = false;

			~ScopedTexture()
			{
				if (!released && handle.IsValid())
				{
					gpu::ResourceRegistry::Destroy(handle);
				}
			}
		};

		std::uint32_t MipCountFor(int width, int height)
		{
			const auto largest = static_cast<std::uint32_t>(std::max(width, height));
			return static_cast<std::uint32_t>(std::bit_width(largest));
		}

		gpu::TextureHandle UploadRgbaToGpuImage(const stbi_uc* pixels, int width, int height, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace, const char* debugName = nullptr)
		{
			// Without a mip chain a pixel that covers many texels reads exactly one of
			// them, so a surface picks a different texel every frame as the camera moves
			// and minified detail turns into crawling noise. This is not something
			// post-process antialiasing can recover: by the time the frame exists the
			// information that the other texels ever existed is gone.
			const std::uint32_t mipLevels = MipCountFor(width, height);

			const gpu::TextureDesc desc{
			        .format = colorSpace == TextureColorSpace::Srgb ? gpu::Format::R8G8B8A8Srgb : gpu::Format::R8G8B8A8Unorm,
			        .extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
			        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | gpu::ImageUsage::HostTransfer,
			        .aspect = gpu::ImageAspect::Color,
			        .mipLevels = mipLevels,
			        .debugName = debugName,
			};
			gpu::TextureHandle handle = gpu::ResourceRegistry::CreateTexture(desc);
			if (!handle.IsValid())
			{
				Throw(AetherError::Engine("Texture: CreateTexture failed"));
			}
			ScopedTexture lease{handle}; // any Throw below releases the image

			const gpu::Image image = gpu::ResourceRegistry::ResolveTextureImage(handle);

			{
				// One transition covers every level, then each is filled in turn. Each
				// level is filtered from the one above rather than from the original, so
				// the whole chain costs a third of the base image instead of a copy of it
				// per level.
				if (vkutil::HostTransitionImage(static_cast<VkDevice>(device), static_cast<VkImage>(image), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, mipLevels) != VK_SUCCESS)
				{
					Throw(AetherError::Vulkan(0, "UploadRgbaToGpuImage: host transition to GENERAL failed"));
				}

				// Colour cut-outs dilate transparent RGB; data-map alpha is an independent channel.
				std::vector<stbi_uc> base;
				const stbi_uc* source = pixels;
				if (colorSpace == TextureColorSpace::Srgb)
				{
					base.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
					DilateTransparentRgb(base.data(), width, height, 4);
					source = base.data();
				}
				const std::int32_t copyResult = vkutil::HostCopyMipToImage(device, image, source, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0u);
				if (copyResult != 0)
				{
					Throw(AetherError::Vulkan(copyResult, "UploadRgbaToGpuImage: HostCopyMipToImage failed"));
				}

				const stbi_uc* parent = source;
				std::vector<stbi_uc> parentOwned;
				int parentWidth = width;
				int parentHeight = height;
				for (std::uint32_t level = 1; level < mipLevels; ++level)
				{
					const int levelWidth = std::max(1, parentWidth / 2);
					const int levelHeight = std::max(1, parentHeight / 2);

					std::vector<stbi_uc> levelPixels(static_cast<std::size_t>(levelWidth) * static_cast<std::size_t>(levelHeight) * 4u);
					DownsampleBox2x2(parent, parentWidth, parentHeight, 4, levelPixels.data(), colorSpace == TextureColorSpace::Srgb);
					if (colorSpace == TextureColorSpace::Srgb)
					{
						DilateTransparentRgb(levelPixels.data(), levelWidth, levelHeight, 4);
					}
					const std::int32_t levelResult =
					        vkutil::HostCopyMipToImage(device, image, levelPixels.data(), static_cast<uint32_t>(levelWidth), static_cast<uint32_t>(levelHeight), level);
					if (levelResult != 0)
					{
						Throw(AetherError::Vulkan(levelResult, "UploadRgbaToGpuImage: HostCopyMipToImage failed for a mip level"));
					}

					parentOwned = std::move(levelPixels);
					parent = parentOwned.data();
					parentWidth = levelWidth;
					parentHeight = levelHeight;
				}
			}

			// Finish on the host when the device allows it: a host layout transition
			// needs no queue, no fence, and no cross-thread submit. The one-shot
			// barrier below only remains for devices whose host-image-copy layout
			// list omits SHADER_READ_ONLY_OPTIMAL.
			if (vkutil::SupportsHostImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
			{
				if (vkutil::HostTransitionImageToShaderRead(device, image, mipLevels) != 0)
				{
					Throw(AetherError::Vulkan(0, "UploadRgbaToGpuImage: host layout transition failed"));
				}
			}
			else
			{
				gpu::OneShotCmd cmd;
				if (!cmd.Begin(device, uploadPool))
				{
					Throw(AetherError::Vulkan(0, "UploadRgbaToGpuImage: failed to begin OneShotCmd"));
				}
				cmd.CmdList().ImageMemoryBarrier(
				        image, gpu::ImageLayout::General, gpu::ImageLayout::ShaderReadOnly, gpu::ImageAspect::Color, gpu::PipelineStage::AllCommands, gpu::AccessFlags::None, gpu::PipelineStage::FragmentShader, gpu::AccessFlags::ShaderRead);
				if (!cmd.EndAndSubmit(uploadQueue))
				{
					Throw(AetherError::Vulkan(0, "UploadRgbaToGpuImage: failed to submit OneShotCmd"));
				}
			}

			RegisterTextureBindless(handle, gpu::ImageLayout::ShaderReadOnly);

			lease.released = true;
			return handle;
		}
	} // namespace

	namespace
	{

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

		// The requested colour space decides the decode, exactly as the
		// R8G8B8A8 path above does: BC7's Unorm and Unorm_sRGB variants share a
		// block layout, so either payload can serve either request and the
		// file's own encoding never overrides the caller's. BC4 is
		// single-channel data with no sRGB variant at all - an sRGB request
		// cannot be honoured, and serving it undecoded would silently tilt the
		// surface (see TextureColorSpace), so it is refused instead.
		Expected<gpu::Format> DxgiToGpuFormat(uint32_t dxgi, TextureColorSpace colorSpace, std::string_view debugPath)
		{
			switch (dxgi)
			{
				case DXGI_BC4_UNORM:
					if (colorSpace == TextureColorSpace::Srgb)
					{
						AE_UNEXPECTED(AetherError::Asset("BC4 data texture '" + std::string(debugPath) + "' requested as sRGB - acquire it as TextureColorSpace::Linear"));
					}
					return gpu::Format::BC4UnormBlock;
				case DXGI_BC7_UNORM:
				case DXGI_BC7_UNORM_SRGB:
					return colorSpace == TextureColorSpace::Srgb ? gpu::Format::BC7SrgbBlock : gpu::Format::BC7UnormBlock;
				default:
					AE_UNEXPECTED(AetherError::Vulkan(0, "unsupported DXGI format " + std::to_string(dxgi) + " in: " + std::string(debugPath)));
			}
		}

		Expected<gpu::TextureHandle> UploadBcnDds(std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace)
		{
			constexpr std::size_t kMinSize = sizeof(uint32_t) + sizeof(DdsHeader) + sizeof(DdsDx10Header);
			if (fileData.size() < kMinSize)
			{
				AE_UNEXPECTED(AetherError::Asset("DDS file too small: " + std::string(debugPath)));
			}

			const std::byte* p = fileData.data();
			p += sizeof(uint32_t);

			const auto& hdr = *reinterpret_cast<const DdsHeader*>(p);
			p += sizeof(DdsHeader);
			const auto& dx10 = *reinterpret_cast<const DdsDx10Header*>(p);
			p += sizeof(DdsDx10Header);

			if (hdr.ddspf.fourCC != FOURCC_DX10)
			{
				AE_UNEXPECTED(AetherError::Asset("only DX10-extended DDS files are supported: " + std::string(debugPath)));
			}

			AE_TRY(gpuFmt, DxgiToGpuFormat(dx10.dxgiFormat, colorSpace, debugPath));

			const uint32_t width = hdr.width;
			const uint32_t height = hdr.height;
			const std::string ddsName(debugPath);
			const gpu::TextureDesc desc{
			const uint32_t mipLevels = std::max(1u, hdr.mipMapCount);
			if (!width || !height || mipLevels > std::bit_width(std::max(width, height)))
			{
				AE_UNEXPECTED(AetherError::Asset("invalid DDS dimensions or mip count: " + std::string(debugPath)));
			}
			const std::size_t blockBytes = dx10.dxgiFormat == DXGI_BC4_UNORM ? 8u : 16u;
			std::size_t remaining = fileData.size() - kMinSize;
			uint32_t mipWidth = width, mipHeight = height;
			for (uint32_t level = 0; level < mipLevels; ++level)
			{
				const std::size_t blocksX = (std::size_t(mipWidth) + 3) / 4;
				const std::size_t blocksY = (std::size_t(mipHeight) + 3) / 4;
				if (blocksX > remaining / blockBytes / blocksY)
				{
					AE_UNEXPECTED(AetherError::Asset("truncated DDS mip chain: " + std::string(debugPath)));
				}
				remaining -= blocksX * blocksY * blockBytes;
				mipWidth = std::max(1u, mipWidth / 2);
				mipHeight = std::max(1u, mipHeight / 2);
			}
			        .format = *gpuFmt,
			        .extent = {width, height},
			        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | gpu::ImageUsage::HostTransfer,
			        .aspect = gpu::ImageAspect::Color,
			        .debugName = ddsName.c_str(),
			};
			        .mipLevels = mipLevels,
			gpu::TextureHandle handle = gpu::ResourceRegistry::CreateTexture(desc);
			if (!handle.IsValid())
			{
				Throw(AetherError::Engine("Texture: CreateTexture failed (DDS)"));
			}

			const gpu::Image image = gpu::ResourceRegistry::ResolveTextureImage(handle);
			ScopedTexture lease{handle};

			{
				Throw(AetherError::Vulkan(0, "UploadBcnDds: host transition to GENERAL failed"));
			}
			mipWidth = width;
			mipHeight = height;
			for (uint32_t level = 0; level < mipLevels; ++level)
			{
				const std::int32_t copyResult = vkutil::HostCopyMipToImage(device, image, p, mipWidth, mipHeight, level);
			if (vkutil::HostTransitionImage(static_cast<VkDevice>(device), static_cast<VkImage>(image), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, mipLevels) != VK_SUCCESS)
				if (copyResult != 0)
				{
					Throw(AetherError::Vulkan(copyResult, "UploadBcnDds: HostCopyMipToImage failed"));
				}
			}

				p += ((std::size_t(mipWidth) + 3) / 4) * ((std::size_t(mipHeight) + 3) / 4) * blockBytes;
				mipWidth = std::max(1u, mipWidth / 2);
				mipHeight = std::max(1u, mipHeight / 2);
			// Same host-side finish as UploadRgbaToGpuImage (see comment there).
			if (vkutil::SupportsHostImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
			{
				if (vkutil::HostTransitionImageToShaderRead(device, image, mipLevels) != 0)
				{
					Throw(AetherError::Vulkan(0, "UploadBcnDds: host layout transition failed"));
				}
			}
			else
			{
				gpu::OneShotCmd cmd;
				if (!cmd.Begin(device, uploadPool))
				{
					Throw(AetherError::Vulkan(0, "UploadBcnDds: failed to begin OneShotCmd"));
				}
				cmd.CmdList().ImageMemoryBarrier(
				        image, gpu::ImageLayout::General, gpu::ImageLayout::ShaderReadOnly, gpu::ImageAspect::Color, gpu::PipelineStage::AllCommands, gpu::AccessFlags::None, gpu::PipelineStage::FragmentShader, gpu::AccessFlags::ShaderRead);
				if (!cmd.EndAndSubmit(uploadQueue))
				{
					Throw(AetherError::Vulkan(0, "UploadBcnDds: failed to submit OneShotCmd"));
				}
			}

			RegisterTextureBindless(handle, gpu::ImageLayout::ShaderReadOnly);

			return handle;
			lease.released = true;
		}
	} // namespace

	Expected<Texture> Texture::LoadFromFileData(std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace)
	{
		if (fileData.size() < 4)
		{
			// Too small to carry a 4-byte magic or be a decodable image; fail early
			// instead of routing through the stbi fallback (which can only fail too).
			AE_UNEXPECTED(AetherError::Asset("failed to decode '" + std::string(debugPath) + "': file too small to identify (" + std::to_string(fileData.size()) + " bytes)"));
		}

		uint32_t magic = 0;
		std::memcpy(&magic, fileData.data(), 4);
		if (magic == DDS_MAGIC)
		{
			Texture texture;
			AE_TRY(handle, UploadBcnDds(fileData, debugPath, device, uploadQueue, uploadPool, colorSpace));
			texture.m_handle = *handle;
			texture.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(texture.m_handle);
			return texture;
		}

		int width = 0, height = 0, channels = 0;
		stbi_uc* const pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fileData.data()), static_cast<int>(fileData.size()), &width, &height, &channels, STBI_rgb_alpha);
		if (!pixels)
		{
			AE_UNEXPECTED(AetherError::Asset("failed to decode '" + std::string(debugPath) + "': " + stbi_failure_reason()));
		}

		Texture texture;
		texture.m_handle = UploadRgbaToGpuImage(pixels, width, height, device, uploadQueue, uploadPool, colorSpace, std::string(debugPath).c_str());
		texture.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(texture.m_handle);
		stbi_image_free(pixels);
		return texture;
	}

	Expected<Texture> Texture::CreateSolidColor(std::array<std::uint8_t, 4> rgba, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool)
	{
		static_assert(sizeof(stbi_uc) == sizeof(std::uint8_t));
		Texture texture;
		texture.m_handle = UploadRgbaToGpuImage(rgba.data(), 1, 1, device, uploadQueue, uploadPool, TextureColorSpace::Srgb, "builtin:solid-color");
		texture.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(texture.m_handle);
		return texture;
	}

	std::string Texture::ResolveTexturePath(std::string_view path)
	{
		const std::string pathStr(path);
		std::string texturePath;
		{
			const std::filesystem::path fp(pathStr);
			const std::filesystem::path stem = fp.parent_path() / fp.stem();
			texturePath = stem.generic_string() + ".texture";
			const std::size_t slashSlash = pathStr.find("://");
			if (slashSlash != std::string::npos)
			{
				const std::string mount = pathStr.substr(0, slashSlash);
				const std::filesystem::path rel = std::filesystem::path(pathStr.substr(slashSlash + 3));
				texturePath = mount + "://" + (rel.parent_path() / rel.stem()).generic_string() + ".texture";
			}
		}
		return io::FileSystem::Exists(texturePath) ? texturePath : pathStr;
	}

	Expected<Texture> Texture::LoadFromFile(std::string_view path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace)
	{
		AE_PROFILE_ZONE();
		AE_PROFILE_SET_ZONE_NAME(path.data());

		std::string resolved = ResolveTexturePath(path);
		if (colorSpace == TextureColorSpace::Linear && resolved.ends_with(".texture"))
		{
			const std::string variant = resolved.substr(0, resolved.size() - 8) + ".linear.texture";
			if (io::FileSystem::Exists(variant)) { resolved = variant; }
		}
		AE_TRY(data, io::FileSystem::ReadFile(resolved));
		return LoadFromFileData(*data, resolved, device, uploadQueue, uploadPool, colorSpace);
	}

	Expected<Texture> Texture::LoadFromDiskPath(const std::filesystem::path& path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace)
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
		texture.m_handle = UploadRgbaToGpuImage(pixels, width, height, device, uploadQueue, uploadPool, colorSpace, path.string().c_str());
		texture.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(texture.m_handle);

		stbi_image_free(pixels);
		return texture;
	}

	void Texture::Destroy()
	{
		if (m_handle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_handle);
		}
		m_handle = {};
		m_bindlessSlot = 0xFFFFFFFFu;
	}

	gpu::ImageView Texture::GetView() const
	{
		return gpu::ResourceRegistry::ResolveTexture(m_handle).view;
	}

	std::uint32_t Texture::GetBindlessSlot() const
	{
		return m_bindlessSlot;
	}
} // namespace aether
