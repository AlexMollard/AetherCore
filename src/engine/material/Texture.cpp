#include "material/Texture.hpp"

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpu/OneShotCmd.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuDeviceFactory.hpp"
#include "gpu/ResourceRegistry.hpp"

// stb_image - single-header image loader.
// STB_IMAGE_IMPLEMENTATION must be defined in exactly one compilation unit.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "gpu/BindlessManager.hpp"
#include "io/FileSystem.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		// Helper: acquire a bindless slot, configure a sampler, and update
		// the descriptor with the resolved image view. Engine-side; the
		// backend translates the slot to a real descriptor write.
		std::uint32_t RegisterTextureBindless(BindlessManager& bindless, gpu::ImageView view, TextureFilter filter, gpu::ImageLayout layout)
		{
			const auto slotResult = bindless.AllocateSampledImageSlot();
			if (!slotResult)
			{
				Throw(AetherError::Engine("Texture: AllocateSampledImageSlot failed"));
			}
			const std::uint32_t slot = *slotResult;
			const auto samplerResult = bindless.GetOrCreateSampler(
			        filter == TextureFilter::Nearest ? gpu::Filter::Nearest : gpu::Filter::Linear, filter == TextureFilter::Nearest ? gpu::SamplerMipmapMode::Nearest : gpu::SamplerMipmapMode::Linear, gpu::SamplerAddressMode::Repeat);
			if (!samplerResult)
			{
				Throw(AetherError::Engine("Texture: GetOrCreateSampler failed"));
			}
			const auto updateResult = bindless.UpdateSampledImage(slot, view, *samplerResult, layout);
			if (!updateResult)
			{
				Throw(AetherError::Engine("Texture: UpdateSampledImage failed"));
			}
			return slot;
		}

		gpu::TextureHandle UploadRgbaToGpuImage(const stbi_uc* pixels, int width, int height, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter, const char* debugName = nullptr)
		{
			const gpu::TextureDesc desc{
			        .format = gpu::Format::R8G8B8A8Srgb,
			        .extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
			        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | gpu::ImageUsage::HostTransfer,
			        .aspect = gpu::ImageAspect::Color,
			        .debugName = debugName,
			};
			gpu::TextureHandle handle = gpu::ResourceRegistry::CreateTexture(desc);
			if (!handle.IsValid())
			{
				Throw(AetherError::Engine("Texture: CreateTexture failed"));
			}

			const gpu::Image image = gpu::ResourceRegistry::ResolveTextureImage(handle);
			const gpu::ImageView view = gpu::ResourceRegistry::ResolveTexture(handle).view;

			{
				const std::int32_t copyResult = gpu::Factory::HostCopyToImage(device, image, pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
				if (copyResult != 0)
				{
					Throw(AetherError::Vulkan(copyResult, "UploadRgbaToGpuImage: HostCopyToImage failed"));
				}
			}

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

			(void) RegisterTextureBindless(bindless, view, filter, gpu::ImageLayout::ShaderReadOnly);

			return handle;
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

		// DXGI -> gpu::Format. The result maps one-for-one through
		// gpu::ToVk on the backend; the engine only sees the engine-side enum.
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

		Expected<gpu::TextureHandle> UploadBcnDds(std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
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

			const gpu::Format gpuFmt = DxgiToGpuFormat(dx10.dxgiFormat);
			if (gpuFmt == gpu::Format::Undefined)
			{
				AE_UNEXPECTED(AetherError::Vulkan(0, "unsupported DXGI format " + std::to_string(dx10.dxgiFormat) + " in: " + std::string(debugPath)));
			}

			const uint32_t width = hdr.width;
			const uint32_t height = hdr.height;
			const gpu::TextureDesc desc{
			        .format = gpuFmt,
			        .extent = {width, height},
			        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | gpu::ImageUsage::HostTransfer,
			        .aspect = gpu::ImageAspect::Color,
			};
			gpu::TextureHandle handle = gpu::ResourceRegistry::CreateTexture(desc);
			if (!handle.IsValid())
			{
				Throw(AetherError::Engine("Texture: CreateTexture failed (DDS)"));
			}

			const gpu::Image image = gpu::ResourceRegistry::ResolveTextureImage(handle);
			const gpu::ImageView view = gpu::ResourceRegistry::ResolveTexture(handle).view;

			{
				const std::int32_t copyResult = gpu::Factory::HostCopyToImage(device, image, p, width, height);
				if (copyResult != 0)
				{
					Throw(AetherError::Vulkan(copyResult, "UploadBcnDds: HostCopyToImage failed"));
				}
			}

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

			(void) RegisterTextureBindless(bindless, view, filter, gpu::ImageLayout::ShaderReadOnly);

			return handle;
		}
	} // namespace

	Expected<Texture> Texture::LoadFromFileData(
	        std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Allocator allocator, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, BindlessManager& bindless, TextureFilter filter)
	{
		(void) allocator;

		if (fileData.size() < 4)
		{
			int width = 0, height = 0, channels = 0;
			stbi_uc* const pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fileData.data()), static_cast<int>(fileData.size()), &width, &height, &channels, STBI_rgb_alpha);
			if (!pixels)
			{
				AE_UNEXPECTED(AetherError::Asset("failed to decode '" + std::string(debugPath) + "': " + stbi_failure_reason()));
			}
			Texture texture;
			texture.m_handle = UploadRgbaToGpuImage(pixels, width, height, device, uploadQueue, uploadPool, bindless, filter, std::string(debugPath).c_str());
			stbi_image_free(pixels);
			return texture;
		}

		uint32_t magic = 0;
		std::memcpy(&magic, fileData.data(), 4);
		if (magic == DDS_MAGIC)
		{
			Texture texture;
			AE_TRY(handle, UploadBcnDds(fileData, debugPath, device, uploadQueue, uploadPool, bindless, filter));
			texture.m_handle = *handle;
			return texture;
		}

		int width = 0, height = 0, channels = 0;
		stbi_uc* const pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fileData.data()), static_cast<int>(fileData.size()), &width, &height, &channels, STBI_rgb_alpha);
		if (!pixels)
		{
			AE_UNEXPECTED(AetherError::Asset("failed to decode '" + std::string(debugPath) + "': " + stbi_failure_reason()));
		}

		Texture texture;
		texture.m_handle = UploadRgbaToGpuImage(pixels, width, height, device, uploadQueue, uploadPool, bindless, filter, std::string(debugPath).c_str());
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
		(void) allocator;

		int width = 0;
		int height = 0;
		int channels = 0;

		stbi_uc* const pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);

		if (pixels == nullptr)
		{
			AE_UNEXPECTED(AetherError::Asset("failed to load '" + path.string() + "': " + stbi_failure_reason()));
		}

		Texture texture;
		texture.m_handle = UploadRgbaToGpuImage(pixels, width, height, device, uploadQueue, uploadPool, bindless, filter, path.string().c_str());

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

	std::uint32_t Texture::GetBindlessSlot() const
	{
		return m_bindlessSlot;
	}
} // namespace aether
