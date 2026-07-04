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

	// Per-frame feature flags. Replaces the previous static-bool feature
	// toggles (ForwardPass::s_enabled, etc.) - see plan doc S6. The struct
	// is owned by RenderingSubsystem, snapshotted into FrameContext per
	// frame, and consumed on the render thread via the immutable context.
	struct RenderFeatureFlags
	{
		bool forwardEnabled = true;
	};

	// Stable per-frame/runtime references shared by every render-graph pass
	// registration site (RenderTargetService, ForwardPass, ...). Replaces
	// the previous 8-11 argument lists that suffered from argument-ordering
	// bugs and pointer-defaulting hazards. Defined in plan doc S4.
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
