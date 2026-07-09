#include "material/Texture.hpp"

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

		gpu::TextureHandle UploadRgbaToGpuImage(const stbi_uc* pixels, int width, int height, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, const char* debugName = nullptr)
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

			{
				const std::int32_t copyResult = vkutil::HostCopyToImage(device, image, pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
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

			RegisterTextureBindless(handle, gpu::ImageLayout::ShaderReadOnly);

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

		Expected<gpu::TextureHandle> UploadBcnDds(std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool)
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

			RegisterTextureBindless(handle, gpu::ImageLayout::ShaderReadOnly);

			return handle;
		}
	} // namespace

	Expected<Texture> Texture::LoadFromFileData(std::span<const std::byte> fileData, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool)
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
			texture.m_handle = UploadRgbaToGpuImage(pixels, width, height, device, uploadQueue, uploadPool, std::string(debugPath).c_str());
			texture.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(texture.m_handle);
			stbi_image_free(pixels);
			return texture;
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
		texture.m_handle = UploadRgbaToGpuImage(pixels, width, height, device, uploadQueue, uploadPool, std::string(debugPath).c_str());
		texture.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(texture.m_handle);
		stbi_image_free(pixels);
		return texture;
	}

	Expected<Texture> Texture::CreateSolidColor(std::array<std::uint8_t, 4> rgba, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool)
	{
		// A 1x1 image samples identically at every UV, so a solid colour needs
		// exactly one pixel regardless of how it is stretched over a mesh.
		static_assert(sizeof(stbi_uc) == sizeof(std::uint8_t));
		Texture texture;
		texture.m_handle = UploadRgbaToGpuImage(rgba.data(), 1, 1, device, uploadQueue, uploadPool, "builtin:solid-color");
		texture.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(texture.m_handle);
		return texture;
	}

	std::string Texture::ResolveTexturePath(std::string_view path)
	{
		// Derive the .texture sibling path (e.g. "project://assets/foo/bar.png" -> ".../bar.texture")
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
		// Prefer the pre-transcoded .texture (DDS) sibling if present; else the
		// original path (assets not processed by AssetPacker).
		return io::FileSystem::Exists(texturePath) ? texturePath : pathStr;
	}

	Expected<Texture> Texture::LoadFromFile(std::string_view path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool)
	{
		AE_PROFILE_ZONE();
		AE_PROFILE_SET_ZONE_NAME(path.data());

		const std::string resolved = ResolveTexturePath(path);
		AE_TRY(data, io::FileSystem::ReadFile(resolved));
		return LoadFromFileData(*data, resolved, device, uploadQueue, uploadPool);
	}

	Expected<Texture> Texture::LoadFromDiskPath(const std::filesystem::path& path, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool)
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
		texture.m_handle = UploadRgbaToGpuImage(pixels, width, height, device, uploadQueue, uploadPool, path.string().c_str());
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

	std::uint32_t Texture::GetBindlessSlot() const
	{
		return m_bindlessSlot;
	}
} // namespace aether
