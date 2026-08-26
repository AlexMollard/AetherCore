#pragma once

#include <memory>

#include "gpu/BindlessManager.hpp"
#include "utils/Expected.hpp"
#include "vulkan/DiagnosticEngine.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "vulkan/PresentTimingTracker.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether
{
	class GraphicsDevice
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			gpu::PresentMode presentMode = gpu::PresentMode::Fifo;
			bool enableGpuDiagnostics = false;
			bool enableValidation = true;
		std::uint32_t maxAnisotropy = 16;
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

		// Null-safe to read even where present timing is unsupported; HasEstimate() stays
		// false and callers fall back to not pacing.
		[[nodiscard]] const PresentTimingTracker& GetPresentTiming() const
		{
			return m_presentTiming;
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
		// REVERSE order, and teardown has hard dependencies that must hold even when
		std::unique_ptr<VulkanContext> m_vulkanContext;
		DiagnosticEngine m_diagnosticEngine;
		Swapchain m_swapchain;
		PresentTimingTracker m_presentTiming;
		BindlessManager m_bindlessManager;
		ResourceRegistry m_resourceRegistry;
	};
} // namespace aether
