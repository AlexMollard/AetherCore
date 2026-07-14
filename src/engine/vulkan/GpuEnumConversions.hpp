#pragma once

#include "vulkan/volk.hpp"

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuFormat.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	[[nodiscard]] VkFormat ToVk(Format format) noexcept;
	[[nodiscard]] Format FromVk(VkFormat format) noexcept;
	[[nodiscard]] bool IsDepthFormat(Format format) noexcept;
	[[nodiscard]] bool IsStencilFormat(Format format) noexcept;
	[[nodiscard]] bool IsBlockCompressed(Format format) noexcept;
	[[nodiscard]] std::uint32_t BytesPerPixel(Format format) noexcept;

	[[nodiscard]] VkPipelineStageFlags2 ToVk(PipelineStage stage) noexcept;
	[[nodiscard]] VkAccessFlags2 ToVk(AccessFlags access) noexcept;

	[[nodiscard]] VkCompareOp ToVk(CompareOp op) noexcept;

	[[nodiscard]] VkAttachmentLoadOp ToVk(LoadOp op) noexcept;
	[[nodiscard]] VkAttachmentStoreOp ToVk(StoreOp op) noexcept;

	[[nodiscard]] VkImageUsageFlags ToVk(ImageUsage usage) noexcept;

	[[nodiscard]] VkBufferUsageFlags2 ToVk(BufferUsage usage) noexcept;

	[[nodiscard]] VkImageAspectFlags ToVk(ImageAspect aspect) noexcept;
	[[nodiscard]] ImageAspect FromVk(VkImageAspectFlags aspect) noexcept;

	// Image layout conversion.
	[[nodiscard]] VkImageLayout ToVk(ImageLayout layout) noexcept;
	[[nodiscard]] ImageLayout FromVk(VkImageLayout layout) noexcept;

	[[nodiscard]] VkComponentSwizzle ToVk(ComponentSwizzle s) noexcept;

	[[nodiscard]] VkFilter ToVk(Filter filter) noexcept;
	[[nodiscard]] VkSamplerMipmapMode ToVk(SamplerMipmapMode mode) noexcept;
	[[nodiscard]] VkSamplerAddressMode ToVk(SamplerAddressMode mode) noexcept;

	[[nodiscard]] VkClearValue ToVk(const ClearValue& value) noexcept;

	[[nodiscard]] VkPrimitiveTopology ToVk(PrimitiveTopology topology) noexcept;
	[[nodiscard]] VkPolygonMode ToVk(PolygonMode mode) noexcept;
	[[nodiscard]] VkCullModeFlags ToVk(CullMode mode) noexcept;

	[[nodiscard]] VkImageMemoryBarrier2 ToVk(const ImageMemoryBarrier& barrier, VkImage resolvedImage) noexcept;
	[[nodiscard]] VkBufferMemoryBarrier2 ToVk(const BufferMemoryBarrier& barrier, VkBuffer resolvedBuffer) noexcept;
	[[nodiscard]] VkRenderingAttachmentInfo ToVk(const RenderingAttachmentInfo& info) noexcept;
	[[nodiscard]] VkRenderingInfo ToVk(const RenderingInfo& info, const VkRenderingAttachmentInfo* vkColorAttachments, std::uint32_t colorAttachmentCount, const VkRenderingAttachmentInfo* vkDepthAttachment) noexcept;
} // namespace aether::gpu
