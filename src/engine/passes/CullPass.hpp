#pragma once

#include <string>
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

		[[nodiscard]] gpu::Pipeline GetSinglePipeline() const
		{
			return m_singlePipeline;
		}

		[[nodiscard]] gpu::PipelineLayout GetSingleLayout() const
		{
			return m_singleLayout;
		}

		[[nodiscard]] gpu::Pipeline GetMultiPipeline() const
		{
			return m_multiPipeline;
		}

		[[nodiscard]] gpu::PipelineLayout GetMultiLayout() const
		{
			return m_multiLayout;
		}

	private:
		Expected<void> EnsureSinglePipeline();
		Expected<void> EnsureMultiPipeline();

		gpu::Device m_device = nullptr;
		gpu::PipelineCache m_pipelineCache = nullptr;
		gpu::Pipeline m_singlePipeline = nullptr;
		gpu::PipelineLayout m_singleLayout = nullptr;
		gpu::Pipeline m_multiPipeline = nullptr;
		gpu::PipelineLayout m_multiLayout = nullptr;
	};
} // namespace aether
