#pragma once

#include <optional>

#include <VkBootstrap.h>
#include <vulkan/vulkan.h>

namespace meow
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

	private:
		std::optional<vkb::Instance> m_instance;
		std::optional<vkb::Device> m_device;
		VkSurfaceKHR m_surface = VK_NULL_HANDLE;
	};
}