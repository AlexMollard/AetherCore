#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "gpu/CommandList.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/Semaphore.hpp"
#include "rendering/FrameConstants.hpp"

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
			bool enableVsync = true;
		};

		GpuDevice();
		~GpuDevice();

		GpuDevice(const GpuDevice&) = delete;
		GpuDevice& operator=(const GpuDevice&) = delete;

		void Init(ServiceContainer& services, const Config& config);
		void Shutdown();

		void WaitIdle();

		[[nodiscard]] bool HasDedicatedComputeQueue() const;

		[[nodiscard]] GpuFormat GetSwapchainColorFormat() const;
		[[nodiscard]] GpuFormat GetSwapchainDepthFormat() const;
		[[nodiscard]] gpu::Extent2D GetSwapchainExtent() const;
		[[nodiscard]] bool SwapchainNeedsRecreation() const;
		void ClearSwapchainRecreationFlag();
		[[nodiscard]] bool IsSwapchainFrameValid() const;

		void BeginSwapchainFrame();
		void RecreateSwapchain(class Window& window, bool enableVsync);
		// End-of-frame submission. The 3 timeline-semaphore handles are the
		// opaque engine-side `gpu::TimelineSemaphoreHandle` typedef (a pImpl
		// pointer). All casts to `VkSemaphore` happen in the swapchain
		// implementation (`vulkan/Swapchain.cpp`); this TU never sees a
		// `Vk*` token.
		void SubmitAndPresent(gpu::TimelineSemaphoreHandle asyncComputeSemaphoreHandle = nullptr, std::uint64_t asyncComputeTimelineValue = 0, gpu::TimelineSemaphoreHandle rootMotionSignalSemaphore = nullptr, std::uint64_t rootMotionSignalValue = 0);

		[[nodiscard]] FrameTarget BuildFrameTarget() const;

		void SetSwapchainRecreatedCallback(std::function<void()> cb);

		void AdvanceBindlessFrame(std::uint64_t frameIndex);

		// Tick the ResourceRegistry deferred-destruction ring forward by one
		// frame. Call once per frame from the engine thread at the same point
		// as AdvanceBindlessFrame; the registry destroys resources that were
		// scheduled for teardown kMaxFramesInFlight frames earlier, by which
		// point the GPU is guaranteed done with them.
		void AdvanceResourceRegistryFrame();

		[[nodiscard]] Swapchain& GetSwapchain();
		[[nodiscard]] VulkanContext& GetVulkanContext();
		[[nodiscard]] BindlessManager& GetBindlessManager();
		[[nodiscard]] ResourceRegistry& GetResourceRegistry();

		[[nodiscard]] std::uint32_t GetComputeQueueFamily() const;
		[[nodiscard]] std::uint32_t GetGraphicsQueueFamily() const;

		// Engine-side accessors that return opaque `gpu::*` types. These
		// are the preferred way for engine code to obtain device, queue,
		// and allocator handles; reaching through GetVulkanContext()
		// pulls the raw Vk* types into the caller and forces a
		// `static_cast<gpu::*>` back to opaque. Use these accessors
		// instead.
		[[nodiscard]] gpu::Device GetDevice() const noexcept;
		[[nodiscard]] gpu::Queue GetGraphicsQueue() const noexcept;
		[[nodiscard]] gpu::Queue GetComputeQueue() const noexcept;
		[[nodiscard]] gpu::PipelineCache GetPipelineCache() const noexcept;

		[[nodiscard]] static constexpr GpuFormat GetForwardColorFormat()
		{
			return GpuFormat::R16G16B16A16Sfloat;
		}

		[[nodiscard]] FrameConstants ComposeBaseFrameConstants(const RenderFramePacket& packet, const glm::mat4& fallbackViewProj);

		static void ApplyNoCameraLightingFallback(FrameConstants& fc);

	private:
		std::unique_ptr<GraphicsDevice> m_gfx;
		std::function<void()> m_swapchainRecreatedCallback;
	};
} // namespace aether
