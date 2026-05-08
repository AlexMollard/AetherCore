#pragma once
#include <cstddef>
#include <vector>

namespace SpirvProcessor
{
	// Strip debug/name instructions from SPIR-V binary in-place.
	// Returns stripped bytes, or an empty vector if the input is not valid SPIR-V.
	// Removes: OpSource, OpSourceContinued, OpSourceExtension, OpName,
	//          OpMemberName, OpString, OpLine, OpNoLine, OpModuleProcessed.
	std::vector<std::byte> Strip(const std::vector<std::byte>& spv);
} // namespace SpirvProcessor
