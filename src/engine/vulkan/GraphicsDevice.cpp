#include "vulkan/GraphicsDevice.hpp"

#include "utils/Profiler.hpp"
#include "gpu/CommandList.hpp"
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "utils/ServiceContainer.hpp"
#include "platform/Window.hpp"

namespace aether
{
	namespace
	{
		DiagnosticEngine* g_activeDiagnosticEngine = nullptr;

		void DiagnosticFaultThunk()
		{
			if (g_activeDiagnosticEngine != nullptr)
			{
				g_activeDiagnosticEngine->CaptureFaults();
			}
		}
	} // namespace

	Expected<void> GraphicsDevice::Init(ServiceContainer& services, const Config& config)
	{
		AE_PROFILE_ZONE();
		auto& window = services.Get<Window>();

		AE_TRY(ctxResult, VulkanContext::Create(window, config.appName, config.enableGpuDiagnostics, config.enableValidation));
		m_vulkanContext = std::move(*ctxResult);
		m_resourceRegistry.Init(m_vulkanContext->GetDevice().device, m_vulkanContext->GetAllocator());
		m_resourceRegistry.SetSharedBufferQueueFamilies({m_vulkanContext->GetGraphicsQueueFamily(), m_vulkanContext->GetComputeQueueFamily(), m_vulkanContext->GetTransferQueueFamily()});
		m_diagnosticEngine.Init(m_vulkanContext->GetDevice().device, m_vulkanContext->GetPhysicalDevice(), m_vulkanContext->GetGraphicsQueue());
		gpu::CommandList::SetDiagnosticEngine(&m_diagnosticEngine);
		g_activeDiagnosticEngine = &m_diagnosticEngine;
		m_vulkanContext->SetFaultCallback(&DiagnosticFaultThunk);
		aether::VulkanContext::SetGlobalAddressBindingTracker(&m_diagnosticEngine.GetMemoryTracker());
		// Before the first Initialize, so the very first present is already tagged.
		if (m_vulkanContext->HasPresentTiming())
		{
			m_presentTiming.Start(m_vulkanContext->GetDevice().device);
			m_swapchain.SetPresentTimingTracker(&m_presentTiming);
		}
		m_swapchain.Initialize(*m_vulkanContext, window, config.presentMode);
		AE_TRY_VOID(m_bindlessManager.Initialize(*m_vulkanContext, {.maxAnisotropy = config.maxAnisotropy}));
		m_resourceRegistry.SetBindlessManager(&m_bindlessManager);

		auto& memTracker = m_diagnosticEngine.GetMemoryTracker();
		m_resourceRegistry.SetMemoryTracker(&memTracker);
		m_bindlessManager.SetMemoryTracker(&memTracker);

		services.Register<DiagnosticEngine>(m_diagnosticEngine);
		return {};
	}

	void GraphicsDevice::Shutdown()
	{
		AE_PROFILE_ZONE();
		vkDeviceWaitIdle(m_vulkanContext->GetDevice().device);
		// Device idle says nothing about the present waiter: it blocks on the presentation
		// engine, not on queue work. Join it here, while its swapchain and device are both
		// still alive, rather than leaving it to member destruction after both are gone.
		m_presentTiming.Stop();
		aether::VulkanContext::SetGlobalAddressBindingTracker(nullptr);
		m_vulkanContext->SetFaultCallback(nullptr);
		gpu::CommandList::SetDiagnosticEngine(nullptr);
		g_activeDiagnosticEngine = nullptr;
		m_bindlessManager.Shutdown();
		m_swapchain.Shutdown(m_vulkanContext->GetDevice().device);
		m_resourceRegistry.Shutdown();
		m_diagnosticEngine.Shutdown();
		m_vulkanContext.reset();
	}
} // namespace aether
