#pragma once

#include "vulkan/volk.hpp"

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuFormat.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	// Format conversions
	[[nodiscard]] VkFormat ToVk(Format format) noexcept;
	[[nodiscard]] Format FromVk(VkFormat format) noexcept;
	[[nodiscard]] bool IsDepthFormat(Format format) noexcept;
	[[nodiscard]] bool IsStencilFormat(Format format) noexcept;
	[[nodiscard]] bool IsBlockCompressed(Format format) noexcept;
	[[nodiscard]] std::uint32_t BytesPerPixel(Format format) noexcept;

	// Pipeline bind point conversion
	[[nodiscard]] VkPipelineBindPoint ToVk(PipelineBindPoint bindPoint) noexcept;

	// Pipeline stage and access flag conversions
	[[nodiscard]] VkPipelineStageFlags2 ToVk(PipelineStage stage) noexcept;
	[[nodiscard]] VkAccessFlags2 ToVk(AccessFlags access) noexcept;

	// Descriptor type conversion
	[[nodiscard]] VkDescriptorType ToVk(DescriptorType type) noexcept;
	[[nodiscard]] VkShaderStageFlags ToVk(ShaderStage stage) noexcept;
	[[nodiscard]] VkDescriptorSetLayoutCreateFlags ToVk(DescriptorSetLayoutFlags flags) noexcept;

	// Depth / stencil compare operation conversion
	[[nodiscard]] VkCompareOp ToVk(CompareOp op) noexcept;

	// Attachment load / store op conversion
	[[nodiscard]] VkAttachmentLoadOp ToVk(LoadOp op) noexcept;
	[[nodiscard]] VkAttachmentStoreOp ToVk(StoreOp op) noexcept;

	// Image usage flag conversion (bit-preserving; engine bits mirror Vk bits).
	[[nodiscard]] VkImageUsageFlags ToVk(ImageUsage usage) noexcept;

	// Buffer usage flag conversion (bit-preserving; engine bits mirror Vk bits).
	[[nodiscard]] VkBufferUsageFlags ToVk(BufferUsage usage) noexcept;

	// Image aspect flag conversion (bit-preserving).
	[[nodiscard]] VkImageAspectFlags ToVk(ImageAspect aspect) noexcept;
	[[nodiscard]] ImageAspect FromVk(VkImageAspectFlags aspect) noexcept;

	// Image layout conversion.
	[[nodiscard]] VkImageLayout ToVk(ImageLayout layout) noexcept;
	[[nodiscard]] ImageLayout FromVk(VkImageLayout layout) noexcept;

	// Component swizzle conversion. Mirrors VkComponentSwizzle.
	[[nodiscard]] VkComponentSwizzle ToVk(ComponentSwizzle s) noexcept;

	// Sampler parameter conversions. Mirror VkFilter, VkSamplerMipmapMode,
	// VkSamplerAddressMode. Only the values BindlessManager's sampler cache
	// emits are enumerated; new values force a switch-case compile error
	// in the backend conversion.
	[[nodiscard]] VkFilter ToVk(Filter filter) noexcept;
	[[nodiscard]] VkSamplerMipmapMode ToVk(SamplerMipmapMode mode) noexcept;
	[[nodiscard]] VkSamplerAddressMode ToVk(SamplerAddressMode mode) noexcept;

	// Clear value conversion. The backend copies the color or depth/stencil
	// fields into the VkClearValue union.
	[[nodiscard]] VkClearValue ToVk(const ClearValue& value) noexcept;

	// Primitive topology and polygon mode conversions.
	[[nodiscard]] VkPrimitiveTopology ToVk(PrimitiveTopology topology) noexcept;
	[[nodiscard]] VkPolygonMode ToVk(PolygonMode mode) noexcept;

	// -------------------------------------------------------------------------
	// Barrier + dynamic-rendering conversions (P5(d))
	// -------------------------------------------------------------------------
	// Engine-side barrier structs (ImageMemoryBarrier / BufferMemoryBarrier)
	// are translated to VkImageMemoryBarrier2 / VkBufferMemoryBarrier2 here.
	// The .image / .buffer fields are opaque gpu::Image / gpu::Buffer; the
	// caller passes in the resolved Vulkan handle via the helper functions
	// below (the engine-side struct holds the opaque type, the storage knows
	// the actual VkImage / VkBuffer to populate).
	[[nodiscard]] VkImageMemoryBarrier2 ToVk(
	        const ImageMemoryBarrier& barrier,
	        VkImage resolvedImage) noexcept;
	[[nodiscard]] VkBufferMemoryBarrier2 ToVk(
	        const BufferMemoryBarrier& barrier,
	        VkBuffer resolvedBuffer) noexcept;
	[[nodiscard]] VkRenderingAttachmentInfo ToVk(const RenderingAttachmentInfo& info) noexcept;
	[[nodiscard]] VkRenderingInfo ToVk(
	        const RenderingInfo& info,
	        const VkRenderingAttachmentInfo* vkColorAttachments,
	        std::uint32_t colorAttachmentCount,
	        const VkRenderingAttachmentInfo* vkDepthAttachment) noexcept;
} // namespace aether::gpu
