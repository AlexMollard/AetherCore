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
		void Initialize(gpu::Device device, gpu::PipelineCache pipelineCache);
		void Shutdown();
		void RegisterPass(RenderGraph& graph, RenderQueue& renderQueue, const std::string& namePrefix = {});

		// Bound each frame in PrepareAndDispatch. Returns the gpu::Pipeline /
		// gpu::PipelineLayout currently registered for the single-pass path.
		[[nodiscard]] gpu::Pipeline GetSinglePipeline() const;
		[[nodiscard]] gpu::PipelineLayout GetSingleLayout() const;
		[[nodiscard]] gpu::Pipeline GetMultiPipeline() const;
		[[nodiscard]] gpu::PipelineLayout GetMultiLayout() const;

	private:
		Expected<void> EnsureSinglePipeline();
		Expected<void> EnsureMultiPipeline();

		gpu::Device m_device = nullptr;
		gpu::PipelineCache m_pipelineCache = nullptr;
		gpu::PipelineHandle m_singleHandle{};
		gpu::PipelineHandle m_multiHandle{};
	};
} // namespace aether
