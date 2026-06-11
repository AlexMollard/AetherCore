#pragma once

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "vulkan/volk.hpp"

namespace aether::gpu
{
	inline constexpr VkDeviceSize kWholeSizeVk = VK_WHOLE_SIZE;

	inline VkCommandBuffer AsVkCommandBuffer(void* p) noexcept
	{
		return static_cast<VkCommandBuffer>(p);
	}

	inline VkPipeline AsVkPipeline(void* p) noexcept
	{
		return static_cast<VkPipeline>(p);
	}

	inline VkPipelineLayout AsVkPipelineLayout(void* p) noexcept
	{
		return static_cast<VkPipelineLayout>(p);
	}

	inline VkBuffer AsVkBuffer(void* p) noexcept
	{
		return static_cast<VkBuffer>(p);
	}

	inline VkDescriptorSet AsVkDescriptorSet(void* p) noexcept
	{
		return static_cast<VkDescriptorSet>(p);
	}

	inline VkImage AsVkImage(void* p) noexcept
	{
		return static_cast<VkImage>(p);
	}

	inline VkImageView AsVkImageView(void* p) noexcept
	{
		return static_cast<VkImageView>(p);
	}

	inline VkSampler AsVkSampler(void* p) noexcept
	{
		return static_cast<VkSampler>(p);
	}

	inline VkDevice AsVkDevice(void* p) noexcept
	{
		return static_cast<VkDevice>(p);
	}

	inline VkDescriptorSetLayout AsVkDescriptorSetLayout(void* p) noexcept
	{
		return static_cast<VkDescriptorSetLayout>(p);
	}

	inline VkRenderPass AsVkRenderPass(void* p) noexcept
	{
		return static_cast<VkRenderPass>(p);
	}

	inline VkFramebuffer AsVkFramebuffer(void* p) noexcept
	{
		return static_cast<VkFramebuffer>(p);
	}

	inline void* FromVkCommandBuffer(VkCommandBuffer v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkPipeline(VkPipeline v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkPipelineLayout(VkPipelineLayout v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkBuffer(VkBuffer v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkDescriptorSet(VkDescriptorSet v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkImage(VkImage v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkImageView(VkImageView v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkSampler(VkSampler v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkDevice(VkDevice v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkDescriptorSetLayout(VkDescriptorSetLayout v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkRenderPass(VkRenderPass v) noexcept
	{
		return static_cast<void*>(v);
	}

	inline void* FromVkFramebuffer(VkFramebuffer v) noexcept
	{
		return static_cast<void*>(v);
	}

	[[nodiscard]] inline VkBufferUsageFlags ToVkBufferUsage(BufferUsage usage) noexcept
	{
		return static_cast<VkBufferUsageFlags>(static_cast<std::uint32_t>(usage));
	}
} // namespace aether::gpu
