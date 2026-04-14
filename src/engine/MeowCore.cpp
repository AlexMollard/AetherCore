#include "MeowCore.hpp"

#include <string>
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

		m_input.Init(m_window.GetHandle());

		// Create a default main camera so rendering works without app setup.
		const CameraHandle mainCam = m_cameraManager.Create(CameraDesc{});
		m_cameraManager.SetMainCamera(mainCam);

		// Upload pool — used for immediate-submit texture uploads.
		// TRANSIENT: hints that command buffers are short-lived.
		// RESET_COMMAND_BUFFER: allows individual buffer reset/reuse.
		const VkCommandPoolCreateInfo uploadPoolInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
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

		// Preserve current post-process settings across recreation.
		const TonemapMode tonemapMode = m_postProcessStack.GetTonemapMode();
		const float       exposure = m_postProcessStack.GetExposure();
		const bool        fxaaEnabled = m_postProcessStack.IsFxaaEnabled();

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
		m_postProcessStack.SetTonemapMode(tonemapMode);
		m_postProcessStack.SetExposure(exposure);
		m_postProcessStack.SetFxaaEnabled(fxaaEnabled);

		// Re-register existing RTT images after graph clear.
		for (auto& [id, rt] : m_rtCameras)
		{
			if (rt.depthImage.GetFormat() != m_swapchain.GetDepthFormat())
			{
				rt.depthImage.Reset();
				rt.depthImage = UniqueImage::Create(
					m_vulkanContext.GetDevice().device,
					m_vulkanContext.GetAllocator(),
					{
						.extent = rt.extent,
						.format = m_swapchain.GetDepthFormat(),
						.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
					});
			}

			rt.rgColor = m_renderGraph.RegisterImage(
				rt.colorImage.Get(), rt.colorImage.GetDefaultView(), VK_IMAGE_ASPECT_COLOR_BIT);
			rt.rgDepth = m_renderGraph.RegisterImage(
				rt.depthImage.Get(), rt.depthImage.GetDefaultView(), VK_IMAGE_ASPECT_DEPTH_BIT);
		}
		RegisterPasses();

		INFO(LogCategory::Engine, "Swapchain recreated ({}x{}).", w, h);
	}

	void MeowCore::RegisterPasses()
	{
		for (auto& [id, rt] : m_rtCameras)
		{
			RegisterRttPassesFor(id);
		}

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

	void MeowCore::RegisterRttPassesFor(const uint32_t id)
	{
		auto it = m_rtCameras.find(id);
		if (it == m_rtCameras.end())
		{
			return;
		}

		const std::string passName = "$CameraRT_" + std::to_string(id);
		const RGImage color = it->second.rgColor;
		const RGImage depth = it->second.rgDepth;
		const VkExtent2D extent = it->second.extent;

		m_renderGraph.AddPass(passName)
			.WriteColor(
				color,
				VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_STORE,
				ClearColorValue(0.02f, 0.02f, 0.03f, 1.0f))
			.WriteDepth(
				depth,
				VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_DONT_CARE,
				ClearDepthValue(1.0f))
			.SetExtent(extent)
			.Execute([this, id](PassContext& ctx)
				{
					auto rit = m_rtCameras.find(id);
					if (rit == m_rtCameras.end())
					{
						return;
					}

					Camera* cam = m_cameraManager.TryGet(rit->second.camera);
					if (cam == nullptr || !rit->second.constants)
					{
						return;
					}

					const float aspect = static_cast<float>(rit->second.extent.width) / static_cast<float>(rit->second.extent.height);

					FrameConstants fc{};
					fc.view = cam->GetViewMatrix();
					fc.proj = cam->GetProjectionMatrix(aspect);
					fc.viewProj = fc.proj * fc.view;

					const auto frameIdx = static_cast<std::uint32_t>(m_frameIndex % Swapchain::kMaxFramesInFlight);
					rit->second.constants->Write(frameIdx, fc);
					const VkDeviceAddress frameAddr = rit->second.constants->GetDeviceAddress(frameIdx);

					m_scene.FlushToQueue(m_renderQueue);
					m_world.FlushToQueue(m_renderQueue);
					m_renderQueue.Flush(ctx.recorder, frameAddr, m_bindlessManager.GetSet());
					m_renderQueue.Clear();
				});
	}

	void MeowCore::Tick(const float dt)
	{
		m_input.Update();
		m_cameraManager.Update(m_input, dt);
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
			FrameConstants fc{};
			const float aspect = static_cast<float>(m_swapchain.GetExtent().width) / static_cast<float>(m_swapchain.GetExtent().height);

			if (const Camera* cam = m_cameraManager.TryGetMainCamera())
			{
				fc.view = cam->GetViewMatrix();
				fc.proj = cam->GetProjectionMatrix(aspect);
				fc.viewProj = fc.proj * fc.view;
			}
			else
			{
				// Backward compatibility
				fc.viewProj = m_scene.GetViewProjection();
			}

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

	Input& MeowCore::GetInput() { return m_input; }
	const Input& MeowCore::GetInput() const { return m_input; }

	CameraManager& MeowCore::GetCameraManager() { return m_cameraManager; }
	const CameraManager& MeowCore::GetCameraManager() const { return m_cameraManager; }

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

	MeowCore::CameraRenderTarget MeowCore::CreateCameraRenderTarget(
		const CameraHandle camera,
		const VkExtent2D extent)
	{
		if (!m_cameraManager.TryGet(camera))
		{
			throw std::runtime_error("CreateCameraRenderTarget: invalid camera handle.");
		}

		CameraRtEntry rt{};
		rt.camera = camera;
		rt.extent = extent;

		rt.colorImage = UniqueImage::Create(
			m_vulkanContext.GetDevice().device,
			m_vulkanContext.GetAllocator(),
			{
				.extent = extent,
				.format = GetForwardColorFormat(),
				.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			});
		rt.colorImage.EnsureBindlessSampled(
			m_bindlessManager,
			m_vulkanContext.GetDevice().device,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		rt.depthImage = UniqueImage::Create(
			m_vulkanContext.GetDevice().device,
			m_vulkanContext.GetAllocator(),
			{
				.extent = extent,
				.format = m_swapchain.GetDepthFormat(),
				.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			});

		rt.rgColor = m_renderGraph.RegisterImage(
			rt.colorImage.Get(), rt.colorImage.GetDefaultView(), VK_IMAGE_ASPECT_COLOR_BIT);
		rt.rgDepth = m_renderGraph.RegisterImage(
			rt.depthImage.Get(), rt.depthImage.GetDefaultView(), VK_IMAGE_ASPECT_DEPTH_BIT);

		rt.constants = std::make_unique<FrameConstantsBuffer>();
		rt.constants->Initialize(m_vulkanContext);

		const uint32_t id = m_nextRtId++;
		m_rtCameras.emplace(id, std::move(rt));
		RegisterRttPassesFor(id);

		return CameraRenderTarget{ id };
	}

	void MeowCore::DestroyCameraRenderTarget(const CameraRenderTarget rt)
	{
		if (!rt.IsValid())
		{
			return;
		}

		auto it = m_rtCameras.find(rt.id);
		if (it == m_rtCameras.end())
		{
			return;
		}

		m_renderGraph.RemovePass("$CameraRT_" + std::to_string(rt.id));

		if (it->second.constants)
		{
			it->second.constants->Shutdown();
		}
		it->second.depthImage.Reset();
		it->second.colorImage.Reset();

		m_rtCameras.erase(it);
	}

	RGImage MeowCore::GetRenderTargetColorImage(const CameraRenderTarget rt) const
	{
		auto it = m_rtCameras.find(rt.id);
		if (it == m_rtCameras.end())
		{
			return {};
		}
		return it->second.rgColor;
	}

	uint32_t MeowCore::GetRenderTargetBindlessSlot(const CameraRenderTarget rt) const
	{
		auto it = m_rtCameras.find(rt.id);
		if (it == m_rtCameras.end() || !it->second.colorImage.HasBindlessSampled())
		{
			return 0xFFFFFFFFu;
		}
		return it->second.colorImage.GetBindlessSampledSlot();
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
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = m_uploadPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
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
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd,
		};
		vkQueueSubmit(m_vulkanContext.GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
		vkQueueWaitIdle(m_vulkanContext.GetGraphicsQueue());

		vkFreeCommandBuffers(m_vulkanContext.GetDevice().device, m_uploadPool, 1, &cmd);
	}
}