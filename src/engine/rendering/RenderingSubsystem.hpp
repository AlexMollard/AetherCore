#pragma once

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
#include "passes/PostProcessStack.hpp"
#include "rendering/LocalShadowService.hpp"
#include "rendering/Renderer.hpp"
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
		void Shutdown();
		void RegisterPasses(ServiceContainer& services);

		void RecreateSwapchainResources(ServiceContainer& services);
		void SetSceneViewportEnabled(ServiceContainer& services, bool enabled);
		void SetSceneViewportSettings(ServiceContainer& services, const SceneViewportSettings& settings);
		bool CommitPendingSceneViewportSettings();
		void ApplyPendingSceneViewportChanges(ServiceContainer& services);

		// Non-consuming peek used by the main-thread quiesced recreate to decide
		[[nodiscard]] bool IsSceneViewportRebuildPending() const
		{
			return m_sceneViewportRebuildPending.load(std::memory_order_acquire);
		}

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

		[[nodiscard]] gpu::ImageView GetTexturePreviewView() const
		{
			return m_texturePreviewView;
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
		gpu::TextureHandle m_sceneDepthHandle;
		RGImage m_sceneDepth;
		BindlessManager* m_bindlessManager = nullptr;
		std::uint32_t m_sceneDepthBindlessSlot = 0xFFFFFFFFu;

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
		std::function<std::uint64_t()> m_frameIndexProvider;
		PhysicsDebugRenderer m_physicsDebug;
		ui::UiRenderer m_uiRenderer;
		std::atomic_bool m_forwardPassEnabled = true;
		std::atomic_uint32_t m_sceneFeatures{static_cast<std::uint32_t>(DefaultSceneFeatures(SceneKind::Scene3D))};
		std::atomic_bool m_sceneViewportRebuildPending = false;
		mutable std::mutex m_sceneViewportMutex;
		bool m_sceneViewportEnabled = false;
		bool m_requestedSceneViewportEnabled = false;
		SceneViewportSettings m_sceneViewportSettings;
		SceneViewportSettings m_requestedSceneViewportSettings;
	};
} // namespace aether
