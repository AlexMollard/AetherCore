#pragma once

#include <mutex>
#include <optional>
#include "utils/Assert.hpp"
#include "utils/GpuProfiler.hpp"
#include "vulkan/volk.hpp"
#include <vk_mem_alloc.h>
#include <VkBootstrap.h>

namespace aether
{
	class Window;

	class VulkanContext
	{
	public:
		VulkanContext(const Window& window, const char* appName);
		~VulkanContext();

		VulkanContext(const VulkanContext&) = AE_DELETE_MSG("VulkanContext owns VkDevice and VmaAllocator - use reference");
		VulkanContext& operator=(const VulkanContext&) = AE_DELETE_MSG("VulkanContext owns VkDevice and VmaAllocator - use reference");

		[[nodiscard]] const vkb::Instance& GetInstance() const;
		[[nodiscard]] const vkb::Device& GetDevice() const;
		[[nodiscard]] VkPhysicalDevice GetPhysicalDevice() const;

		[[nodiscard]] TracyVkCtx GetTracyVkCtx() const
		{
			return m_tracyVkCtx;
		}

		[[nodiscard]] VkSurfaceKHR GetSurface() const;
		[[nodiscard]] VmaAllocator GetAllocator() const;
		[[nodiscard]] VkQueue GetGraphicsQueue() const;
		[[nodiscard]] VkQueue GetComputeQueue() const;
		[[nodiscard]] VkQueue GetPresentQueue() const;
		[[nodiscard]] std::uint32_t GetGraphicsQueueFamily() const;
		[[nodiscard]] std::uint32_t GetComputeQueueFamily() const;

		[[nodiscard]] std::mutex& GetGraphicsQueueMutex() const
		{
			return m_graphicsQueueMutex;
		}

	private:
		std::optional<vkb::Instance> m_instance;
		std::optional<vkb::Device> m_device;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkSurfaceKHR m_surface = VK_NULL_HANDLE;
		VkQueue m_graphicsQueue = VK_NULL_HANDLE;
		VkQueue m_computeQueue = VK_NULL_HANDLE;
		VkQueue m_presentQueue = VK_NULL_HANDLE;
		std::uint32_t m_graphicsQueueFamily = 0;
		std::uint32_t m_computeQueueFamily = 0;
		mutable std::mutex m_graphicsQueueMutex;
		TracyVkCtx m_tracyVkCtx = nullptr;
	};
} // namespace aether
