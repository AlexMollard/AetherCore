#pragma once

#include <memory>
#include <optional>
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "gpu/GpuProfiler.hpp"
#include "vulkan/volk.hpp"
#include <vk_mem_alloc.h>
#include <VkBootstrap.h>

// Forward declaration of Tracy's context type so the raw pointer
// can be stored as a member without dragging TracyVulkan.hpp into
// every TU that includes this header. The full type is needed only
// in VulkanContext.cpp.
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

		// Factory: create and initialize a VulkanContext. Returns an error
		// on failure instead of throwing, so callers can propagate via
		// Expected / AE_TRY.
		[[nodiscard]] static Expected<std::unique_ptr<VulkanContext>> Create(const Window& window, const char* appName);

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

		// Descriptor-heap properties (VK_EXT_descriptor_heap). Queried once
		// during init; used by BindlessManager to size/align the resource and
		// sampler heap backing buffers and to compute per-descriptor strides.
		[[nodiscard]] const VkPhysicalDeviceDescriptorHeapPropertiesEXT& GetDescriptorHeapProperties() const;

		// Block until the device finishes all in-flight work. Returns an
		// error on backend failure. If the device is lost, queries and logs
		// fault info via VK_EXT_device_fault before returning the error.
		[[nodiscard]] Expected<void> WaitIdle() const;

		// Query and log device fault information via VK_KHR_device_fault.
		// Called automatically on device loss; can also be called manually
		// after observing VK_ERROR_DEVICE_LOST from any Vulkan call.
		void QueryDeviceFaultInfo() const;

		// Set an external diagnostic callback invoked on device loss. When
		// set, WaitIdle() calls this instead of the built-in QueryDeviceFaultInfo
		// so the full DiagnosticEngine (with address resolution + flight
		// recorder) can run. Pass nullptr to revert to the built-in query.
		using FaultCallback = void (*)();

		void SetFaultCallback(FaultCallback callback)
		{
			m_faultCallback = callback;
		}

		// Forward to the file-static address binding tracker so GraphicsDevice
		// can wire up the debug-messenger-driven alloc tracking.
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
		VulkanContext(const Window& window, const char* appName);

		std::optional<vkb::Instance> m_instance;
		VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
		std::optional<vkb::Device> m_device;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
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
