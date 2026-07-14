#pragma once

#include <cstdint>
#include <functional>

#include "gpu/GpuFormat.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class BindlessManager;
	class CameraManager;
	class CullPass;
	class LightingManager;
	class MaterialBuffer;
	class EffectParamBuffer;
	class RenderGraph;
	class Renderer;

	// frame, and consumed on the render thread via the immutable context.
	struct RenderFeatureFlags
	{
		bool forwardEnabled = true;
	};

	struct FrameContext
	{
		RenderGraph* graph = nullptr;
		BindlessManager* bindless = nullptr;
		CameraManager* cameras = nullptr;
		LightingManager* lighting = nullptr;
		Renderer* renderer = nullptr;
		MaterialBuffer* materials = nullptr;
		EffectParamBuffer* effectParams = nullptr;
		const CullPass* cullPass = nullptr;
		std::function<std::uint64_t()> frameIndex;
		gpu::Format depthFormat = gpu::Format::Undefined;
		gpu::Format colorFormat = gpu::Format::Undefined;
		RenderFeatureFlags featureFlags{};
	};
} // namespace aether
