#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <span>

#include "rendering/FrameConstants.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderThread.hpp"
#include "rendering/ShadowAtlasManager.hpp"
#include "vulkan/UniqueBuffer.hpp"

namespace aether
{
	class AnimationDatabase;
	class BindlessManager;
	class CameraManager;
	class CullPass;
	class Renderer;
	class Scene;
	class Swapchain;
	class VulkanContext;
	class World;

	// GPU-side per-light shadow data.
	struct ShadowLightData
	{
		glm::mat4 viewProj{ 1.0f };
		glm::vec4 atlasRegion{ 0.0f }; // xy=UV offset, zw=UV scale
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
		void Shutdown(VkDevice device);

		void PrepareQueues(std::uint32_t drawSlot, Scene& scene, World& world);

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
		void BuildFrameShadowData(const RenderFramePacket& packet, std::uint32_t frameIdx, CameraManager& cameraManager, const Renderer& renderer, Scene& scene, World& world, FrameConstants& fc);

		// Register render graph passes: cull shadow casters, render atlas, blur.
		void RegisterPasses(RenderGraph& graph, BindlessManager& bindless, VkDevice device, CullPass& cullPass, VkFormat depthFormat);

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

	private:
		struct PerLightShadow
		{
			glm::mat4 viewProj{ 1.0f };
			ShadowAtlasManager::Region region;
			float depthBias = 0.005f;
			float normalBias = 0.015f;
			std::uint32_t lightType = 0;
		};

		ShadowAtlasManager m_atlasManager;
		RenderQueue m_shadowRenderQueue;
		GraphicsPipeline m_shadowPipeline;
		RGImage m_atlasImage{};
		std::uint32_t m_atlasBindlessSlot = 0xFFFFFFFFu;

		// Per-light shadow data (CPU side, rebuilt each frame).
		std::vector<PerLightShadow> m_perLightShadows;

		// Per-frame GPU buffer for ShadowLightData array (double-buffered for kMaxFramesInFlight).
		std::array<UniqueBuffer, kMaxFramesInFlight> m_shadowDataBuffer;
		std::array<VkDeviceAddress, kMaxFramesInFlight> m_shadowDataAddr{};

		// Per-frame small per-light frame constants buffer for atlas rendering.
		// Each light gets a full FrameConstants-sized block.
		std::array<UniqueBuffer, kMaxFramesInFlight> m_lightConstantsBuffer;
		std::array<VkDeviceAddress, kMaxFramesInFlight> m_lightConstantsAddr{};

		// Shadow index for each light in GpuLight buffer order:
		// x=shadowDataIndex(-1=none), y=shadowStrength.
		std::vector<glm::vec2> m_lightShadowIndices;

		// ── VSM blur resources ──────────────────────────────────────────────
		VkPipeline m_blurPipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_blurPipelineLayout = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_blurDescriptorSetLayout = VK_NULL_HANDLE;
		VkDescriptorPool m_blurDescriptorPool = VK_NULL_HANDLE;
		VkDescriptorSet m_blurDescriptorSetH = VK_NULL_HANDLE; // horizontal: output=scratch, input=atlas
		VkDescriptorSet m_blurDescriptorSetV = VK_NULL_HANDLE; // vertical:   output=atlas,   input=scratch
		VkSampler m_blurSampler = VK_NULL_HANDLE;
		UniqueImage m_blurScratch;
		RGImage m_blurScratchImage{};
	};
} // namespace aether
