#pragma once

#include <string>
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	class RenderGraph;
	class RenderQueue;

	class CullPass
	{
	public:
		void Initialize(gpu::Device device);
		void Shutdown();
		void RegisterPass(RenderGraph& graph, RenderQueue& renderQueue, const std::string& namePrefix = {});

		// Bound each frame in PrepareAndDispatch. Returns the opaque
		// pipeline state for the single-pass / multi-pass cull shaders.
		[[nodiscard]] gpu::Pipeline GetSinglePipeline() const;
		[[nodiscard]] gpu::Pipeline GetMultiPipeline() const;

	private:
		Expected<void> EnsureSinglePipeline();
		Expected<void> EnsureMultiPipeline();

		gpu::Device m_device = nullptr;
		gpu::PipelineHandle m_singleHandle{};
		gpu::PipelineHandle m_multiHandle{};
	};
} // namespace aether
