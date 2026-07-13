#pragma once

#include <memory>

#include "gpu/BindlessManager.hpp"
#include "utils/Expected.hpp"
#include "vulkan/DiagnosticEngine.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether
{
	// Owns the core Vulkan resources: context, swapchain, bindless
	// descriptor manager, and the central ResourceRegistry. Requires Window from
	// the service container.
	class GraphicsDevice
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			bool enableVsync = true;
			// See AetherCore::Config::enableGpuDiagnostics - forwarded unchanged
			// to VulkanContext::Create's enableGpuDiagnostics parameter.
			bool enableGpuDiagnostics = false;
		};

		[[nodiscard]] Expected<void> Init(ServiceContainer& services, const Config& config);
		void Shutdown();

		[[nodiscard]] VulkanContext& GetVulkanContext()
		{
			return *m_vulkanContext;
		}

		[[nodiscard]] Swapchain& GetSwapchain()
		{
			return m_swapchain;
		}

		[[nodiscard]] BindlessManager& GetBindlessManager()
		{
			return m_bindlessManager;
		}

		[[nodiscard]] ResourceRegistry& GetResourceRegistry()
		{
			return m_resourceRegistry;
		}

		[[nodiscard]] DiagnosticEngine& GetDiagnosticEngine()
		{
			return m_diagnosticEngine;
		}

	private:
		// Declaration order is destruction-order critical: members are destroyed in
		// REVERSE order, and teardown has hard dependencies that must hold even when
		// the explicit Shutdown() is skipped (e.g. an exception during AetherCore's
		// destructor bypasses GpuDevice::Shutdown, leaving pure member destruction).
		//
		//   - m_vulkanContext owns the VkDevice/VmaAllocator that every other
		//     member's destructor touches -> must die LAST -> declared FIRST.
		//   - m_diagnosticEngine owns the GpuMemoryTracker that m_resourceRegistry
		//     and m_bindlessManager call Unregister() on while draining their GPU
		//     resources -> must outlive them -> declared before them.
		//   - m_resourceRegistry drains its deferred-destroyer ring in ~ResourceRegistry
		//     (Unregister + vkDestroyBuffer) -> must die FIRST -> declared LAST.
		//
		// Getting this wrong is a use-after-free: the tracker's std::map is freed and
		// the ring drain reads it as 0xDDDD... freed memory.
		std::unique_ptr<VulkanContext> m_vulkanContext;
		DiagnosticEngine m_diagnosticEngine;
		Swapchain m_swapchain;
		BindlessManager m_bindlessManager;
		ResourceRegistry m_resourceRegistry;
	};
} // namespace aether
