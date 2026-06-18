#include "rendering/GraphicsPipeline.hpp"

#include "gpu/ResourceRegistry.hpp"

namespace aether
{
	GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
	      : m_handle(std::exchange(other.m_handle, {})), m_setLayoutCount(std::exchange(other.m_setLayoutCount, 0))
	{
	}

	GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept
	{
		if (this != &other)
		{
			if (m_handle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(m_handle);
			}
			m_handle = std::exchange(other.m_handle, {});
			m_setLayoutCount = std::exchange(other.m_setLayoutCount, 0);
		}
		return *this;
	}

	GraphicsPipeline::~GraphicsPipeline()
	{
		if (m_handle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_handle);
		}
	}

	void GraphicsPipeline::Destroy()
	{
		if (m_handle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_handle);
			m_handle = {};
		}
	}

	Expected<GraphicsPipeline> GraphicsPipeline::Create(gpu::Device device, gpu::PipelineCache pipelineCache, const Desc& desc)
	{
		const gpu::GraphicsPipelineDesc facadeDesc{
		        .shaderVfsPath = desc.shaderVfsPath.data() ? desc.shaderVfsPath.data() : "",
		        .vertexEntry = desc.vertexEntry.data() ? desc.vertexEntry.data() : "vertexMain",
		        .fragmentEntry = desc.fragmentEntry.data() ? desc.fragmentEntry.data() : "fragmentMain",
		        .colorFormat = desc.colorFormat,
		        .depthFormat = desc.depthFormat,
		        .depthTestEnable = desc.depthTestEnable,
		        .depthWriteEnable = desc.depthWriteEnable,
		        .depthCompareOp = desc.depthCompareOp,
		        .blendEnable = desc.blendEnable,
		        .pushConstantSize = desc.pushConstantSize,
		        .pushConstantStages = desc.pushConstantStages,
		        .setLayouts = desc.setLayouts,
		        .debugName = desc.debugName,
		        .descriptorHeapMappings = desc.descriptorHeapMappings,
		};
		GraphicsPipeline out;
		out.m_handle = gpu::ResourceRegistry::CreateGraphicsPipeline(device, pipelineCache, facadeDesc);
		if (!out.m_handle.IsValid())
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "GraphicsPipeline: failed to register with ResourceRegistry."));
		}
		out.m_setLayoutCount = static_cast<std::uint32_t>(desc.setLayouts.size());
		return out;
	}

	gpu::Pipeline GraphicsPipeline::GetPipeline() const
	{
		return gpu::ResourceRegistry::ResolvePipeline(m_handle).pipeline;
	}

	gpu::PipelineLayout GraphicsPipeline::GetLayout() const
	{
		return gpu::ResourceRegistry::ResolvePipeline(m_handle).layout;
	}
} // namespace aether
