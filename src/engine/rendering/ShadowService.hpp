#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderFramePacket.hpp"

namespace aether
{
	class AnimationDatabase;
	class BindlessManager;
	class CameraManager;
	class CullPass;
	class Swapchain;
	class VulkanContext;
	class World;

	class ShadowService
	{
	public:
		void Initialize(VulkanContext& context, const Swapchain& swapchain, BindlessManager& bindless, const RenderQueueSharedPipelines& pipelines);
		void Shutdown();

		void RecreatePipeline(gpu::Device device, gpu::Format depthFormat);

		// The three cascade depth maps cost 96 MB and only a scene that actually draws
		// shadow-casting geometry under a sun can use a texel of them, so they are
		// created on demand rather than at startup. Resolution is unchanged; only the
		// moment of allocation moved. Both calls must run with the GPU quiesced.
		void CreateShadowTargets();
		void DestroyShadowTargets();

		[[nodiscard]] bool HasShadowTargets() const
		{
			return m_shadowTargetsReady;
		}

		void PrepareWriteSlot(std::uint32_t drawSlot);

		// Returns true when this frame submitted at least one shadow-casting draw. That
		// is the signal the cascade targets are gated on - the geometry that would land
		// in them, not any scene flag.
		[[nodiscard]] bool PrepareQueues(std::uint32_t drawSlot, World& world);
		void SetAnimationDatabase(const AnimationDatabase* animationDb);

		[[nodiscard]] RenderQueue& GetShadowQueue()
		{
			return m_shadowRenderQueue;
		}

		void SetDirectionalShadowEnabled(bool enabled)
		{
			m_directionalShadowEnabled = enabled;
		}

		[[nodiscard]] bool IsDirectionalShadowEnabled() const
		{
			return m_directionalShadowEnabled;
		}

		[[nodiscard]] bool IsDirectionalShadowEnabledForFrame(std::uint32_t frameIdx) const
		{
			return m_directionalShadowFrameEnabled[frameIdx % kMaxFramesInFlight];
		}

		void RegisterPasses(RenderGraph& graph, const CullPass& cullPass);
		void SetupPassResources(RenderGraph& graph);
		void RegisterComputePasses(RenderGraph& graph, const CullPass& cullPass);
		void RegisterGraphicsPasses(RenderGraph& graph);
		void BuildFrameShadowData(const RenderFramePacket& packet, std::uint32_t frameIdx, CameraManager& cameraManager, FrameConstants& fc);

		void ClearAllQueues()
		{
			m_shadowRenderQueue.DiscardAllPending();
		}

		void DiscardPendingQueue(std::uint32_t slot)
		{
			m_shadowRenderQueue.DiscardPending(slot);
		}

		[[nodiscard]] std::span<const RGImage> GetShadowDepthImages() const
		{
			return {m_shadowDepth.data(), m_shadowDepth.size()};
		}

		[[nodiscard]] std::uint32_t GetShadowMapSlot(std::uint32_t cascade) const
		{
			return m_shadowMapSlots[cascade];
		}

		[[nodiscard]] gpu::Extent2D GetShadowMapExtent(std::uint32_t cascade) const
		{
			return m_shadowMapExtents[cascade];
		}

	private:
		RenderQueue m_shadowRenderQueue;
		std::array<FrameConstantsBuffer, kShadowCascadeCount> m_shadowFrameConstants;
		GraphicsPipeline m_shadowPipeline;
		PreparedDrawList m_shadowDrawList{};
		std::array<RGImage, kShadowCascadeCount> m_shadowDepth{};
		std::array<gpu::TextureHandle, kShadowCascadeCount> m_shadowDepthHandle{};
		std::array<gpu::Image, kShadowCascadeCount> m_shadowDepthImage{};
		std::array<gpu::ImageView, kShadowCascadeCount> m_shadowDepthView{};
		BindlessManager* m_bindless = nullptr;
		bool m_directionalShadowEnabled = true;
		std::array<bool, kMaxFramesInFlight> m_directionalShadowFrameEnabled{true, true, true};
		std::array<gpu::Extent2D, kShadowCascadeCount> m_shadowMapExtents{
		        gpu::Extent2D{4096u, 4096u},
		        gpu::Extent2D{2048u, 2048u},
		        gpu::Extent2D{2048u, 2048u},
		};
		std::array<std::uint32_t, kShadowCascadeCount> m_shadowMapSlots{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
		gpu::Format m_shadowDepthFormat = gpu::Format::Undefined;
		bool m_shadowTargetsReady = false;
	};
} // namespace aether
