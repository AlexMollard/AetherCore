
#include "VulkanContext.hpp"

#include <stdexcept>
#include <string>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Window.hpp"

namespace
{
	std::runtime_error MakeVkBootstrapError(const char* message, const vkb::Result<vkb::Instance>& result)
	{
		return std::runtime_error(std::string(message) + result.error().message());
	}
}

namespace meow
{
	VulkanContext::VulkanContext(const Window& window, const char* appName)
	{
		vkb::InstanceBuilder instanceBuilder;
		auto instanceResult = instanceBuilder.set_app_name(appName)
			.require_api_version(1, 4, 0)
			.request_validation_layers()
			.use_default_debug_messenger()
			.build();

		if (!instanceResult)
		{
			throw MakeVkBootstrapError("Failed to create Vulkan instance: ", instanceResult);
		}

		m_instance = instanceResult.value();

		if (glfwCreateWindowSurface(m_instance->instance, window.GetHandle(), nullptr, &m_surface) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to create Vulkan surface.");
		}

		vkb::PhysicalDeviceSelector selector{ *m_instance };
		auto physicalDeviceResult = selector.set_surface(m_surface)
			.set_minimum_version(1, 4)
			.add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)
			.select();

		if (!physicalDeviceResult)
		{
			throw std::runtime_error("Failed to select a suitable Vulkan physical device.");
		}

		vkb::DeviceBuilder deviceBuilder{ physicalDeviceResult.value() };
		auto deviceResult = deviceBuilder.build();
		if (!deviceResult)
		{
			throw std::runtime_error("Failed to create Vulkan logical device.");
		}

		m_device = deviceResult.value();
	}

	VulkanContext::~VulkanContext()
	{
		if (m_device.has_value())
		{
			vkb::destroy_device(*m_device);
		}

		if (m_surface != VK_NULL_HANDLE && m_instance.has_value())
		{
			vkb::destroy_surface(*m_instance, m_surface);
		}

		if (m_instance.has_value())
		{
			vkb::destroy_instance(*m_instance);
		}
	}

	const vkb::Instance& VulkanContext::GetInstance() const
	{
		return *m_instance;
	}

	const vkb::Device& VulkanContext::GetDevice() const
	{
		return *m_device;
	}

	VkSurfaceKHR VulkanContext::GetSurface() const
	{
		return m_surface;
	}
}
