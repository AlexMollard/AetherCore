#include "gpu/GpuDevice.hpp"

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "platform/Window.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Logger.hpp"
#include "vulkan/GraphicsDevice.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	GpuDevice::~GpuDevice()
	{
		delete m_gfx;
		m_gfx = nullptr;
	}

	void GpuDevice::Init(ServiceContainer& services, const Config& config)
	{
		AE_PROFILE_ZONE();
		m_gfx = new GraphicsDevice();
		m_gfx->Init(services, {.appName = config.appName, .enableVsync = config.enableVsync});

		gpu::ResourceRegistryInitDesc regInit{};
		regInit.vulkanDevice = static_cast<void*>(m_gfx->GetVulkanContext().GetDevice().device);
		regInit.vmaAllocator = static_cast<void*>(m_gfx->GetVulkanContext().GetAllocator());
		regInit.backendRegistry = static_cast<void*>(&m_gfx->GetResourceRegistry());
		gpu::ResourceRegistry::Initialize(regInit);

		services.Register<GpuDevice>(*this);
		services.Register<VulkanContext>(m_gfx->GetVulkanContext());
		services.Register<Swapchain>(m_gfx->GetSwapchain());
		services.Register<ResourcePool>(m_gfx->GetResourcePool());
		services.Register<BindlessManager>(m_gfx->GetBindlessManager());
		services.Register<ResourceRegistry>(m_gfx->GetResourceRegistry());

	}

	void GpuDevice::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_gfx)
		{
			m_gfx->Shutdown();
			delete m_gfx;
			m_gfx = nullptr;
		}
	}

	void GpuDevice::WaitIdle()
	{
		AE_PROFILE_ZONE();
		if (vkDeviceWaitIdle(m_gfx->GetVulkanContext().GetDevice().device) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "GpuDevice: failed to wait for device idle."));
		}
	}

	bool GpuDevice::HasDedicatedComputeQueue() const
	{
		return m_gfx->GetVulkanContext().GetComputeQueue() != VK_NULL_HANDLE;
	}

	std::uint32_t GpuDevice::GetComputeQueueFamily() const
	{
		return m_gfx->GetVulkanContext().GetComputeQueueFamily();
	}

	std::uint32_t GpuDevice::GetGraphicsQueueFamily() const
	{
		return m_gfx->GetVulkanContext().GetGraphicsQueueFamily();
	}

	GpuFormat GpuDevice::GetSwapchainColorFormat() const
	{
		return m_gfx->GetSwapchain().GetImageFormat();
	}

	GpuFormat GpuDevice::GetSwapchainDepthFormat() const
	{
		return m_gfx->GetSwapchain().GetDepthFormat();
	}

	GpuExtent2D GpuDevice::GetSwapchainExtent() const
	{
		const gpu::Extent2D extent = m_gfx->GetSwapchain().GetExtent();
		return {extent.width, extent.height};
	}

	bool GpuDevice::SwapchainNeedsRecreation() const
	{
		return m_gfx->GetSwapchain().NeedsRecreation();
	}

	void GpuDevice::ClearSwapchainRecreationFlag()
	{
		m_gfx->GetSwapchain().ClearRecreationFlag();
	}

	bool GpuDevice::IsSwapchainFrameValid() const
	{
		return m_gfx->GetSwapchain().IsFrameValid();
	}

	void GpuDevice::BeginSwapchainFrame()
	{
		m_gfx->GetSwapchain().BeginFrame(m_gfx->GetVulkanContext().GetDevice().device);
	}

	void GpuDevice::RecreateSwapchain(Window& window, bool enableVsync)
	{
		AE_PROFILE_ZONE();
		VulkanContext& vk = m_gfx->GetVulkanContext();
		Swapchain& swapchain = m_gfx->GetSwapchain();

		if (vkDeviceWaitIdle(vk.GetDevice().device) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "GpuDevice: failed to wait for device idle during swapchain recreation."));
		}
		swapchain.Shutdown(vk.GetDevice().device);
		swapchain.ClearRecreationFlag();
		swapchain.Initialize(vk, window, enableVsync);

		if (m_swapchainRecreatedCallback)
		{
			m_swapchainRecreatedCallback();
		}

		AE_INFO(LogCategory::Engine, "Swapchain recreated.");
	}

	void GpuDevice::SubmitAndPresent(std::uint64_t asyncComputeSemaphoreHandle, std::uint64_t asyncComputeTimelineValue, std::uint64_t rootMotionSignalSemaphore, std::uint64_t rootMotionSignalValue)
	{
		AE_PROFILE_ZONE();
		Swapchain& swapchain = m_gfx->GetSwapchain();
		VulkanContext& vk = m_gfx->GetVulkanContext();

		VkSemaphore computeFinished = asyncComputeSemaphoreHandle ? reinterpret_cast<VkSemaphore>(asyncComputeSemaphoreHandle) : VK_NULL_HANDLE;
		VkSemaphore rmSignal = rootMotionSignalSemaphore ? reinterpret_cast<VkSemaphore>(rootMotionSignalSemaphore) : VK_NULL_HANDLE;

		swapchain.EndFrame(vk.GetGraphicsQueue(), vk.GetPresentQueue(), computeFinished, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, asyncComputeTimelineValue, rmSignal, rootMotionSignalValue);
	}

	gpu::CommandList GpuDevice::GetCurrentCommandList() const
	{
		return gpu::CommandList(m_gfx->GetSwapchain().GetCurrentCommandBuffer());
	}

	FrameTarget GpuDevice::BuildFrameTarget() const
	{
		Swapchain& swapchain = m_gfx->GetSwapchain();
		return FrameTarget{
		        .colorImage = swapchain.GetCurrentImage(),
		        .colorView = swapchain.GetCurrentImageView(),
		        .depthImage = swapchain.GetDepthImage(),
		        .depthView = swapchain.GetDepthImageView(),
		        .colorFormat = gpu::ToVk(swapchain.GetImageFormat()),
		        .depthFormat = gpu::ToVk(swapchain.GetDepthFormat()),
		        .extent = swapchain.GetExtent(),
		};
	}

	void GpuDevice::SetSwapchainRecreatedCallback(std::function<void()> cb)
	{
		m_swapchainRecreatedCallback = std::move(cb);
	}

	Swapchain& GpuDevice::GetSwapchain()
	{
		return m_gfx->GetSwapchain();
	}

	VulkanContext& GpuDevice::GetVulkanContext()
	{
		return m_gfx->GetVulkanContext();
	}

	ResourcePool& GpuDevice::GetResourcePool()
	{
		return m_gfx->GetResourcePool();
	}

	BindlessManager& GpuDevice::GetBindlessManager()
	{
		return m_gfx->GetBindlessManager();
	}

	ResourceRegistry& GpuDevice::GetResourceRegistry()
	{
		return m_gfx->GetResourceRegistry();
	}

	void GpuDevice::AdvanceBindlessFrame(std::uint64_t frameIndex)
	{
		m_gfx->GetBindlessManager().AdvanceFrame(frameIndex);
	}

	void GpuDevice::AdvanceResourceRegistryFrame()
	{
		m_gfx->GetResourceRegistry().AdvanceFrame();
	}
} // namespace aether
