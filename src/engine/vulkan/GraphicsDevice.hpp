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
		std::unique_ptr<VulkanContext> m_vulkanContext;
		Swapchain m_swapchain;
		BindlessManager m_bindlessManager;
		ResourceRegistry m_resourceRegistry;
		DiagnosticEngine m_diagnosticEngine;
	};
} // namespace aether
