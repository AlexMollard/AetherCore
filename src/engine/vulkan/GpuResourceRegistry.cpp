#include "gpu/ResourceRegistry.hpp"

#include "rendering/GraphicsPipeline.hpp"
#include "utils/Assert.hpp"
#include "vulkan/ComputePipelineFactory.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/GraphicsPipelineFactory.hpp"
#include "vulkan/ResourceRegistry.hpp"

#include <vk_mem_alloc.h>

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

	BufferHandle ResourceRegistry::CreateBuffer(const BufferDesc& d, std::source_location loc) noexcept
	{
		return s_reg->CreateBuffer(d, loc);
	}

	BufferHandle ResourceRegistry::CreateMappedBuffer(const MappedBufferDesc& d, std::source_location loc) noexcept
	{
		return s_reg->CreateMappedBuffer(d, loc);
	}

	TextureHandle ResourceRegistry::CreateTexture(const TextureDesc& d, std::source_location loc) noexcept
	{
		return s_reg->CreateTexture(d, loc);
	}

	TextureHandle ResourceRegistry::CreateAliasedTexture(const TextureDesc& desc, void* existingAllocation, DeviceSize memoryOffset, const char* debugName) noexcept
	{
		return s_reg->CreateAliasedTexture(desc, static_cast<VmaAllocation>(existingAllocation), static_cast<VkDeviceSize>(memoryOffset), debugName ? std::string_view(debugName) : std::string_view{});
	}

	BufferHandle ResourceRegistry::CreateAliasedBuffer(DeviceSize size, BufferUsage usage, void* existingAllocation, DeviceSize memoryOffset, const char* debugName) noexcept
	{
		return s_reg->CreateAliasedBuffer(static_cast<VkDeviceSize>(size), gpu::ToVk(usage), static_cast<VmaAllocation>(existingAllocation), static_cast<VkDeviceSize>(memoryOffset), debugName ? std::string_view(debugName) : std::string_view{});
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
		return s_reg->RegisterPipeline(entryExp.value(), desc.debugName ? std::string_view(desc.debugName) : std::string_view{});
	}

	PipelineHandle ResourceRegistry::CreateGraphicsPipeline(Device device, PipelineCache pipelineCache, const GraphicsPipelineDesc& desc) noexcept
	{
		GraphicsPipeline::Desc vkDesc{
		        .shaderVfsPath = desc.shaderVfsPath,
		        .fragmentVfsPath = desc.fragmentVfsPath != nullptr ? std::string_view(desc.fragmentVfsPath) : std::string_view{},
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
		        .topology = desc.topology,
		        .polygonMode = desc.polygonMode,
		        .vertexBindings = desc.vertexBindings,
		        .vertexAttributes = desc.vertexAttributes,
		        .lineWidthDynamic = desc.lineWidthDynamic,
		};
		const auto entryExp = vkutil::CreateGraphicsPipelineEntry(device, pipelineCache, vkDesc);
		if (!entryExp.has_value())
		{
			return PipelineHandle{};
		}
		return s_reg->RegisterPipeline(entryExp.value(), desc.debugName ? std::string_view(desc.debugName) : std::string_view{});
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

	ResourceRegistry::ResolvedTexture ResourceRegistry::ResolveTexture(TextureHandle h) noexcept
	{
		const ::aether::ResourceRegistry::TextureEntry* entry = s_reg->Resolve(h);
		if (entry == nullptr)
		{
			return {};
		}
		ResolvedTexture out{};
		out.view = static_cast<ImageView>(entry->view);
		out.format = gpu::FromVk(entry->format);
		out.extent = gpu::Extent2D{entry->extent.width, entry->extent.height};
		out.mipLevels = entry->mipLevels;
		out.arrayLayers = entry->arrayLayers;
		out.usage = static_cast<ImageUsage>(entry->usage);
		return out;
	}

	ResourceRegistry::ResolvedBuffer ResourceRegistry::ResolveBuffer(BufferHandle h) noexcept
	{
		const ::aether::ResourceRegistry::BufferEntry* entry = s_reg->Resolve(h);
		if (entry == nullptr)
		{
			return {};
		}
		ResolvedBuffer out{};
		out.deviceAddress = entry->deviceAddress;
		out.size = entry->size;
		out.usage = static_cast<BufferUsage>(entry->usage);
		return out;
	}

	void* ResourceRegistry::ResolveBufferVkHandle(BufferHandle handle) noexcept
	{
		const ::aether::ResourceRegistry::BufferEntry* entry = s_reg->Resolve(handle);
		if (entry == nullptr)
		{
			return nullptr;
		}
		return static_cast<void*>(entry->buffer);
	}

	gpu::Image ResourceRegistry::ResolveTextureImage(TextureHandle handle) noexcept
	{
		if (s_reg == nullptr)
		{
			return nullptr;
		}
		const ::aether::ResourceRegistry::TextureEntry* entry = s_reg->Resolve(handle);
		if (entry == nullptr)
		{
			return nullptr;
		}
		return static_cast<gpu::Image>(entry->image);
	}

	void ResourceRegistry::SetBufferName(BufferHandle handle, const char* name)
	{
		s_reg->SetBufferName(handle, name);
	}

	void ResourceRegistry::SetTextureName(TextureHandle handle, const char* name)
	{
		s_reg->SetTextureName(handle, name);
	}

	void ResourceRegistry::SetBindlessManager(aether::BindlessManager* mgr)
	{
		s_reg->SetBindlessManager(mgr);
	}

	void ResourceRegistry::EnsureBindlessSampled(TextureHandle handle, aether::BindlessManager& bindlessManager, ImageAspect aspectMask, ImageLayout descriptorLayout, TextureFilter filter, SamplerAddressMode addressMode)
	{
		auto result = s_reg->EnsureBindlessSampled(handle, bindlessManager, aspectMask, descriptorLayout, filter, addressMode);
		if (!result)
		{
			// Log or assert - the function itself logs on failure
		}
	}

	bool ResourceRegistry::HasBindlessSampled(TextureHandle handle)
	{
		return s_reg->HasBindlessSampled(handle);
	}

	std::uint32_t ResourceRegistry::GetBindlessSampledSlot(TextureHandle handle)
	{
		return s_reg->GetBindlessSampledSlot(handle);
	}

	Format ResourceRegistry::GetTextureFormat(TextureHandle handle)
	{
		return s_reg->GetTextureFormat(handle);
	}

	Extent2D ResourceRegistry::GetTextureExtent(TextureHandle handle)
	{
		return s_reg->GetTextureExtent(handle);
	}

	std::uint32_t ResourceRegistry::GetTextureMipLevels(TextureHandle handle)
	{
		return s_reg->GetTextureMipLevels(handle);
	}

	std::uint32_t ResourceRegistry::GetTextureArrayLayers(TextureHandle handle)
	{
		return s_reg->GetTextureArrayLayers(handle);
	}

	ImageUsage ResourceRegistry::GetTextureUsage(TextureHandle handle)
	{
		return s_reg->GetTextureUsage(handle);
	}

	DeviceSize ResourceRegistry::GetBufferSize(BufferHandle handle)
	{
		return s_reg->GetBufferSize(handle);
	}

	BufferUsage ResourceRegistry::GetBufferUsage(BufferHandle handle)
	{
		return s_reg->GetBufferUsage(handle);
	}
} // namespace aether::gpu
