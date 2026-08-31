#include "rendering/GraphicsPipeline.hpp"

#include "gpu/ResourceRegistry.hpp"
#include "utils/Profiler.hpp"

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
		AE_PROFILE_ZONE();
		if (m_handle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_handle);
			m_handle = {};
		}
	}

	namespace
	{
		// One translation, used by both Create and Prepare: a pipeline built on a worker must
		// be described exactly like one built inline, or the two silently differ.
		gpu::GraphicsPipelineDesc ToFacadeDesc(const GraphicsPipeline::Desc& desc)
		{
			return gpu::GraphicsPipelineDesc{
		        .shaderVfsPath = desc.shaderVfsPath.data() ? desc.shaderVfsPath.data() : "",
		        .fragmentVfsPath = desc.fragmentVfsPath.data() ? desc.fragmentVfsPath.data() : "",
		        .vertexEntry = desc.vertexEntry.data() ? desc.vertexEntry.data() : "vertexMain",
		        .fragmentEntry = desc.fragmentEntry.data() ? desc.fragmentEntry.data() : "fragmentMain",
		        .colorFormat = desc.colorFormat,
		        .colorAttachmentCount = desc.colorAttachmentCount,
		        .depthFormat = desc.depthFormat,
		        .depthTestEnable = desc.depthTestEnable,
		        .depthWriteEnable = desc.depthWriteEnable,
		        .depthCompareOp = desc.depthCompareOp,
		        .blendEnable = desc.blendEnable,
		        .blendMode = desc.blendMode,
		        .topology = desc.topology,
		        .polygonMode = desc.polygonMode,
		        .cullMode = desc.cullMode,
		        .vertexBindings = desc.vertexBindings,
		        .vertexAttributes = desc.vertexAttributes,
		        .lineWidthDynamic = desc.lineWidthDynamic,
		        .debugName = desc.debugName,
		        .descriptorHeapMappings = desc.descriptorHeapMappings,
		};
		}
	} // namespace

	Expected<GraphicsPipeline> GraphicsPipeline::Create(gpu::Device device, const Desc& desc)
	{
		AE_PROFILE_ZONE();
		GraphicsPipeline out;
		out.m_handle = gpu::ResourceRegistry::CreateGraphicsPipeline(device, ToFacadeDesc(desc));
		if (!out.m_handle.IsValid())
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "GraphicsPipeline: failed to register with ResourceRegistry."));
		}
		return out;
	}

	gpu::ResourceRegistry::PreparedPipeline GraphicsPipeline::Prepare(gpu::Device device, const Desc& desc)
	{
		AE_PROFILE_ZONE();
		return gpu::ResourceRegistry::PrepareGraphicsPipeline(device, ToFacadeDesc(desc));
	}

	Expected<GraphicsPipeline> GraphicsPipeline::Commit(gpu::ResourceRegistry::PreparedPipeline prepared)
	{
		GraphicsPipeline out;
		out.m_handle = gpu::ResourceRegistry::CommitPreparedPipeline(prepared, nullptr);
		if (!out.m_handle.IsValid())
		{
			AE_UNEXPECTED(AetherError::Vulkan(0, "GraphicsPipeline: failed to register a prepared pipeline."));
		}
		return out;
	}

	void GraphicsPipeline::Discard(gpu::ResourceRegistry::PreparedPipeline prepared)
	{
		gpu::ResourceRegistry::DiscardPreparedPipeline(prepared);
	}

	gpu::PipelineView GraphicsPipeline::GetPipeline() const
	{
		return gpu::ResourceRegistry::ResolvePipeline(m_handle).state;
	}
} // namespace aether
