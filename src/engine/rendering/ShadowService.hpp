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

	// Owns and orchestrates directional CSM resources and passes.
	//
	// Uses multi-frustum culling: one cull dispatch tests each draw against
	// all 3 cascade view-proj matrices, writing 3 independent output regions.
	// This replaces 3x per-cascade queues that each duplicated the same draw data.
	class ShadowService
	{
	public:
		void Initialize(VulkanContext& context, const Swapchain& swapchain, BindlessManager& bindless, const RenderQueueSharedPipelines& pipelines);
		void Shutdown();

		void RecreatePipeline(gpu::Device device, gpu::Format depthFormat);

		// Set the double-buffer write slot and clear it on the single shadow queue.
		void PrepareWriteSlot(std::uint32_t drawSlot);

		void PrepareQueues(std::uint32_t drawSlot, World& world);
		void SetAnimationDatabase(const AnimationDatabase* animationDb);

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

	private:
		// Single queue replaces the per-cascade arrays - multi-frustum culling
		// handles all 3 cascades in one dispatch on shared draw data.
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
		        gpu::Extent2D{1024u, 1024u},
		};
		std::array<std::uint32_t, kShadowCascadeCount> m_shadowMapSlots{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
	};
} // namespace aether
