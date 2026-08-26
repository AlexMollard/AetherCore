#include "material/Texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpu/OneShotCmd.hpp"
#include "gpu/CommandList.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "gpu/ResourceRegistry.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

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

		// sRGB <-> linear. A mip is an average of the light the parent texels carry, and
		// sRGB values are not proportional to light, so averaging them directly is simply
		// the wrong sum: mid-tones come out too dark and the chain drifts as it descends.
		// The decode is a 256-entry table because there are only 256 possible inputs.
		const std::array<float, 256>& SrgbToLinearTable()
		{
			static const std::array<float, 256> table = []
			{
				std::array<float, 256> t{};
				for (std::size_t i = 0; i < t.size(); ++i)
				{
					const float c = static_cast<float>(i) / 255.0f;
					t[i] = (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
				}
				return t;
			}();
			return table;
		}

		stbi_uc LinearToSrgbByte(float linear)
		{
			const float c = (linear <= 0.0031308f) ? (linear * 12.92f) : (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
			return static_cast<stbi_uc>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
		}

		std::uint32_t MipCountFor(int width, int height)
		{
			const auto largest = static_cast<std::uint32_t>(std::max(width, height));
			return static_cast<std::uint32_t>(std::bit_width(largest));
		}

		// Box-filter the parent level down by two. Colour is averaged in linear light;
		// alpha is already linear and is averaged as-is.
		std::vector<stbi_uc> DownsampleRgba(const stbi_uc* src, int srcWidth, int srcHeight, int dstWidth, int dstHeight, TextureColorSpace colorSpace)
		{
			const bool decode = colorSpace == TextureColorSpace::Srgb;
			const auto& toLinear = SrgbToLinearTable();
			std::vector<stbi_uc> dst(static_cast<std::size_t>(dstWidth) * static_cast<std::size_t>(dstHeight) * 4u);

			for (int y = 0; y < dstHeight; ++y)
			{
				const int y0 = std::min(y * 2, srcHeight - 1);
				const int y1 = std::min(y0 + 1, srcHeight - 1);
				for (int x = 0; x < dstWidth; ++x)
				{
					const int x0 = std::min(x * 2, srcWidth - 1);
					const int x1 = std::min(x0 + 1, srcWidth - 1);

					const std::size_t taps[4] = {
					        (static_cast<std::size_t>(y0) * srcWidth + x0) * 4u,
					        (static_cast<std::size_t>(y0) * srcWidth + x1) * 4u,
					        (static_cast<std::size_t>(y1) * srcWidth + x0) * 4u,
					        (static_cast<std::size_t>(y1) * srcWidth + x1) * 4u,
					};

					float rgb[3] = {0.0f, 0.0f, 0.0f};
					float alpha = 0.0f;
					for (const std::size_t tap: taps)
					{
						rgb[0] += decode ? toLinear[src[tap + 0]] : static_cast<float>(src[tap + 0]);
						rgb[1] += decode ? toLinear[src[tap + 1]] : static_cast<float>(src[tap + 1]);
						rgb[2] += decode ? toLinear[src[tap + 2]] : static_cast<float>(src[tap + 2]);
						alpha += static_cast<float>(src[tap + 3]);
					}

					const std::size_t out = (static_cast<std::size_t>(y) * dstWidth + x) * 4u;
					const auto encode = [decode](float v) -> stbi_uc
					{
						return decode ? LinearToSrgbByte(v) : static_cast<stbi_uc>(std::lround(std::clamp(v, 0.0f, 255.0f)));
					};
					dst[out + 0] = encode(rgb[0] * 0.25f);
					dst[out + 1] = encode(rgb[1] * 0.25f);
					dst[out + 2] = encode(rgb[2] * 0.25f);
					dst[out + 3] = static_cast<stbi_uc>(std::lround(alpha * 0.25f));
				}
			}

			return dst;
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

				const std::int32_t copyResult = vkutil::HostCopyMipToImage(device, image, pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0u);
				if (copyResult != 0)
				{
					Throw(AetherError::Vulkan(copyResult, "UploadRgbaToGpuImage: HostCopyMipToImage failed"));
				}

				const stbi_uc* parent = pixels;
				std::vector<stbi_uc> parentOwned;
				int parentWidth = width;
				int parentHeight = height;
				for (std::uint32_t level = 1; level < mipLevels; ++level)
				{
					const int levelWidth = std::max(1, parentWidth / 2);
					const int levelHeight = std::max(1, parentHeight / 2);

					std::vector<stbi_uc> levelPixels = DownsampleRgba(parent, parentWidth, parentHeight, levelWidth, levelHeight, colorSpace);
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

		gpu::Format DxgiToGpuFormat(uint32_t dxgi)
		{
			switch (dxgi)
			{
				case DXGI_BC4_UNORM:
					return gpu::Format::BC4UnormBlock;
				case DXGI_BC7_UNORM:
					return gpu::Format::BC7UnormBlock;
				case DXGI_BC7_UNORM_SRGB:
					return gpu::Format::BC7SrgbBlock;
				default:
					return gpu::Format::Undefined;
			}
		}

		Expected<gpu::TextureHandle> UploadBcnDds(std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool)
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

			const gpu::Format gpuFmt = DxgiToGpuFormat(dx10.dxgiFormat);
			if (gpuFmt == gpu::Format::Undefined)
			{
				AE_UNEXPECTED(AetherError::Vulkan(0, "unsupported DXGI format " + std::to_string(dx10.dxgiFormat) + " in: " + std::string(debugPath)));
			}

			const uint32_t width = hdr.width;
			const uint32_t height = hdr.height;
			const std::string ddsName(debugPath);
			const gpu::TextureDesc desc{
			        .format = gpuFmt,
			        .extent = {width, height},
			        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | gpu::ImageUsage::HostTransfer,
			        .aspect = gpu::ImageAspect::Color,
			        .debugName = ddsName.c_str(),
			};
			gpu::TextureHandle handle = gpu::ResourceRegistry::CreateTexture(desc);
			if (!handle.IsValid())
			{
				Throw(AetherError::Engine("Texture: CreateTexture failed (DDS)"));
			}

			const gpu::Image image = gpu::ResourceRegistry::ResolveTextureImage(handle);

			{
				const std::int32_t copyResult = vkutil::HostCopyToImage(device, image, p, width, height);
				if (copyResult != 0)
				{
					Throw(AetherError::Vulkan(copyResult, "UploadBcnDds: HostCopyToImage failed"));
				}
			}

			// Same host-side finish as UploadRgbaToGpuImage (see comment there).
			if (vkutil::SupportsHostImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
			{
				if (vkutil::HostTransitionImageToShaderRead(device, image) != 0)
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
			AE_TRY(handle, UploadBcnDds(fileData, debugPath, device, uploadQueue, uploadPool));
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

		const std::string resolved = ResolveTexturePath(path);
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
