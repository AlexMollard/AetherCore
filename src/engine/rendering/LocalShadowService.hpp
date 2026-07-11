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
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

namespace aether
{
	class BindlessManager;
	class CameraManager;
	class CullPass;
	class Renderer;
	class Swapchain;
	class VulkanContext;
	class World;

	// GPU-side per-light shadow data. Must match ShadowLightData in
	// shaders/include/LocalShadow.slangh (explicit [[vk::offset]] layout).
	struct ShadowLightData
	{
		glm::mat4 viewProj{1.0f};
		glm::vec4 atlasRegion{0.0f}; // xy=UV offset, zw=UV scale
		float depthBias = 0.01f;     // world units
		std::uint32_t lightType = 0; // 0=spot, 1=point
		float normalBias = 0.03f;    // world units
		float _pad1 = 0.0f;
		glm::vec4 lightPosRange{0.0f}; // xyz=light world position, w=range (normalizes EVSM linear depth)
	};

	static_assert(sizeof(ShadowLightData) == 112, "ShadowLightData must be 112 bytes for GPU layout");
	static_assert(offsetof(ShadowLightData, viewProj) == 0, "ShadowLightData viewProj offset mismatch");
	static_assert(offsetof(ShadowLightData, atlasRegion) == 64, "ShadowLightData atlasRegion offset mismatch");
	static_assert(offsetof(ShadowLightData, depthBias) == 80, "ShadowLightData depthBias offset mismatch");
	static_assert(offsetof(ShadowLightData, lightType) == 84, "ShadowLightData lightType offset mismatch");
	static_assert(offsetof(ShadowLightData, normalBias) == 88, "ShadowLightData normalBias offset mismatch");
	static_assert(offsetof(ShadowLightData, lightPosRange) == 96, "ShadowLightData lightPosRange offset mismatch");

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
		// The shadow render queue is populated by PrepareQueues on the game thread;
		// this render-thread step must not clear or mutate producer-side queue state.
		void BuildFrameShadowData(const RenderFramePacket& packet, std::uint32_t frameIdx, CameraManager& cameraManager, FrameConstants& fc);

		// Register render graph passes: cull shadow casters, render atlas, blur.
		void RegisterPasses(RenderGraph& graph, CullPass& cullPass);
		void SetupPassResources(RenderGraph& graph);
		void RegisterComputePasses(RenderGraph& graph, CullPass& cullPass);
		void RegisterGraphicsPasses(RenderGraph& graph);

		[[nodiscard]] RGImage GetAtlasRGImage() const
		{
			return m_atlasImage;
		}

		[[nodiscard]] std::uint32_t GetAtlasBindlessSlot() const
		{
			return m_atlasBindlessSlot;
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
			m_shadowRenderQueue.DiscardAllPending();
		}

		void DiscardPendingQueue(std::uint32_t slot)
		{
			m_shadowRenderQueue.DiscardPending(slot);
		}

	private:
		struct PerLightShadow
		{
			glm::mat4 viewProj{1.0f};
			ShadowAtlasManager::Region region;
			float depthBias = 0.01f;  // world units
			float normalBias = 0.03f; // world units
			std::uint32_t lightType = 0;
			glm::vec4 lightPosRange{0.0f}; // xyz=light world position, w=range
		};

		ShadowAtlasManager m_atlasManager;
		RenderQueue m_shadowRenderQueue;
		GraphicsPipeline m_shadowPipeline;
		PreparedDrawList m_shadowDrawList{};
		RGImage m_atlasImage{};
		RGImage m_atlasDepthImage{};
		gpu::TextureHandle m_atlasDepthHandle{};
		gpu::Image m_atlasDepthImageVk{};
		gpu::ImageView m_atlasDepthView{};
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
		gpu::BufferHandle m_blurScratchBuffer;
		gpu::DeviceAddress m_blurBufferAddr = 0;
		gpu::DeviceAddress m_blurScratchBufferAddr = 0;
		RGBuffer m_blurBufferRG{};
		RGBuffer m_blurScratchBufferRG{};
	};
} // namespace aether
