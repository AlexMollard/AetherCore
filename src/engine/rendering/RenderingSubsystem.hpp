#pragma once

#include <algorithm>

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>

#include "RuntimeProfile.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "passes/CullPass.hpp"
#include "passes/GTAOPass.hpp"
#include "rendering/CameraPreviewService.hpp"
#include "rendering/ModelPreviewService.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "rendering/LazyTargetGates.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/LocalShadowService.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/CustomPassRenderer.hpp"
#include "rendering/Light2DCompositor.hpp"
#include "rendering/Renderer2D.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "rendering/ShadowService.hpp"
#include "scene/SceneKind.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "ui/UiRenderer.hpp"

namespace aether
{
	class BindlessManager;
	class ServiceContainer;
} // namespace aether

namespace aether
{
	enum class SceneViewportResolutionMode : std::uint32_t
	{
		WindowNative = 0,
		Fixed720p,
		Fixed1080p,
		Fixed1440p,
		Custom,
		MatchPanel,
	};

	struct SceneViewportSettings
	{
		SceneViewportResolutionMode resolutionMode = SceneViewportResolutionMode::WindowNative;
		gpu::Extent2D customExtent{1280, 720};
	};

	class RenderingSubsystem
	{
	public:
		void Init(ServiceContainer& services, RuntimeProfile profile = RuntimeProfile::Full);
		// Takes the container so it can withdraw the services Init registered by reference
		// (FontRegistry lives inside m_uiRenderer), rather than leaving dangling pointers behind.
		void Shutdown(ServiceContainer& services);
		void RegisterPasses(ServiceContainer& services);

		void RecreateSwapchainResources(ServiceContainer& services);
		// Fraction of the output the SCENE renders at; the final fullscreen pass upscales.
		// Only applies outside the editor viewport, which has its own resolution control.
		void SetRenderScale(float scale)
		{
			m_renderScale = std::clamp(scale, 0.25f, 1.0f);
		}

		[[nodiscard]] gpu::Extent2D ApplyRenderScale(gpu::Extent2D extent) const
		{
			if (m_renderScale >= 1.0f)
			{
				return extent;
			}
			extent.width = std::max(64u, static_cast<std::uint32_t>(static_cast<float>(extent.width) * m_renderScale));
			extent.height = std::max(64u, static_cast<std::uint32_t>(static_cast<float>(extent.height) * m_renderScale));
			return extent;
		}

		void SetSceneViewportEnabled(ServiceContainer& services, bool enabled);
		void SetSceneViewportSettings(ServiceContainer& services, const SceneViewportSettings& settings);
		bool CommitPendingSceneViewportSettings();
		void ApplyPendingSceneViewportChanges(ServiceContainer& services);

		// Non-consuming peek used by the main-thread quiesced recreate to decide
		[[nodiscard]] bool IsSceneViewportRebuildPending() const
		{
			return m_sceneViewportRebuildPending.load(std::memory_order_acquire);
		}

		// Feeds this frame's measured content into the lazy-target gates. Called from the
		// game thread once the render queues and light lists for the frame are complete.
		void PublishContentSignals(const RenderContentSignals& signals);

		// Non-consuming peek: true when a gate wants targets created or released.
		[[nodiscard]] bool IsLazyTargetRebuildPending() const
		{
			return m_lazyTargetRebuildPending.load(std::memory_order_acquire);
		}

		// Consumes the pending request and commits the gates. Returns true when the
		// committed state changed and the render graph therefore has to be rebuilt.
		bool CommitPendingLazyTargets();

		[[nodiscard]] const LazyTargetGates& GetLazyTargetGates() const
		{
			return m_lazyGates;
		}

		// True when any render queue was handed a skinned draw for this slot. Called on the
		// game thread after every queue for the frame has been filled, and fed to the
		// skinning gate as the content signal.
		[[nodiscard]] bool HasSkinnedDrawsQueued(std::uint32_t drawSlot);

		void DiscardPendingFrameQueues(std::uint32_t slot);

		[[nodiscard]] SceneViewportSettings GetSceneViewportSettings() const;
		[[nodiscard]] gpu::Extent2D ResolveRequestedSceneViewportExtent(gpu::Extent2D swapchainExtent) const;

		void SetFrameIndexProvider(std::function<std::uint64_t()> provider)
		{
			m_frameIndexProvider = std::move(provider);
		}

		void SetForwardPassEnabled(bool enabled)
		{
			m_forwardPassEnabled.store(enabled, std::memory_order_relaxed);
		}

		[[nodiscard]] bool IsForwardPassEnabled() const
		{
			return m_forwardPassEnabled.load(std::memory_order_relaxed);
		}

		[[nodiscard]] bool IsSceneViewportEnabled() const;

		[[nodiscard]] RenderGraph& GetRenderGraph()
		{
			return m_renderGraph;
		}

		[[nodiscard]] RenderQueue& GetRenderQueue()
		{
			return m_renderQueue;
		}

		[[nodiscard]] Renderer& GetRenderer()
		{
			return m_renderer;
		}

		void SetSceneFeatures(SceneFeatureFlags features)
		{
			m_sceneFeatures.store(static_cast<std::uint32_t>(features), std::memory_order_relaxed);
		}

		// Packet-carried "this frame submitted 3D geometry". Render-thread passes gate on
		// this rather than on a scene feature flag.
		void SetFrameSceneDraws(bool hasSceneDraws)
		{
			m_frameSceneDraws.store(hasSceneDraws, std::memory_order_relaxed);
		}

		[[nodiscard]] bool HasFrameSceneDraws() const
		{
			return m_frameSceneDraws.load(std::memory_order_relaxed);
		}

		void SetBackgroundParams(std::uint32_t mode, float angleRadians, std::uint32_t stopCount, const std::array<glm::vec4, PostProcessStack::kMaxBackgroundStops>& stops)
		{
			m_postProcessStack.SetBackgroundParams(mode, angleRadians, stopCount, stops);
		}

		[[nodiscard]] bool IsSceneFeatureEnabled(SceneFeatureFlags feature) const
		{
			const auto features = static_cast<SceneFeatureFlags>(m_sceneFeatures.load(std::memory_order_relaxed));
			return HasSceneFeature(features, feature);
		}

		[[nodiscard]] Renderer2D& GetRenderer2D()
		{
			return m_renderer2D;
		}

		[[nodiscard]] CustomPassRenderer& GetCustomPassRenderer()
		{
			return m_customPassRenderer;
		}

		[[nodiscard]] Light2DCompositor& GetLight2DCompositor()
		{
			return m_light2D;
		}

		[[nodiscard]] FrameConstantsBuffer& GetFrameConstantsBuffer()
		{
			return m_frameConstantsBuffer;
		}

		[[nodiscard]] ShadowService& GetShadowService()
		{
			return m_shadowService;
		}

		[[nodiscard]] LocalShadowService& GetLocalShadowService()
		{
			return m_localShadowService;
		}

		[[nodiscard]] RenderTargetService& GetRenderTargetService()
		{
			return m_renderTargetService;
		}

		[[nodiscard]] CullPass& GetCullPass()
		{
			return m_cullPass;
		}

		[[nodiscard]] GTAOPass& GetGtaoPass()
		{
			return m_gtaoPass;
		}

		[[nodiscard]] PostProcessStack& GetPostProcessStack()
		{
			return m_postProcessStack;
		}

		static constexpr std::uint32_t kTexturePreviewSize = 2048;

		// (enabled=false / slot 0xFFFFFFFF disables). Read on the render thread each
		void SetTexturePreviewRequest(std::uint32_t bindlessSlot, gpu::Extent2D srcExtent, std::uint32_t channel, float exposure, std::uint32_t flags, std::uint32_t tonemapMode, bool enabled);

		// True while a host tool is asking for the GPU texture preview. The 16 MB preview
		// target is allocated against this, not held for the life of the editor.
		[[nodiscard]] bool IsTexturePreviewRequested() const
		{
			return m_previewEnabled;
		}

		[[nodiscard]] gpu::ImageView GetTexturePreviewView() const
		{
			return m_texturePreviewView;
		}

		// Bumped every time the preview image is created or destroyed. A UI holding a
		// backend texture handle for the view must drop it when this changes.
		[[nodiscard]] std::uint32_t GetTexturePreviewGeneration() const
		{
			return m_texturePreviewGeneration;
		}

		[[nodiscard]] PhysicsDebugRenderer& GetPhysicsDebugRenderer()
		{
			return m_physicsDebug;
		}

		[[nodiscard]] CameraPreviewService& GetCameraPreview()
		{
			return m_cameraPreview;
		}

		[[nodiscard]] ModelPreviewService& GetModelPreview()
		{
			return m_modelPreview;
		}

		[[nodiscard]] ui::UiRenderer& GetUiRenderer()
		{
			return m_uiRenderer;
		}

		void WriteResourceTable(std::uint32_t frameIndex, std::span<const ResourceEntry> entries);
		[[nodiscard]] gpu::DeviceAddress PublishFrameResourceTable(std::uint32_t frameIndex);

		[[nodiscard]] gpu::DeviceAddress GetResourceTableAddress(std::uint32_t frameIndex) const
		{
			return m_resourceTableBuffers[frameIndex].address;
		}

	private:
		[[nodiscard]] gpu::Extent2D ResolveSceneViewportExtent(gpu::Extent2D swapchainExtent) const;
		void DestroySceneViewportDepth();
		void CreateSceneViewportDepth(gpu::Device device, gpu::Format depthFormat, RenderGraph& graph, BindlessManager& bindless);

		// lifetime of the subsystem, but RenderGraph::Clear() (on every scene-viewport
		void RegisterTexturePreviewImage();

		void CreateTexturePreviewImage();
		void DestroyTexturePreviewImage();

		// Creates or releases every lazily-allocated target to match the committed gates.
		// Runs on the main thread with the render thread parked and the GPU quiesced,
		// immediately before the render graph is rebuilt around what now exists.
		void ApplyLazyTargetState(ServiceContainer& services);

		// Visits every RenderQueue this subsystem owns, directly or through a service.
		void ForEachRenderQueue(const std::function<void(RenderQueue&)>& fn);

		struct PerFrameResourceTable
		{
			gpu::BufferHandle handle{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
		};

		RuntimeProfile m_profile = RuntimeProfile::Full;

		RenderQueueSharedPipelines m_renderQueuePipelines;
		RenderGraph m_renderGraph;
		RenderQueue m_renderQueue;
		Renderer m_renderer;
		Renderer2D m_renderer2D;
		CustomPassRenderer m_customPassRenderer;
		Light2DCompositor m_light2D;
		FrameConstantsBuffer m_frameConstantsBuffer;
		std::array<PerFrameResourceTable, kMaxFramesInFlight> m_resourceTableBuffers{};
		ShadowService m_shadowService;
		LocalShadowService m_localShadowService;
		CameraPreviewService m_cameraPreview;
		ModelPreviewService m_modelPreview;
		RenderTargetService m_renderTargetService;
		CullPass m_cullPass;
		GraphicsPipeline m_preDepthPipeline;
		GraphicsPipeline m_skyboxPipeline;
		GTAOPass m_gtaoPass;
		PostProcessStack m_postProcessStack;
		RGImage m_sceneDepth;
		// Thin G-buffer written alongside depth in the prepass: octahedral normal,
		// roughness, metallic. Transient - only screen-space passes read it.
		RGImage m_sceneGBuffer;
		BindlessManager* m_bindlessManager = nullptr;
		std::uint32_t m_sceneDepthBindlessSlot = 0xFFFFFFFFu;
		std::uint32_t m_sceneGBufferBindlessSlot = 0xFFFFFFFFu;
		GraphicsPipeline m_prepassPipeline;
		// Screen-space reflections march into their own buffer, because the march has to
		// sample the HDR colour it would otherwise be writing.
		RGImage m_ssrColor;
		std::uint32_t m_ssrBindlessSlot = 0xFFFFFFFFu;
		GraphicsPipeline m_ssrPipeline;
		GraphicsPipeline m_ssrCompositePipeline;

		// producer thread and read in the render-thread Execute (a torn read just
		GraphicsPipeline m_texturePreviewPipeline;
		gpu::TextureHandle m_texturePreviewHandle;
		RGImage m_texturePreview;
		gpu::ImageView m_texturePreviewView = nullptr;
		gpu::Extent2D m_previewSrcExtent;
		std::uint32_t m_previewSrcSlot = 0xFFFFFFFFu;
		std::uint32_t m_previewChannel = 0;
		float m_previewExposure = 1.0f;
		std::uint32_t m_previewFlags = 0;
		std::uint32_t m_previewTonemap = 0;
		bool m_previewEnabled = false;
		std::uint32_t m_texturePreviewGeneration = 0;
		std::function<std::uint64_t()> m_frameIndexProvider;
		PhysicsDebugRenderer m_physicsDebug;
		ui::UiRenderer m_uiRenderer;
		std::atomic_bool m_forwardPassEnabled = true;
		std::atomic_uint32_t m_sceneFeatures{static_cast<std::uint32_t>(DefaultSceneFeatures(SceneKind::Scene3D))};
		std::atomic_bool m_frameSceneDraws = false;
		std::atomic_bool m_sceneViewportRebuildPending = false;
		std::atomic_bool m_lazyTargetRebuildPending = false;
		LazyTargetGates m_lazyGates;
		mutable std::mutex m_sceneViewportMutex;
		bool m_sceneViewportEnabled = false;
		float m_renderScale = 1.0f;
		bool m_requestedSceneViewportEnabled = false;
		SceneViewportSettings m_sceneViewportSettings;
		SceneViewportSettings m_requestedSceneViewportSettings;
	};
} // namespace aether
