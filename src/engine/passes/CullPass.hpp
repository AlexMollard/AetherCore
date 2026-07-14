#pragma once

#include <string>
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	class RenderQueue;

	class CullPass
	{
	public:
		void Initialize(gpu::Device device);
		void Shutdown();
		void RegisterPass(RenderGraph& graph, RenderQueue& renderQueue, const std::string& namePrefix = {}, PreparedDrawList drawList = {});

		[[nodiscard]] gpu::PipelineView GetSinglePipeline() const;
		[[nodiscard]] gpu::PipelineView GetMultiPipeline() const;

	private:
		Expected<void> EnsureSinglePipeline();
		Expected<void> EnsureMultiPipeline();

		gpu::Device m_device = nullptr;
		gpu::PipelineHandle m_singleHandle{};
		gpu::PipelineHandle m_multiHandle{};
	};
} // namespace aether
