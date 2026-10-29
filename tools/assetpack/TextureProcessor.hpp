#pragma once
#include <cstddef>
#include <filesystem>
#include <span>

#include "PipelineUtils.hpp"

namespace aether::assetpipeline
{
	namespace TextureProcessor
	{
		enum class BC7Quality : uint8_t
		{
			Normal = 0,
			High = 1,
			Ultra = 2,
		};

		[[nodiscard]] ByteBuffer ToDDS(std::span<const std::byte> imageData, const std::filesystem::path& sourcePath, bool srgb, BC7Quality quality = BC7Quality::Normal);
		[[nodiscard]] ByteBuffer ToRGBA8(std::span<const std::byte> imageData);
	} // namespace TextureProcessor
} // namespace aether::assetpipeline
