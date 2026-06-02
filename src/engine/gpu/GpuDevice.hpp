#pragma once

#include <cstdint>
#include <functional>

#include "gpu/GpuTypes.hpp"

namespace aether { class ServiceContainer; }

namespace aether
{
	class BindlessManager;
	class CommandRecorder;
	class GraphicsDevice;
	class ResourcePool;
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

		GpuDevice() = default;
		~GpuDevice();

		GpuDevice(const GpuDevice&) = delete;
		GpuDevice& operator=(const GpuDevice&) = delete;

		void Init(ServiceContainer& services, const Config& config);
		void Shutdown();

		void WaitIdle();

		[[nodiscard]] bool HasDedicatedComputeQueue() const;

		[[nodiscard]] GpuFormat GetSwapchainColorFormat() const;
		[[nodiscard]] GpuFormat GetSwapchainDepthFormat() const;
		[[nodiscard]] GpuExtent2D GetSwapchainExtent() const;
		[[nodiscard]] bool SwapchainNeedsRecreation() const;
		void ClearSwapchainRecreationFlag();
		[[nodiscard]] bool IsSwapchainFrameValid() const;

		void BeginSwapchainFrame();
		void RecreateSwapchain(class Window& window, bool enableVsync);
		void SubmitAndPresent(std::uint64_t asyncComputeSemaphoreHandle = 0, std::uint64_t asyncComputeTimelineValue = 0);

		[[nodiscard]] CommandRecorder GetCurrentCommandRecorder() const;
		[[nodiscard]] FrameTarget BuildFrameTarget() const;

		void SetSwapchainRecreatedCallback(std::function<void()> cb);

		void AdvanceBindlessFrame(std::uint64_t frameIndex);

		[[nodiscard]] Swapchain& GetSwapchain();
		[[nodiscard]] VulkanContext& GetVulkanContext();
		[[nodiscard]] ResourcePool& GetResourcePool();
		[[nodiscard]] BindlessManager& GetBindlessManager();

		[[nodiscard]] std::uint32_t GetComputeQueueFamily() const;
		[[nodiscard]] std::uint32_t GetGraphicsQueueFamily() const;

		[[nodiscard]] static constexpr GpuFormat GetForwardColorFormat()
		{
			return GpuFormat::R16G16B16A16Sfloat;
		}

	private:
		GraphicsDevice* m_gfx = nullptr;
		std::function<void()> m_swapchainRecreatedCallback;
	};
} // namespace aether
