#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "passes/CullPass.hpp"
#include "passes/GTAOPass.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/LocalShadowService.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "rendering/ShadowService.hpp"
#include "physics/PhysicsDebugRenderer.hpp"

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
		MatchPanel, // render at the Viewport panel's physical pixel size (logical x DPI)
	};

	struct SceneViewportSettings
	{
		SceneViewportResolutionMode resolutionMode = SceneViewportResolutionMode::WindowNative;
		gpu::Extent2D customExtent{1280, 720};
	};

	// Owns all rendering passes and the render graph. Depends on Vulkan context,
	// camera, lighting, and material services from the container.
	class RenderingSubsystem
	{
	public:
		void Init(ServiceContainer& services);
		void Shutdown();
		void RegisterPasses(ServiceContainer& services);

		// Called on swapchain recreation to rebuild extent-dependent resources.
		void RecreateSwapchainResources(ServiceContainer& services);
		void SetSceneViewportEnabled(ServiceContainer& services, bool enabled);
		void SetSceneViewportSettings(ServiceContainer& services, const SceneViewportSettings& settings);
		bool CommitPendingSceneViewportSettings();
		void ApplyPendingSceneViewportChanges(ServiceContainer& services);

		// Non-consuming peek used by the main-thread quiesced recreate to decide
		// whether a rebuild is pending. CommitPendingSceneViewportSettings is the
		// sole consumer of the flag.
		[[nodiscard]] bool IsSceneViewportRebuildPending() const
		{
			return m_sceneViewportRebuildPending.load(std::memory_order_acquire);
		}

		void DiscardPendingFrameQueues(std::uint32_t slot);

		[[nodiscard]] SceneViewportSettings GetSceneViewportSettings() const;
		[[nodiscard]] gpu::Extent2D ResolveRequestedSceneViewportExtent(gpu::Extent2D swapchainExtent) const;

		// Set by AetherCore after construction to provide the current frame index
		// for RTT queue preparation and pass callbacks.
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

		// Fixed square edge of the debug texture-preview render target.
		static constexpr std::uint32_t kTexturePreviewSize = 2048;

		// Textures panel: request a processed preview of the source at `bindlessSlot`
		// (enabled=false / slot 0xFFFFFFFF disables). Read on the render thread each
		// frame by the $TexturePreview pass.
		void SetTexturePreviewRequest(std::uint32_t bindlessSlot, gpu::Extent2D srcExtent, std::uint32_t channel, float exposure, std::uint32_t flags, std::uint32_t tonemapMode, bool enabled);
		[[nodiscard]] gpu::ImageView GetTexturePreviewView() const
		{
			return m_texturePreviewView;
		}

		[[nodiscard]] PhysicsDebugRenderer& GetPhysicsDebugRenderer()
		{
			return m_physicsDebug;
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

		struct PerFrameResourceTable
		{
			gpu::BufferHandle handle{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
		};

		RenderQueueSharedPipelines m_renderQueuePipelines;
		RenderGraph m_renderGraph;
		RenderQueue m_renderQueue;
		Renderer m_renderer;
		FrameConstantsBuffer m_frameConstantsBuffer;
		std::array<PerFrameResourceTable, kMaxFramesInFlight> m_resourceTableBuffers{};
		ShadowService m_shadowService;
		LocalShadowService m_localShadowService;
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

		// Debug texture-preview pass (Textures panel). Request scalars are set on the
		// producer thread and read in the render-thread Execute (a torn read just
		// yields a one-frame-stale preview).
		GraphicsPipeline m_texturePreviewPipeline;
		gpu::TextureHandle m_texturePreviewHandle;
		RGImage m_texturePreview;
		gpu::ImageView m_texturePreviewView = nullptr;
		gpu::Extent2D m_previewSrcExtent{};
		std::uint32_t m_previewSrcSlot = 0xFFFFFFFFu;
		std::uint32_t m_previewChannel = 0;
		float m_previewExposure = 1.0f;
		std::uint32_t m_previewFlags = 0;
		std::uint32_t m_previewTonemap = 0;
		bool m_previewEnabled = false;
		std::function<std::uint64_t()> m_frameIndexProvider;
		PhysicsDebugRenderer m_physicsDebug;
		std::atomic_bool m_forwardPassEnabled = true;
		std::atomic_bool m_sceneViewportRebuildPending = false;
		mutable std::mutex m_sceneViewportMutex;
		bool m_sceneViewportEnabled = false;
		bool m_requestedSceneViewportEnabled = false;
		SceneViewportSettings m_sceneViewportSettings;
		SceneViewportSettings m_requestedSceneViewportSettings;
	};
} // namespace aether
