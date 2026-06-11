#pragma once

#include <cstdint>
#include <functional>

#include "gpu/GpuFormat.hpp"

namespace aether
{
	class BindlessManager;
	class CameraManager;
	class CullPass;
	class LightingManager;
	class MaterialBuffer;
	class RenderGraph;
	class Renderer;

	// Stable per-frame/runtime references shared by every render-graph pass
	// registration site (RenderTargetService, ForwardPass, ...). Replaces
	// the previous 8-11 argument lists that suffered from argument-ordering
	// bugs and pointer-defaulting hazards. Defined in plan doc §4.
	struct FrameContext
	{
		RenderGraph* graph = nullptr;
		BindlessManager* bindless = nullptr;
		CameraManager* cameras = nullptr;
		LightingManager* lighting = nullptr;
		Renderer* renderer = nullptr;
		MaterialBuffer* materials = nullptr;
		const CullPass* cullPass = nullptr;
		std::function<std::uint64_t()> frameIndex;
		gpu::Format depthFormat = gpu::Format::Undefined;
		gpu::Format colorFormat = gpu::Format::Undefined;
	};
} // namespace aether
