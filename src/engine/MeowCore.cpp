#include "MeowCore.hpp"

#include <stdexcept>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

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

		m_postProcessStack = PostProcessStack::Create({
			.device = m_vulkanContext.GetDevice().device,
			.allocator = m_vulkanContext.GetAllocator(),
			.extent = m_swapchain.GetExtent(),
			.swapchainFormat = m_swapchain.GetImageFormat(),
			.bindlessManager = &m_bindlessManager,
			.renderGraph = &m_renderGraph,
			});
		m_postProcessStack.SetFxaaEnabled(false);
		RegisterPasses();

		// Upload pool — used for immediate-submit texture uploads.
		// TRANSIENT: hints that command buffers are short-lived.
		// RESET_COMMAND_BUFFER: allows individual buffer reset/reuse.
		const VkCommandPoolCreateInfo uploadPoolInfo{
			.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
			                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = m_vulkanContext.GetGraphicsQueueFamily(),
		};
		if (vkCreateCommandPool(
			m_vulkanContext.GetDevice().device,
			&uploadPoolInfo,
			nullptr,
			&m_uploadPool) != VK_SUCCESS)
		{
			throw std::runtime_error("MeowCore: failed to create upload command pool.");
		}

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
		m_postProcessStack.Destroy();
		m_frameConstantsBuffer.Shutdown();
		m_swapchain.Shutdown(m_vulkanContext.GetDevice().device);
		if (m_uploadPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(m_vulkanContext.GetDevice().device, m_uploadPool, nullptr);
			m_uploadPool = VK_NULL_HANDLE;
		}
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

	void MeowCore::RecreateSwapchain()
	{
		// Wait out minimized state (extent = 0,0) before recreating.
		int w = 0;
		int h = 0;
		glfwGetFramebufferSize(m_window.GetHandle(), &w, &h);
		while (w == 0 || h == 0)
		{
			glfwWaitEvents();
			glfwGetFramebufferSize(m_window.GetHandle(), &w, &h);
		}

		vkDeviceWaitIdle(m_vulkanContext.GetDevice().device);
		m_swapchain.Shutdown(m_vulkanContext.GetDevice().device);
		m_swapchain.ClearRecreationFlag();
		m_swapchain.Initialize(m_vulkanContext, m_window);

		// Rebuild extent-dependent offscreen targets and re-register passes.
		m_postProcessStack.Destroy();
		m_renderGraph.Clear();
		m_postProcessStack = PostProcessStack::Create({
			.device = m_vulkanContext.GetDevice().device,
			.allocator = m_vulkanContext.GetAllocator(),
			.extent = m_swapchain.GetExtent(),
			.swapchainFormat = m_swapchain.GetImageFormat(),
			.bindlessManager = &m_bindlessManager,
			.renderGraph = &m_renderGraph,
			});
		RegisterPasses();

		INFO(LogCategory::Engine, "Swapchain recreated ({}x{}).", w, h);
	}

	void MeowCore::RegisterPasses()
	{
		// ── Pass 1: Forward ───────────────────────────────────────────────────
		// Renders all scene objects into the HDR offscreen buffer.
		m_renderGraph.AddPass("$EngineForward")
			.WriteColor(
				m_postProcessStack.GetHdrColor(),
				VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_STORE,
				ClearColorValue(0.05f, 0.05f, 0.07f, 1.0f))
			.WriteDepth(
				m_renderGraph.GetSwapchainDepth(),
				VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_DONT_CARE,
				ClearDepthValue(1.0f))
			.Execute([this](PassContext& ctx)
				{
					m_scene.FlushToQueue(m_renderQueue);
					m_world.FlushToQueue(m_renderQueue);
					m_renderQueue.Flush(
						ctx.recorder,
						ctx.frameConstantsAddr,
						m_bindlessManager.GetSet());
					m_renderQueue.Clear();
				});

		// ── Passes 2–3: Tonemap + FXAA ────────────────────────────────────────
		m_postProcessStack.RegisterPasses(m_renderGraph, m_bindlessManager);

		// ── Pass 4: UI overlay ────────────────────────────────────────────────
		// Loads the FXAA output and composites UI on top.
		m_renderGraph.AddPass("$UIOverlay")
			.WriteColor(
				m_renderGraph.GetSwapchainColor(),
				VK_ATTACHMENT_LOAD_OP_LOAD,
				VK_ATTACHMENT_STORE_OP_STORE,
				{})
			.Execute([this](PassContext& ctx)
				{
					// TODO: record ImGui draw commands via ctx.recorder.
					(void)ctx;
				});
	}

	void MeowCore::BeginFrame()
	{
		if (m_swapchain.NeedsRecreation())
		{
			RecreateSwapchain();
		}

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
			FrameConstants fc;
			fc.viewProj = m_scene.GetViewProjection();
			m_frameConstantsBuffer.Write(frameIdx, fc);
			const VkDeviceAddress frameAddr = m_frameConstantsBuffer.GetDeviceAddress(frameIdx);

			const FrameTarget frameTarget{
				.colorImage = m_swapchain.GetCurrentImage(),
				.colorView = m_swapchain.GetCurrentImageView(),
				.depthImage = m_swapchain.GetDepthImage(),
				.depthView = m_swapchain.GetDepthImageView(),
				.colorFormat = m_swapchain.GetImageFormat(),
				.depthFormat = m_swapchain.GetDepthFormat(),
				.extent = m_swapchain.GetExtent(),
			};

			m_renderGraph.Execute(m_swapchain.GetCurrentCommandBuffer(), frameTarget, frameAddr);
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

	RenderGraph& MeowCore::GetRenderGraph()
	{
		return m_renderGraph;
	}

	const RenderGraph& MeowCore::GetRenderGraph() const
	{
		return m_renderGraph;
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

	void MeowCore::SetTonemapMode(TonemapMode mode)
	{
		m_postProcessStack.SetTonemapMode(mode);
	}

	TonemapMode MeowCore::GetTonemapMode() const
	{
		return m_postProcessStack.GetTonemapMode();
	}

	void MeowCore::SetFxaaEnabled(bool enabled)
	{
		m_postProcessStack.SetFxaaEnabled(enabled);
	}

	bool MeowCore::IsFxaaEnabled() const
	{
		return m_postProcessStack.IsFxaaEnabled();
	}

	World& MeowCore::GetWorld()
	{
		return m_world;
	}

	const World& MeowCore::GetWorld() const
	{
		return m_world;
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

	Texture MeowCore::CreateTexture(std::string_view path)
	{
		return Texture::LoadFromFile(
			path,
			m_vulkanContext.GetDevice().device,
			m_vulkanContext.GetAllocator(),
			m_vulkanContext.GetGraphicsQueue(),
			m_uploadPool,
			m_bindlessManager);
	}

	void MeowCore::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn)
	{
		const VkCommandBufferAllocateInfo allocInfo{
			.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool        = m_uploadPool,
			.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		vkAllocateCommandBuffers(m_vulkanContext.GetDevice().device, &allocInfo, &cmd);

		const VkCommandBufferBeginInfo beginInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vkBeginCommandBuffer(cmd, &beginInfo);
		fn(cmd);
		vkEndCommandBuffer(cmd);

		const VkSubmitInfo submitInfo{
			.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers    = &cmd,
		};
		vkQueueSubmit(m_vulkanContext.GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
		vkQueueWaitIdle(m_vulkanContext.GetGraphicsQueue());

		vkFreeCommandBuffers(m_vulkanContext.GetDevice().device, m_uploadPool, 1, &cmd);
	}
}