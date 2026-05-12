#include "vulkan/GraphicsDevice.hpp"

#include "ServiceContainer.hpp"
#include "platform/Window.hpp"

namespace aether
{
	void GraphicsDevice::Init(ServiceContainer& services, const Config& config)
	{
		Window& window = services.Get<Window>();

		m_vulkanContext.emplace(window, config.appName);
		m_swapchain.Initialize(*m_vulkanContext, window, config.enableVsync);
		m_bindlessManager.Initialize(*m_vulkanContext);
		m_resourcePool.ConfigureBindlessImages({
		        .manager = &m_bindlessManager,
		        .device = m_vulkanContext->GetDevice().device,
		});
	}

	void GraphicsDevice::Shutdown()
	{
		m_resourcePool.Shutdown();
		m_bindlessManager.Shutdown();
		m_swapchain.Shutdown(m_vulkanContext->GetDevice().device);
		m_vulkanContext.reset();
	}
} // namespace aether
