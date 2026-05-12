#include "scene/AetherCore.hpp"

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
#include "rendering/FrameConstants.hpp"
#include "rendering/WorldRenderer.hpp"
#include "scene/EcsHelpers.hpp"
#include "scene/World.hpp"
#include "FileSystem.hpp"
#include "material/BindlessManager.hpp"
#include "material/MaterialBuffer.hpp"
#include "rendering/RenderThread.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	AetherCore::AetherCore(const Config& config)
	      : AetherCore(config, EngineSettingsIO::LoadOrCreate(config.settingsFile))
	{
	}

	AetherCore::AetherCore(const Config& config, const EngineSettings& settings)
	{
		io::FileSystem::InitializeDefaultMounts();
		m_settings = settings;
		m_settings.window.width = config.width;
		m_settings.window.height = config.height;

		// ── 1. Platform ─────────────────────────────────────────────────────
		m_platform.Init({ .appName = config.appName, .width = config.width, .height = config.height });
		m_services.Register<Window>(m_platform.GetWindow());
		m_services.Register<Input>(m_platform.GetInput());

		// ── 2. Graphics device ──────────────────────────────────────────────
		m_gfx.Init(m_services, { .appName = config.appName, .enableVsync = config.enableVsync });
		m_services.Register<VulkanContext>(m_gfx.GetVulkanContext());
		m_services.Register<Swapchain>(m_gfx.GetSwapchain());
		m_services.Register<ResourcePool>(m_gfx.GetResourcePool());
		m_services.Register<BindlessManager>(m_gfx.GetBindlessManager());

		// ── 3. Scene (ECS + legacy) ─────────────────────────────────────────
		m_sceneSub.Init();
		m_services.Register<World>(m_sceneSub.GetWorld());
		m_services.Register<Scene>(m_sceneSub.GetScene());

		// ── 4. Assets ───────────────────────────────────────────────────────
		m_assetsSub.Init(m_services);
		m_services.Register<AssetManager>(m_assetsSub.GetAssetManager());
		m_services.Register<MeshArena>(m_assetsSub.GetMeshArena());
		m_services.Register<MeshUploadQueue>(m_assetsSub.GetMeshUploadQueue());
		m_services.Register<MaterialBuffer>(m_assetsSub.GetMaterialBuffer());
		m_services.Register<AssetSubsystem>(m_assetsSub);

		// ── 5. Cameras ──────────────────────────────────────────────────────
		m_cameras.Init(m_services);
		m_services.Register<CameraManager>(m_cameras.GetCameraManager());
		m_services.Register<LightingManager>(m_cameras.GetLightingManager());

		// ── 6. Rendering ────────────────────────────────────────────────────
		m_rendering.Init(m_services);
		m_rendering.SetFrameIndexProvider([this]() { return m_frameIndex; });
		m_services.Register<Renderer>(m_rendering.GetRenderer());
		m_services.Register<RenderQueue>(m_rendering.GetRenderQueue());
		m_services.Register<RenderGraph>(m_rendering.GetRenderGraph());
		m_services.Register<ShadowService>(m_rendering.GetShadowService());
		m_services.Register<RenderTargetService>(m_rendering.GetRenderTargetService());

		// ── 7. UI ──────────────────────────────────────────────────────────
		if (config.uiFontPath != nullptr && config.uiFontPath[0] != '\0')
		{
			m_ui.Init(m_services, config.uiFontPath, config.uiPassNamePrefix, config.uiGlyphSize);
			m_services.Register<UIRenderer>(m_ui.GetUiRenderer());
			m_services.Register<ui::UiWorld>(m_ui.GetUiWorld());
			m_services.Register<ui::UiContext>(m_ui.GetUiContext());
			m_services.Register<ui::UiSystem>(m_ui.GetUiSystem());
		}

		// Link cross-subsystem dependencies.
		m_cameras.GetLightingManager().LinkRenderer(m_rendering.GetRenderer());
		m_assetsSub.LinkRenderingDeps(m_services);

		// ── 8. Create default main camera ───────────────────────────────────
		CameraManager& cameras = m_services.Get<CameraManager>();
		const CameraHandle mainCam = cameras.Create(CameraDesc{});
		cameras.SetMainCamera(mainCam);

		// ── 9. Async compute (optional) ─────────────────────────────────────
		VulkanContext& vk = m_gfx.GetVulkanContext();
		m_asyncComputeEnabled = m_settings.graphics.asyncCompute && vk.GetComputeQueue() != VK_NULL_HANDLE;
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
			const VkSemaphoreTypeCreateInfo timelineTypeInfo{
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
				.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
				.initialValue = 0,
			};
			const VkSemaphoreCreateInfo semInfo{
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
				.pNext = &timelineTypeInfo,
			};
			if (vkCreateSemaphore(vk.GetDevice().device, &semInfo, nullptr, &m_computeTimelineSemaphore) != VK_SUCCESS)
			{
				throw std::runtime_error("AetherCore: failed to create compute timeline semaphore.");
			}
			CommandRecorder::SetObjectName(vk.GetDevice().device, reinterpret_cast<std::uint64_t>(m_computeTimelineSemaphore), VK_OBJECT_TYPE_SEMAPHORE, "AsyncCompute.Timeline");

			VkFenceCreateInfo fenceInfo{};
			fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

			for (std::size_t frameI = 0; frameI < m_asyncComputeFrames.size(); ++frameI)
			{
				auto& frame = m_asyncComputeFrames[frameI];
				const VkCommandPoolCreateInfo poolInfo{
					.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					.queueFamilyIndex = vk.GetComputeQueueFamily(),
				};
				if (vkCreateCommandPool(vk.GetDevice().device, &poolInfo, nullptr, &frame.commandPool) != VK_SUCCESS)
				{
					throw std::runtime_error("AetherCore: failed to create async compute command pool.");
				}

				const VkCommandBufferAllocateInfo allocInfo{
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					.commandPool = frame.commandPool,
					.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					.commandBufferCount = 1,
				};
				if (vkAllocateCommandBuffers(vk.GetDevice().device, &allocInfo, &frame.commandBuffer) != VK_SUCCESS)
				{
					throw std::runtime_error("AetherCore: failed to allocate async compute command buffer.");
				}

				if (vkCreateFence(vk.GetDevice().device, &fenceInfo, nullptr, &frame.inFlight) != VK_SUCCESS)
				{
					throw std::runtime_error("AetherCore: failed to create async compute fence.");
				}

				const std::string suffix = "[" + std::to_string(frameI) + "]";
				CommandRecorder::SetObjectName(vk.GetDevice().device, reinterpret_cast<std::uint64_t>(frame.commandBuffer), VK_OBJECT_TYPE_COMMAND_BUFFER, ("AsyncCompute.Cmd" + suffix).c_str());
				CommandRecorder::SetObjectName(vk.GetDevice().device, reinterpret_cast<std::uint64_t>(frame.inFlight), VK_OBJECT_TYPE_FENCE, ("AsyncCompute.Fence" + suffix).c_str());
			}
		}

		INFO(LogCategory::Engine, "Engine core initialized. Bindless sampled-image capacity: {}", m_gfx.GetBindlessManager().GetCapacity());
	}

	AetherCore::~AetherCore()
	{
		VulkanContext& vk = m_gfx.GetVulkanContext();
		VkDevice device = vk.GetDevice().device;
		vkDeviceWaitIdle(device);

		// Async compute resources freed first (VkDevice is still alive).
		for (auto& frame: m_asyncComputeFrames)
		{
			if (frame.inFlight != VK_NULL_HANDLE)
			{
				vkDestroyFence(device, frame.inFlight, nullptr);
				frame.inFlight = VK_NULL_HANDLE;
			}
			if (frame.commandPool != VK_NULL_HANDLE)
			{
				vkDestroyCommandPool(device, frame.commandPool, nullptr);
				frame.commandPool = VK_NULL_HANDLE;
				frame.commandBuffer = VK_NULL_HANDLE;
			}
		}
		if (m_computeTimelineSemaphore != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(device, m_computeTimelineSemaphore, nullptr);
			m_computeTimelineSemaphore = VK_NULL_HANDLE;
		}
		m_asyncComputeEnabled = false;

		// Subsystems free their VMA-backed allocations (VMA still alive).
		m_rendering.Shutdown(m_services);
		m_ui.Shutdown(m_services);
		m_cameras.Shutdown();
		m_assetsSub.Shutdown();
		// SceneSubsystem has no shutdown work.

		// Graphics device shutdown destroys VMA, VkDevice.
		m_gfx.Shutdown();
		m_platform.Shutdown();

		m_services.Clear();
		io::FileSystem::Shutdown();
	}

	void AetherCore::WaitIdle()
	{
		vkDeviceWaitIdle(m_gfx.GetVulkanContext().GetDevice().device);
	}

	bool AetherCore::ShouldClose()
	{
		return m_platform.GetWindow().ShouldClose();
	}

	void AetherCore::PumpEvents()
	{
		m_platform.GetWindow().PollEvents();
	}

	void AetherCore::Tick(const float dt)
	{
		AE_PROFILE_ZONE();
		m_platform.GetInput().Update();
		m_cameras.GetCameraManager().Update(m_platform.GetInput(), dt);
	}

	void AetherCore::BeginFrame()
	{
		AE_PROFILE_ZONE();
		Swapchain& swapchain = m_gfx.GetSwapchain();
		if (swapchain.NeedsRecreation())
		{
			RecreateSwapchain();
		}

		swapchain.BeginFrame(m_gfx.GetVulkanContext().GetDevice().device);
		m_currentRecorder = CommandRecorder(swapchain.GetCurrentCommandBuffer());
	}

	void AetherCore::RecreateSwapchain()
	{
		Swapchain& swapchain = m_gfx.GetSwapchain();
		VulkanContext& vk = m_gfx.GetVulkanContext();

		int w = 0;
		int h = 0;
		glfwGetFramebufferSize(m_platform.GetWindow().GetHandle(), &w, &h);
		while (w == 0 || h == 0)
		{
			glfwWaitEvents();
			glfwGetFramebufferSize(m_platform.GetWindow().GetHandle(), &w, &h);
		}

		vkDeviceWaitIdle(vk.GetDevice().device);
		swapchain.Shutdown(vk.GetDevice().device);
		swapchain.ClearRecreationFlag();
		swapchain.Initialize(vk, m_platform.GetWindow(), m_settings.graphics.vsync);

		m_rendering.RecreateSwapchainResources(m_services);

		if (m_swapchainRecreatedCallback)
		{
			m_swapchainRecreatedCallback(*this);
		}

		INFO(LogCategory::Engine, "Swapchain recreated ({}x{}).", w, h);
	}

	RenderFramePacket AetherCore::PrepareFrame(std::uint32_t drawSlot, std::uint64_t frameIndex)
	{
		AE_PROFILE_ZONE();
		Swapchain& swapchain = m_gfx.GetSwapchain();
		RenderQueue& renderQueue = m_rendering.GetRenderQueue();
		Scene& scene = m_sceneSub.GetScene();
		World& world = m_sceneSub.GetWorld();
		RenderTargetService& rttService = m_rendering.GetRenderTargetService();
		ShadowService& shadowService = m_rendering.GetShadowService();
		MaterialBuffer& materialBuffer = m_assetsSub.GetMaterialBuffer();
		CameraManager& cameras = m_cameras.GetCameraManager();
		Renderer& renderer = m_rendering.GetRenderer();

		renderQueue.SetWriteSlot(drawSlot);
		WorldRenderer::Flush(scene, renderQueue);
		WorldRenderer::Flush(world, renderQueue);

		rttService.PrepareQueues(drawSlot, scene, world);
		shadowService.PrepareQueues(drawSlot, scene, world);

		RenderFramePacket packet;
		packet.frameIndex = frameIndex;
		packet.drawSlot = drawSlot;
		packet.materialBufferAddr = materialBuffer.GetDeviceAddress();

		if (const Camera* cam = cameras.TryGetMainCamera())
		{
			packet.hasCameraData = true;
			packet.view = cam->GetViewMatrix();
			const float aspect = static_cast<float>(swapchain.GetExtent().width) / static_cast<float>(swapchain.GetExtent().height);
			packet.proj = cam->GetProjectionMatrix(aspect);
			packet.cameraWorldPos = glm::vec4(cam->GetPosition(), 1.0f);
		}

		packet.sunDirectionIntensity = renderer.GetDirectionalLightVector();
		packet.ambientColor = renderer.GetAmbientLightVector();
		packet.sunColor = renderer.GetSunColorVector();
		packet.skyHorizonColor = renderer.GetSkyHorizonColorVector();
		packet.skyZenithColor = renderer.GetSkyZenithColorVector();
		packet.skyVoidColor = renderer.GetSkyVoidColorVector();

		return packet;
	}

	void AetherCore::ExecuteRenderFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();
		m_frameIndex = packet.frameIndex;
		BeginFrame();
		EndFrame(packet);
	}

	void AetherCore::EndFrame(const RenderFramePacket& packet)
	{
		AE_PROFILE_ZONE();
		VkSemaphore computeFinished = VK_NULL_HANDLE;

		Swapchain& swapchain = m_gfx.GetSwapchain();
		VulkanContext& vk = m_gfx.GetVulkanContext();

		if (swapchain.IsFrameValid())
		{
			const auto frameIdx = static_cast<std::uint32_t>(packet.frameIndex % Swapchain::kMaxFramesInFlight);

			FrameConstants fc = m_rendering.GetFrameComposer().ComposeBaseFrameConstants(packet, m_sceneSub.GetScene().GetViewProjection());

			m_rendering.GetShadowService().BuildFrameShadowData(packet, frameIdx, m_cameras.GetCameraManager(), fc);

			if (packet.hasCameraData)
			{
				const Camera* cam = m_cameras.GetCameraManager().TryGetMainCamera();
				if (cam)
				{
					const std::uint32_t computeFamily = vk.GetComputeQueueFamily();
					const std::uint32_t graphicsFamily = vk.GetGraphicsQueueFamily();

					VkCommandBuffer lightingCmd = swapchain.GetCurrentCommandBuffer();
					if (m_asyncComputeEnabled)
					{
						auto& asyncFrame = m_asyncComputeFrames[frameIdx];
						vkWaitForFences(vk.GetDevice().device, 1, &asyncFrame.inFlight, VK_TRUE, UINT64_MAX);
						vkResetFences(vk.GetDevice().device, 1, &asyncFrame.inFlight);
						vkResetCommandPool(vk.GetDevice().device, asyncFrame.commandPool, 0);

						const VkCommandBufferBeginInfo beginInfo{
							.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
							.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
						};
						vkBeginCommandBuffer(asyncFrame.commandBuffer, &beginInfo);
						CommandRecorder(asyncFrame.commandBuffer).BeginDebugLabel("AsyncCompute.LightCull", 0.9f, 0.45f, 0.1f);
						lightingCmd = asyncFrame.commandBuffer;
					}

					m_cameras.GetLightingManager().UpdateForView(frameIdx, lightingCmd, *cam, swapchain.GetExtent(), fc, true, computeFamily, graphicsFamily);

					if (m_asyncComputeEnabled)
					{
						auto& asyncFrame = m_asyncComputeFrames[frameIdx];
						CommandRecorder(asyncFrame.commandBuffer).EndDebugLabel();
						vkEndCommandBuffer(asyncFrame.commandBuffer);

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
						vkQueueSubmit(vk.GetComputeQueue(), 1, &submitInfo, asyncFrame.inFlight);
						computeFinished = m_computeTimelineSemaphore;

						if (computeFamily != graphicsFamily)
						{
							m_cameras.GetLightingManager().EmitAcquireBarriers(frameIdx, swapchain.GetCurrentCommandBuffer(), computeFamily, graphicsFamily);
						}
					}
				}
			}
			else
			{
				m_rendering.GetFrameComposer().ApplyNoCameraLightingFallback(fc);
			}

			m_rendering.GetFrameConstantsBuffer().Write(frameIdx, fc);
			const VkDeviceAddress frameAddr = m_rendering.GetFrameConstantsBuffer().GetDeviceAddress(frameIdx);

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
			vkCmdPipelineBarrier2(swapchain.GetCurrentCommandBuffer(), &frameConstantsDep);

			const FrameTarget frameTarget{
				.colorImage = swapchain.GetCurrentImage(),
				.colorView = swapchain.GetCurrentImageView(),
				.depthImage = swapchain.GetDepthImage(),
				.depthView = swapchain.GetDepthImageView(),
				.colorFormat = swapchain.GetImageFormat(),
				.depthFormat = swapchain.GetDepthFormat(),
				.extent = swapchain.GetExtent(),
			};

			m_currentRecorder.BeginDebugLabel("Frame.RenderGraph", 0.35f, 0.55f, 0.95f, 1.0f);
			m_rendering.GetRenderGraph().Execute(swapchain.GetCurrentCommandBuffer(), frameTarget, frameAddr, frameIdx);
			m_currentRecorder.EndDebugLabel();
		}
		{
			std::lock_guard lock(vk.GetGraphicsQueueMutex());
			swapchain.EndFrame(vk.GetGraphicsQueue(), vk.GetPresentQueue(), computeFinished, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, m_computeTimelineValue);
		}
		++m_frameIndex;
		m_gfx.GetBindlessManager().AdvanceFrame(m_frameIndex);
	}

} // namespace aether
