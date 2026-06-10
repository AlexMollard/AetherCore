#include "vulkan/GraphicsDevice.hpp"

#include "utils/Assert.hpp"
#include "utils/ServiceContainer.hpp"
#include "platform/Window.hpp"

namespace aether
{
	void GraphicsDevice::Init(ServiceContainer& services, const Config& config)
	{
		Window& window = services.Get<Window>();

		m_vulkanContext.emplace(window, config.appName);
		m_swapchain.Initialize(*m_vulkanContext, window, config.enableVsync);
		AE_EXPECT_OR_THROW_VOID(m_bindlessManager.Initialize(*m_vulkanContext, {}));
		m_resourcePool.ConfigureBindlessImages({
		        .manager = &m_bindlessManager,
		        .device = m_vulkanContext->GetDevice().device,
		});
	}

	void GraphicsDevice::Shutdown()
	{
		vkDeviceWaitIdle(m_vulkanContext->GetDevice().device);
		m_resourcePool.Shutdown();
		m_bindlessManager.Shutdown();
		m_swapchain.Shutdown(m_vulkanContext->GetDevice().device);
		// ResourceRegistry::Shutdown must run after every other subsystem
		// has torn down its resources (so DrainAll can find slots empty)
		// and before VulkanContext resets, since the registry's queued
		// destroyers need the VmaAllocator and VkDevice to still be live
		// (Invariant #1: WaitIdle before any GPU resource destruction;
		// Invariant #6: VMA outlives all allocations).
		m_resourceRegistry.Shutdown();
		m_vulkanContext.reset();
	}
} // namespace aether
