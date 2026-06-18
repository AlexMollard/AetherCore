#include "rendering/GraphicsPipeline.hpp"

#include "gpu/ResourceRegistry.hpp"

namespace aether
{
	GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
	      : m_handle(std::exchange(other.m_handle, {}))
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
		        .fragmentVfsPath = desc.fragmentVfsPath.data() ? desc.fragmentVfsPath.data() : "",
		        .vertexEntry = desc.vertexEntry.data() ? desc.vertexEntry.data() : "vertexMain",
		        .fragmentEntry = desc.fragmentEntry.data() ? desc.fragmentEntry.data() : "fragmentMain",
		        .colorFormat = desc.colorFormat,
		        .depthFormat = desc.depthFormat,
		        .depthTestEnable = desc.depthTestEnable,
		        .depthWriteEnable = desc.depthWriteEnable,
		        .depthCompareOp = desc.depthCompareOp,
		        .blendEnable = desc.blendEnable,
		        .topology = desc.topology,
		        .polygonMode = desc.polygonMode,
		        .vertexBindings = desc.vertexBindings,
		        .vertexAttributes = desc.vertexAttributes,
		        .lineWidthDynamic = desc.lineWidthDynamic,
		        .debugName = desc.debugName,
		        .descriptorHeapMappings = desc.descriptorHeapMappings,
		};
		GraphicsPipeline out;
		out.m_handle = gpu::ResourceRegistry::CreateGraphicsPipeline(device, pipelineCache, facadeDesc);
		if (!out.m_handle.IsValid())
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "GraphicsPipeline: failed to register with ResourceRegistry."));
		}
		return out;
	}

	gpu::Pipeline GraphicsPipeline::GetPipeline() const
	{
		return const_cast<void*>(gpu::ResourceRegistry::ResolvePipeline(m_handle).state);
	}
} // namespace aether
