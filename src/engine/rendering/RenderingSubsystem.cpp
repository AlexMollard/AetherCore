#include "rendering/RenderingSubsystem.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

#include "assets/AssetSubsystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "camera/CameraManager.hpp"
#include "rendering/FrameContext.hpp"
#include "rendering/LightingManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "material/MaterialBuffer.hpp"
#include "material/EffectParamBuffer.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/DiagnosticEngine.hpp"

namespace
{
	constexpr std::uint32_t kRenderQueueMaxDraws = 65536;
	constexpr bool kEnableForwardGtao = true;
} // namespace

namespace aether
{
	namespace
	{
		// Render scale is invisible in a frame-time graph on content that is not GPU-bound, so
		// state the target size outright rather than leaving it to be inferred.
		void LogSceneRenderExtent(const gpu::Extent2D scene, const gpu::Extent2D swapchain)
		{
			if (scene.width == swapchain.width && scene.height == swapchain.height)
			{
				return;
			}
			// Which direction it goes matters to whoever reads this: one is a performance
			// trade and the other is antialiasing, and calling both "upscaled" made the log
			// contradict what the setting was asked to do.
			const char* const direction = (scene.width > swapchain.width) ? "downsampled" : "upscaled";
			AE_INFO(LogCategory::Engine, "Scene renders at {}x{}, {} to {}x{} on present.", scene.width, scene.height, direction, swapchain.width, swapchain.height);
		}
	} // namespace

	gpu::Extent2D RenderingSubsystem::ResolveSceneViewportExtent(gpu::Extent2D swapchainExtent) const
	{
		if (!m_sceneViewportEnabled)
		{
			return ApplyRenderScale(swapchainExtent);
		}

		auto clampExtent = [](gpu::Extent2D extent)
		{
			extent.width = std::clamp(extent.width, 64u, 8192u);
			extent.height = std::clamp(extent.height, 64u, 8192u);
			return extent;
		};

		switch (m_sceneViewportSettings.resolutionMode)
		{
			case SceneViewportResolutionMode::Fixed720p:
				return {1280, 720};
			case SceneViewportResolutionMode::Fixed1080p:
				return {1920, 1080};
			case SceneViewportResolutionMode::Fixed1440p:
				return {2560, 1440};
			case SceneViewportResolutionMode::MatchPanel:
			case SceneViewportResolutionMode::Custom:
				return clampExtent(m_sceneViewportSettings.customExtent);
			case SceneViewportResolutionMode::WindowNative:
			default:
				return clampExtent(swapchainExtent);
		}
	}

	gpu::Extent2D RenderingSubsystem::ResolveRequestedSceneViewportExtent(gpu::Extent2D swapchainExtent) const
	{
		auto clampExtent = [](gpu::Extent2D extent)
		{
			extent.width = std::clamp(extent.width, 64u, 8192u);
			extent.height = std::clamp(extent.height, 64u, 8192u);
			return extent;
		};

		SceneViewportSettings settings;
		bool enabled = false;
		{
			const std::lock_guard lock(m_sceneViewportMutex);
			settings = m_requestedSceneViewportSettings;
			enabled = m_requestedSceneViewportEnabled;
		}
		if (!enabled)
		{
			// Must match ResolveSceneViewportExtent above: this one sizes the packet (and so
			// the clustered light binning), that one sizes the post-process chain. If they
			// disagree, lights are culled against a grid that is not the render target.
			return ApplyRenderScale(swapchainExtent);
		}

		switch (settings.resolutionMode)
		{
			case SceneViewportResolutionMode::Fixed720p:
				return {1280, 720};
			case SceneViewportResolutionMode::Fixed1080p:
				return {1920, 1080};
			case SceneViewportResolutionMode::Fixed1440p:
				return {2560, 1440};
			case SceneViewportResolutionMode::MatchPanel:
			case SceneViewportResolutionMode::Custom:
				return clampExtent(settings.customExtent);
			case SceneViewportResolutionMode::WindowNative:
			default:
				return clampExtent(swapchainExtent);
		}
	}

	SceneViewportSettings RenderingSubsystem::GetSceneViewportSettings() const
	{
		const std::lock_guard lock(m_sceneViewportMutex);
		return m_requestedSceneViewportSettings;
	}

	bool RenderingSubsystem::IsSceneViewportEnabled() const
	{
		const std::lock_guard lock(m_sceneViewportMutex);
		return m_requestedSceneViewportEnabled;
	}

	void RenderingSubsystem::DestroySceneViewportDepth()
	{
		// The slot belongs to the graph: RenderGraph::Clear() releases it on the rebuild path
		// and Shutdown() on the teardown path, both of which run after this.
		m_sceneDepthBindlessSlot = 0xFFFFFFFFu;
		m_sceneDepth = {};
	}

	void RenderingSubsystem::RegisterTexturePreviewImage()
	{
		if (!m_texturePreviewHandle.IsValid())
		{
			m_texturePreview = {};
			return;
		}
		m_texturePreview = m_renderGraph.RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_texturePreviewHandle), m_texturePreviewView, gpu::ImageAspect::Color);
	}

	// 16 MB that only the texture inspector's GPU preview can read, and only while it
	// is on screen. Created when the inspector asks for a preview, released when it has
	// stopped asking for long enough.
	void RenderingSubsystem::CreateTexturePreviewImage()
	{
		if (m_texturePreviewHandle.IsValid())
		{
			return;
		}
		const gpu::TextureDesc previewDesc{
		        .format = gpu::Format::R8G8B8A8Unorm,
		        .extent = {kTexturePreviewSize, kTexturePreviewSize},
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "Debug.TexturePreview",
		};
		m_texturePreviewHandle = gpu::ResourceRegistry::CreateTexture(previewDesc);
		if (!m_texturePreviewHandle.IsValid())
		{
			return;
		}
		m_texturePreviewView = gpu::ResourceRegistry::ResolveTexture(m_texturePreviewHandle).view;
		++m_texturePreviewGeneration;
	}

	void RenderingSubsystem::DestroyTexturePreviewImage()
	{
		if (!m_texturePreviewHandle.IsValid())
		{
			return;
		}
		gpu::ResourceRegistry::Destroy(m_texturePreviewHandle);
		m_texturePreviewHandle = {};
		m_texturePreviewView = nullptr;
		m_texturePreview = {};
		++m_texturePreviewGeneration;
	}

	void RenderingSubsystem::ForEachRenderQueue(const std::function<void(RenderQueue&)>& fn)
	{
		fn(m_renderQueue);
		fn(m_shadowService.GetShadowQueue());
		fn(m_localShadowService.GetShadowQueue());
		fn(m_cameraPreview.GetRenderQueue());
		fn(m_modelPreview.GetRenderQueue());
		fn(m_materialThumbnailBaker.GetRenderQueue());
		m_renderTargetService.ForEachRenderQueue(fn);
	}

	bool RenderingSubsystem::HasSkinnedDrawsQueued(const std::uint32_t drawSlot)
	{
		bool any = false;
		ForEachRenderQueue([&any, drawSlot](RenderQueue& queue)
		        {
			        any = queue.HasAnimatedDraws(drawSlot) || any;
		        });
		return any;
	}

	void RenderingSubsystem::PublishContentSignals(const RenderContentSignals& signals)
	{
		if (m_profile != RuntimeProfile::Full)
		{
			return;
		}

		if (m_lazyGates.Publish(signals))
		{
			m_lazyTargetRebuildPending.store(true, std::memory_order_release);
		}
	}

	bool RenderingSubsystem::CommitPendingLazyTargets()
	{
		if (!m_lazyTargetRebuildPending.exchange(false, std::memory_order_acq_rel))
		{
			return false;
		}
		return m_lazyGates.Commit();
	}

	void RenderingSubsystem::ApplyLazyTargetState(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		if (m_profile != RuntimeProfile::Full)
		{
			return;
		}

		auto& gpu = services.Get<GpuDevice>();
		auto& swapchain = services.Get<Swapchain>();
		auto& bindless = services.Get<BindlessManager>();

		if (m_lazyGates.DirectionalShadowTargets())
		{
			m_shadowService.CreateShadowTargets();
		}
		else
		{
			m_shadowService.DestroyShadowTargets();
		}

		if (m_lazyGates.LocalShadowTargets())
		{
			m_localShadowService.CreateShadowTargets();
		}
		else
		{
			m_localShadowService.DestroyShadowTargets();
		}

		// GTAO's targets are sized from the scene viewport, so they are rebuilt rather
		// than kept across a graph reset even when the gate has not moved.
		m_gtaoPass.Destroy();
		if constexpr (kEnableForwardGtao)
		{
			if (m_lazyGates.GtaoTargets())
			{
				m_gtaoPass.Create({
				        .device = gpu.GetDevice(),
				        .extent = ResolveSceneViewportExtent(swapchain.GetExtent()),
				        .bindlessManager = &bindless,
				        .renderGraph = &m_renderGraph,
				});
			}
		}

		if (m_lazyGates.TexturePreviewTarget())
		{
			CreateTexturePreviewImage();
		}
		else
		{
			DestroyTexturePreviewImage();
		}
		RegisterTexturePreviewImage();

		// Only the release direction is driven from here. Each queue creates its own skin
		// palette and pose pools the instant it is handed a skinned draw, on the render
		// thread, which is a frame earlier than this path could manage - and a frame drawn
		// with those pools missing is a broken pose, not a dropped frame. Growing must not
		// wait on a quiesced rebuild; shrinking must.
		if (!m_lazyGates.SkinningBuffers())
		{
			ForEachRenderQueue([](RenderQueue& queue) { queue.ReleaseAnimationBuffers(); });
		}
	}

	void RenderingSubsystem::CreateSceneViewportDepth(gpu::Device device, gpu::Format depthFormat, RenderGraph& graph, BindlessManager& bindless)
	{
		(void) device;
		const gpu::Extent2D extent = m_postProcessStack.GetExtent();
		(void) bindless;
		// Written by $ScenePreDepth, read for the last time by $EngineForward. Nothing outside
		// the graph reads it, so the graph decides where it lives.
		m_sceneDepth = graph.CreateTransientDepth(depthFormat, extent, gpu::ImageUsage::Sampled);
		m_sceneDepthBindlessSlot = graph.EnsureBindlessSampled(m_sceneDepth);
		if (m_sceneDepthBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("RenderingSubsystem: Scene.Depth bindless registration failed"));
		}
		// Camera motion blur reprojects through this in the tonemap pass.
		m_postProcessStack.SetSceneDepthSlot(m_sceneDepthBindlessSlot);

		// Same extent and lifetime as the depth it is written beside.
		m_sceneGBuffer = graph.CreateTransientColor(gpu::Format::R8G8B8A8Unorm, extent, gpu::ImageUsage::Sampled);
		m_sceneGBufferBindlessSlot = graph.EnsureBindlessSampled(m_sceneGBuffer);
		if (m_sceneGBufferBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("RenderingSubsystem: Scene.GBuffer bindless registration failed"));
		}

		// sRGB, not Unorm: base colour is perceptual data, so the hardware encode spends
		// its eight bits where the eye needs them and still hands back linear on read.
		m_sceneBaseColor = graph.CreateTransientColor(gpu::Format::R8G8B8A8Srgb, extent, gpu::ImageUsage::Sampled);
		m_sceneBaseColorBindlessSlot = graph.EnsureBindlessSampled(m_sceneBaseColor);
		if (m_sceneBaseColorBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("RenderingSubsystem: Scene.BaseColor bindless registration failed"));
		}

		m_dofColor = graph.CreateTransientColor(gpu::Format::R16G16B16A16Sfloat, gpu::Extent2D{(extent.width + 1u) / 2u, (extent.height + 1u) / 2u}, gpu::ImageUsage::Sampled);
		m_dofBindlessSlot = graph.EnsureBindlessSampled(m_dofColor);
		if (m_dofBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("RenderingSubsystem: Scene.DepthOfField bindless registration failed"));
		}

		// Half resolution. Rounded up, so an odd extent still covers every full-res
		// pixel rather than leaving a column with no fog.
		const gpu::Extent2D halfExtent{(extent.width + 1u) / 2u, (extent.height + 1u) / 2u};
		m_volumetricFog = graph.CreateTransientColor(gpu::Format::R16G16B16A16Sfloat, halfExtent, gpu::ImageUsage::Sampled);
		m_volumetricBindlessSlot = graph.EnsureBindlessSampled(m_volumetricFog);
		if (m_volumetricBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("RenderingSubsystem: Scene.VolumetricFog bindless registration failed"));
		}

		// HDR, because a reflection carries the same range as what it reflects.
		m_ssrColor = graph.CreateTransientColor(gpu::Format::R16G16B16A16Sfloat, extent, gpu::ImageUsage::Sampled);
		m_ssrBindlessSlot = graph.EnsureBindlessSampled(m_ssrColor);
		if (m_ssrBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("RenderingSubsystem: Scene.SSR bindless registration failed"));
		}
	}

	void RenderingSubsystem::Init(ServiceContainer& services, RuntimeProfile profile)
	{
		AE_PROFILE_ZONE();
		m_profile = profile;
		auto& vk = services.Get<VulkanContext>();
		auto& swapchain = services.Get<Swapchain>();
		auto& bindless = services.Get<BindlessManager>();
		auto& gpu = services.Get<GpuDevice>();
		m_bindlessManager = &bindless;

		m_renderGraph.Initialize(static_cast<void*>(vk.GetDevice().device), static_cast<void*>(vk.GetAllocator()));
		m_renderGraph.SetVulkanContext(&vk);
		m_renderGraph.SetDiagnosticEngine(&services.Get<DiagnosticEngine>());
		m_frameConstantsBuffer.Initialize();

		for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			const gpu::MappedBufferDesc desc{
			        .size = sizeof(ResourceEntry) * kFrameResourceCount,
			        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
			        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
			        .debugName = "ResourceTable",
			};
			m_resourceTableBuffers[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
			if (!m_resourceTableBuffers[i].handle.IsValid())
			{
				Throw(AetherError::Engine("RenderingSubsystem: ResourceTable CreateMappedBuffer failed"));
			}
			const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_resourceTableBuffers[i].handle);
			m_resourceTableBuffers[i].mapped = view.mappedPtr;
			m_resourceTableBuffers[i].address = view.deviceAddress;
		}

		m_renderQueuePipelines.Initialize(vk.GetDevice().device);

		m_renderQueue.Initialize(m_renderQueuePipelines, RenderQueueConfig{.maxDraws = kRenderQueueMaxDraws, .debugName = "Main"});
		m_renderQueue.SetDebugForceVisible(true);
		m_renderQueue.SetDebugBypassIndirect(false);
		m_renderQueue.SetDebugDisableAnimation(false);
		m_renderQueue.SetDebugAnimPassMask(0xFu);

		auto& assets = services.Get<AssetSubsystem>();
		m_uiRenderer.Init(gpu, assets.GetUploadContext(), assets.GetTextureRegistry(), swapchain.GetImageFormat());
		services.Register<ui::FontRegistry>(m_uiRenderer.Fonts());

		if (m_profile != RuntimeProfile::Full)
		{
			return;
		}

		m_renderer2D.Initialize(gpu, PostProcessStack::GetForwardColorFormat());
		m_customPassRenderer.Initialize(gpu);
		m_light2D.Initialize(gpu, PostProcessStack::GetForwardColorFormat());

		auto& cameras = services.Get<CameraManager>();
		auto& lighting = services.Get<LightingManager>();
		auto& materials = services.Get<MaterialBuffer>();
		auto& effectParams = services.Get<EffectParamBuffer>();

		m_shadowService.Initialize(vk, swapchain, bindless, m_renderQueuePipelines);
		m_localShadowService.Initialize(vk, bindless, swapchain, m_renderQueuePipelines);
		m_cameraPreview.Initialize(vk, bindless, m_renderQueuePipelines, PostProcessStack::GetForwardColorFormat(), swapchain.GetDepthFormat());
		m_modelPreview.Initialize(vk, bindless, m_renderQueuePipelines, PostProcessStack::GetForwardColorFormat(), swapchain.GetDepthFormat());
		// 128px: a content-browser tile is around a hundred pixels, and a thumbnail per
		// material adds up, so it is baked at the size it is shown rather than the
		// interactive preview's 384.
		m_materialThumbnailBaker.Initialize(vk, bindless, m_renderQueuePipelines, PostProcessStack::GetForwardColorFormat(), swapchain.GetDepthFormat(), 128u, "$MaterialThumb", 8u);
		// A thumbnail is a still, and it is compared against its neighbours: no turntable,
		// and neutral studio light rather than whatever the open level looks like.
		m_materialThumbnailBaker.SetTurntableEnabled(false);
		m_materialThumbnailBaker.SetSceneEnvironmentEnabled(false);
		m_renderTargetService.Initialize(vk, m_renderQueuePipelines);
		m_cullPass.Initialize(vk.GetDevice().device);

		m_postProcessStack = PostProcessStack::Create({
		        .device = vk.GetDevice().device,
		        .extent = ResolveSceneViewportExtent(swapchain.GetExtent()),
		        .swapchainFormat = swapchain.GetImageFormat(),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});
		LogSceneRenderExtent(m_postProcessStack.GetExtent(), swapchain.GetExtent());
		CreateSceneViewportDepth(vk.GetDevice().device, swapchain.GetDepthFormat(), m_renderGraph, bindless);

		m_renderer.Initialize(&m_postProcessStack);
		m_renderer.AttachGtaoPass(&m_gtaoPass);

		{
			AE_EXPECT_OR_THROW(texturePreviewPipeline,
			        GraphicsPipeline::Create(vk.GetDevice().device,
			                {
			                        .shaderVfsPath = "shaders://texture_preview.spv",
			                        .colorFormat = gpu::Format::R8G8B8A8Unorm,
			                        .debugName = "TexturePreview",
			                        .descriptorHeapMappings = bindless.GetDescriptorHeapMappings(),
			                }));
			m_texturePreviewPipeline = std::move(texturePreviewPipeline);
		}

		AE_EXPECT_OR_THROW(skyboxPipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://skybox.spv",
		                        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		                        .depthTestEnable = false,
		                        .depthWriteEnable = false,
		                        .debugName = "Skybox",
		                }));
		m_skyboxPipeline = std::move(skyboxPipeline);

		AE_EXPECT_OR_THROW(preDepthPipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://shadow_depth.spv",
		                        .colorFormat = gpu::Format::Undefined,
		                        .depthFormat = swapchain.GetDepthFormat(),
		                        .depthTestEnable = true,
		                        .depthWriteEnable = true,
		                        .depthCompareOp = gpu::CompareOp::LessOrEqual,
		                        .debugName = "Scene.PreDepth",
		                }));
		m_preDepthPipeline = std::move(preDepthPipeline);

		// Same geometry as the depth-only pipeline above, plus the thin G-buffer. Kept
		// separate because shadow_depth.spv is shared with the shadow passes, which have
		// no colour attachment to write.
		AE_EXPECT_OR_THROW(prepassPipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://scene_prepass.spv",
		                        .colorFormat = gpu::Format::R8G8B8A8Unorm,
		                        // Surface buffer and base colour. The formats come from
		                        // vkCmdBeginRendering; the pipeline only needs the count, so
		                        // that blend state and write masks reach the second target.
		                        .colorAttachmentCount = 2,
		                        .depthFormat = swapchain.GetDepthFormat(),
		                        .depthTestEnable = true,
		                        .depthWriteEnable = true,
		                        .depthCompareOp = gpu::CompareOp::LessOrEqual,
		                        .debugName = "Scene.Prepass",
		                        .descriptorHeapMappings = bindless.GetDescriptorHeapMappings(),
		                }));
		m_prepassPipeline = std::move(prepassPipeline);

		AE_EXPECT_OR_THROW(ssrPipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://ssr.spv",
		                        .colorFormat = gpu::Format::R16G16B16A16Sfloat,
		                        .depthTestEnable = false,
		                        .depthWriteEnable = false,
		                        .debugName = "SSR",
		                        .descriptorHeapMappings = bindless.GetDescriptorHeapMappings(),
		                }));
		m_ssrPipeline = std::move(ssrPipeline);

		// Additive: the SSR buffer is premultiplied by its own confidence, so adding it
		// contributes nothing where a ray failed and leaves the sky probe showing.
		AE_EXPECT_OR_THROW(ssrCompositePipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://ssr_composite.spv",
		                        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		                        .depthTestEnable = false,
		                        .depthWriteEnable = false,
		                        .blendEnable = true,
		                        .blendMode = gpu::BlendMode::Additive,
		                        .debugName = "SSR.Composite",
		                        .descriptorHeapMappings = bindless.GetDescriptorHeapMappings(),
		                }));
		m_ssrCompositePipeline = std::move(ssrCompositePipeline);

		AE_EXPECT_OR_THROW(volumetricPipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://volumetric_fog.spv",
		                        .colorFormat = gpu::Format::R16G16B16A16Sfloat,
		                        .depthTestEnable = false,
		                        .depthWriteEnable = false,
		                        .debugName = "Volumetric.Fog",
		                        .descriptorHeapMappings = bindless.GetDescriptorHeapMappings(),
		                }));
		m_volumetricPipeline = std::move(volumetricPipeline);

		// Premultiplied, because that IS the medium's compositing rule: the scattered
		// light is added whole and the scene behind it is attenuated by what the air
		// hides. One blend, and it reproduces the lerp the analytic path did.
		AE_EXPECT_OR_THROW(volumetricCompositePipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://volumetric_composite.spv",
		                        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		                        .depthTestEnable = false,
		                        .depthWriteEnable = false,
		                        .blendEnable = true,
		                        .blendMode = gpu::BlendMode::Premultiplied,
		                        .debugName = "Volumetric.Composite",
		                        .descriptorHeapMappings = bindless.GetDescriptorHeapMappings(),
		                }));
		m_volumetricCompositePipeline = std::move(volumetricCompositePipeline);

		AE_EXPECT_OR_THROW(dofPipeline,
		        GraphicsPipeline::Create(vk.GetDevice().device,
		                {
		                        .shaderVfsPath = "shaders://dof.spv",
		                        .colorFormat = gpu::Format::R16G16B16A16Sfloat,
		                        .depthTestEnable = false,
		                        .depthWriteEnable = false,
		                        .debugName = "DepthOfField",
		                        .descriptorHeapMappings = bindless.GetDescriptorHeapMappings(),
		                }));
		m_dofPipeline = std::move(dofPipeline);

		m_renderTargetService.BindRuntime(FrameContext{
		        .graph = &m_renderGraph,
		        .bindless = &bindless,
		        .cameras = &cameras,
		        .lighting = &lighting,
		        .renderer = &m_renderer,
		        .materials = &materials,
		        .effectParams = &effectParams,
		        .cullPass = &m_cullPass,
		        .frameIndex = [this]() { return m_frameIndexProvider ? m_frameIndexProvider() : 0ull; },
		        .depthFormat = swapchain.GetDepthFormat(),
		        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		        .featureFlags = {.forwardEnabled = IsForwardPassEnabled()},
		});

		m_physicsDebug.Init(gpu, swapchain.GetImageFormat(), swapchain.GetDepthFormat());
	}

	void RenderingSubsystem::Shutdown(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();

		// Registered non-owning in Init and it lives inside m_uiRenderer, which is torn down at
		// the end of this function - so the container must not go on handing out a pointer to it.
		services.Unregister<ui::FontRegistry>();

		// were never initialized, so their Destroy/Shutdown must be skipped - several
		if (m_profile == RuntimeProfile::Full)
		{
			m_renderer2D.Shutdown();
			m_customPassRenderer.Shutdown();
			m_light2D.Shutdown();
			DestroySceneViewportDepth();
			m_gtaoPass.Destroy();
			m_postProcessStack.Destroy();
			m_texturePreviewPipeline.Destroy();
			if (m_texturePreviewHandle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(m_texturePreviewHandle);
				m_texturePreviewHandle = {};
			}
			m_preDepthPipeline.Destroy();
			m_skyboxPipeline.Destroy();
			m_cullPass.Shutdown();
			m_cameraPreview.Shutdown();
			m_modelPreview.Shutdown(nullptr);
			m_materialThumbnailBaker.Shutdown(nullptr);
			m_shadowService.Shutdown();
			m_localShadowService.Shutdown();
			m_renderTargetService.Shutdown();
			m_physicsDebug.Shutdown();
		}

		m_frameConstantsBuffer.Shutdown();

		for (auto& buf: m_resourceTableBuffers)
		{
			if (buf.handle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(buf.handle);
			}
			buf.handle = {};
			buf.mapped = nullptr;
			buf.address = 0;
		}

		m_renderQueue.Shutdown();
		m_renderGraph.Shutdown();
		m_renderQueuePipelines.Shutdown();
		m_uiRenderer.Shutdown();
		m_bindlessManager = nullptr;
	}

	void RenderingSubsystem::RecreateSwapchainResources(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		auto& gpu = services.Get<GpuDevice>();
		auto& swapchain = services.Get<Swapchain>();
		auto& bindless = services.Get<BindlessManager>();

		gpu.WaitIdle();

		if (m_profile != RuntimeProfile::Full)
		{
			m_renderGraph.Clear();
			RegisterPasses(services);
			return;
		}

		m_shadowService.RecreatePipeline(gpu.GetDevice(), swapchain.GetDepthFormat());
		m_preDepthPipeline.Destroy();
		AE_EXPECT_OR_THROW(preDepthPipeline,
		        GraphicsPipeline::Create(gpu.GetDevice(),
		                {
		                        .shaderVfsPath = "shaders://shadow_depth.spv",
		                        .colorFormat = gpu::Format::Undefined,
		                        .depthFormat = swapchain.GetDepthFormat(),
		                        .depthTestEnable = true,
		                        .depthWriteEnable = true,
		                        .depthCompareOp = gpu::CompareOp::LessOrEqual,
		                        .debugName = "Scene.PreDepth",
		                }));
		m_preDepthPipeline = std::move(preDepthPipeline);

		const TonemapMode tonemapMode = m_postProcessStack.GetTonemapMode();
		const float exposure = m_postProcessStack.GetExposure();
		const bool fxaaEnabled = m_postProcessStack.IsFxaaEnabled();
		const float gradeContrast = m_postProcessStack.GetGradeContrast();
		const float gradeSaturation = m_postProcessStack.GetGradeSaturation();
		const float gradeTemperature = m_postProcessStack.GetGradeTemperature();
		const float gradeTint = m_postProcessStack.GetGradeTint();
		const float vignetteIntensity = m_postProcessStack.GetVignetteIntensity();
		const float vignetteRoundness = m_postProcessStack.GetVignetteRoundness();
		const float chromaticAberration = m_postProcessStack.GetChromaticAberration();
		const float sharpness = m_postProcessStack.GetSharpness();
		const float filmGrain = m_postProcessStack.GetFilmGrain();
		const float motionBlurStrength = m_postProcessStack.GetMotionBlurStrength();
		const float motionBlurMaxRadius = m_postProcessStack.GetMotionBlurMaxRadiusPixels();
		const float bloomStrength = m_postProcessStack.GetBloomStrength();
		const float bloomFilterRadius = m_postProcessStack.GetBloomFilterRadius();
		const bool autoExposureEnabled = m_postProcessStack.IsAutoExposureEnabled();
		const float autoExposureKey = m_postProcessStack.GetAutoExposureKey();
		const float autoExposureSpeed = m_postProcessStack.GetAutoExposureSpeed();
		const bool histogramCaptureEnabled = m_postProcessStack.IsHistogramCaptureEnabled();
		const std::uint32_t histogramUpdatePeriod = m_postProcessStack.GetHistogramUpdatePeriod();
		const std::uint32_t histogramSampleStride = m_postProcessStack.GetHistogramSampleStride();

		m_renderQueue.DiscardAllPending();
		m_shadowService.ClearAllQueues();
		m_localShadowService.ClearAllQueues();
		m_renderTargetService.ClearAllQueues();
		DestroySceneViewportDepth();
		m_postProcessStack.Destroy();
		m_renderGraph.Clear();
		m_postProcessStack = PostProcessStack::Create({
		        .device = gpu.GetDevice(),
		        .extent = ResolveSceneViewportExtent(swapchain.GetExtent()),
		        .swapchainFormat = swapchain.GetImageFormat(),
		        .bindlessManager = &bindless,
		        .renderGraph = &m_renderGraph,
		});
		LogSceneRenderExtent(m_postProcessStack.GetExtent(), swapchain.GetExtent());
		CreateSceneViewportDepth(gpu.GetDevice(), swapchain.GetDepthFormat(), m_renderGraph, bindless);
		m_postProcessStack.SetTonemapMode(tonemapMode);
		m_postProcessStack.SetExposure(exposure);
		m_postProcessStack.SetFxaaEnabled(fxaaEnabled);
		m_postProcessStack.SetGrade(gradeContrast, gradeSaturation, gradeTemperature, gradeTint);
		m_postProcessStack.SetVignette(vignetteIntensity, vignetteRoundness);
		m_postProcessStack.SetChromaticAberration(chromaticAberration);
		m_postProcessStack.SetSharpness(sharpness);
		m_postProcessStack.SetFilmGrain(filmGrain);
		m_postProcessStack.SetMotionBlur(motionBlurStrength, motionBlurMaxRadius);
		m_postProcessStack.SetBloomStrength(bloomStrength);
		m_postProcessStack.SetBloomFilterRadius(bloomFilterRadius);
		m_postProcessStack.SetAutoExposureEnabled(autoExposureEnabled);
		m_postProcessStack.SetAutoExposureKey(autoExposureKey);
		m_postProcessStack.SetAutoExposureSpeed(autoExposureSpeed);
		m_postProcessStack.SetHistogramCaptureEnabled(histogramCaptureEnabled);
		m_postProcessStack.SetHistogramUpdatePeriod(histogramUpdatePeriod);
		m_postProcessStack.SetHistogramSampleStride(histogramSampleStride);
		m_postProcessStack.SetOutputToTexture(m_sceneViewportEnabled);

		m_renderTargetService.OnRenderGraphReset(gpu.GetDevice(), swapchain.GetDepthFormat(), PostProcessStack::GetForwardColorFormat());

		services.Get<LightingManager>().RegisterPasses(m_renderGraph);
		RegisterPasses(services);
	}

	void RenderingSubsystem::SetSceneViewportEnabled(ServiceContainer& services, bool enabled)
	{
		{
			const std::lock_guard lock(m_sceneViewportMutex);
			if (m_requestedSceneViewportEnabled == enabled)
			{
				return;
			}
			m_requestedSceneViewportEnabled = enabled;
		}
		m_sceneViewportRebuildPending.store(true, std::memory_order_release);
		(void) services;
	}

	void RenderingSubsystem::SetSceneViewportSettings(ServiceContainer& services, const SceneViewportSettings& settings)
	{
		SceneViewportSettings next = settings;
		next.customExtent.width = std::clamp(next.customExtent.width, 64u, 8192u);
		next.customExtent.height = std::clamp(next.customExtent.height, 64u, 8192u);
		{
			const std::lock_guard lock(m_sceneViewportMutex);
			if (m_requestedSceneViewportSettings.resolutionMode == next.resolutionMode && m_requestedSceneViewportSettings.customExtent.width == next.customExtent.width
			        && m_requestedSceneViewportSettings.customExtent.height == next.customExtent.height)
			{
				return;
			}
			m_requestedSceneViewportSettings = next;
		}

		m_sceneViewportRebuildPending.store(true, std::memory_order_release);
		(void) services;
	}

	bool RenderingSubsystem::CommitPendingSceneViewportSettings()
	{
		if (!m_sceneViewportRebuildPending.exchange(false, std::memory_order_acq_rel))
		{
			return false;
		}

		const std::lock_guard lock(m_sceneViewportMutex);
		m_sceneViewportEnabled = m_requestedSceneViewportEnabled;
		m_sceneViewportSettings = m_requestedSceneViewportSettings;
		return true;
	}

	void RenderingSubsystem::ApplyPendingSceneViewportChanges(ServiceContainer& services)
	{
		if (CommitPendingSceneViewportSettings())
		{
			RecreateSwapchainResources(services);
		}
	}

	void RenderingSubsystem::DiscardPendingFrameQueues(const std::uint32_t slot)
	{
		m_renderQueue.DiscardPending(slot);
		m_shadowService.DiscardPendingQueue(slot);
		m_localShadowService.DiscardPendingQueue(slot);
		m_renderTargetService.DiscardPendingQueues(slot);
		m_cameraPreview.DiscardPendingQueue(slot);
		m_modelPreview.DiscardPendingQueue(slot);
		m_materialThumbnailBaker.DiscardPendingQueue(slot);
	}

	void RenderingSubsystem::RegisterPasses(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();

		// pass that writes the swapchain, the image is never transitioned and its
		if (m_profile != RuntimeProfile::Full)
		{
			m_renderGraph
			        .AddFullscreenPass({
			                .name = "$UiShellClear",
			                .color = aether::RenderGraph::GetSwapchainColor(),
			                .loadOp = gpu::LoadOp::Clear,
			        })
			        .Execute([](PassContext&) {});
			(void) services;
			return;
		}

		// Every lazily-allocated target is created or released before a single pass is
		// declared, so the graph is always built around what actually exists rather than
		// around what a later frame might want.
		ApplyLazyTargetState(services);

		auto& lightingManager = services.Get<LightingManager>();
		auto& bindless = services.Get<BindlessManager>();
		auto& swapchain = services.Get<Swapchain>();

		const FrameContext frame{
		        .graph = &m_renderGraph,
		        .bindless = &bindless,
		        .cameras = &services.Get<CameraManager>(),
		        .lighting = &lightingManager,
		        .renderer = &m_renderer,
		        .materials = &services.Get<MaterialBuffer>(),
		        .effectParams = &services.Get<EffectParamBuffer>(),
		        .cullPass = &m_cullPass,
		        .frameIndex = [this]() { return m_frameIndexProvider ? m_frameIndexProvider() : 0ull; },
		        .depthFormat = swapchain.GetDepthFormat(),
		        .colorFormat = PostProcessStack::GetForwardColorFormat(),
		        .featureFlags = {.forwardEnabled = IsForwardPassEnabled()},
		};

		// A shadow service with no targets declares nothing: no product, no passes, no
		// transient blur scratch. Its frame constants report "no shadows" for as long as
		// that holds, so the forward shader reads a fully-lit visibility term.
		const bool directionalShadows = m_shadowService.HasShadowTargets();
		const bool localShadows = m_localShadowService.HasShadowTargets();

		if (directionalShadows)
		{
			m_shadowService.SetupPassResources(m_renderGraph);
			m_shadowService.RegisterComputePasses(m_renderGraph, m_cullPass);
		}
		if (localShadows)
		{
			m_localShadowService.SetupPassResources(m_renderGraph);
			m_localShadowService.RegisterComputePasses(m_renderGraph, m_cullPass);
		}
		(void) m_renderGraph.CreatePreparedDrawList("MainSceneDraws");
		auto& blackboard = m_renderGraph.GetBlackboard();
		const PreparedDrawList mainSceneDraws = blackboard.Require<PreparedDrawList>("MainSceneDraws");
		const gpu::Extent2D sceneExtent = m_postProcessStack.GetExtent();
		const RGImage hdrColor = m_postProcessStack.GetHdrColor();
		(void) blackboard.DeclareGraphProduct<FrameTextureProduct>(std::string{kFrameProductHdrColor},
		        FrameTextureProduct{
		                .image = hdrColor,
		                .extent = sceneExtent,
		                .format = PostProcessStack::GetForwardColorFormat(),
		                .bindlessSlot = m_postProcessStack.GetHdrBindlessSlot(),
		        },
		        FrameBlackboard::ProductMetadata{
		                .extent = sceneExtent,
		                .format = PostProcessStack::GetForwardColorFormat(),
		                .bindlessSlot = m_postProcessStack.GetHdrBindlessSlot(),
		        });
		if (m_sceneDepth.IsValid())
		{
			(void) blackboard.DeclareGraphProduct<FrameTextureProduct>(std::string{kFrameProductSceneDepth},
			        FrameTextureProduct{
			                .image = m_sceneDepth,
			                .extent = sceneExtent,
			                .format = swapchain.GetDepthFormat(),
			                .bindlessSlot = m_sceneDepthBindlessSlot,
			        },
			        FrameBlackboard::ProductMetadata{
			                .extent = sceneExtent,
			                .format = swapchain.GetDepthFormat(),
			                .bindlessSlot = m_sceneDepthBindlessSlot,
			        });
		}
		if (m_sceneGBuffer.IsValid())
		{
			(void) blackboard.DeclareGraphProduct<FrameTextureProduct>(std::string{kFrameProductSceneGBuffer},
			        FrameTextureProduct{
			                .image = m_sceneGBuffer,
			                .extent = sceneExtent,
			                .format = gpu::Format::R8G8B8A8Unorm,
			                .bindlessSlot = m_sceneGBufferBindlessSlot,
			        },
			        FrameBlackboard::ProductMetadata{
			                .extent = sceneExtent,
			                .format = gpu::Format::R8G8B8A8Unorm,
			                .bindlessSlot = m_sceneGBufferBindlessSlot,
			        });
		}
		if (m_sceneBaseColor.IsValid())
		{
			(void) blackboard.DeclareGraphProduct<FrameTextureProduct>(std::string{kFrameProductSceneBaseColor},
			        FrameTextureProduct{
			                .image = m_sceneBaseColor,
			                .extent = sceneExtent,
			                .format = gpu::Format::R8G8B8A8Srgb,
			                .bindlessSlot = m_sceneBaseColorBindlessSlot,
			        },
			        FrameBlackboard::ProductMetadata{
			                .extent = sceneExtent,
			                .format = gpu::Format::R8G8B8A8Srgb,
			                .bindlessSlot = m_sceneBaseColorBindlessSlot,
			        });
		}
		if constexpr (kEnableForwardGtao)
		{
			if (m_gtaoPass.GetAoImage().IsValid())
			{
				(void) blackboard.DeclareGraphProduct<FrameTextureProduct>(std::string{kFrameProductGtao},
				        FrameTextureProduct{
				                .image = m_gtaoPass.GetAoImage(),
				                .extent = m_gtaoPass.GetAoExtent(),
				                .format = gpu::Format::R8Unorm,
				                .bindlessSlot = m_gtaoPass.GetAoBindlessSlot(),
				        },
				        FrameBlackboard::ProductMetadata{
				                .extent = m_gtaoPass.GetAoExtent(),
				                .format = gpu::Format::R8Unorm,
				                .bindlessSlot = m_gtaoPass.GetAoBindlessSlot(),
				        });
			}
		}
		m_cullPass.RegisterPass(m_renderGraph, m_renderQueue, {}, mainSceneDraws);

		if (m_sceneDepth.IsValid())
		{
			m_renderGraph
			        .AddDepthOnlyPass({
			                .name = "$ScenePreDepth",
			                .depth = m_sceneDepth,
			                .draws = mainSceneDraws,
			                .extent = sceneExtent,
			                .consumes = {RenderGraph::Product<MainViewProduct>(kFrameProductMainView)},
			                .produces = {RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth), RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneGBuffer),
			                        RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneBaseColor)},
			        })
			        .WriteColor(m_sceneGBuffer, gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearColorValue(0.5f, 0.5f, 1.0f, 0.0f))
			        // Cleared to white so a pixel the prepass never covers reads as an
			        // untinted metal rather than a black one, which would subtract nothing
			        // and leave the sky reflection double counted.
			        .WriteColor(m_sceneBaseColor, gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearColorValue(1.0f, 1.0f, 1.0f, 1.0f))
			        .Execute(
			                [this](PassContext& ctx)
			                {
				                if (!IsForwardPassEnabled() || !HasFrameSceneDraws())
				                {
					                return;
				                }
				                // The prepass now samples the roughness map, so unlike the old
				                // depth-only pass it needs the bindless heaps bound.
				                if (m_bindlessManager != nullptr)
				                {
					                m_bindlessManager->CmdBindGlobalResources(ctx.recorder);
				                }
				                const std::optional<gpu::CullMode> cullMode = m_renderer.GetCullMode();
				                m_renderQueue.FlushDrawWithFrameAddr(ctx.recorder, ctx.frameSlot, nullptr, ctx.frameConstantsAddr, &m_prepassPipeline, 0, cullMode ? &*cullMode : nullptr);
			                });
		}

		{
			m_renderGraph
			        .AddFullscreenPass({
			                .name = "$Skybox",
			                .color = hdrColor,
			                .extent = sceneExtent,
			                .loadOp = gpu::LoadOp::Clear,
			                .consumes = {RenderGraph::Product<MainViewProduct>(kFrameProductMainView)},
			        })
			        .Execute(
			                [this](PassContext& ctx)
			                {
				                // Always draw: the frame constants decide between the 3D
				                // gradient and a camera-owned flat clear (skyVoidColor.w
				                // == 0), so 2D scenes get their background painted too.
				                gpu::CommandList& cmd = ctx.recorder;
				                cmd.BindPipeline(m_skyboxPipeline.GetPipeline());
				                const gpu::DeviceAddress frameAddr = ctx.frameConstantsAddr;
				                std::byte bytes[sizeof(gpu::DeviceAddress)];
				                std::memcpy(bytes, &frameAddr, sizeof(bytes));
				                cmd.PushDataRaw(0, std::span<const std::byte>(bytes, sizeof(bytes)));
				                cmd.Draw(3);
			                });
		}
		if (directionalShadows)
		{
			m_shadowService.RegisterGraphicsPasses(m_renderGraph);
		}
		if (localShadows)
		{
			m_localShadowService.RegisterGraphicsPasses(m_renderGraph);
		}
		if constexpr (kEnableForwardGtao)
		{
			// The AO targets only exist while the scene is submitting 3D draws, and
			// RegisterPasses is a no-op without them. Inside the release grace window the
			// targets outlive the content for a while, so the compute still self-skips on
			// the packet-carried draw count.
			m_gtaoPass.RegisterPasses(m_renderGraph, m_sceneDepth, [this] { return HasFrameSceneDraws(); });
		}

		{
			const RGImage depth = m_sceneDepth.IsValid() ? m_sceneDepth : aether::RenderGraph::GetSwapchainDepth();
			std::vector<RenderGraph::FrameProductRef> forwardConsumes{
			        RenderGraph::Product<MainViewProduct>(kFrameProductMainView),
			};
			if (m_sceneDepth.IsValid())
			{
				forwardConsumes.push_back(RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth));
			}
			if (frame.lighting != nullptr)
			{
				forwardConsumes.push_back(RenderGraph::Product<LightBuffersProduct>(kFrameProductLightBuffers));
			}
			auto pass = m_renderGraph.AddDrawQueuePass({
			        .name = "$EngineForward",
			        .color = hdrColor,
			        .depth = depth,
			        .draws = mainSceneDraws,
			        .extent = sceneExtent,
			        .depthLoadOp = gpu::LoadOp::Clear,
			        .consumes = std::move(forwardConsumes),
			        .produces = {RenderGraph::Product<FrameTextureProduct>(kFrameProductHdrColor)},
			});

			if (directionalShadows)
			{
				pass.ConsumeTextureProduct<FrameTextureArrayProduct>(kFrameProductDirectionalShadows, FrameResourceId::DirectionalShadowC0);
			}
			if (localShadows)
			{
				pass.ConsumeTextureProduct<LocalShadowProduct>(kFrameProductLocalShadows, FrameResourceId::LocalShadowAtlas);
			}
			if constexpr (kEnableForwardGtao)
			{
				if (m_gtaoPass.GetAoImage().IsValid())
				{
					pass.ConsumeTextureProduct<FrameTextureProduct>(kFrameProductGtao, FrameResourceId::Gtao);
				}
			}

			if (auto* lighting = frame.lighting)
			{
				pass.ReadBuffer(lighting->GetLightsBufferHandle());
				pass.ReadBuffer(lighting->GetTileHeadersBufferHandle());
				pass.ReadBuffer(lighting->GetTileIndicesBufferHandle());
			}

			pass.Execute(
			        [this, &m_renderQueue = m_renderQueue, bindless = frame.bindless, lighting = frame.lighting](PassContext& ctx)
			        {
				        if (!IsForwardPassEnabled() || !HasFrameSceneDraws())
				        {
					        return;
				        }
				        const auto frameSlot = ctx.frameSlot;
				        const DrawContracts::LightingAddresses lightingAddr = lighting != nullptr ? lighting->GetLightingAddresses(frameSlot) : DrawContracts::LightingAddresses{};
				        bindless->CmdBindGlobalResources(ctx.recorder);
				        const std::optional<gpu::CullMode> cullMode = m_renderer.GetCullMode();
				        m_renderQueue.FlushDrawPush(ctx.recorder, ctx.frameSlot, lightingAddr, nullptr, 0, cullMode ? &*cullMode : nullptr);
			        });
		}

		// Screen-space reflections, after the forward pass so the HDR colour it samples
		// is the shaded scene. Two passes: the march cannot write the buffer it reads.
		if (m_ssrColor.IsValid() && m_sceneGBuffer.IsValid())
		{
			m_renderGraph
			        .AddFullscreenPass({
			                .name = "$SSR",
			                .color = m_ssrColor,
			                .extent = sceneExtent,
			                .loadOp = gpu::LoadOp::Clear,
			                .clearValue = ClearColorValue(0.0f, 0.0f, 0.0f, 0.0f),
			                .consumes = {RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth),
			                        RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneGBuffer),
			                        RenderGraph::Product<FrameTextureProduct>(kFrameProductHdrColor)},
			        })
			        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneDepth, FrameResourceId::SceneDepth)
			        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneGBuffer, FrameResourceId::SceneGBuffer)
			        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductHdrColor, FrameResourceId::HdrColor)
			        .Execute(
			                [this, bindless = frame.bindless](PassContext& ctx)
			                {
				                if (!IsForwardPassEnabled() || !HasFrameSceneDraws() || !m_renderer.AreReflectionsEnabled())
				                {
					                return;
				                }
				                gpu::CommandList cmd = ctx.recorder.View();
				                bindless->CmdBindGlobalResources(cmd);
				                cmd.SetViewport(gpu::Viewport{.width = static_cast<float>(ctx.extent.width), .height = static_cast<float>(ctx.extent.height)});
				                cmd.SetScissor(gpu::Rect2D{.width = ctx.extent.width, .height = ctx.extent.height});
				                cmd.BindPipeline(m_ssrPipeline.GetPipeline());
				                struct
				                {
					                std::uint64_t frameConstantsAddr;
					                std::uint32_t width;
					                std::uint32_t height;
					                float maxRoughness;
					                float intensity;
					                std::uint32_t pad0;
					                std::uint32_t pad1;
				                } push{
				                        .frameConstantsAddr = ctx.frameConstantsAddr,
				                        .width = ctx.extent.width,
				                        .height = ctx.extent.height,
				                        .maxRoughness = m_renderer.GetReflectionMaxRoughness(),
				                        .intensity = m_renderer.GetReflectionIntensity(),
				                };
				                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                cmd.Draw(3, 1, 0, 0);
			                });

			// By VALUE, not by reference. AddFullscreenPass returns a PassBuilder as a
			// prvalue and the chained calls hand back references into that temporary, so
			// binding auto& to one leaves a reference to an object destroyed at the end of
			// the statement. It appears to work - the builder is just a graph reference and
			// an index, with a trivial destructor - right up until something reuses the
			// storage, at which point a later ReadTexture records onto some other pass and
			// the producer it was meant to keep alive is silently culled instead.
			auto compositePass = m_renderGraph
			        .AddFullscreenPass({
			                .name = "$SSRComposite",
			                .color = hdrColor,
			                .extent = sceneExtent,
			                .loadOp = gpu::LoadOp::Load,
			                .consumes = {RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth),
			                        RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneGBuffer),
			                        RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneBaseColor)},
			        });
			compositePass.ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneDepth, FrameResourceId::SceneDepth)
			        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneGBuffer, FrameResourceId::SceneGBuffer)
			        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneBaseColor, FrameResourceId::SceneBaseColor)
			        .ReadTexture(m_ssrColor);

			// Occlusion only exists when the AO pass ran; the shader falls back to fully
			// open without it, so consuming it unconditionally would only invent a
			// dependency on a product nothing produced.
			if constexpr (kEnableForwardGtao)
			{
				if (m_gtaoPass.GetAoImage().IsValid())
				{
					compositePass.ConsumeTextureProduct<FrameTextureProduct>(kFrameProductGtao, FrameResourceId::Gtao);
				}
			}

			compositePass.Execute(
			                [this, bindless = frame.bindless](PassContext& ctx)
			                {
				                if (!IsForwardPassEnabled() || !HasFrameSceneDraws() || !m_renderer.AreReflectionsEnabled())
				                {
					                return;
				                }
				                gpu::CommandList cmd = ctx.recorder.View();
				                bindless->CmdBindGlobalResources(cmd);
				                cmd.SetViewport(gpu::Viewport{.width = static_cast<float>(ctx.extent.width), .height = static_cast<float>(ctx.extent.height)});
				                cmd.SetScissor(gpu::Rect2D{.width = ctx.extent.width, .height = ctx.extent.height});
				                cmd.BindPipeline(m_ssrCompositePipeline.GetPipeline());
				                struct
				                {
					                std::uint64_t frameConstantsAddr;
					                std::uint32_t ssrSlot;
					                std::uint32_t width;
					                std::uint32_t height;
					                std::uint32_t pad0;
				                } push{
				                        .frameConstantsAddr = ctx.frameConstantsAddr,
				                        .ssrSlot = m_ssrBindlessSlot,
				                        .width = ctx.extent.width,
				                        .height = ctx.extent.height,
				                };
				                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                cmd.Draw(3, 1, 0, 0);
			                });
		}

		// Volumetric fog, after reflections so the air sits in front of everything the
		// scene put in the HDR buffer, and before bloom so a beam can glow.
		if (m_volumetricFog.IsValid() && m_sceneDepth.IsValid())
		{
			const gpu::Extent2D halfExtent{(sceneExtent.width + 1u) / 2u, (sceneExtent.height + 1u) / 2u};

			// The shadow cascades are an ARRAY product, and asking for them as a plain
			// texture matched nothing: the graph reported "exists in metadata but has no
			// typed storage" and "no producer pass was found", twice a frame forever.
			//
			// What that cost was the ORDERING, not the picture. The frame resource table is
			// written once from every pass's bindings, so the forward pass had already put
			// the cascades in it and this shader's lookup by id found them anyway - the
			// beams were never broken. But a consume that matches no producer creates no
			// edge, so nothing in the graph actually required the shadow maps to be drawn
			// before the march read them; that it held was incidental.
			//
			// Declared only when a producer exists, the way CameraPreviewService does it: a
			// 2D scene has no directional shadows, and consuming an unpublished product
			// leaves a dependency nothing can satisfy - which is what filled the log.
			const bool haveDirectionalShadows =
			        m_renderGraph.GetBlackboard().TryGet<FrameTextureArrayProduct>(kFrameProductDirectionalShadows) != nullptr;

			std::vector<RenderGraph::FrameProductRef> volumetricConsumes{
			        RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth)};
			if (haveDirectionalShadows)
			{
				volumetricConsumes.push_back(RenderGraph::Product<FrameTextureArrayProduct>(kFrameProductDirectionalShadows));
			}

			auto volumetricPass = m_renderGraph.AddFullscreenPass({
			        .name = "$VolumetricFog",
			        .color = m_volumetricFog,
			        .extent = halfExtent,
			        .loadOp = gpu::LoadOp::Clear,
			        .clearValue = ClearColorValue(0.0f, 0.0f, 0.0f, 0.0f),
			        .consumes = std::move(volumetricConsumes),
			});
			volumetricPass.ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneDepth, FrameResourceId::SceneDepth);
			if (haveDirectionalShadows)
			{
				volumetricPass.ConsumeTextureProduct<FrameTextureArrayProduct>(kFrameProductDirectionalShadows, FrameResourceId::DirectionalShadowC0);
			}
			volumetricPass
			        .Execute(
			                [this, bindless = frame.bindless](PassContext& ctx)
			                {
				                if (!IsForwardPassEnabled() || !HasFrameSceneDraws() || !ShouldMarchVolumetrics())
				                {
					                return;
				                }
				                gpu::CommandList cmd = ctx.recorder.View();
				                bindless->CmdBindGlobalResources(cmd);
				                cmd.SetViewport(gpu::Viewport{.width = static_cast<float>(ctx.extent.width), .height = static_cast<float>(ctx.extent.height)});
				                cmd.SetScissor(gpu::Rect2D{.width = ctx.extent.width, .height = ctx.extent.height});
				                cmd.BindPipeline(m_volumetricPipeline.GetPipeline());
				                struct
				                {
					                std::uint64_t frameConstantsAddr;
					                std::uint32_t width;
					                std::uint32_t height;
					                std::uint32_t depthSlot;
					                std::uint32_t pad0;
				                } push{
				                        .frameConstantsAddr = ctx.frameConstantsAddr,
				                        .width = ctx.extent.width,
				                        .height = ctx.extent.height,
				                        .depthSlot = m_sceneDepthBindlessSlot,
				                };
				                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                cmd.Draw(3, 1, 0, 0);
			                });

			m_renderGraph
			        .AddFullscreenPass({
			                .name = "$VolumetricComposite",
			                .color = hdrColor,
			                .extent = sceneExtent,
			                .loadOp = gpu::LoadOp::Load,
			                .consumes = {RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth)},
			        })
			        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneDepth, FrameResourceId::SceneDepth)
			        .ReadTexture(m_volumetricFog)
			        .Execute(
			                [this, bindless = frame.bindless, halfExtent](PassContext& ctx)
			                {
				                if (!IsForwardPassEnabled() || !HasFrameSceneDraws() || !ShouldMarchVolumetrics())
				                {
					                return;
				                }
				                gpu::CommandList cmd = ctx.recorder.View();
				                bindless->CmdBindGlobalResources(cmd);
				                cmd.SetViewport(gpu::Viewport{.width = static_cast<float>(ctx.extent.width), .height = static_cast<float>(ctx.extent.height)});
				                cmd.SetScissor(gpu::Rect2D{.width = ctx.extent.width, .height = ctx.extent.height});
				                cmd.BindPipeline(m_volumetricCompositePipeline.GetPipeline());
				                struct
				                {
					                std::uint64_t frameConstantsAddr;
					                std::uint32_t volumetricSlot;
					                std::uint32_t depthSlot;
					                std::uint32_t halfWidth;
					                std::uint32_t halfHeight;
					                std::uint32_t pad0;
					                std::uint32_t pad1;
				                } push{
				                        .frameConstantsAddr = ctx.frameConstantsAddr,
				                        .volumetricSlot = m_volumetricBindlessSlot,
				                        .depthSlot = m_sceneDepthBindlessSlot,
				                        .halfWidth = halfExtent.width,
				                        .halfHeight = halfExtent.height,
				                };
				                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                cmd.Draw(3, 1, 0, 0);
			                });
		}

		// Depth of field, after the fog so the air is blurred with everything it sits in
		// front of. Mixed in during tonemapping rather than written back over the HDR
		// buffer, which would need a second full-resolution target for a pass that only
		// ever produces something out of focus.
		if (m_dofColor.IsValid() && m_sceneDepth.IsValid())
		{
			m_renderGraph
			        .AddFullscreenPass({
			                .name = "$DepthOfField",
			                .color = m_dofColor,
			                .extent = gpu::Extent2D{(sceneExtent.width + 1u) / 2u, (sceneExtent.height + 1u) / 2u},
			                .loadOp = gpu::LoadOp::Clear,
			                .clearValue = ClearColorValue(0.0f, 0.0f, 0.0f, 0.0f),
			                .consumes = {RenderGraph::Product<FrameTextureProduct>(kFrameProductSceneDepth),
			                        RenderGraph::Product<FrameTextureProduct>(kFrameProductHdrColor)},
			        })
			        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductSceneDepth, FrameResourceId::SceneDepth)
			        .ConsumeTextureProduct<FrameTextureProduct>(kFrameProductHdrColor, FrameResourceId::HdrColor)
			        .Execute(
			                [this, bindless = frame.bindless](PassContext& ctx)
			                {
				                // Told to the tonemap pass from here, every frame: the graph
				                // is built once but which camera is main - and whether it has
				                // a lens - is a per-frame answer.
				                const bool wantLens = IsForwardPassEnabled() && HasFrameSceneDraws() && IsDepthOfFieldEnabled();
				                // Read now, never captured at build time. A stale or unset
				                // slot is an out-of-bounds descriptor index, which does not
				                // fail gracefully - it hangs the device.
				                const std::uint32_t hdrSlot = m_postProcessStack.GetHdrBindlessSlot();
				                const bool slotsValid = hdrSlot != 0xFFFFFFFFu && m_sceneDepthBindlessSlot != 0xFFFFFFFFu && m_dofBindlessSlot != 0xFFFFFFFFu;
				                m_postProcessStack.SetDepthOfFieldSlot((wantLens && slotsValid) ? m_dofBindlessSlot : 0xFFFFFFFFu);
				                if (!wantLens || !slotsValid)
				                {
					                return;
				                }
				                gpu::CommandList cmd = ctx.recorder.View();
				                bindless->CmdBindGlobalResources(cmd);
				                cmd.SetViewport(gpu::Viewport{.width = static_cast<float>(ctx.extent.width), .height = static_cast<float>(ctx.extent.height)});
				                cmd.SetScissor(gpu::Rect2D{.width = ctx.extent.width, .height = ctx.extent.height});
				                cmd.BindPipeline(m_dofPipeline.GetPipeline());
				                struct
				                {
					                std::uint64_t frameConstantsAddr;
					                std::uint32_t hdrSlot;
					                std::uint32_t depthSlot;
					                std::uint32_t width;
					                std::uint32_t height;
					                float focusDistance;
					                float cocCoeff;
					                float maxCocPixels;
				                } push{
				                        .frameConstantsAddr = ctx.frameConstantsAddr,
				                        .hdrSlot = hdrSlot,
				                        .depthSlot = m_sceneDepthBindlessSlot,
				                        .width = ctx.extent.width,
				                        .height = ctx.extent.height,
				                        .focusDistance = m_dofParams.x,
				                        .cocCoeff = m_dofParams.y,
				                        .maxCocPixels = m_dofParams.z,
				                };
				                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                cmd.Draw(3, 1, 0, 0);
			                });
		}

		// Project-registered custom passes, injected around the 2D scene (both stages draw into the
		// scene HDR colour). BehindScene2D runs before sprites/tiles, OverScene2D after.
		constexpr gpu::Format kSceneColorFormat = PostProcessStack::GetForwardColorFormat();
		m_customPassRenderer.RegisterPass(m_renderGraph, CustomPassStage::BehindScene2D, hdrColor, sceneExtent, kSceneColorFormat, bindless, "$CustomPassBehind2D");
		m_renderer2D.RegisterPass(m_renderGraph, hdrColor, sceneExtent, bindless);
		m_customPassRenderer.RegisterPass(m_renderGraph, CustomPassStage::OverScene2D, hdrColor, sceneExtent, kSceneColorFormat, bindless, "$CustomPassOver2D");
		// Screen-space 2D light map: multiplies the sprite/tile layer by (ambient + point/spot lights).
		// Self-skips on non-2D scenes and unlit 2D scenes (BeginFrame records zero lights there).
		m_light2D.RegisterPass(m_renderGraph, hdrColor, sceneExtent, bindless);
		// Emissive project passes run AFTER the light map, so they keep their own brightness instead of
		// being darkened by it (a glowing ink stroke lights the cave without being dimmed by the cave).
		m_customPassRenderer.RegisterPass(m_renderGraph, CustomPassStage::EmissiveOverLight2D, hdrColor, sceneExtent, kSceneColorFormat, bindless, "$CustomPassEmissive2D");

		m_cameraPreview.RegisterComputePasses(m_renderGraph, m_cullPass);
		m_cameraPreview.RegisterGraphicsPasses(m_renderGraph, frame.lighting, bindless, m_postProcessStack, m_skyboxPipeline.GetPipeline(), m_renderer2D);
		m_modelPreview.RegisterComputePasses(m_renderGraph, m_cullPass);
		m_materialThumbnailBaker.RegisterComputePasses(m_renderGraph, m_cullPass);
		m_modelPreview.RegisterGraphicsPasses(m_renderGraph, bindless, m_postProcessStack);
		m_materialThumbnailBaker.RegisterGraphicsPasses(m_renderGraph, bindless, m_postProcessStack);

		m_renderTargetService.RegisterPasses();
		m_postProcessStack.SetOutputToTexture(m_sceneViewportEnabled);
		// Handed over before the stack registers, so its tonemap pass can declare the read
		// that keeps this producer from being culled.
		m_postProcessStack.SetDepthOfFieldImage(m_dofColor);
		m_postProcessStack.RegisterPasses(m_renderGraph, *frame.bindless);
		m_physicsDebug.RegisterPass(m_renderGraph, m_sceneViewportEnabled ? m_postProcessStack.GetFinalColor() : RGImage{}, m_sceneViewportEnabled ? m_sceneDepth : RGImage{}, m_sceneViewportEnabled ? sceneExtent : gpu::Extent2D{});
		// SetFrameDebugVertices there). Must run before $SceneViewportReady below
		m_uiRenderer.RegisterPass(m_renderGraph, m_sceneViewportEnabled ? m_postProcessStack.GetFinalColor() : RGImage{}, m_sceneViewportEnabled ? sceneExtent : gpu::Extent2D{}, *frame.bindless);
		if (m_sceneViewportEnabled)
		{
			m_renderGraph.AddPass("$SceneViewportReady").ReadTexture(m_postProcessStack.GetFinalColor()).Execute([](PassContext&) {});

			m_renderGraph
			        .AddFullscreenPass({
			                .name = "$SceneViewportClearSwapchain",
			                .color = aether::RenderGraph::GetSwapchainColor(),
			                .loadOp = gpu::LoadOp::Clear,
			        })
			        .Execute([](PassContext&) {});
		}

		if (m_texturePreview.IsValid())
		{
			m_renderGraph
			        .AddFullscreenPass({
			                .name = "$TexturePreview",
			                .color = m_texturePreview,
			                .extent = {kTexturePreviewSize, kTexturePreviewSize},
			                .loadOp = gpu::LoadOp::Clear,
			        })
			        .Execute(
			                [this, bindless = frame.bindless](PassContext& ctx)
			                {
				                if (!m_previewEnabled || m_previewSrcSlot == 0xFFFFFFFFu)
				                {
					                return;
				                }
				                const std::uint32_t w = std::min(m_previewSrcExtent.width, kTexturePreviewSize);
				                const std::uint32_t h = std::min(m_previewSrcExtent.height, kTexturePreviewSize);
				                if (w == 0u || h == 0u)
				                {
					                return;
				                }
				                gpu::CommandList& cmd = ctx.recorder;
				                cmd.SetViewport(gpu::Viewport{.width = static_cast<float>(w), .height = static_cast<float>(h)});
				                cmd.SetScissor(gpu::Rect2D{.x = 0, .y = 0, .width = w, .height = h});
				                bindless->CmdBindGlobalResources(cmd);
				                cmd.BindPipeline(m_texturePreviewPipeline.GetPipeline());
				                struct
				                {
					                std::uint32_t srcSlot;
					                std::uint32_t channel;
					                float exposure;
					                std::uint32_t flags;
					                std::uint32_t tonemapMode;
					                std::uint32_t width;
					                std::uint32_t height;
					                std::uint32_t pad;
				                } push;
				                push.srcSlot = m_previewSrcSlot;
				                push.channel = m_previewChannel;
				                push.exposure = m_previewExposure;
				                push.flags = m_previewFlags;
				                push.tonemapMode = m_previewTonemap;
				                push.width = w;
				                push.height = h;
				                push.pad = 0u;
				                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                cmd.Draw(3, 1, 0, 0);
			                });
			m_renderGraph.AddPass("$TexturePreviewReady").ReadTexture(m_texturePreview).Execute([](PassContext&) {});
		}
	}

	void RenderingSubsystem::SetTexturePreviewRequest(std::uint32_t bindlessSlot, gpu::Extent2D srcExtent, std::uint32_t channel, float exposure, std::uint32_t flags, std::uint32_t tonemapMode, bool enabled)
	{
		m_previewSrcSlot = bindlessSlot;
		m_previewSrcExtent = srcExtent;
		m_previewChannel = channel;
		m_previewExposure = exposure;
		m_previewFlags = flags;
		m_previewTonemap = tonemapMode;
		m_previewEnabled = enabled && bindlessSlot != 0xFFFFFFFFu;
	}

	void RenderingSubsystem::WriteResourceTable(std::uint32_t frameIndex, std::span<const ResourceEntry> entries)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT(entries.size() <= kFrameResourceCount, "ResourceEntry count exceeds kFrameResourceCount");
		const std::size_t byteCount = entries.size() * sizeof(ResourceEntry);
		std::memcpy(m_resourceTableBuffers[frameIndex].mapped, entries.data(), byteCount);
		gpu::ResourceRegistry::FlushMappedBuffer(m_resourceTableBuffers[frameIndex].handle, 0, static_cast<gpu::DeviceSize>(byteCount));
	}

	gpu::DeviceAddress RenderingSubsystem::PublishFrameResourceTable(std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		std::array<ResourceEntry, kFrameResourceCount> resourceTable{};
		resourceTable.fill(ResourceEntry{});

		m_renderGraph.PopulateResourceTable(std::span(resourceTable));

		if constexpr (kEnableForwardGtao)
		{
			const auto id = static_cast<std::size_t>(FrameResourceId::Gtao);
			if (id < resourceTable.size() && m_gtaoPass.GetAoBindlessSlot() != 0xFFFFFFFFu)
			{
				const gpu::Extent2D extent = m_gtaoPass.GetAoExtent();
				resourceTable[id] = ResourceEntry{
				        .address = m_gtaoPass.GetAoBindlessSlot(),
				        .type = kResourceTypeBindlessTexture,
				        .width = extent.width,
				        .height = extent.height,
				        .format = static_cast<std::uint32_t>(gpu::Format::R8Unorm),
				};
			}
		}

		if (!m_shadowService.IsDirectionalShadowEnabledForFrame(frameIndex))
		{
			for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
			{
				const auto id = static_cast<std::size_t>(FrameResourceId::DirectionalShadowC0) + static_cast<std::size_t>(cascade);
				resourceTable[id] = ResourceEntry{};
			}
		}

		WriteResourceTable(frameIndex, std::span(resourceTable));
		return GetResourceTableAddress(frameIndex);
	}
} // namespace aether
