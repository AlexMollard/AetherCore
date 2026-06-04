#pragma once
#include <cstddef>
#include <span>

#include "PipelineUtils.hpp"

namespace SpirvProcessor
{
	// Strip debug/name instructions from SPIR-V binary in-place.
	// Returns stripped bytes, or an empty vector if the input is not valid SPIR-V.
	// Removes: OpSource, OpSourceContinued, OpSourceExtension, OpName,
	//          OpMemberName, OpString, OpLine, OpNoLine, OpModuleProcessed.
	[[nodiscard]] ByteBuffer Strip(std::span<const std::byte> spv);
} // namespace SpirvProcessor
