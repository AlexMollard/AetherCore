#include "vulkan/GraphicsDevice.hpp"

#include "utils/Assert.hpp"
#include "utils/ServiceContainer.hpp"
#include "platform/Window.hpp"

namespace aether
{
	namespace
	{
		// Thunk used by VulkanContext::WaitIdle to route device-loss diagnosis
		// to the DiagnosticEngine. Stored as a file-static pointer because
		// VulkanContext's FaultCallback is a plain function pointer (no
		// capture).
		DiagnosticEngine* g_activeDiagnosticEngine = nullptr;

		void DiagnosticFaultThunk()
		{
			if (g_activeDiagnosticEngine != nullptr)
			{
				g_activeDiagnosticEngine->CaptureFaults();
			}
		}
	} // namespace

	void GraphicsDevice::Init(ServiceContainer& services, const Config& config)
	{
		auto& window = services.Get<Window>();

		m_vulkanContext.emplace(window, config.appName);
		m_resourceRegistry.Init(m_vulkanContext->GetDevice().device, m_vulkanContext->GetAllocator());
		m_diagnosticEngine.Init(m_vulkanContext->GetDevice().device, m_vulkanContext->GetPhysicalDevice());
		g_activeDiagnosticEngine = &m_diagnosticEngine;
		m_vulkanContext->SetFaultCallback(&DiagnosticFaultThunk);
		m_swapchain.Initialize(*m_vulkanContext, window, config.enableVsync);
		AE_EXPECT_OR_THROW_VOID(m_bindlessManager.Initialize(*m_vulkanContext, {}));
		m_resourceRegistry.SetBindlessManager(&m_bindlessManager);

		// Wire diagnostic memory tracking to all GPU allocation sites.
		// ResourceRegistry will register every buffer's BDA range; BindlessManager
		// registers both descriptor heaps; GpuHeap instances (MeshArena, etc.)
		// are wired independently when they initialize.
		auto& memTracker = m_diagnosticEngine.GetMemoryTracker();
		m_resourceRegistry.SetMemoryTracker(&memTracker);
		m_bindlessManager.SetMemoryTracker(&memTracker);

		services.Register<DiagnosticEngine>(m_diagnosticEngine);
	}

	void GraphicsDevice::Shutdown()
	{
		vkDeviceWaitIdle(m_vulkanContext->GetDevice().device);
		m_vulkanContext->SetFaultCallback(nullptr);
		g_activeDiagnosticEngine = nullptr;
		m_bindlessManager.Shutdown();
		m_swapchain.Shutdown(m_vulkanContext->GetDevice().device);
		m_resourceRegistry.Shutdown();
		m_diagnosticEngine.Shutdown();
		m_vulkanContext.reset();
	}
} // namespace aether
