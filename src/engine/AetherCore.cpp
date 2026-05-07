#include "AetherCore.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "assets/GltfAsset.hpp"
#include "EcsHelpers.hpp"
#include "FileSystem.hpp"
#include "FrameConstants.hpp"
#include "Logger.hpp"
#include "Profiler.hpp"
#include "RenderThread.hpp"

namespace aether
{
	AetherCore::AetherCore(const Config& config)
	      : AetherCore(config, EngineSettingsIO::LoadOrCreate(config.settingsFile))
	{
	}

	AetherCore::AetherCore(const Config& config, const EngineSettings& settings)
	      : m_window(config.appName, config.width, config.height), m_vulkanContext(m_window, config.appName)
	{
		io::FileSystem::InitializeDefaultMounts();
		m_settings = settings;
		m_settings.window.width = config.width;
		m_settings.window.height = config.height;

		m_swapchain.Initialize(m_vulkanContext, m_window, m_settings.graphics.vsync);
		m_renderGraph.Initialize(m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator());
		m_bindlessManager.Initialize(m_vulkanContext);
		m_frameConstantsBuffer.Initialize(m_vulkanContext);
		m_materialBuffer.Initialize(m_vulkanContext);
		m_renderQueue.Initialize(m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator(), 65536);
		m_shadowService.Initialize(m_vulkanContext, m_swapchain);
		m_renderTargetService.Initialize(m_vulkanContext);
		m_renderQueue.SetDebugForceVisible(false);
		m_renderQueue.SetDebugBypassIndirect(false);
		m_cullPass.Initialize(m_vulkanContext.GetDevice().device);
		m_lightingManager.Initialize(m_vulkanContext, m_renderer);
		m_resourcePool.ConfigureBindlessImages({
		        .manager = &m_bindlessManager,
		        .device = m_vulkanContext.GetDevice().device,
		});

		// Upload command pool - transient, per-buffer reset.
		const VkCommandPoolCreateInfo uploadPoolInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = m_vulkanContext.GetGraphicsQueueFamily(),
		};
		if (vkCreateCommandPool(m_vulkanContext.GetDevice().device, &uploadPoolInfo, nullptr, &m_uploadPool) != VK_SUCCESS)
		{
			throw std::runtime_error("AetherCore: failed to create upload command pool.");
		}

		m_primitiveMeshes.Initialize(m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator(), m_vulkanContext.GetGraphicsQueue(), m_uploadPool);
		m_meshArena.Initialize(m_vulkanContext);
		m_meshUploadQueue.Initialize(m_vulkanContext);

		m_postProcessStack = PostProcessStack::Create({
		        .device = m_vulkanContext.GetDevice().device,
		        .allocator = m_vulkanContext.GetAllocator(),
		        .extent = m_swapchain.GetExtent(),
		        .swapchainFormat = m_swapchain.GetImageFormat(),
		        .bindlessManager = &m_bindlessManager,
		        .renderGraph = &m_renderGraph,
		});
		m_postProcessStack.SetFxaaEnabled(m_settings.graphics.fxaa);

		m_renderer.Initialize(&m_postProcessStack);

		m_skyboxPass = SkyboxPass::Create({
		        .device = m_vulkanContext.GetDevice().device,
		        .hdrColorFormat = PostProcessStack::GetForwardColorFormat(),
		});

		m_assetManager.Initialize(m_vulkanContext, m_bindlessManager, m_materialBuffer, m_renderQueue, m_shadowService, m_renderTargetService, m_world, m_uploadPool);

		m_renderTargetService.BindRuntime(m_renderGraph, m_bindlessManager, m_cameraManager, m_lightingManager, m_renderer, m_materialBuffer, m_cullPass, [this]() { return m_frameIndex; }, m_vulkanContext.GetDevice().device, m_swapchain.GetDepthFormat(), GetForwardColorFormat());

		RegisterPasses();

		m_input.Init(m_window.GetHandle());

		// Create a default main camera so rendering works without app setup.
		const CameraHandle mainCam = m_cameraManager.Create(CameraDesc{});
		m_cameraManager.SetMainCamera(mainCam);

		m_asyncComputeEnabled = m_settings.graphics.asyncCompute && m_vulkanContext.GetComputeQueue() != VK_NULL_HANDLE;
		if (!m_settings.graphics.asyncCompute)
		{
			INFO(LogCategory::Engine, "Async compute disabled by settings.");
		}
		if (!m_asyncComputeEnabled)
		{
			WARN(LogCategory::Engine, "Async compute disabled: no dedicated compute queue available.");
		}

		if (m_asyncComputeEnabled)
		{
			// Timeline semaphore for compute->graphics synchronisation across frames in flight.
			const VkSemaphoreTypeCreateInfo timelineTypeInfo{
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
				.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
				.initialValue = 0,
			};
			const VkSemaphoreCreateInfo semInfo{
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
				.pNext = &timelineTypeInfo,
			};
			if (vkCreateSemaphore(m_vulkanContext.GetDevice().device, &semInfo, nullptr, &m_computeTimelineSemaphore) != VK_SUCCESS)
			{
				throw std::runtime_error("AetherCore: failed to create compute timeline semaphore.");
			}
			CommandRecorder::SetObjectName(m_vulkanContext.GetDevice().device, reinterpret_cast<std::uint64_t>(m_computeTimelineSemaphore), VK_OBJECT_TYPE_SEMAPHORE, "AsyncCompute.Timeline");
			VkFenceCreateInfo fenceInfo{};
			fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

			for (std::size_t frameI = 0; frameI < m_asyncComputeFrames.size(); ++frameI)
			{
				auto& frame = m_asyncComputeFrames[frameI];
				const VkCommandPoolCreateInfo poolInfo{
					.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					.queueFamilyIndex = m_vulkanContext.GetComputeQueueFamily(),
				};
				if (vkCreateCommandPool(m_vulkanContext.GetDevice().device, &poolInfo, nullptr, &frame.commandPool) != VK_SUCCESS)
				{
					throw std::runtime_error("AetherCore: failed to create async compute command pool.");
				}

				const VkCommandBufferAllocateInfo allocInfo{
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					.commandPool = frame.commandPool,
					.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					.commandBufferCount = 1,
				};
				if (vkAllocateCommandBuffers(m_vulkanContext.GetDevice().device, &allocInfo, &frame.commandBuffer) != VK_SUCCESS)
				{
					throw std::runtime_error("AetherCore: failed to allocate async compute command buffer.");
				}

				if (vkCreateFence(m_vulkanContext.GetDevice().device, &fenceInfo, nullptr, &frame.inFlight) != VK_SUCCESS)
				{
					throw std::runtime_error("AetherCore: failed to create async compute fence.");
				}

				const std::string suffix = "[" + std::to_string(frameI) + "]";
				CommandRecorder::SetObjectName(m_vulkanContext.GetDevice().device, reinterpret_cast<std::uint64_t>(frame.commandBuffer), VK_OBJECT_TYPE_COMMAND_BUFFER, ("AsyncCompute.Cmd" + suffix).c_str());
				CommandRecorder::SetObjectName(m_vulkanContext.GetDevice().device, reinterpret_cast<std::uint64_t>(frame.inFlight), VK_OBJECT_TYPE_FENCE, ("AsyncCompute.Fence" + suffix).c_str());
			}
		}

		INFO(LogCategory::Engine, "Engine core initialized. Bindless sampled-image capacity: {}", m_bindlessManager.GetCapacity());
	}

	void AetherCore::WaitIdle() const
	{
		vkDeviceWaitIdle(m_vulkanContext.GetDevice().device);
	}

	AetherCore::~AetherCore()
	{
		vkDeviceWaitIdle(m_vulkanContext.GetDevice().device);

		m_renderTargetService.Shutdown();
		m_shadowService.Shutdown(m_vulkanContext.GetDevice().device);

		for (auto& frame: m_asyncComputeFrames)
		{
			if (frame.inFlight != VK_NULL_HANDLE)
			{
				vkDestroyFence(m_vulkanContext.GetDevice().device, frame.inFlight, nullptr);
				frame.inFlight = VK_NULL_HANDLE;
			}
			if (frame.commandPool != VK_NULL_HANDLE)
			{
				vkDestroyCommandPool(m_vulkanContext.GetDevice().device, frame.commandPool, nullptr);
				frame.commandPool = VK_NULL_HANDLE;
				frame.commandBuffer = VK_NULL_HANDLE;
			}
		}
		if (m_computeTimelineSemaphore != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(m_vulkanContext.GetDevice().device, m_computeTimelineSemaphore, nullptr);
			m_computeTimelineSemaphore = VK_NULL_HANDLE;
		}
		m_asyncComputeEnabled = false;

		m_postProcessStack.Destroy();
		m_skyboxPass.Destroy();
		m_cullPass.Shutdown();
		m_frameConstantsBuffer.Shutdown();
		m_renderQueue.Shutdown();
		m_meshUploadQueue.Shutdown();
		m_meshArena.Shutdown();
		m_renderGraph.Shutdown();
		m_swapchain.Shutdown(m_vulkanContext.GetDevice().device);
		if (m_uploadPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(m_vulkanContext.GetDevice().device, m_uploadPool, nullptr);
			m_uploadPool = VK_NULL_HANDLE;
		}
		m_lightingManager.Shutdown();
		m_materialBuffer.Shutdown();
		m_bindlessManager.Shutdown();
		io::FileSystem::Shutdown();
	}

	bool AetherCore::ShouldClose() const
	{
		return m_window.ShouldClose();
	}

	void AetherCore::PumpEvents() const
	{
		m_window.PollEvents();
	}

	void AetherCore::RecreateSwapchain()
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
		m_swapchain.Initialize(m_vulkanContext, m_window, m_settings.graphics.vsync);

		m_shadowService.RecreatePipeline(m_vulkanContext.GetDevice().device, m_swapchain.GetDepthFormat());

		// Preserve current post-process settings across recreation.
		const TonemapMode tonemapMode = m_postProcessStack.GetTonemapMode();
		const float exposure = m_postProcessStack.GetExposure();
		const bool fxaaEnabled = m_postProcessStack.IsFxaaEnabled();

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

		m_renderTargetService.OnRenderGraphReset(m_vulkanContext.GetDevice().device, m_swapchain.GetDepthFormat(), GetForwardColorFormat());
		RegisterPasses();

		INFO(LogCategory::Engine, "Swapchain recreated ({}x{}).", w, h);
	}

	void AetherCore::RegisterPasses()
	{
		m_renderPipelineCoordinator.RegisterPasses(
		        m_renderGraph,
		        m_skyboxPass,
		        m_postProcessStack,
		        m_shadowService,
		        m_bindlessManager,
		        m_vulkanContext.GetDevice().device,
		        m_swapchain.GetDepthFormat(),
		        m_cullPass,
		        m_renderQueue,
		        m_forwardPass,
		        [this]()
		        {
			        const auto frameIdx = static_cast<std::uint32_t>(m_frameIndex % Swapchain::kMaxFramesInFlight);
			        return m_lightingManager.GetSet(frameIdx);
		        },
		        m_renderTargetService);
	}

	void AetherCore::Tick(const float dt)
	{
		AE_PROFILE_ZONE();
		m_input.Update();
		m_cameraManager.Update(m_input, dt);
	}

	void AetherCore::BeginFrame()
	{
		AE_PROFILE_ZONE();
		if (m_swapchain.NeedsRecreation())
		{
			RecreateSwapchain();
		}

		m_swapchain.BeginFrame(m_vulkanContext.GetDevice().device);
		m_currentRecorder = CommandRecorder(m_swapchain.GetCurrentCommandBuffer());
	}

	RenderFramePacket AetherCore::PrepareFrame(std::uint32_t drawSlot, std::uint64_t frameIndex)
	{
		AE_PROFILE_ZONE();
		// Pre-populate the render queue from ECS into the designated double-buffer slot.
		m_renderQueue.SetWriteSlot(drawSlot);
		m_scene.FlushToQueue(m_renderQueue);
		m_world.FlushToQueue(m_renderQueue);

		// Pre-populate each RTT queue on the game thread to avoid scene/world
		// race conditions inside render-thread pass callbacks.
		m_renderTargetService.PrepareQueues(drawSlot, m_scene, m_world);

		m_shadowService.PrepareQueues(drawSlot, m_scene, m_world);

		// Snapshot per-frame render state so the render thread never reads live
		// game-thread state after this function returns.
		RenderFramePacket packet;
		packet.frameIndex = frameIndex;
		packet.drawSlot = drawSlot;
		packet.materialBufferAddr = m_materialBuffer.GetDeviceAddress();

		if (const Camera* cam = m_cameraManager.TryGetMainCamera())
		{
			packet.hasCameraData = true;
			packet.view = cam->GetViewMatrix();
			const float aspect = static_cast<float>(m_swapchain.GetExtent().width) / static_cast<float>(m_swapchain.GetExtent().height);
			packet.proj = cam->GetProjectionMatrix(aspect);
			packet.cameraWorldPos = glm::vec4(cam->GetPosition(), 1.0f);
		}

		packet.sunDirectionIntensity = m_renderer.GetDirectionalLightVector();
		packet.ambientColor = m_renderer.GetAmbientLightVector();
		packet.sunColor = m_renderer.GetSunColorVector();
		packet.skyHorizonColor = m_renderer.GetSkyHorizonColorVector();
		packet.skyZenithColor = m_renderer.GetSkyZenithColorVector();
		packet.skyVoidColor = m_renderer.GetSkyVoidColorVector();

		return packet;
	}

	void AetherCore::ExecuteRenderFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();
		// Synchronise m_frameIndex with the packet so that RTT pass callbacks
		// that capture 	his and read m_frameIndex see the correct value.
		m_frameIndex = packet.frameIndex;
		BeginFrame();
		EndFrame(packet);
	}

	void AetherCore::EndFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();
		VkSemaphore computeFinished = VK_NULL_HANDLE;

		if (m_swapchain.IsFrameValid())
		{
			const auto frameIdx = static_cast<std::uint32_t>(packet.frameIndex % Swapchain::kMaxFramesInFlight);

			// Build base per-frame constants from the immutable frame packet.
			FrameConstants fc = m_frameComposer.ComposeBaseFrameConstants(packet, m_scene.GetViewProjection());

			m_shadowService.BuildFrameShadowData(packet, frameIdx, m_cameraManager, fc);

			// TODO: cam pointer is read on the render thread while the game thread
			// may advance it for frame N+1. This race is benign in practice (1-frame
			// lag for tiled-light frustum culling) and will be resolved when
			// LightingManager accepts matrices instead of a Camera object.
			if (packet.hasCameraData)
			{
				const Camera* cam = m_cameraManager.TryGetMainCamera();
				if (cam)
				{
					const std::uint32_t computeFamily = m_vulkanContext.GetComputeQueueFamily();
					const std::uint32_t graphicsFamily = m_vulkanContext.GetGraphicsQueueFamily();

					VkCommandBuffer lightingCmd = m_swapchain.GetCurrentCommandBuffer();
					if (m_asyncComputeEnabled)
					{
						auto& asyncFrame = m_asyncComputeFrames[frameIdx];
						vkWaitForFences(m_vulkanContext.GetDevice().device, 1, &asyncFrame.inFlight, VK_TRUE, UINT64_MAX);
						vkResetFences(m_vulkanContext.GetDevice().device, 1, &asyncFrame.inFlight);
						vkResetCommandPool(m_vulkanContext.GetDevice().device, asyncFrame.commandPool, 0);

						const VkCommandBufferBeginInfo beginInfo{
							.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
							.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
						};
						vkBeginCommandBuffer(asyncFrame.commandBuffer, &beginInfo);
						CommandRecorder(asyncFrame.commandBuffer).BeginDebugLabel("AsyncCompute.LightCull", 0.9f, 0.45f, 0.1f);
						lightingCmd = asyncFrame.commandBuffer;
					}

					m_lightingManager.UpdateForView(frameIdx, lightingCmd, *cam, m_swapchain.GetExtent(), fc, true, computeFamily, graphicsFamily);

					if (m_asyncComputeEnabled)
					{
						auto& asyncFrame = m_asyncComputeFrames[frameIdx];
						CommandRecorder(asyncFrame.commandBuffer).EndDebugLabel();
						vkEndCommandBuffer(asyncFrame.commandBuffer);

						// Advance the timeline value and signal it from the compute queue.
						const std::uint64_t signalValue = ++m_computeTimelineValue;
						const VkTimelineSemaphoreSubmitInfo timelineSubmit{
							.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
							.waitSemaphoreValueCount = 0,
							.pWaitSemaphoreValues = nullptr,
							.signalSemaphoreValueCount = 1,
							.pSignalSemaphoreValues = &signalValue,
						};
						const VkSubmitInfo submitInfo{
							.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
							.pNext = &timelineSubmit,
							.waitSemaphoreCount = 0,
							.pWaitSemaphores = nullptr,
							.pWaitDstStageMask = nullptr,
							.commandBufferCount = 1,
							.pCommandBuffers = &asyncFrame.commandBuffer,
							.signalSemaphoreCount = 1,
							.pSignalSemaphores = &m_computeTimelineSemaphore,
						};
						vkQueueSubmit(m_vulkanContext.GetComputeQueue(), 1, &submitInfo, asyncFrame.inFlight);
						computeFinished = m_computeTimelineSemaphore;

						// When the compute and graphics queue families differ, the lighting
						// buffers need a QFOT acquire barrier on the graphics command buffer
						// before the fragment shader reads them.
						if (computeFamily != graphicsFamily)
						{
							m_lightingManager.EmitAcquireBarriers(frameIdx, m_swapchain.GetCurrentCommandBuffer(), computeFamily, graphicsFamily);
						}
					} // if (m_asyncComputeEnabled)
				} // if (cam)
			} // if (packet.hasCameraData)
			else
			{
				m_frameComposer.ApplyNoCameraLightingFallback(fc);
			}

			m_frameConstantsBuffer.Write(frameIdx, fc);
			const VkDeviceAddress frameAddr = m_frameConstantsBuffer.GetDeviceAddress(frameIdx);

			const VkMemoryBarrier2 frameConstantsHostToShaders{
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
				.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
				.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			};
			const VkDependencyInfo frameConstantsDep{
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.memoryBarrierCount = 1,
				.pMemoryBarriers = &frameConstantsHostToShaders,
			};
			vkCmdPipelineBarrier2(m_swapchain.GetCurrentCommandBuffer(), &frameConstantsDep);

			const FrameTarget frameTarget{
				.colorImage = m_swapchain.GetCurrentImage(),
				.colorView = m_swapchain.GetCurrentImageView(),
				.depthImage = m_swapchain.GetDepthImage(),
				.depthView = m_swapchain.GetDepthImageView(),
				.colorFormat = m_swapchain.GetImageFormat(),
				.depthFormat = m_swapchain.GetDepthFormat(),
				.extent = m_swapchain.GetExtent(),
			};

			m_currentRecorder.BeginDebugLabel("Frame.RenderGraph", 0.35f, 0.55f, 0.95f, 1.0f);
			m_renderGraph.Execute(m_swapchain.GetCurrentCommandBuffer(), frameTarget, frameAddr, frameIdx);
			m_currentRecorder.EndDebugLabel();
		}
		m_swapchain.EndFrame(m_vulkanContext.GetGraphicsQueue(), m_vulkanContext.GetPresentQueue(), computeFinished, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, m_computeTimelineValue);
		++m_frameIndex;
		m_bindlessManager.AdvanceFrame(m_frameIndex);
	}

	VulkanContext& AetherCore::GetVulkanContext()
	{
		return m_vulkanContext;
	}

	const VulkanContext& AetherCore::GetVulkanContext() const
	{
		return m_vulkanContext;
	}

	BindlessManager& AetherCore::GetBindlessManager()
	{
		return m_bindlessManager;
	}

	const BindlessManager& AetherCore::GetBindlessManager() const
	{
		return m_bindlessManager;
	}

	ResourcePool& AetherCore::GetResourcePool()
	{
		return m_resourcePool;
	}

	const ResourcePool& AetherCore::GetResourcePool() const
	{
		return m_resourcePool;
	}

	RenderGraph& AetherCore::GetRenderGraph()
	{
		return m_renderGraph;
	}

	const RenderGraph& AetherCore::GetRenderGraph() const
	{
		return m_renderGraph;
	}

	VkDescriptorSetLayout AetherCore::GetLightingSetLayout() const
	{
		return m_lightingManager.GetSetLayout();
	}

	VkCommandBuffer AetherCore::GetCurrentCommandBuffer() const
	{
		return m_swapchain.GetCurrentCommandBuffer();
	}

	VkFormat AetherCore::GetSwapchainImageFormat() const
	{
		return m_swapchain.GetImageFormat();
	}

	VkFormat AetherCore::GetSwapchainDepthFormat() const
	{
		return m_swapchain.GetDepthFormat();
	}

	VkExtent2D AetherCore::GetSwapchainExtent() const
	{
		return m_swapchain.GetExtent();
	}

	RenderQueue& AetherCore::GetRenderQueue()
	{
		return m_renderQueue;
	}

	const RenderQueue& AetherCore::GetRenderQueue() const
	{
		return m_renderQueue;
	}

	ShadowService& AetherCore::GetShadowService()
	{
		return m_shadowService;
	}

	const ShadowService& AetherCore::GetShadowService() const
	{
		return m_shadowService;
	}

	Scene& AetherCore::GetScene()
	{
		return m_scene;
	}

	const Scene& AetherCore::GetScene() const
	{
		return m_scene;
	}

	Swapchain& AetherCore::GetSwapchain()
	{
		return m_swapchain;
	}

	const Swapchain& AetherCore::GetSwapchain() const
	{
		return m_swapchain;
	}

	Input& AetherCore::GetInput()
	{
		return m_input;
	}

	const Input& AetherCore::GetInput() const
	{
		return m_input;
	}

	CameraManager& AetherCore::GetCameraManager()
	{
		return m_cameraManager;
	}

	const CameraManager& AetherCore::GetCameraManager() const
	{
		return m_cameraManager;
	}

	Renderer& AetherCore::GetRenderer()
	{
		return m_renderer;
	}

	const Renderer& AetherCore::GetRenderer() const
	{
		return m_renderer;
	}

	Window& AetherCore::GetWindow()
	{
		return m_window;
	}

	const Window& AetherCore::GetWindow() const
	{
		return m_window;
	}

	const EngineSettings& AetherCore::GetSettings() const
	{
		return m_settings;
	}

	AssetManager& AetherCore::GetAssets()
	{
		return m_assetManager;
	}

	const AssetManager& AetherCore::GetAssets() const
	{
		return m_assetManager;
	}

	void AetherCore::SetTonemapMode(TonemapMode mode)
	{
		m_renderer.SetTonemapMode(mode);
	}

	TonemapMode AetherCore::GetTonemapMode() const
	{
		return m_renderer.GetTonemapMode();
	}

	void AetherCore::SetFxaaEnabled(bool enabled)
	{
		m_renderer.SetFxaaEnabled(enabled);
	}

	bool AetherCore::IsFxaaEnabled() const
	{
		return m_renderer.IsFxaaEnabled();
	}

	void AetherCore::SetDirectionalLight(glm::vec3 direction, const float intensity)
	{
		m_renderer.SetDirectionalLight(direction, intensity);
	}

	glm::vec3 AetherCore::GetDirectionalLightDirection() const
	{
		return m_renderer.GetDirectionalLightDirection();
	}

	float AetherCore::GetDirectionalLightIntensity() const
	{
		return m_renderer.GetDirectionalLightIntensity();
	}

	void AetherCore::SetAmbientLight(glm::vec3 color)
	{
		m_renderer.SetAmbientLight(color);
	}

	glm::vec3 AetherCore::GetAmbientLight() const
	{
		return m_renderer.GetAmbientLight();
	}

	void AetherCore::SetRttLightingBinningEnabled(const bool enabled)
	{
		m_lightingManager.SetRttBinningEnabled(enabled);
	}

	bool AetherCore::IsRttLightingBinningEnabled() const
	{
		return m_lightingManager.IsRttBinningEnabled();
	}

	void AetherCore::SetGpuLightingBinningEnabled(const bool enabled)
	{
		m_lightingManager.SetGpuBinningEnabled(enabled);
	}

	bool AetherCore::IsGpuLightingBinningEnabled() const
	{
		return m_lightingManager.IsGpuBinningEnabled();
	}

	AetherCore::CameraRenderTarget AetherCore::CreateCameraRenderTarget(const CameraHandle camera, const VkExtent2D extent)
	{
		if (!m_cameraManager.TryGet(camera))
		{
			throw std::runtime_error("CreateCameraRenderTarget: invalid camera handle.");
		}
		const uint32_t id = m_renderTargetService.CreateCameraRenderTarget(camera.id, extent);
		return CameraRenderTarget{ id };
	}

	void AetherCore::DestroyCameraRenderTarget(const CameraRenderTarget rt)
	{
		m_renderTargetService.DestroyCameraRenderTarget(rt.id);
	}

	RGImage AetherCore::GetRenderTargetColorImage(const CameraRenderTarget rt) const
	{
		return m_renderTargetService.GetRenderTargetColorImage(rt.id);
	}

	uint32_t AetherCore::GetRenderTargetBindlessSlot(const CameraRenderTarget rt) const
	{
		return m_renderTargetService.GetRenderTargetBindlessSlot(rt.id);
	}

	World& AetherCore::GetWorld()
	{
		return m_world;
	}

	const World& AetherCore::GetWorld() const
	{
		return m_world;
	}

	const Mesh& AetherCore::GetPrimitiveMesh(PrimitiveMesh primitive) const
	{
		return m_primitiveMeshes.Get(primitive);
	}

	GraphicsPipeline AetherCore::CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc)
	{
		return m_assetManager.CreateGraphicsPipeline(desc);
	}

	Mesh AetherCore::CreateMesh(std::span<const Mesh::Vertex> vertices)
	{
		return m_assetManager.CreateMesh(vertices);
	}

	Mesh AetherCore::CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices)
	{
		return m_assetManager.CreateMesh(vertices, indices);
	}

	MeshArena& AetherCore::GetMeshArena()
	{
		return m_meshArena;
	}

	MeshUploadQueue& AetherCore::GetMeshUploadQueue()
	{
		return m_meshUploadQueue;
	}

	void AetherCore::FlushMeshUploads()
	{
		if (!m_meshUploadQueue.HasPendingUploads())
		{
			return;
		}
		ImmediateSubmit([this](VkCommandBuffer cmd) { m_meshUploadQueue.Flush(cmd); });
	}

	Texture AetherCore::CreateTexture(std::string_view path, TextureFilter filter)
	{
		return m_assetManager.CreateTexture(path, filter);
	}

	void AetherCore::RegisterMaterial(Material& mat)
	{
		m_assetManager.RegisterMaterial(mat);
	}

	void AetherCore::UnregisterMaterial(Material& mat)
	{
		m_assetManager.UnregisterMaterial(mat);
	}

	LoadedModel AetherCore::LoadModel(std::string_view path)
	{
		return m_assetManager.LoadModel(path);
	}

	std::vector<Entity> AetherCore::SpawnModel(LoadedModel& model, GraphicsPipeline& pipeline, float scale)
	{
		return m_assetManager.SpawnModel(model, pipeline, scale);
	}

	void AetherCore::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn)
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
} // namespace aether
