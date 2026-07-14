#pragma once
#include <cstddef>
#include <span>

#include "PipelineUtils.hpp"

namespace aether::assetpipeline
{
	namespace SpirvProcessor
	{
		[[nodiscard]] ByteBuffer Strip(std::span<const std::byte> spv);
	}
} // namespace aether::assetpipeline
