#include "vulkan/GraphicsDevice.hpp"

#include "utils/Assert.hpp"
#include "utils/ServiceContainer.hpp"
#include "platform/Window.hpp"

namespace aether
{
	void GraphicsDevice::Init(ServiceContainer& services, const Config& config)
	{
		auto& window = services.Get<Window>();

		m_vulkanContext.emplace(window, config.appName);
		m_resourceRegistry.Init(m_vulkanContext->GetDevice().device, m_vulkanContext->GetAllocator());
		m_swapchain.Initialize(*m_vulkanContext, window, config.enableVsync);
		AE_EXPECT_OR_THROW_VOID(m_bindlessManager.Initialize(*m_vulkanContext, {}));
		m_resourceRegistry.SetBindlessManager(&m_bindlessManager);
	}

	void GraphicsDevice::Shutdown()
	{
		vkDeviceWaitIdle(m_vulkanContext->GetDevice().device);
		m_bindlessManager.Shutdown();
		m_swapchain.Shutdown(m_vulkanContext->GetDevice().device);
		// ResourceRegistry::Shutdown runs after all other subsystems (DrainAll needs slots empty) and before VulkanContext resets (queue destroyers need VmaAllocator + VkDevice).
		m_resourceRegistry.Shutdown();
		m_vulkanContext.reset();
	}
} // namespace aether
