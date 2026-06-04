#pragma once

#include <optional>
#include "utils/Assert.hpp"
#include "utils/GpuProfiler.hpp"
#include "vulkan/volk.hpp"
#include <vk_mem_alloc.h>
#include <VkBootstrap.h>

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
#	include "vulkan/AftermathContext.hpp"
#endif

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

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		[[nodiscard]] const AftermathContext& GetAftermathContext() const
		{
			return m_aftermathContext;
		}

		[[nodiscard]] AftermathContext& GetAftermathContext()
		{
			return m_aftermathContext;
		}
#endif

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
		TracyVkCtx m_tracyVkCtx = nullptr;

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		AftermathContext m_aftermathContext;
#endif
	};
} // namespace aether
