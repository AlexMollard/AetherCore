#pragma once

#include "vulkan/volk.hpp"

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuFormat.hpp"

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

	// Image aspect flag conversion (bit-preserving).
	[[nodiscard]] VkImageAspectFlags ToVk(ImageAspect aspect) noexcept;

	// Image layout conversion.
	[[nodiscard]] VkImageLayout ToVk(ImageLayout layout) noexcept;

	// Clear value conversion. The ClearValue struct has a layout-compatible
	// union with VkClearValue (color[4] floats + depth float + stencil uint).
	[[nodiscard]] VkClearValue ToVk(const ClearValue& value) noexcept;
} // namespace aether::gpu
