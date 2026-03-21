
#include "VulkanContext.hpp"

#include <string>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Logger.hpp"
#include "MeowExceptions.hpp"
#include "Window.hpp"

namespace
{
	meow::VulkanError MakeVkBootstrapError(const char* message, const vkb::Result<vkb::Instance>& result)
	{
		return meow::VulkanError(std::string(message) + result.error().message());
	}

	VKAPI_ATTR VkBool32 VKAPI_CALL LogValidationMessage(
		VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
		VkDebugUtilsMessageTypeFlagsEXT messageType,
		const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
		void* userData)
	{
		(void)userData;

		const char* type = vkb::to_string_message_type(messageType);
		const char* message = callbackData != nullptr && callbackData->pMessage != nullptr
			? callbackData->pMessage
			: "Unknown validation layer message.";

		if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
		{
			meow::Logger::ErrorAt(meow::LogCategory::Validation, std::source_location::current(), "{}: {}", type, message);
		}
		else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
		{
			meow::Logger::WarnAt(meow::LogCategory::Validation, std::source_location::current(), "{}: {}", type, message);
		}
		else
		{
			meow::Logger::VerboseAt(meow::LogCategory::Validation, std::source_location::current(), "{}: {}", type, message);
		}

		return VK_FALSE;
	}
}

namespace meow
{
	VulkanContext::VulkanContext(const Window& window, const char* appName)
	{
		INFO(LogCategory::Vulkan, "Creating Vulkan context for '{}'.", appName);

		vkb::InstanceBuilder instanceBuilder;
		auto instanceResult = instanceBuilder.set_app_name(appName)
			.require_api_version(1, 4, 0)
			.request_validation_layers()
			.set_debug_callback(LogValidationMessage)
			.set_debug_messenger_severity(
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
			.set_debug_messenger_type(
				VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
			.build();

		if (!instanceResult)
		{
			throw MakeVkBootstrapError("Failed to create Vulkan instance: ", instanceResult);
		}

		m_instance = instanceResult.value();

		if (glfwCreateWindowSurface(m_instance->instance, window.GetHandle(), nullptr, &m_surface) != VK_SUCCESS)
		{
			throw VulkanError("Failed to create Vulkan surface.");
		}

		VkPhysicalDeviceVulkan12Features requiredFeatures12{};
		requiredFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		requiredFeatures12.bufferDeviceAddress = VK_TRUE;

		VkPhysicalDeviceVulkan13Features requiredFeatures13{};
		requiredFeatures13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		requiredFeatures13.dynamicRendering = VK_TRUE;
		requiredFeatures13.synchronization2 = VK_TRUE;

		vkb::PhysicalDeviceSelector selector{ *m_instance };
		auto physicalDeviceResult = selector.set_surface(m_surface)
			.set_minimum_version(1, 4)
			.set_required_features_12(requiredFeatures12)
			.set_required_features_13(requiredFeatures13)
			.select();

		if (!physicalDeviceResult)
		{
			throw VulkanError("Failed to select a suitable Vulkan physical device.");
		}

		vkb::DeviceBuilder deviceBuilder{ physicalDeviceResult.value() };
		auto deviceResult = deviceBuilder.build();
		if (!deviceResult)
		{
			throw VulkanError("Failed to create Vulkan logical device.");
		}

		m_device = deviceResult.value();

		VmaAllocatorCreateInfo allocatorCreateInfo{};
		allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
		allocatorCreateInfo.physicalDevice = physicalDeviceResult.value().physical_device;
		allocatorCreateInfo.device = m_device->device;
		allocatorCreateInfo.instance = m_instance->instance;
		allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_4;

		const VkResult allocatorResult = vmaCreateAllocator(&allocatorCreateInfo, &m_allocator);
		if (allocatorResult != VK_SUCCESS)
		{
			throw VulkanError(std::format("Failed to create VMA allocator. VkResult={}", static_cast<int>(allocatorResult)));
		}

		INFO(LogCategory::Vulkan, "Vulkan context initialized successfully.");
	}

	VulkanContext::~VulkanContext()
	{
		VERBOSE(LogCategory::Vulkan, "Destroying Vulkan context resources.");

		if (m_allocator != VK_NULL_HANDLE)
		{
			vmaDestroyAllocator(m_allocator);
			m_allocator = VK_NULL_HANDLE;
		}

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

	VmaAllocator VulkanContext::GetAllocator() const
	{
		return m_allocator;
	}
}
