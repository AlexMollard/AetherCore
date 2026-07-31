#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "gpu/CommandList.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/Semaphore.hpp"
#include "rendering/FrameConstants.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether
{
	struct RenderFramePacket;
	class BindlessManager;
	class GraphicsDevice;
	class ResourceRegistry;
	class Swapchain;
	class VulkanContext;
	struct FrameTarget;

	class GpuDevice
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			gpu::PresentMode presentMode = gpu::PresentMode::Fifo;
			bool enableGpuDiagnostics = false;
			bool enableValidation = true;
		};

		GpuDevice();
		~GpuDevice();

		GpuDevice(const GpuDevice&) = delete;
		GpuDevice& operator=(const GpuDevice&) = delete;

		[[nodiscard]] Expected<void> Init(ServiceContainer& services, const Config& config);
		void Shutdown();

		void WaitIdle();

		[[nodiscard]] bool HasDedicatedComputeQueue() const;

		[[nodiscard]] GpuFormat GetSwapchainColorFormat() const;
		[[nodiscard]] GpuFormat GetSwapchainDepthFormat() const;
		[[nodiscard]] gpu::Extent2D GetSwapchainExtent() const;
		[[nodiscard]] std::uint32_t GetCurrentSwapchainImageIndex() const;
		[[nodiscard]] bool SwapchainNeedsRecreation() const;
		void ClearSwapchainRecreationFlag();
		// Forces a swapchain recreate on the next producer-thread poll (VSync change).
		void RequestSwapchainRecreation();
		[[nodiscard]] bool IsSwapchainFrameValid() const;

		void BeginSwapchainFrame();
		void RecreateSwapchain(class Window& window, gpu::PresentMode presentMode);
		// implementation (`vulkan/Swapchain.cpp`); this TU never sees a
		void SubmitAndPresent(gpu::TimelineSemaphoreHandle asyncComputeSemaphoreHandle = nullptr, std::uint64_t asyncComputeTimelineValue = 0);

		[[nodiscard]] FrameTarget BuildFrameTarget() const;

		void SetSwapchainRecreatedCallback(std::function<void()> cb);

		void AdvanceBindlessFrame(std::uint64_t frameIndex);

		// frame. Call once per frame from the engine thread at the same point
		void AdvanceResourceRegistryFrame();

		[[nodiscard]] Swapchain& GetSwapchain();
		[[nodiscard]] VulkanContext& GetVulkanContext();
		[[nodiscard]] BindlessManager& GetBindlessManager();
		[[nodiscard]] ResourceRegistry& GetResourceRegistry();

		[[nodiscard]] std::uint32_t GetComputeQueueFamily() const;
		[[nodiscard]] std::uint32_t GetGraphicsQueueFamily() const;

		[[nodiscard]] gpu::Device GetDevice() const noexcept;
		[[nodiscard]] gpu::Queue GetGraphicsQueue() const noexcept;
		[[nodiscard]] gpu::Queue GetComputeQueue() const noexcept;
		[[nodiscard]] gpu::PipelineCache GetPipelineCache() const noexcept;

		[[nodiscard]] static constexpr GpuFormat GetForwardColorFormat()
		{
			return GpuFormat::R16G16B16A16Sfloat;
		}

		[[nodiscard]] static FrameConstants ComposeBaseFrameConstants(const RenderFramePacket& packet, const glm::mat4& fallbackViewProj);

		static void ApplyNoCameraLightingFallback(FrameConstants& fc);

	private:
		std::unique_ptr<GraphicsDevice> m_gfx;
		std::function<void()> m_swapchainRecreatedCallback;
	};
} // namespace aether
