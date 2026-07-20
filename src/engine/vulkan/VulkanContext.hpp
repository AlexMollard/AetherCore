#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "gpu/GpuProfiler.hpp"
#include "vulkan/volk.hpp"
#include <vk_mem_alloc.h>
#include <VkBootstrap.h>

namespace tracy
{
	struct VkCtx;
}

namespace aether
{
	class GpuMemoryTracker;
}

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
#	include "vulkan/AftermathContext.hpp"
#endif

namespace aether
{
	class Window;

	class VulkanContext
	{
	public:
		~VulkanContext();

		// device selection so a non-NVIDIA device never has its NVIDIA-only
		[[nodiscard]] static Expected<std::unique_ptr<VulkanContext>> Create(const Window& window, const char* appName, bool enableGpuDiagnostics = false, bool enableValidation = true);

		VulkanContext(const VulkanContext&) = AE_DELETE_MSG("VulkanContext owns VkDevice and VmaAllocator - use reference");
		VulkanContext& operator=(const VulkanContext&) = AE_DELETE_MSG("VulkanContext owns VkDevice and VmaAllocator - use reference");
		VulkanContext(VulkanContext&&) = delete;
		VulkanContext& operator=(VulkanContext&&) = delete;

		[[nodiscard]] const vkb::Instance& GetInstance() const;
		[[nodiscard]] const vkb::Device& GetDevice() const;
		[[nodiscard]] VkPhysicalDevice GetPhysicalDevice() const;

		[[nodiscard]] VkSurfaceKHR GetSurface() const;
		[[nodiscard]] VmaAllocator GetAllocator() const;
		[[nodiscard]] VkPipelineCache GetPipelineCache() const;
		[[nodiscard]] VkQueue GetGraphicsQueue() const;
		[[nodiscard]] VkQueue GetComputeQueue() const;
		[[nodiscard]] VkQueue GetPresentQueue() const;
		[[nodiscard]] std::uint32_t GetGraphicsQueueFamily() const;
		[[nodiscard]] std::uint32_t GetComputeQueueFamily() const;

		[[nodiscard]] const VkPhysicalDeviceDescriptorHeapPropertiesEXT& GetDescriptorHeapProperties() const;

		[[nodiscard]] Expected<void> WaitIdle() const;

		void QueryDeviceFaultInfo() const;

		using FaultCallback = void (*)();

		void SetFaultCallback(FaultCallback callback)
		{
			m_faultCallback = callback;
		}

		static void SetGlobalAddressBindingTracker(GpuMemoryTracker* tracker);

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
		VulkanContext(const Window& window, const char* appName, bool enableGpuDiagnostics, bool enableValidation);

		std::optional<vkb::Instance> m_instance;
		VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
		std::optional<vkb::Device> m_device;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
		// (never CWD/exe-dir-relative -- see ResolveGpuCacheDir in VulkanContext.cpp).
		std::filesystem::path m_pipelineCachePath;
		VkSurfaceKHR m_surface = VK_NULL_HANDLE;
		VkQueue m_graphicsQueue = VK_NULL_HANDLE;
		VkQueue m_computeQueue = VK_NULL_HANDLE;
		VkQueue m_presentQueue = VK_NULL_HANDLE;
		std::uint32_t m_graphicsQueueFamily = 0;
		std::uint32_t m_computeQueueFamily = 0;
		VkPhysicalDeviceDescriptorHeapPropertiesEXT m_descriptorHeapProps{};
		tracy::VkCtx* m_tracyVkCtx = nullptr;
		gpu::ProfilerContextHandle m_tracyProfilerHandle = nullptr;
		FaultCallback m_faultCallback = nullptr;

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		AftermathContext m_aftermathContext;
#endif
	};
} // namespace aether
