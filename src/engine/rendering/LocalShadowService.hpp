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
	struct ShadowLightData
	{
		glm::mat4 viewProj{1.0f};
		glm::vec4 atlasRegion{0.0f};
		// World units a single shadow texel covers per unit of distance from the light
		// (2*tan(fov/2)/resolution). Bias scales off this, so it tracks the real texel
		// footprint at the receiver instead of a constant that is wrong at some range.
		float texelScale = 0.0f;
		std::uint32_t lightType = 0;
		float _pad1 = 0.0f;
		float sourceRadius = 0.1f;
		glm::vec4 lightPosRange{0.0f};
	};

	static_assert(sizeof(ShadowLightData) == 112, "ShadowLightData must be 112 bytes for GPU layout");
	static_assert(offsetof(ShadowLightData, viewProj) == 0, "ShadowLightData viewProj offset mismatch");
	static_assert(offsetof(ShadowLightData, atlasRegion) == 64, "ShadowLightData atlasRegion offset mismatch");
	static_assert(offsetof(ShadowLightData, texelScale) == 80, "ShadowLightData texelScale offset mismatch");
	static_assert(offsetof(ShadowLightData, lightType) == 84, "ShadowLightData lightType offset mismatch");
	static_assert(offsetof(ShadowLightData, sourceRadius) == 92, "ShadowLightData sourceRadius offset mismatch");
	static_assert(offsetof(ShadowLightData, lightPosRange) == 96, "ShadowLightData lightPosRange offset mismatch");

	inline constexpr std::uint32_t kMaxLocalShadows = 256u;

	class LocalShadowService
	{
	public:
		void Initialize(VulkanContext& context, BindlessManager& bindless, const Swapchain& swapchain, const RenderQueueSharedPipelines& pipelines);
		void Shutdown();

		// The atlas is 128 MB and drags a 64 MB depth target plus 256 MB of blur scratch
		// through the graph's transient heap behind it. Nothing but a shadow-casting local
		// light with geometry in front of it can use any of it, so the atlas is created on
		// demand. Resolution and entry budget are unchanged; only the moment of allocation
		// moved. Both calls must run with the GPU quiesced.
		void CreateShadowTargets();
		void DestroyShadowTargets();

		[[nodiscard]] bool HasShadowTargets() const
		{
			return m_atlasReady;
		}

		// Returns true when this frame submitted at least one shadow-casting draw - the
		// geometry half of the gate the atlas is allocated on.
		[[nodiscard]] bool PrepareQueues(std::uint32_t drawSlot, World& world);

		[[nodiscard]] ShadowAtlasManager& GetAtlasManager()
		{
			return m_atlasManager;
		}

		[[nodiscard]] RenderQueue& GetShadowQueue()
		{
			return m_shadowRenderQueue;
		}

		// The shadow render queue is populated by PrepareQueues on the game thread;
		void BuildFrameShadowData(const RenderFramePacket& packet, std::uint32_t frameIdx, CameraManager& cameraManager, FrameConstants& fc);

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
			float texelScale = 0.0f;
			float sourceRadius = 0.1f;
			std::uint32_t lightType = 0;
			glm::vec4 lightPosRange{0.0f};
		};

		ShadowAtlasManager m_atlasManager;
		RenderQueue m_shadowRenderQueue;
		GraphicsPipeline m_shadowPipeline;
		PreparedDrawList m_shadowDrawList{};
		RGImage m_atlasImage{};
		// Cleared and discarded inside $LocalShadowAtlasRender and never read anywhere else,
		// so the graph owns it and may lend its 64 MB to another pass.
		RGImage m_atlasDepthImage{};
		gpu::Format m_atlasDepthFormat = gpu::Format::Undefined;
		std::uint32_t m_atlasBindlessSlot = 0xFFFFFFFFu;
		bool m_atlasReady = false;

		std::vector<PerLightShadow> m_perLightShadows;

		struct PerFrameMapping
		{
			gpu::BufferHandle handle{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
		};

		std::array<PerFrameMapping, kMaxFramesInFlight> m_shadowDataBuffer;

		std::array<PerFrameMapping, kMaxFramesInFlight> m_lightConstantsBuffer;

		std::vector<glm::vec2> m_lightShadowIndices;

	};
} // namespace aether
