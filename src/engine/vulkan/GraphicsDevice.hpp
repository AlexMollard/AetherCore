#pragma once

#include <optional>

#include "gpu/BindlessManager.hpp"
#include "vulkan/ResourcePool.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

class ServiceContainer;

namespace aether
{
	// Owns the core Vulkan resources: context, swapchain, resource pool, and
	// bindless descriptor manager. Requires Window from the service container.
	class GraphicsDevice
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			bool enableVsync = true;
		};

		void Init(ServiceContainer& services, const Config& config = {});
		void Shutdown();

		[[nodiscard]] VulkanContext& GetVulkanContext()
		{
			return *m_vulkanContext;
		}

		[[nodiscard]] Swapchain& GetSwapchain()
		{
			return m_swapchain;
		}

		[[nodiscard]] ResourcePool& GetResourcePool()
		{
			return m_resourcePool;
		}

		[[nodiscard]] BindlessManager& GetBindlessManager()
		{
			return m_bindlessManager;
		}

	private:
		std::optional<VulkanContext> m_vulkanContext;
		Swapchain m_swapchain;
		ResourcePool m_resourcePool;
		BindlessManager m_bindlessManager;
	};
} // namespace aether
