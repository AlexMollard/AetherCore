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
	      : m_window(config.appName, config.width, config.height), m_vulkanContext(m_window, config.appName)
	{
		io::FileSystem::InitializeDefaultMounts();
		m_settings = EngineSettingsIO::LoadOrCreate(config.settingsFile);
		m_settings.window.width = config.width;
		m_settings.window.height = config.height;

		m_swapchain.Initialize(m_vulkanContext, m_window, m_settings.graphics.vsync);
		m_bindlessManager.Initialize(m_vulkanContext);
		m_frameConstantsBuffer.Initialize(m_vulkanContext);
		m_materialBuffer.Initialize(m_vulkanContext);
		m_renderQueue.Initialize(m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator());
		m_renderQueue.SetDebugForceVisible(false);
		m_renderQueue.SetDebugBypassIndirect(false);
		m_cullPass.Initialize(m_vulkanContext.GetDevice().device);
		m_lightingManager.Initialize(m_vulkanContext, m_renderer);
		m_resourcePool.ConfigureBindlessImages({
		        .manager = &m_bindlessManager,
		        .device = m_vulkanContext.GetDevice().device,
		});

		// Upload pool — used for one-shot staging uploads (meshes, textures).
		// TRANSIENT: hints that command buffers are short-lived.
		// RESET_COMMAND_BUFFER: allows individual buffer reset/reuse.
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

		m_postProcessStack = PostProcessStack::Create({
		        .device = m_vulkanContext.GetDevice().device,
		        .allocator = m_vulkanContext.GetAllocator(),
		        .extent = m_swapchain.GetExtent(),
		        .swapchainFormat = m_swapchain.GetImageFormat(),
		        .bindlessManager = &m_bindlessManager,
		        .renderGraph = &m_renderGraph,
		});
		m_postProcessStack.SetFxaaEnabled(m_settings.graphics.fxaa);

		// Initialize services.
		m_renderer.Initialize(&m_postProcessStack);

		m_skyboxPass = SkyboxPass::Create({
		        .device = m_vulkanContext.GetDevice().device,
		        .hdrColorFormat = PostProcessStack::GetForwardColorFormat(),
		});

		m_assetManager.Initialize(this);

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
			// Timeline semaphore — monotonically increasing value used to chain
			// compute→graphics submissions across all frames in flight.
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

		for (auto& [id, rt]: m_rtCameras)
		{
			(void) id;
			rt.renderQueue.Shutdown();
			if (rt.constants)
			{
				rt.constants->Shutdown();
			}
			rt.depthImage.Reset();
			rt.colorImage.Reset();
		}
		m_rtCameras.clear();

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

		// Re-register existing RTT images after graph clear.
		for (auto& [id, rt]: m_rtCameras)
		{
			if (rt.depthImage.GetFormat() != m_swapchain.GetDepthFormat())
			{
				rt.depthImage.Reset();
				rt.depthImage = UniqueImage::Create(m_vulkanContext.GetDevice().device,
				        m_vulkanContext.GetAllocator(),
				        {
				                .extent = rt.extent,
				                .format = m_swapchain.GetDepthFormat(),
				                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				        });
			}

			rt.rgColor = m_renderGraph.RegisterImage(rt.colorImage.Get(), rt.colorImage.GetDefaultView(), VK_IMAGE_ASPECT_COLOR_BIT);
			rt.rgDepth = m_renderGraph.RegisterImage(rt.depthImage.Get(), rt.depthImage.GetDefaultView(), VK_IMAGE_ASPECT_DEPTH_BIT);
		}
		RegisterPasses();

		INFO(LogCategory::Engine, "Swapchain recreated ({}x{}).", w, h);
	}

	void AetherCore::RegisterPasses()
	{
		// ── Pass 1: Skybox ────────────────────────────────────────────────────
		// Clears the HDR buffer with a procedural gradient sky.
		// Forward geometry then loads this colour as its background.
		m_skyboxPass.RegisterPass(m_renderGraph, m_postProcessStack.GetHdrColor());

		// ── Pass 2: Cull (compute) ────────────────────────────────────────────
		// Flushes scene + world into the render queue and dispatches the GPU
		// frustum-cull compute shader. Surviving draws are written into the
		// device-local output indirect buffer.
		m_cullPass.RegisterPass(m_renderGraph, m_renderQueue);

		// ── Pass 3: Forward ───────────────────────────────────────────────────
		// Issues DrawIndexedIndirect per batch using the GPU-written indirect
		// buffer produced by the preceding cull pass.
		m_forwardPass.RegisterPass(m_renderGraph,
		        m_postProcessStack.GetHdrColor(),
		        m_renderGraph.GetSwapchainDepth(),
		        m_renderQueue,
		        m_bindlessManager.GetSet(),
		        [this]()
		        {
			        const auto frameIdx = static_cast<std::uint32_t>(m_frameIndex % Swapchain::kMaxFramesInFlight);
			        return m_lightingManager.GetSet(frameIdx);
		        });

		// ── Passes 4–5: Render-to-texture cameras ────────────────────────────
		for (auto& [id, rt]: m_rtCameras)
		{
			RegisterRttPassesFor(id);
		}

		// ── Passes 6–7: Tonemap + FXAA ────────────────────────────────────────
		m_postProcessStack.RegisterPasses(m_renderGraph, m_bindlessManager);
	}

	void AetherCore::RegisterRttPassesFor(const uint32_t id)
	{
		auto it = m_rtCameras.find(id);
		if (it == m_rtCameras.end())
		{
			return;
		}

		const std::string idStr = std::to_string(id);
		const RGImage color = it->second.rgColor;
		const RGImage depth = it->second.rgDepth;
		const VkExtent2D extent = it->second.extent;

		// ── Compute cull pass ─────────────────────────────────────────────────
		// Writes per-camera FrameConstants and dispatches frustum-cull compute.
		// Draw queue population is done on the game thread in PrepareFrame.
		const VkPipeline cullPipeline = m_cullPass.GetPipeline();
		const VkPipelineLayout cullLayout = m_cullPass.GetPipelineLayout();

		m_renderGraph.AddComputePass("$CullDraws_RTT_" + idStr)
		        .ExecuteCompute(
		                [this, id, cullPipeline, cullLayout](PassContext& ctx)
		                {
			                auto rit = m_rtCameras.find(id);
			                if (rit == m_rtCameras.end())
				                return;

			                Camera* cam = m_cameraManager.TryGet(rit->second.camera);
			                if (cam == nullptr || !rit->second.constants)
				                return;

			                const float aspect = static_cast<float>(rit->second.extent.width) / static_cast<float>(rit->second.extent.height);

			                FrameConstants fc{};
			                fc.view = cam->GetViewMatrix();
			                fc.proj = cam->GetProjectionMatrix(aspect);
			                fc.viewProj = fc.proj * fc.view;
			                fc.cameraWorldPos = glm::vec4(cam->GetPosition(), 1.0f);
			                fc.materialBufferAddr = m_materialBuffer.GetDeviceAddress();
			                fc.sunDirectionIntensity = m_renderer.GetDirectionalLightVector();
			                fc.ambientColor = m_renderer.GetAmbientLightVector();
			                fc.sunColor = m_renderer.GetSunColorVector();
			                fc.skyHorizonColor = m_renderer.GetSkyHorizonColorVector();
			                fc.skyZenithColor = m_renderer.GetSkyZenithColorVector();
			                fc.skyVoidColor = m_renderer.GetSkyVoidColorVector();

			                const auto frameIdx = static_cast<std::uint32_t>(m_frameIndex % Swapchain::kMaxFramesInFlight);
			                m_lightingManager.UpdateForView(frameIdx, VK_NULL_HANDLE, *cam, rit->second.extent, fc, m_lightingManager.IsRttBinningEnabled());
			                rit->second.constants->Write(frameIdx, fc);
			                const VkDeviceAddress frameAddr = rit->second.constants->GetDeviceAddress(frameIdx);

			                rit->second.renderQueue.PrepareAndDispatch(ctx.recorder.GetCommandBuffer(), frameAddr, cullPipeline, cullLayout, ctx.frameIndex);
		                });

		// ── Graphics draw pass ────────────────────────────────────────────────
		// Issues DrawIndexedIndirect per batch using the GPU-written indirect
		// buffer produced by the preceding compute cull pass.
		m_renderGraph.AddPass("$CameraRT_" + idStr)
		        .WriteColor(color, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ClearColorValue(0.02f, 0.02f, 0.03f, 1.0f))
		        .WriteDepth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, ClearDepthValue(1.0f))
		        .SetExtent(extent)
		        .Execute(
		                [this, id](PassContext& ctx)
		                {
			                auto rit = m_rtCameras.find(id);
			                if (rit == m_rtCameras.end())
				                return;
			                if (!rit->second.constants)
				                return;

			                const auto frameIdx = static_cast<std::uint32_t>(m_frameIndex % Swapchain::kMaxFramesInFlight);
			                rit->second.renderQueue.FlushDraw(ctx.recorder, m_bindlessManager.GetSet(), m_lightingManager.GetSet(frameIdx));
			                rit->second.renderQueue.Clear(static_cast<std::uint32_t>(ctx.frameIndex % RenderQueue::kFramesInFlight));
		                });
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
		for (auto& [id, rt]: m_rtCameras)
		{
			(void) id;
			rt.renderQueue.SetWriteSlot(drawSlot);
			m_scene.FlushToQueue(rt.renderQueue);
			m_world.FlushToQueue(rt.renderQueue);
		}

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

			// Build FrameConstants from the snapshotted packet state.
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
				// Backward compatibility path (no main camera).
				fc.viewProj = m_scene.GetViewProjection();
			}

			fc.materialBufferAddr = packet.materialBufferAddr;
			fc.sunDirectionIntensity = packet.sunDirectionIntensity;
			fc.ambientColor = packet.ambientColor;
			fc.sunColor = packet.sunColor;
			fc.skyHorizonColor = packet.skyHorizonColor;
			fc.skyZenithColor = packet.skyZenithColor;
			fc.skyVoidColor = packet.skyVoidColor;

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
				fc.tiledLightGridInfo = glm::uvec4(0u);
				fc.tiledLightBufferOffsets = glm::uvec4(0u);
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

		CameraRtEntry rt{};
		rt.camera = camera;
		rt.extent = extent;

		rt.colorImage = UniqueImage::Create(m_vulkanContext.GetDevice().device,
		        m_vulkanContext.GetAllocator(),
		        {
		                .extent = extent,
		                .format = GetForwardColorFormat(),
		                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		        });
		rt.colorImage.EnsureBindlessSampled(m_bindlessManager, m_vulkanContext.GetDevice().device, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		rt.depthImage = UniqueImage::Create(m_vulkanContext.GetDevice().device,
		        m_vulkanContext.GetAllocator(),
		        {
		                .extent = extent,
		                .format = m_swapchain.GetDepthFormat(),
		                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		        });

		rt.rgColor = m_renderGraph.RegisterImage(rt.colorImage.Get(), rt.colorImage.GetDefaultView(), VK_IMAGE_ASPECT_COLOR_BIT);
		rt.rgDepth = m_renderGraph.RegisterImage(rt.depthImage.Get(), rt.depthImage.GetDefaultView(), VK_IMAGE_ASPECT_DEPTH_BIT);

		rt.constants = std::make_unique<FrameConstantsBuffer>();
		rt.constants->Initialize(m_vulkanContext);
		rt.renderQueue.Initialize(m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator());

		const uint32_t id = m_nextRtId++;
		m_rtCameras.emplace(id, std::move(rt));
		RegisterRttPassesFor(id);

		return CameraRenderTarget{ id };
	}

	void AetherCore::DestroyCameraRenderTarget(const CameraRenderTarget rt)
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

		it->second.renderQueue.Shutdown();
		if (it->second.constants)
		{
			it->second.constants->Shutdown();
		}
		it->second.depthImage.Reset();
		it->second.colorImage.Reset();

		m_rtCameras.erase(it);
	}

	RGImage AetherCore::GetRenderTargetColorImage(const CameraRenderTarget rt) const
	{
		auto it = m_rtCameras.find(rt.id);
		if (it == m_rtCameras.end())
		{
			return {};
		}
		return it->second.rgColor;
	}

	uint32_t AetherCore::GetRenderTargetBindlessSlot(const CameraRenderTarget rt) const
	{
		auto it = m_rtCameras.find(rt.id);
		if (it == m_rtCameras.end() || !it->second.colorImage.HasBindlessSampled())
		{
			return 0xFFFFFFFFu;
		}
		return it->second.colorImage.GetBindlessSampledSlot();
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
		return GraphicsPipeline::Create(m_vulkanContext.GetDevice().device, desc);
	}

	Mesh AetherCore::CreateMesh(std::span<const Mesh::Vertex> vertices)
	{
		return Mesh::Create(m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator(), m_vulkanContext.GetGraphicsQueue(), m_uploadPool, vertices);
	}

	Mesh AetherCore::CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices)
	{
		return Mesh::Create(m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator(), m_vulkanContext.GetGraphicsQueue(), m_uploadPool, vertices, indices);
	}

	Texture AetherCore::CreateTexture(std::string_view path)
	{
		return Texture::LoadFromFile(path, m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator(), m_vulkanContext.GetGraphicsQueue(), m_uploadPool, m_bindlessManager);
	}

	void AetherCore::RegisterMaterial(Material& mat)
	{
		if (mat.materialSlot != Material::kNoTexture)
		{
			// Already registered — just refresh the GPU copy.
			GpuMaterial gpu{};
			gpu.baseColorFactor = mat.baseColorFactor;
			gpu.metallicFactor = mat.metallicFactor;
			gpu.roughnessFactor = mat.roughnessFactor;
			gpu.occlusionStrength = mat.occlusionStrength;
			gpu.alphaCutoff = mat.alphaCutoff;
			gpu.emissiveFactor = glm::vec4(mat.emissiveFactor, 0.0f);
			gpu.flags = (mat.doubleSided ? GpuMaterial::kDoubleSided : 0u) | (mat.alphaBlend ? GpuMaterial::kAlphaBlend : 0u) | (mat.alphaMask ? GpuMaterial::kAlphaMask : 0u);
			gpu.albedoSlot = mat.albedoSlot;
			gpu.normalSlot = mat.normalSlot;
			gpu.metallicRoughnessSlot = mat.metallicRoughnessSlot;
			gpu.occlusionSlot = mat.occlusionSlot;
			gpu.emissiveSlot = mat.emissiveSlot;
			m_materialBuffer.Write(mat.materialSlot, gpu);
			return;
		}

		const std::uint32_t slot = m_materialBuffer.AllocateSlot();
		if (slot == MaterialBuffer::kInvalidSlot)
		{
			WARN(LogCategory::Engine,
			        "RegisterMaterial: MaterialBuffer is full — "
			        "material will render as default.");
			return;
		}

		GpuMaterial gpu{};
		gpu.baseColorFactor = mat.baseColorFactor;
		gpu.metallicFactor = mat.metallicFactor;
		gpu.roughnessFactor = mat.roughnessFactor;
		gpu.occlusionStrength = mat.occlusionStrength;
		gpu.alphaCutoff = mat.alphaCutoff;
		gpu.emissiveFactor = glm::vec4(mat.emissiveFactor, 0.0f);
		gpu.flags = (mat.doubleSided ? GpuMaterial::kDoubleSided : 0u) | (mat.alphaBlend ? GpuMaterial::kAlphaBlend : 0u) | (mat.alphaMask ? GpuMaterial::kAlphaMask : 0u);
		gpu.albedoSlot = mat.albedoSlot;
		gpu.normalSlot = mat.normalSlot;
		gpu.metallicRoughnessSlot = mat.metallicRoughnessSlot;
		gpu.occlusionSlot = mat.occlusionSlot;
		gpu.emissiveSlot = mat.emissiveSlot;

		m_materialBuffer.Write(slot, gpu);
		mat.materialSlot = slot;
	}

	void AetherCore::UnregisterMaterial(Material& mat)
	{
		if (mat.materialSlot == Material::kNoTexture)
		{
			return;
		}
		m_materialBuffer.FreeSlot(mat.materialSlot);
		mat.materialSlot = Material::kNoTexture;
	}

	LoadedModel AetherCore::LoadModel(std::string_view path)
	{
		const assets::GltfAsset source = assets::GltfAsset::LoadFromVfsPath(path);
		LoadedModel loaded;

		std::vector<std::uint32_t> imageSlots(source.images.size(), Material::kNoTexture);
		loaded.textures.reserve(source.images.size());
		for (std::size_t imageIndex = 0; imageIndex < source.images.size(); ++imageIndex)
		{
			const assets::GltfImage& image = source.images[imageIndex];
			if (image.uri.empty() || std::string_view(image.uri).starts_with("data:"))
			{
				continue;
			}

			Texture texture = CreateTexture(image.uri);

			imageSlots[imageIndex] = texture.GetBindlessSlot();
			loaded.textures.push_back(std::move(texture));
		}

		std::vector<glm::mat4> localNodeTransforms(source.nodes.size(), glm::mat4(1.0f));
		for (std::size_t nodeIndex = 0; nodeIndex < source.nodes.size(); ++nodeIndex)
		{
			const assets::GltfNode& node = source.nodes[nodeIndex];
			if (node.hasMatrix)
			{
				localNodeTransforms[nodeIndex] = node.matrix;
				continue;
			}

			const glm::mat4 t = glm::translate(glm::mat4(1.0f), node.translation);
			const glm::mat4 r = glm::mat4_cast(node.rotation);
			const glm::mat4 s = glm::scale(glm::mat4(1.0f), node.scale);
			localNodeTransforms[nodeIndex] = t * r * s;
		}

		std::vector<glm::mat4> worldNodeTransforms(source.nodes.size(), glm::mat4(1.0f));
		for (std::size_t nodeIndex = 0; nodeIndex < source.nodes.size(); ++nodeIndex)
		{
			glm::mat4 transform = localNodeTransforms[nodeIndex];
			std::int32_t parent = source.nodes[nodeIndex].parentIndex;
			while (parent >= 0)
			{
				transform = localNodeTransforms[static_cast<std::size_t>(parent)] * transform;
				parent = source.nodes[static_cast<std::size_t>(parent)].parentIndex;
			}
			worldNodeTransforms[nodeIndex] = transform;
		}

		loaded.primitives.reserve(source.primitives.size());
		for (const assets::GltfPrimitive& primitive: source.primitives)
		{
			if (primitive.vertices.empty())
			{
				continue;
			}

			LoadedModelPrimitive loadedPrim;
			loadedPrim.mesh = CreateMesh(primitive.vertices, primitive.indices);

			loadedPrim.skinIndex = primitive.skinIndex;

			// For skinned primitives the skin matrices handle node placement;
			// leave localTransform as identity so the scene-level model matrix alone
			// positions the mesh.
			if (primitive.skinIndex < 0 && primitive.nodeIndex < worldNodeTransforms.size())
			{
				loadedPrim.localTransform = worldNodeTransforms[primitive.nodeIndex];
			}

			if (primitive.materialIndex >= 0 && static_cast<std::size_t>(primitive.materialIndex) < source.materials.size())
			{
				const assets::GltfMaterial& srcMat = source.materials[static_cast<std::size_t>(primitive.materialIndex)];
				Material& mat = loadedPrim.material;

				// ── PBR factors ─────────────────────────────────────────────
				mat.baseColorFactor = srcMat.baseColorFactor;
				mat.metallicFactor = srcMat.metallicFactor;
				mat.roughnessFactor = srcMat.roughnessFactor;
				mat.emissiveFactor = srcMat.emissiveFactor;
				mat.alphaCutoff = srcMat.alphaCutoff;
				mat.doubleSided = srcMat.doubleSided;
				mat.alphaBlend = srcMat.alphaBlend;
				mat.alphaMask = srcMat.alphaMask;

				// Helper: resolve texture → bindless image slot
				auto resolveSlot = [&](std::int32_t texIdx) -> std::uint32_t
				{
					if (texIdx < 0 || static_cast<std::size_t>(texIdx) >= source.textures.size())
					{
						return Material::kNoTexture;
					}
					const assets::GltfTexture& tex = source.textures[static_cast<std::size_t>(texIdx)];
					if (tex.imageIndex < 0 || static_cast<std::size_t>(tex.imageIndex) >= imageSlots.size())
					{
						return Material::kNoTexture;
					}
					return imageSlots[static_cast<std::size_t>(tex.imageIndex)];
				};

				mat.albedoSlot = resolveSlot(srcMat.baseColorTexture);
				mat.normalSlot = resolveSlot(srcMat.normalTexture);
				mat.metallicRoughnessSlot = resolveSlot(srcMat.metallicRoughnessTexture);
				mat.occlusionSlot = resolveSlot(srcMat.occlusionTexture);
				mat.emissiveSlot = resolveSlot(srcMat.emissiveTexture);

				RegisterMaterial(mat);
			}

			loaded.primitives.push_back(std::move(loadedPrim));
		}

		INFO(LogCategory::Engine, "Loaded glTF '{}': {} primitive(s), {} texture(s), {} animation(s).", std::string(path), loaded.primitives.size(), loaded.textures.size(), source.animations.size());

		// Build animator and GPU animation database when the asset has skins.
		if (!source.skins.empty())
		{
			loaded.animator = ModelAnimator::Create(m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator(), source);

			// Create GPU-friendly animation database for GPU clip sampling.
			if (!source.animations.empty())
			{
				loaded.animationDb = AnimationDatabase::Create(m_vulkanContext.GetDevice().device, m_vulkanContext.GetAllocator(), source);
			}
		}

		return loaded;
	}

	std::vector<Entity> AetherCore::SpawnModel(LoadedModel& model, GraphicsPipeline& pipeline, float scale)
	{
		std::vector<Entity> entities;
		entities.reserve(model.primitives.size());

		if (model.animationDb.IsValid())
		{
			m_renderQueue.SetAnimationDatabase(&model.animationDb);
			for (auto& [_, rt]: m_rtCameras)
			{
				rt.renderQueue.SetAnimationDatabase(&model.animationDb);
			}
		}

		const glm::mat4 scaleMat = glm::scale(glm::mat4(1.0f), glm::vec3(scale));

		for (const LoadedModelPrimitive& primitive: model.primitives)
		{
			const Entity entity = aether::ecs::SpawnMesh(m_world, pipeline, primitive.mesh, primitive.material, scaleMat * primitive.localTransform);

			if (model.animator && primitive.skinIndex >= 0)
			{
				const VkDeviceAddress addr = model.animator->GetSkinBufferAddr(primitive.skinIndex);
				const std::uint32_t joints = model.animator->GetSkinJointCount(primitive.skinIndex);
				if (addr != 0 && joints > 0)
					m_world.EmplaceOrReplace<SkinComponent>(entity, SkinComponent{ .sourceSkinBufferAddr = addr, .skinIndex = primitive.skinIndex, .jointCount = joints });
			}

			entities.push_back(entity);
		}

		return entities;
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
