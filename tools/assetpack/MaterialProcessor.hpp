#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

#include "PipelineUtils.hpp"

namespace aether::assetpipeline
{
	namespace MaterialProcessor
	{
		// sourcePath is the on-disk path to the .toml file.
		[[nodiscard]] ByteBuffer Process(std::span<const std::byte> tomlData, const std::filesystem::path& sourcePath, const std::filesystem::path& sourceDir);
	} // namespace MaterialProcessor
} // namespace aether::assetpipeline
