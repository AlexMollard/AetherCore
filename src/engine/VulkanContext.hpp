#pragma once

#include <optional>

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace aether
{
	class Window;

	class VulkanContext
	{
	public:
		VulkanContext(const Window& window, const char* appName);
		~VulkanContext();

		VulkanContext(const VulkanContext&) = delete;
		VulkanContext& operator=(const VulkanContext&) = delete;

		[[nodiscard]] const vkb::Instance& GetInstance() const;
		[[nodiscard]] const vkb::Device& GetDevice() const;
		[[nodiscard]] VkSurfaceKHR GetSurface() const;
		[[nodiscard]] VmaAllocator GetAllocator() const;
		[[nodiscard]] VkQueue GetGraphicsQueue() const;
		[[nodiscard]] VkQueue GetPresentQueue() const;
		[[nodiscard]] std::uint32_t GetGraphicsQueueFamily() const;

	private:
		std::optional<vkb::Instance> m_instance;
		std::optional<vkb::Device> m_device;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkSurfaceKHR m_surface = VK_NULL_HANDLE;
		VkQueue m_graphicsQueue = VK_NULL_HANDLE;
		VkQueue m_presentQueue = VK_NULL_HANDLE;
		std::uint32_t m_graphicsQueueFamily = 0;
	};
}