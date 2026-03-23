#include "MeowCore.hpp"

#include "FileSystem.hpp"
#include "FrameConstants.hpp"
#include "Logger.hpp"

namespace meow
{
	MeowCore::MeowCore(const Config& config)
		: m_window(config.appName, config.width, config.height),
		m_vulkanContext(m_window, config.appName)
	{
		m_swapchain.Initialize(m_vulkanContext, m_window);
		m_bindlessManager.Initialize(m_vulkanContext);
		m_frameConstantsBuffer.Initialize(m_vulkanContext);
		m_resourcePool.ConfigureBindlessImages({
			.manager = &m_bindlessManager,
			.device = m_vulkanContext.GetDevice().device,
			});
		m_primitiveMeshes.Initialize(
			m_vulkanContext.GetDevice().device,
			m_vulkanContext.GetAllocator());
		io::FileSystem::InitializeDefaultMounts();

		INFO(
			LogCategory::Engine,
			"Engine core initialized. Bindless sampled-image capacity: {}",
			m_bindlessManager.GetCapacity());
	}

	void MeowCore::WaitIdle() const
	{
		vkDeviceWaitIdle(m_vulkanContext.GetDevice().device);
	}

	MeowCore::~MeowCore()
	{
		vkDeviceWaitIdle(m_vulkanContext.GetDevice().device);
		m_frameConstantsBuffer.Shutdown();
		m_swapchain.Shutdown(m_vulkanContext.GetDevice().device);
		m_bindlessManager.Shutdown();
		io::FileSystem::Shutdown();
	}

	bool MeowCore::ShouldClose() const
	{
		return m_window.ShouldClose();
	}

	void MeowCore::PumpEvents() const
	{
		m_window.PollEvents();
	}

	void MeowCore::BeginFrame()
	{
		m_swapchain.BeginFrame(m_vulkanContext.GetDevice().device);
		m_currentRecorder = CommandRecorder(m_swapchain.GetCurrentCommandBuffer());
	}

	void MeowCore::EndFrame()
	{
		if (m_swapchain.IsFrameValid())
		{
			const auto frameIdx = static_cast<std::uint32_t>(
				m_frameIndex % Swapchain::kMaxFramesInFlight);

			// Write per-frame camera data into the GPU buffer then get its BDA.
			// The address is pushed alongside the model matrix as a raw pointer —
			// no descriptor set binding required.
			FrameConstants fc;
			fc.viewProj = m_scene.GetViewProjection();
			m_frameConstantsBuffer.Write(frameIdx, fc);
			const VkDeviceAddress frameAddr = m_frameConstantsBuffer.GetDeviceAddress(frameIdx);

			m_scene.FlushToQueue(m_renderQueue);
			m_renderQueue.Flush(m_currentRecorder, frameAddr);
			m_renderQueue.Clear();
		}
		m_swapchain.EndFrame(m_vulkanContext.GetGraphicsQueue(), m_vulkanContext.GetPresentQueue());
		++m_frameIndex;
		m_bindlessManager.AdvanceFrame(m_frameIndex);
	}

	Window& MeowCore::GetWindow()
	{
		return m_window;
	}

	const Window& MeowCore::GetWindow() const
	{
		return m_window;
	}

	VulkanContext& MeowCore::GetVulkanContext()
	{
		return m_vulkanContext;
	}

	const VulkanContext& MeowCore::GetVulkanContext() const
	{
		return m_vulkanContext;
	}

	BindlessManager& MeowCore::GetBindlessManager()
	{
		return m_bindlessManager;
	}

	const BindlessManager& MeowCore::GetBindlessManager() const
	{
		return m_bindlessManager;
	}

	ResourcePool& MeowCore::GetResourcePool()
	{
		return m_resourcePool;
	}

	const ResourcePool& MeowCore::GetResourcePool() const
	{
		return m_resourcePool;
	}

	VkCommandBuffer MeowCore::GetCurrentCommandBuffer() const
	{
		return m_swapchain.GetCurrentCommandBuffer();
	}

	VkFormat MeowCore::GetSwapchainImageFormat() const
	{
		return m_swapchain.GetImageFormat();
	}

	VkFormat MeowCore::GetSwapchainDepthFormat() const
	{
		return m_swapchain.GetDepthFormat();
	}

	VkExtent2D MeowCore::GetSwapchainExtent() const
	{
		return m_swapchain.GetExtent();
	}

	RenderQueue* MeowCore::GetRenderQueue()
	{
		return &m_renderQueue;
	}

	Scene* MeowCore::GetScene()
	{
		return &m_scene;
	}

	const Mesh& MeowCore::GetPrimitiveMesh(PrimitiveMesh primitive) const
	{
		return m_primitiveMeshes.Get(primitive);
	}

	GraphicsPipeline MeowCore::CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc)
	{
		return GraphicsPipeline::Create(m_vulkanContext.GetDevice().device, desc);
	}

	Mesh MeowCore::CreateMesh(std::span<const Mesh::Vertex> vertices)
	{
		return Mesh::Create(
			m_vulkanContext.GetDevice().device,
			m_vulkanContext.GetAllocator(),
			vertices);
	}
}