#include "material/Texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpu/TextureUpload.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "gpu/UploadContext.hpp"

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
			const std::uint32_t mipLevels = MipCountFor(width, height);
			const auto format = colorSpace == TextureColorSpace::Srgb ? gpu::Format::R8G8B8A8Srgb : gpu::Format::R8G8B8A8Unorm;
			const gpu::Extent2D extent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
			const bool hostCopy = gpu::CanUseHostImageCopy(format, extent, mipLevels);
			const gpu::TextureDesc desc{
			        .format = format,
			        .extent = extent,
			        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | (hostCopy ? gpu::ImageUsage::HostTransfer : gpu::ImageUsage::None),
			        .aspect = gpu::ImageAspect::Color,
			        .mipLevels = mipLevels,
			        .debugName = debugName,
			};
			const gpu::TextureHandle handle = gpu::ResourceRegistry::CreateTexture(desc);
			if (!handle.IsValid())
			{
				Throw(AetherError::Engine("Texture: CreateTexture failed"));
			}
			ScopedTexture lease{handle};

			std::vector<gpu::ImageUploadRegion> regions;
			std::size_t totalBytes = 0;
			for (std::uint32_t level = 0; level < mipLevels; ++level)
			{
				const auto levelWidth = std::max(1u, extent.width >> level);
				const auto levelHeight = std::max(1u, extent.height >> level);
				regions.push_back({.offset = totalBytes, .width = levelWidth, .height = levelHeight, .mipLevel = level});
				totalBytes += std::size_t(levelWidth) * levelHeight * 4u;
			}
			std::vector<std::byte> data(totalBytes);
			std::memcpy(data.data(), pixels, std::size_t(width) * height * 4u);
			auto* base = reinterpret_cast<stbi_uc*>(data.data());
			if (colorSpace == TextureColorSpace::Srgb)
			{
				DilateTransparentRgb(base, width, height, 4);
			}
			// Preserve colour-space filtering and transparent RGB dilation identically
			// for both upload mechanisms. Every mip filters the previous one.
			for (std::uint32_t level = 1; level < mipLevels; ++level)
			{
				const auto& parent = regions[level - 1];
				const auto& next = regions[level];
				const auto* source = reinterpret_cast<const stbi_uc*>(data.data() + parent.offset);
				auto* target = reinterpret_cast<stbi_uc*>(data.data() + next.offset);
				DownsampleBox2x2(source, static_cast<int>(parent.width), static_cast<int>(parent.height), 4, target, colorSpace == TextureColorSpace::Srgb);
				if (colorSpace == TextureColorSpace::Srgb)
				{
					DilateTransparentRgb(target, static_cast<int>(next.width), static_cast<int>(next.height), 4);
				}
			}
			if (const auto result = gpu::UploadTexture(device, uploadQueue, uploadPool, handle, data, regions, hostCopy); !result)
			{
				Throw(result.error());
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
			const std::string ddsName(debugPath);
			const bool hostCopy = gpu::CanUseHostImageCopy(*gpuFmt, {width, height}, mipLevels);
			const gpu::TextureDesc desc{
			        .format = *gpuFmt,
			        .extent = {width, height},
			        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | (hostCopy ? gpu::ImageUsage::HostTransfer : gpu::ImageUsage::None),
			        .aspect = gpu::ImageAspect::Color,
			        .mipLevels = mipLevels,
			        .debugName = ddsName.c_str(),
			};
			const gpu::TextureHandle handle = gpu::ResourceRegistry::CreateTexture(desc);
			if (!handle.IsValid())
			{
				Throw(AetherError::Engine("Texture: CreateTexture failed (DDS)"));
			}
			ScopedTexture lease{handle};
			std::vector<gpu::ImageUploadRegion> regions;
			std::size_t offset = 0;
			mipWidth = width;
			mipHeight = height;
			for (std::uint32_t level = 0; level < mipLevels; ++level)
			{
				regions.push_back({.offset = offset, .width = mipWidth, .height = mipHeight, .mipLevel = level});
				offset += ((std::size_t(mipWidth) + 3) / 4) * ((std::size_t(mipHeight) + 3) / 4) * blockBytes;
				mipWidth = std::max(1u, mipWidth / 2);
				mipHeight = std::max(1u, mipHeight / 2);
			}
			if (const auto result = gpu::UploadTexture(device, uploadQueue, uploadPool, handle, fileData.subspan(kMinSize, offset), regions, hostCopy); !result)
			{
				Throw(result.error());
			}

			RegisterTextureBindless(handle, gpu::ImageLayout::ShaderReadOnly);
			lease.released = true;

			return handle;
		}
	} // namespace

	namespace
	{
		struct PreparedLayout
		{
			gpu::Format format;
			std::vector<gpu::ImageUploadRegion> regions;
			std::size_t sourceOffset = 0;
			std::uint64_t bytes = 0;
		};

		Expected<PreparedLayout> GetPreparedLayout(const Texture::Prepared& prepared, std::string_view path, TextureColorSpace colorSpace)
		{
			PreparedLayout layout{};
			std::uint32_t width, height, levels;
			std::size_t blockBytes = 0;
			if (prepared.dds)
			{
				constexpr std::size_t headerSize = sizeof(std::uint32_t) + sizeof(DdsHeader) + sizeof(DdsDx10Header);
				if (prepared.data.size() < headerSize)
					AE_UNEXPECTED(AetherError::Asset("DDS file too small: " + std::string(path)));
				DdsHeader hdr{};
				DdsDx10Header dx10{};
				std::memcpy(&hdr, prepared.data.data() + sizeof(std::uint32_t), sizeof(hdr));
				std::memcpy(&dx10, prepared.data.data() + sizeof(std::uint32_t) + sizeof(hdr), sizeof(dx10));
				if (hdr.ddspf.fourCC != FOURCC_DX10)
					AE_UNEXPECTED(AetherError::Asset("Unsupported DDS header: " + std::string(path)));
				AE_TRY(format, DxgiToGpuFormat(dx10.dxgiFormat, colorSpace, path));
				layout.format = *format;
				width = hdr.width;
				height = hdr.height;
				levels = std::max(1u, hdr.mipMapCount);
				blockBytes = dx10.dxgiFormat == DXGI_BC4_UNORM ? 8u : 16u;
				layout.sourceOffset = headerSize;
			}
			else
			{
				if (prepared.width <= 0 || prepared.height <= 0)
					AE_UNEXPECTED(AetherError::Asset("Invalid texture dimensions: " + std::string(path)));
				width = static_cast<std::uint32_t>(prepared.width);
				height = static_cast<std::uint32_t>(prepared.height);
				levels = std::bit_width(std::max(width, height));
				layout.format = colorSpace == TextureColorSpace::Srgb ? gpu::Format::R8G8B8A8Srgb : gpu::Format::R8G8B8A8Unorm;
			}
			if (!width || !height || levels > std::bit_width(std::max(width, height)))
				AE_UNEXPECTED(AetherError::Asset("Invalid texture dimensions or mip count: " + std::string(path)));
			for (std::uint32_t i = 0; i < levels; ++i)
			{
				const std::uint64_t x = blockBytes ? (std::uint64_t(width) + 3) / 4 : width;
				const std::uint64_t y = blockBytes ? (std::uint64_t(height) + 3) / 4 : height;
				const std::uint64_t pixelBytes = blockBytes ? blockBytes : 4;
				if (x > (std::numeric_limits<std::uint64_t>::max() - layout.bytes) / pixelBytes / y)
					AE_UNEXPECTED(AetherError::Asset("Texture size overflow: " + std::string(path)));
				layout.regions.push_back({layout.bytes, width, height});
				layout.bytes += x * y * pixelBytes;
				width = std::max(1u, width / 2);
				height = std::max(1u, height / 2);
			}
			const std::uint64_t required = prepared.dds ? layout.bytes : std::uint64_t(prepared.width) * std::uint64_t(prepared.height) * 4;
			if (required > prepared.data.size() - layout.sourceOffset)
				AE_UNEXPECTED(AetherError::Asset("Truncated texture data: " + std::string(path)));
			return layout;
		}
	}

	Expected<std::uint64_t> Texture::PreparedUploadBytes(const Prepared& prepared, std::string_view debugPath, TextureColorSpace colorSpace)
	{
		AE_TRY(layout, GetPreparedLayout(prepared, debugPath, colorSpace));
		std::uint64_t bytes = layout->bytes;
		if (!prepared.dds && colorSpace == TextureColorSpace::Srgb)
		{
			// Dilation keeps one byte per texel plus a geometrically grown
			// size_t frontier; reserve the transient peak before it allocates.
			const std::uint64_t texels = std::uint64_t(prepared.width) * std::uint64_t(prepared.height);
			constexpr std::uint64_t scratchPerTexel = 1 + 2 * sizeof(std::size_t);
			if (texels > (std::numeric_limits<std::uint64_t>::max() - bytes) / scratchPerTexel)
				AE_UNEXPECTED(AetherError::Asset("Texture scratch size overflow"));
			bytes += texels * scratchPerTexel;
		}
		return bytes;
	}

	Expected<void> Texture::PreparedAllocationSizes(const Prepared& prepared, std::string_view debugPath, TextureColorSpace colorSpace, std::uint64_t& stagingBytes, std::uint64_t& gpuBytes)
	{
		AE_TRY(layout, GetPreparedLayout(prepared, debugPath, colorSpace));
		const gpu::TextureDesc desc{.format = layout->format, .extent = {layout->regions[0].width, layout->regions[0].height}, .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled, .aspect = gpu::ImageAspect::Color, .mipLevels = static_cast<std::uint32_t>(layout->regions.size())};
		const gpu::MappedBufferDesc staging{.size = layout->bytes, .usage = gpu::BufferUsage::TransferSrc, .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu};
		gpuBytes = gpu::ResourceRegistry::GetTextureAllocationRequirement(desc);
		stagingBytes = gpu::ResourceRegistry::GetMappedBufferAllocationRequirement(staging);
		if (!gpuBytes || !stagingBytes)
			AE_UNEXPECTED(AetherError::Engine("Failed to query streamed texture allocation requirements"));
		return {};
	}

	Expected<Texture::ChunkedUpload> Texture::BeginChunkedUpload(const Prepared& prepared, std::string_view debugPath, gpu::UploadContext& upload, TextureColorSpace colorSpace, std::function<void()> onGpuFailureRetired, std::function<void()> onGpuFailureBeginRetire)
	{
		struct FailureGuard
		{
			gpu::TextureHandle handle{};
			std::function<void()> callback;
			std::function<void()> beginRetire;
			~FailureGuard()
			{
				if (beginRetire) beginRetire();
				if (handle.IsValid())
				{
					const std::lock_guard lock(gpu::ResourceRegistry::BufferMutex());
					gpu::ResourceRegistry::Destroy(handle);
					if (callback) gpu::ResourceRegistry::DeferDestructionCallback(std::move(callback));
				}
				else if (callback) callback();
			}
		} guard{{}, std::move(onGpuFailureRetired), std::move(onGpuFailureBeginRetire)};
		AE_TRY(layout, GetPreparedLayout(prepared, debugPath, colorSpace));
		ChunkedUpload state;
		state.upload = &upload;
		state.mips = layout->regions;
		state.pixels.resize(static_cast<std::size_t>(layout->bytes));
		auto* pixels = reinterpret_cast<stbi_uc*>(state.pixels.data());
		if (prepared.dds)
		{
			state.blockWidth = state.blockHeight = 4;
			state.blockBytes = layout->format == gpu::Format::BC4UnormBlock ? 8 : 16;
			std::memcpy(pixels, prepared.data.data() + layout->sourceOffset, state.pixels.size());
		}
		else
		{
			const auto& base = state.mips[0];
			std::memcpy(pixels, prepared.data.data(), std::size_t(base.width) * base.height * 4);
			if (colorSpace == TextureColorSpace::Srgb) DilateTransparentRgb(pixels, base.width, base.height, 4);
			for (std::size_t i = 1; i < state.mips.size(); ++i)
			{
				const auto& prev = state.mips[i - 1];
				const auto& next = state.mips[i];
				auto* target = pixels + next.offset;
				DownsampleBox2x2(pixels + prev.offset, prev.width, prev.height, 4, target, colorSpace == TextureColorSpace::Srgb);
				if (colorSpace == TextureColorSpace::Srgb) DilateTransparentRgb(target, next.width, next.height, 4);
			}
		}
		const std::string name(debugPath);
		const gpu::TextureDesc desc{.format = layout->format, .extent = {state.mips[0].width, state.mips[0].height}, .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled, .aspect = gpu::ImageAspect::Color, .mipLevels = static_cast<std::uint32_t>(state.mips.size()), .debugName = name.c_str()};
		guard.handle = gpu::ResourceRegistry::CreateTexture(desc);
		if (!guard.handle.IsValid())
			AE_UNEXPECTED(AetherError::Engine("Failed to allocate streamed texture"));
		RegisterTextureBindless(guard.handle, gpu::ImageLayout::ShaderReadOnly);
		state.texture.m_handle = guard.handle;
		state.texture.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(guard.handle);
		guard.handle = {};
		guard.callback = {};
		guard.beginRetire = {};
		return state;
	}

	Expected<std::uint64_t> Texture::PumpChunkedUpload(ChunkedUpload& state, std::uint64_t maxBytes, const std::function<bool(std::uint64_t)>& reserveStaging, const std::function<void(std::uint64_t)>& retireStaging)
	{
		if (state.complete || state.mip >= state.mips.size()) return std::uint64_t{0};
		const auto& mip = state.mips[state.mip];
		const std::uint32_t blocksWide = (mip.width + state.blockWidth - 1) / state.blockWidth;
		const std::uint32_t blocksHigh = (mip.height + state.blockHeight - 1) / state.blockHeight;
		auto blocks = static_cast<std::uint32_t>(std::min<std::uint64_t>(blocksWide - state.blockX, maxBytes / state.blockBytes));
		if (!blocks) return std::uint64_t{0};
		std::uint64_t payload = 0;
		std::uint64_t allocation = 0;
		gpu::MappedBufferDesc desc{.usage = gpu::BufferUsage::TransferSrc, .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu, .debugName = "Texture streaming chunk"};
		while (blocks)
		{
			payload = std::uint64_t(blocks) * state.blockBytes;
			desc.size = payload;
			allocation = gpu::ResourceRegistry::GetMappedBufferAllocationRequirement(desc);
			if (!allocation) AE_UNEXPECTED(AetherError::Engine("Failed to query texture staging requirement"));
			if (reserveStaging(allocation)) break;
			blocks /= 2;
		}
		if (!blocks) return std::uint64_t{0};
		const auto staging = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!staging.IsValid())
		{
			retireStaging(allocation);
			AE_UNEXPECTED(AetherError::Engine("Failed to allocate texture staging chunk"));
		}
		const auto mapped = gpu::ResourceRegistry::ResolveMappedBuffer(staging);
		if (!mapped.mappedPtr)
		{
			const std::lock_guard lock(gpu::ResourceRegistry::BufferMutex());
			gpu::ResourceRegistry::Destroy(staging);
			gpu::ResourceRegistry::DeferDestructionCallback([retireStaging, allocation] { retireStaging(allocation); });
			AE_UNEXPECTED(AetherError::Engine("Failed to map texture staging chunk"));
		}
		const auto source = mip.offset + (std::uint64_t(state.blockY) * blocksWide + state.blockX) * state.blockBytes;
		std::memcpy(mapped.mappedPtr, state.pixels.data() + source, static_cast<std::size_t>(payload));
		gpu::ResourceRegistry::FlushMappedBuffer(staging, 0, payload);
		const std::uint32_t x = state.blockX * state.blockWidth;
		const std::uint32_t y = state.blockY * state.blockHeight;
		gpu::ImageUploadRegion region{.offset = 0, .width = std::min(blocks * state.blockWidth, mip.width - x), .height = std::min(state.blockHeight, mip.height - y), .mipLevel = state.mip, .x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y)};
		const bool final = state.mip + 1 == state.mips.size() && state.blockY + 1 == blocksHigh && state.blockX + blocks == blocksWide;
		const auto ticket = state.upload->CopyImageChunkAsyncOwned(staging, state.texture.m_handle, std::span(&region, 1), static_cast<std::uint32_t>(state.mips.size()), !state.submitted, final, [retireStaging, allocation] { retireStaging(allocation); });
		if (!ticket) AE_UNEXPECTED(AetherError::Engine("Failed to submit texture staging chunk"));
		state.ticket = ticket;
		state.submitted = true;
		state.blockX += blocks;
		if (state.blockX == blocksWide)
		{
			state.blockX = 0;
			if (++state.blockY == blocksHigh)
			{
				state.blockY = 0;
				++state.mip;
			}
		}
		state.complete = final;
		return payload;
	}

	Expected<Texture> Texture::UploadAsync(const Prepared& prepared, std::string_view debugPath, gpu::UploadContext& upload, std::uint64_t& ticket, std::uint64_t& bytes, TextureColorSpace colorSpace, std::function<void()> onStagingRetired, std::function<void()> onGpuFailureRetired)
	{
		struct ImageGuard
		{
			gpu::TextureHandle handle{};
			std::function<void()> callback;
			~ImageGuard()
			{
				if (handle.IsValid())
				{
					const std::lock_guard lock(gpu::ResourceRegistry::BufferMutex());
					gpu::ResourceRegistry::Destroy(handle);
					if (callback) gpu::ResourceRegistry::DeferDestructionCallback(std::move(callback));
				}
				else if (callback) callback();
			}
		} imageGuard{{}, std::move(onGpuFailureRetired)};
		struct StagingGuard
		{
			gpu::BufferHandle handle{};
			std::function<void()> callback;
			~StagingGuard()
			{
				if (handle.IsValid())
				{
					const std::lock_guard lock(gpu::ResourceRegistry::BufferMutex());
					gpu::ResourceRegistry::Destroy(handle);
					if (callback) gpu::ResourceRegistry::DeferDestructionCallback(std::move(callback));
				}
				else if (callback) callback();
			}
		} stagingGuard{{}, std::move(onStagingRetired)};
		ticket = 0;
		bytes = 0;
		AE_TRY(layout, GetPreparedLayout(prepared, debugPath, colorSpace));
		const std::string name(debugPath);
		const gpu::TextureDesc desc{.format = layout->format, .extent = {layout->regions[0].width, layout->regions[0].height}, .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled, .aspect = gpu::ImageAspect::Color, .mipLevels = static_cast<std::uint32_t>(layout->regions.size()), .debugName = name.c_str()};
		const auto handle = gpu::ResourceRegistry::CreateTexture(desc);
		if (!handle.IsValid())
			AE_UNEXPECTED(AetherError::Engine("Failed to allocate streamed texture"));
		imageGuard.handle = handle;
		RegisterTextureBindless(handle, gpu::ImageLayout::ShaderReadOnly);
		const auto staging = gpu::ResourceRegistry::CreateMappedBuffer({.size = layout->bytes, .usage = gpu::BufferUsage::TransferSrc, .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu, .debugName = "Texture streaming staging"});
		if (!staging.IsValid())
			AE_UNEXPECTED(AetherError::Engine("Failed to allocate texture staging"));
		stagingGuard.handle = staging;
		auto mapped = gpu::ResourceRegistry::ResolveMappedBuffer(staging);
		if (!mapped.mappedPtr)
		{
			AE_UNEXPECTED(AetherError::Engine("Failed to map texture staging"));
		}
		auto* dst = static_cast<stbi_uc*>(mapped.mappedPtr);
		if (prepared.dds)
			std::memcpy(dst, prepared.data.data() + layout->sourceOffset, static_cast<std::size_t>(layout->bytes));
		else
		{
			const auto& base = layout->regions[0];
			std::memcpy(dst, prepared.data.data(), std::size_t(base.width) * base.height * 4);
			if (colorSpace == TextureColorSpace::Srgb)
				DilateTransparentRgb(dst, base.width, base.height, 4);
			for (std::size_t i = 1; i < layout->regions.size(); ++i)
			{
				const auto& prev = layout->regions[i - 1];
				const auto& next = layout->regions[i];
				auto* pixels = dst + next.offset;
				DownsampleBox2x2(dst + prev.offset, prev.width, prev.height, 4, pixels, colorSpace == TextureColorSpace::Srgb);
				if (colorSpace == TextureColorSpace::Srgb)
					DilateTransparentRgb(pixels, next.width, next.height, 4);
			}
		}
		gpu::ResourceRegistry::FlushMappedBuffer(staging, 0, layout->bytes);
		stagingGuard.handle = {};
		ticket = upload.CopyImageAsyncOwned(staging, handle, layout->regions, std::exchange(stagingGuard.callback, {}));
		if (!ticket)
			AE_UNEXPECTED(AetherError::Engine("Failed to submit streamed texture upload"));
		Texture result;
		result.m_handle = handle;
		result.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(handle);
		imageGuard.handle = {};
		imageGuard.callback = {};
		bytes = layout->bytes;
		return result;
	}

	Expected<Texture::Prepared> Texture::Decode(std::span<const std::byte> fileData, std::string_view debugPath)
	{
		if (fileData.size() < 4 || fileData.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
			AE_UNEXPECTED(AetherError::Asset("Invalid image size: " + std::string(debugPath)));
		std::uint32_t magic = 0;
		std::memcpy(&magic, fileData.data(), sizeof(magic));
		Prepared result;
		if (magic == DDS_MAGIC)
		{
			result.dds = true;
			result.data.assign(fileData.begin(), fileData.end());
			return result;
		}
		int channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fileData.data()), static_cast<int>(fileData.size()), &result.width, &result.height, &channels, STBI_rgb_alpha);
		if (!pixels)
			AE_UNEXPECTED(AetherError::Asset("failed to decode '" + std::string(debugPath) + "': " + stbi_failure_reason()));
		const auto count = static_cast<std::size_t>(result.width) * static_cast<std::size_t>(result.height) * 4;
		try
		{
			const auto* first = reinterpret_cast<const std::byte*>(pixels);
			result.data.assign(first, first + count);
		}
		catch (...)
		{
			stbi_image_free(pixels);
			throw;
		}
		stbi_image_free(pixels);
		return result;
	}

	Expected<Texture> Texture::Upload(const Prepared& prepared, std::string_view debugPath, gpu::Device device, gpu::Queue uploadQueue, gpu::CommandPool uploadPool, TextureColorSpace colorSpace)
	{
		Texture texture;
		if (prepared.dds)
		{
			AE_TRY(handle, UploadBcnDds(prepared.data, debugPath, device, uploadQueue, uploadPool, colorSpace));
			texture.m_handle = *handle;
		}
		else
		{
			texture.m_handle = UploadRgbaToGpuImage(reinterpret_cast<const stbi_uc*>(prepared.data.data()), prepared.width, prepared.height, device, uploadQueue, uploadPool, colorSpace, std::string(debugPath).c_str());
		}
		texture.m_bindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(texture.m_handle);
		return texture;
	}

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
