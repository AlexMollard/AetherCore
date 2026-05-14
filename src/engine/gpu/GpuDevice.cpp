#include "gpu/GpuDevice.hpp"

#include <mutex>

#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "platform/Window.hpp"
#include "rendering/CommandRecorder.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Logger.hpp"
#include "vulkan/GraphicsDevice.hpp"
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
		m_gfx = new GraphicsDevice();
		m_gfx->Init(services, { .appName = config.appName, .enableVsync = config.enableVsync });

		services.Register<VulkanContext>(m_gfx->GetVulkanContext());
		services.Register<Swapchain>(m_gfx->GetSwapchain());
		services.Register<ResourcePool>(m_gfx->GetResourcePool());
		services.Register<BindlessManager>(m_gfx->GetBindlessManager());
	}

	void GpuDevice::Shutdown()
	{
		if (m_gfx)
		{
			m_gfx->Shutdown();
		}
	}

	void GpuDevice::WaitIdle()
	{
		if (vkDeviceWaitIdle(m_gfx->GetVulkanContext().GetDevice().device) != VK_SUCCESS)
		{
			throw VulkanError("GpuDevice: failed to wait for device idle.");
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
		switch (m_gfx->GetSwapchain().GetImageFormat())
		{
			case VK_FORMAT_R8G8B8A8_UNORM:
				return GpuFormat::R8G8B8A8Unorm;
			case VK_FORMAT_B8G8R8A8_SRGB:
				return GpuFormat::B8G8R8A8Srgb;
			case VK_FORMAT_R16G16B16A16_SFLOAT:
				return GpuFormat::R16G16B16A16Sfloat;
			case VK_FORMAT_D32_SFLOAT:
				return GpuFormat::D32Sfloat;
			case VK_FORMAT_D24_UNORM_S8_UINT:
				return GpuFormat::D24UnormS8Uint;
			default:
				return GpuFormat::Undefined;
		}
	}

	GpuFormat GpuDevice::GetSwapchainDepthFormat() const
	{
		switch (m_gfx->GetSwapchain().GetDepthFormat())
		{
			case VK_FORMAT_D32_SFLOAT:
				return GpuFormat::D32Sfloat;
			case VK_FORMAT_D24_UNORM_S8_UINT:
				return GpuFormat::D24UnormS8Uint;
			default:
				return GpuFormat::Undefined;
		}
	}

	GpuExtent2D GpuDevice::GetSwapchainExtent() const
	{
		const VkExtent2D extent = m_gfx->GetSwapchain().GetExtent();
		return { extent.width, extent.height };
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
		VulkanContext& vk = m_gfx->GetVulkanContext();
		Swapchain& swapchain = m_gfx->GetSwapchain();

		if (vkDeviceWaitIdle(vk.GetDevice().device) != VK_SUCCESS)
		{
			throw VulkanError("GpuDevice: failed to wait for device idle during swapchain recreation.");
		}
		swapchain.Shutdown(vk.GetDevice().device);
		swapchain.ClearRecreationFlag();
		swapchain.Initialize(vk, window, enableVsync);

		if (m_swapchainRecreatedCallback)
		{
			m_swapchainRecreatedCallback();
		}

		INFO(LogCategory::Engine, "Swapchain recreated.");
	}

	void GpuDevice::SubmitAndPresent(std::uint64_t asyncComputeSemaphoreHandle, std::uint64_t asyncComputeTimelineValue)
	{
		Swapchain& swapchain = m_gfx->GetSwapchain();
		VulkanContext& vk = m_gfx->GetVulkanContext();

		VkSemaphore computeFinished = asyncComputeSemaphoreHandle ? reinterpret_cast<VkSemaphore>(asyncComputeSemaphoreHandle) : VK_NULL_HANDLE;

		{
			std::lock_guard lock(vk.GetGraphicsQueueMutex());
			swapchain.EndFrame(vk.GetGraphicsQueue(), vk.GetPresentQueue(), computeFinished, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, asyncComputeTimelineValue);
		}
	}

	CommandRecorder GpuDevice::GetCurrentCommandRecorder() const
	{
		return CommandRecorder(m_gfx->GetSwapchain().GetCurrentCommandBuffer());
	}

	FrameTarget GpuDevice::BuildFrameTarget() const
	{
		Swapchain& swapchain = m_gfx->GetSwapchain();
		return FrameTarget{
			.colorImage = swapchain.GetCurrentImage(),
			.colorView = swapchain.GetCurrentImageView(),
			.depthImage = swapchain.GetDepthImage(),
			.depthView = swapchain.GetDepthImageView(),
			.colorFormat = swapchain.GetImageFormat(),
			.depthFormat = swapchain.GetDepthFormat(),
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

	void GpuDevice::AdvanceBindlessFrame(std::uint64_t frameIndex)
	{
		m_gfx->GetBindlessManager().AdvanceFrame(frameIndex);
	}
} // namespace aether
