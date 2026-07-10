#pragma once
#include <cstddef>
#include <span>

#include "PipelineUtils.hpp"

namespace aether::assetpipeline
{
	namespace SpirvProcessor
	{
		// Strip heavy debug/source metadata from a SPIR-V binary while keeping the
		// module VALID (spirv-val clean) and keeping OpName/OpMemberName (the Khronos
		// validation layer requires names on push-constant variables, and they make
		// RenderDoc/validation output readable).
		// Removes: OpSource(+Continued/Extension), OpLine, OpNoLine, OpModuleProcessed,
		//          the whole NonSemantic.Shader.DebugInfo.* OpExtInst block (import +
		//          instructions), and OpString unless another non-semantic set (e.g.
		//          NonSemantic.DebugPrintf) still references strings.
		// Returns stripped bytes, or an empty vector if the input is not valid SPIR-V.
		[[nodiscard]] ByteBuffer Strip(std::span<const std::byte> spv);
	} // namespace SpirvProcessor
} // namespace aether::assetpipeline
