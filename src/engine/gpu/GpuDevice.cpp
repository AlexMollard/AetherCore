#include "gpu/GpuDevice.hpp"

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "platform/Window.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Logger.hpp"
#include "utils/Expected.hpp"
#include "vulkan/GraphicsDevice.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/TransferManager.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	GpuDevice::GpuDevice() = default;

	GpuDevice::~GpuDevice() = default;

	Expected<void> GpuDevice::Init(ServiceContainer& services, const Config& config)
	{
		AE_PROFILE_ZONE();
		m_gfx = std::make_unique<GraphicsDevice>();
		AE_TRY_VOID(m_gfx->Init(services, {.appName = config.appName, .presentMode = config.presentMode, .enableGpuDiagnostics = config.enableGpuDiagnostics, .enableValidation = config.enableValidation, .maxAnisotropy = config.maxAnisotropy}));

		gpu::ResourceRegistryInitDesc regInit{};
		regInit.vulkanDevice = static_cast<void*>(m_gfx->GetVulkanContext().GetDevice().device);
		regInit.vmaAllocator = static_cast<void*>(m_gfx->GetVulkanContext().GetAllocator());
		regInit.backendRegistry = static_cast<void*>(&m_gfx->GetResourceRegistry());
		gpu::ResourceRegistry::Initialize(regInit);

		services.Register<GpuDevice>(*this);
		services.Register<VulkanContext>(m_gfx->GetVulkanContext());
		services.Register<Swapchain>(m_gfx->GetSwapchain());
		services.Register<BindlessManager>(m_gfx->GetBindlessManager());
		services.Register<ResourceRegistry>(m_gfx->GetResourceRegistry());
		return {};
	}

	void GpuDevice::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_gfx)
		{
			m_gfx->Shutdown();
			m_gfx.reset();
		}
	}

	void GpuDevice::WaitIdle()
	{
		AE_PROFILE_ZONE();
		AE_EXPECT_OR_THROW_VOID(m_gfx->GetVulkanContext().WaitIdle());
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

	gpu::Extent2D GpuDevice::GetSwapchainExtent() const
	{
		const gpu::Extent2D extent = m_gfx->GetSwapchain().GetExtent();
		return {extent.width, extent.height};
	}

	std::uint32_t GpuDevice::GetCurrentSwapchainImageIndex() const
	{
		return m_gfx->GetSwapchain().GetCurrentImageIndex();
	}

	bool GpuDevice::SwapchainNeedsRecreation() const
	{
		return m_gfx->GetSwapchain().NeedsRecreation();
	}

	void GpuDevice::ClearSwapchainRecreationFlag()
	{
		m_gfx->GetSwapchain().ClearRecreationFlag();
	}

	void GpuDevice::RequestSwapchainRecreation()
	{
		m_gfx->GetSwapchain().RequestRecreation();
	}

	bool GpuDevice::IsSwapchainFrameValid() const
	{
		return m_gfx->GetSwapchain().IsFrameValid();
	}

	void GpuDevice::BeginSwapchainFrame()
	{
		m_gfx->GetSwapchain().BeginFrame(m_gfx->GetVulkanContext().GetDevice().device);
	}

	const PresentTimingTracker& GpuDevice::GetPresentTiming() const
	{
		return m_gfx->GetPresentTiming();
	}

	void GpuDevice::RecreateSwapchain(Window& window, gpu::PresentMode presentMode)
	{
		AE_PROFILE_ZONE();
		const VulkanContext& vk = m_gfx->GetVulkanContext();
		Swapchain& swapchain = m_gfx->GetSwapchain();

		AE_EXPECT_OR_THROW_VOID(vk.WaitIdle());
		swapchain.Shutdown(vk.GetDevice().device);
		swapchain.ClearRecreationFlag();
		swapchain.Initialize(vk, window, presentMode);

		if (m_swapchainRecreatedCallback)
		{
			m_swapchainRecreatedCallback();
		}

		AE_INFO(LogCategory::Engine, "Swapchain recreated.");
	}

	void GpuDevice::SetFrameLatchTime(const std::int64_t latchTimeNs)
	{
		m_gfx->GetSwapchain().SetFrameLatchTime(latchTimeNs);
	}

	void GpuDevice::SubmitAndPresent(gpu::TimelineSemaphoreHandle asyncComputeSemaphoreHandle, std::uint64_t asyncComputeTimelineValue)
	{
		AE_PROFILE_ZONE();
		Swapchain& swapchain = m_gfx->GetSwapchain();
		const VulkanContext& vk = m_gfx->GetVulkanContext();

		// Order this frame after every TransferManager upload submitted so far. Skipped
		// entirely until the first upload; waiting an already-signalled timeline value
		// is a no-op, so steady state costs nothing.
		vulkan::TransferManager& transfer = vk.GetTransferManager();
		const std::uint64_t transferTicket = transfer.LastSubmitted();
		void* transferSemaphore = transferTicket > 0 ? static_cast<void*>(transfer.TimelineSemaphore()) : nullptr;

		swapchain.SubmitAndPresent(vk.GetGraphicsQueue(), vk.GetPresentQueue(), asyncComputeSemaphoreHandle, asyncComputeTimelineValue, transferSemaphore, transferTicket);
	}

	FrameTarget GpuDevice::BuildFrameTarget() const
	{
		const Swapchain& swapchain = m_gfx->GetSwapchain();
		return FrameTarget{
		        .colorImage = static_cast<void*>(swapchain.GetCurrentImage()),
		        .colorView = static_cast<void*>(swapchain.GetCurrentImageView()),
		        .depthImage = static_cast<void*>(swapchain.GetDepthImage()),
		        .depthView = static_cast<void*>(swapchain.GetDepthImageView()),
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

	gpu::Device GpuDevice::GetDevice() const noexcept
	{
		return static_cast<gpu::Device>(m_gfx->GetVulkanContext().GetDevice().device);
	}

	gpu::Queue GpuDevice::GetGraphicsQueue() const noexcept
	{
		return static_cast<gpu::Queue>(m_gfx->GetVulkanContext().GetGraphicsQueue());
	}

	gpu::Queue GpuDevice::GetComputeQueue() const noexcept
	{
		return static_cast<gpu::Queue>(m_gfx->GetVulkanContext().GetComputeQueue());
	}

	gpu::PipelineCache GpuDevice::GetPipelineCache() const noexcept
	{
		return static_cast<gpu::PipelineCache>(m_gfx->GetVulkanContext().GetPipelineCache());
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

	FrameConstants GpuDevice::ComposeBaseFrameConstants(const RenderFramePacket& packet, const glm::mat4& fallbackViewProj)
	{
		FrameConstants fc{};

		if (packet.hasCameraData)
		{
			fc.view = packet.view;
			fc.proj = packet.proj;
			fc.viewProj = packet.proj * packet.view;
			fc.cameraWorldPos = packet.cameraWorldPos;
		}
		else
		{
			fc.viewProj = fallbackViewProj;
		}

		fc.materialBufferAddr = packet.materialBufferAddr;
		fc.effectParamBufferAddr = packet.effectParamBufferAddr;
		fc.elapsedTime = packet.elapsedTime;
		fc.sunDirectionIntensity = packet.sunDirectionIntensity;
		fc.ambientColor = packet.ambientColor;
		fc.sunColor = packet.sunColor;
		fc.skyHorizonColor = packet.skyHorizonColor;
		fc.skyZenithColor = packet.skyZenithColor;
		fc.skyVoidColor = packet.skyVoidColor;
		fc.fogParams = packet.fogParams;
		fc.skyParams = packet.skyParams;
		fc.shadingParams = packet.shadingParams;
		fc.objectAmbient = packet.objectAmbient;
		fc.objectLight0Direction = packet.objectLight0Direction;
		fc.objectLight0Color = packet.objectLight0Color;
		fc.objectLight1Direction = packet.objectLight1Direction;
		fc.objectLight1Color = packet.objectLight1Color;

		// A first frame has nothing behind it, and a camera that jumped has nothing
		// meaningful behind it either - reprojecting through either produces a screen-wide
		// smear for one frame. Both are handled by starting from this frame's own matrix:
		// the very first frame because m_hasPrevViewProj is false, a jump because the pass
		// clamps how far it will ever blur.
		fc.prevViewProj = m_hasPrevViewProj ? m_prevViewProj : fc.viewProj;
		m_prevViewProj = fc.viewProj;
		m_hasPrevViewProj = true;
		return fc;
	}

	void GpuDevice::ApplyNoCameraLightingFallback(FrameConstants& fc)
	{
		fc.tiledLightGridInfo = glm::uvec4(0u);
		fc.tiledLightBufferOffsets = glm::uvec4(0u);
	}
} // namespace aether
