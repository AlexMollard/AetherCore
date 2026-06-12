#include "gpu/ResourceRegistry.hpp"

#include "rendering/GraphicsPipeline.hpp"
#include "utils/Assert.hpp"
#include "vulkan/ComputePipelineFactory.hpp"
#include "vulkan/GraphicsPipelineFactory.hpp"
#include "vulkan/ResourceRegistry.hpp"

namespace
{
	::aether::ResourceRegistry* s_reg = nullptr;
}

namespace aether::gpu
{
	void ResourceRegistry::Initialize(const ResourceRegistryInitDesc& desc) noexcept
	{
		s_reg = static_cast<::aether::ResourceRegistry*>(desc.backendRegistry);
		AE_ASSERT(s_reg != nullptr, "ResourceRegistry forwarding pointer is null.");
	}

	BufferHandle ResourceRegistry::CreateBuffer(const BufferDesc& d) noexcept
	{
		return s_reg->CreateBuffer(d);
	}

	BufferHandle ResourceRegistry::CreateMappedBuffer(const MappedBufferDesc& d) noexcept
	{
		return s_reg->CreateMappedBuffer(d);
	}

	TextureHandle ResourceRegistry::CreateTexture(const TextureDesc& d) noexcept
	{
		return s_reg->CreateTexture(d);
	}

	MappedBufferView ResourceRegistry::ResolveMappedBuffer(BufferHandle h) noexcept
	{
		return s_reg->ResolveMappedBuffer(h);
	}

	void ResourceRegistry::FlushMappedBuffer(BufferHandle h, DeviceSize o, DeviceSize s) noexcept
	{
		s_reg->FlushMappedBuffer(h, o, s);
	}

	void ResourceRegistry::Destroy(BufferHandle h) noexcept
	{
		s_reg->Destroy(h);
	}

	void ResourceRegistry::Destroy(TextureHandle h) noexcept
	{
		s_reg->Destroy(h);
	}

	void ResourceRegistry::Destroy(PipelineHandle h) noexcept
	{
		s_reg->Destroy(h);
	}

	PipelineHandle ResourceRegistry::CreateComputePipeline(Device device, PipelineCache pipelineCache, const ComputePipelineDesc& desc) noexcept
	{
		const vkutil::ComputePipelineDesc vkDesc{
		        .shaderVfsPath = desc.shaderVfsPath,
		        .shaderEntry = desc.shaderEntry,
		        .pushConstantSize = desc.pushConstantSize,
		        .debugName = desc.debugName,
		        .existingLayout = static_cast<VkPipelineLayout>(desc.existingLayout),
		};
		const auto entryExp = vkutil::CreateComputePipelineEntry(device, pipelineCache, vkDesc);
		if (!entryExp.has_value())
		{
			return PipelineHandle{};
		}
		return s_reg->RegisterPipeline(entryExp.value());
	}

	PipelineHandle ResourceRegistry::CreateGraphicsPipeline(Device device, PipelineCache pipelineCache, const GraphicsPipelineDesc& desc) noexcept
	{
		GraphicsPipeline::Desc vkDesc{
		        .shaderVfsPath = desc.shaderVfsPath,
		        .vertexEntry = desc.vertexEntry,
		        .fragmentEntry = desc.fragmentEntry,
		        .colorFormat = desc.colorFormat,
		        .depthFormat = desc.depthFormat,
		        .depthTestEnable = desc.depthTestEnable,
		        .depthWriteEnable = desc.depthWriteEnable,
		        .depthCompareOp = desc.depthCompareOp,
		        .blendEnable = desc.blendEnable,
		        .pushConstantSize = desc.pushConstantSize,
		        .pushConstantStages = desc.pushConstantStages,
		        .setLayouts = desc.setLayouts,
		};
		const auto entryExp = vkutil::CreateGraphicsPipelineEntry(device, pipelineCache, vkDesc);
		if (!entryExp.has_value())
		{
			return PipelineHandle{};
		}
		return s_reg->RegisterPipeline(entryExp.value());
	}

	ResourceRegistry::ResolvedPipeline ResourceRegistry::ResolvePipeline(PipelineHandle h) noexcept
	{
		const ::aether::ResourceRegistry::PipelineEntry* entry = s_reg->Resolve(h);
		if (entry == nullptr)
		{
			return {};
		}
		ResolvedPipeline out{};
		out.pipeline = static_cast<Pipeline>(entry->pipeline);
		out.layout = static_cast<PipelineLayout>(entry->layout);
		return out;
	}
} // namespace aether::gpu
