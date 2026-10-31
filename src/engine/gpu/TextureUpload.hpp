#pragma once

#include <cstddef>
#include <span>

#include "gpu/ResourceRegistry.hpp"
#include "gpu/UploadContext.hpp"
#include "utils/Expected.hpp"

namespace aether::gpu
{
	[[nodiscard]] bool CanUseHostImageCopy(Format format, Extent2D extent, std::uint32_t mipLevels) noexcept;

	// A complete, tightly packed 2D mip chain. Source offsets must satisfy buffer-copy
	// alignment even when the current device chooses the host-copy fast path.
	[[nodiscard]] Expected<void> ValidateTextureUpload(Format format, Extent2D extent, std::uint32_t mipLevels,
	        std::size_t dataBytes, std::span<const ImageUploadRegion> regions);

	// Uploads a newly created image and returns only once it is ready for sampling.
	// The caller owns the texture and externally synchronizes its command pool.
	[[nodiscard]] Expected<void> UploadTexture(Device device, Queue queue, CommandPool pool, TextureHandle texture,
	        std::span<const std::byte> data, std::span<const ImageUploadRegion> regions, bool useHostCopy);
}
