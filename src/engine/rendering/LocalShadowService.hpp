#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <span>

#include "rendering/FrameConstants.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/ShadowAtlasManager.hpp"
#include "gpu/GpuHandles.hpp"

namespace aether
{
	class BindlessManager;
	class CameraManager;
	class CullPass;
	class Renderer;
	class Swapchain;
	class VulkanContext;
	class World;

	// GPU-side per-light shadow data.
	struct ShadowLightData
	{
		glm::mat4 viewProj{1.0f};
		glm::vec4 atlasRegion{0.0f}; // xy=UV offset, zw=UV scale
		float depthBias = 0.005f;
		std::uint32_t lightType = 0; // 0=spot, 1=point (2 entries: front+back)
		float normalBias = 0.015f;
		float _pad1 = 0.0f;
	};

	static_assert(sizeof(ShadowLightData) == 96, "ShadowLightData must be 96 bytes for GPU layout");

	// Maximum number of local shadow lights rendered per frame.
	inline constexpr std::uint32_t kMaxLocalShadows = 256u;

	// Orchestrates spot and point light shadow rendering into a shared atlas.
	// Responsibilities:
	//   - Manage shadow atlas (allocate/free regions via ShadowAtlasManager)
	//   - Cull and prioritize lights that should cast shadows
	//   - Build and upload per-light ShadowLightData to GPU
	//   - Register render graph passes (cull + atlas render + blur)
	//   - Write shadow data into FrameConstants
	class LocalShadowService
	{
	public:
		void Initialize(VulkanContext& context, BindlessManager& bindless, const Swapchain& swapchain, const RenderQueueSharedPipelines& pipelines);
		void Shutdown();

		void PrepareQueues(std::uint32_t drawSlot, World& world);

		[[nodiscard]] ShadowAtlasManager& GetAtlasManager()
		{
			return m_atlasManager;
		}

		[[nodiscard]] RenderQueue& GetShadowQueue()
		{
			return m_shadowRenderQueue;
		}

		// Called during frame constant composition to build per-light shadow data,
		// allocate atlas regions, and fill FrameConstants with shadow info.
		// Re-populates the shadow render queue from Scene/World to capture any
		// mid-frame MeshComponent changes from game systems.
		void BuildFrameShadowData(const RenderFramePacket& packet, std::uint32_t frameIdx, CameraManager& cameraManager, World& world, FrameConstants& fc);

		// Register render graph passes: cull shadow casters, render atlas, blur.
		void RegisterPasses(RenderGraph& graph, CullPass& cullPass, gpu::Format depthFormat);
		void SetupPassResources(RenderGraph& graph, gpu::Format depthFormat);
		void RegisterComputePasses(RenderGraph& graph, CullPass& cullPass);
		void RegisterGraphicsPasses(RenderGraph& graph);

		[[nodiscard]] RGImage GetAtlasRGImage() const
		{
			return m_atlasImage;
		}

		// Returns per-light shadow indices (shadowIndex, shadowStrength) for all lights
		// in the GpuLight buffer order (points first, then spots).
		// -1 = no shadow for this light.
		[[nodiscard]] const std::vector<glm::vec2>& GetLightShadowIndices() const
		{
			return m_lightShadowIndices;
		}

		void ClearAllQueues()
		{
			for (std::uint32_t i = 0; i < RenderQueue::kFramesInFlight; ++i)
			{
				m_shadowRenderQueue.Clear(i);
			}
		}

	private:
		struct PerLightShadow
		{
			glm::mat4 viewProj{1.0f};
			ShadowAtlasManager::Region region;
			float depthBias = 0.005f;
			float normalBias = 0.015f;
			std::uint32_t lightType = 0;
		};

		ShadowAtlasManager m_atlasManager;
		RenderQueue m_shadowRenderQueue;
		GraphicsPipeline m_shadowPipeline;
		RGImage m_atlasImage{};
		RGImage m_atlasDepthImage{};
		std::uint32_t m_atlasBindlessSlot = 0xFFFFFFFFu;

		// Per-light shadow data (CPU side, rebuilt each frame).
		std::vector<PerLightShadow> m_perLightShadows;

		struct PerFrameMapping
		{
			gpu::BufferHandle handle{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
		};

		// Per-frame GPU buffer for ShadowLightData array (double-buffered for kMaxFramesInFlight).
		std::array<PerFrameMapping, kMaxFramesInFlight> m_shadowDataBuffer;

		// Per-frame small per-light frame constants buffer for atlas rendering.
		// Each light gets a full FrameConstants-sized block.
		std::array<PerFrameMapping, kMaxFramesInFlight> m_lightConstantsBuffer;

		// Shadow index for each light in GpuLight buffer order:
		// x=shadowDataIndex(-1=none), y=shadowStrength.
		std::vector<glm::vec2> m_lightShadowIndices;

		// -- VSM blur resources ----------------------------------------------
		gpu::PipelineHandle m_blurPipelineHandle;
		gpu::BufferHandle m_blurBuffer;
		gpu::DeviceAddress m_blurBufferAddr = 0;
		RGBuffer m_blurBufferRG{};
	};
} // namespace aether
