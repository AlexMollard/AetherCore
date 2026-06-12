#pragma once

#include <array>
#include <cstdint>
#include <span>
#include "vulkan/volk.hpp"

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
	class Scene;
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
		void Initialize(VulkanContext& context, const Swapchain& swapchain, const RenderQueueSharedPipelines& pipelines);
		void Shutdown(VkDevice device);

		void RecreatePipeline(VkDevice device, VkPipelineCache pipelineCache, gpu::Format depthFormat);

		// Set the double-buffer write slot and clear it on the single shadow queue.
		void PrepareWriteSlot(std::uint32_t drawSlot);

		void PrepareQueues(std::uint32_t drawSlot, Scene& scene, World& world);
		void SetAnimationDatabase(const AnimationDatabase* animationDb);

		void RegisterPasses(RenderGraph& graph, BindlessManager& bindlessManager, VkDevice device, const CullPass& cullPass, gpu::Format depthFormat);
		void BuildFrameShadowData(const RenderFramePacket& packet, std::uint32_t frameIdx, CameraManager& cameraManager, FrameConstants& fc);

		void ClearAllQueues()
		{
			for (std::uint32_t i = 0; i < RenderQueue::kFramesInFlight; ++i)
			{
				m_shadowRenderQueue.Clear(i);
			}
		}

		[[nodiscard]] std::span<const RGImage> GetShadowDepthImages() const
		{
			return std::span<const RGImage>(m_shadowDepth.data(), m_shadowDepth.size());
		}

	private:
		// Single queue replaces the per-cascade arrays - multi-frustum culling
		// handles all 3 cascades in one dispatch on shared draw data.
		RenderQueue m_shadowRenderQueue;
		std::array<FrameConstantsBuffer, kShadowCascadeCount> m_shadowFrameConstants;
		GraphicsPipeline m_shadowPipeline;
		std::array<RGImage, kShadowCascadeCount> m_shadowDepth{};
		std::array<VkExtent2D, kShadowCascadeCount> m_shadowMapExtents{
		        VkExtent2D{4096u, 4096u},
		        VkExtent2D{2048u, 2048u},
		        VkExtent2D{1024u, 1024u},
		};
		std::array<std::uint32_t, kShadowCascadeCount> m_shadowMapSlots{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
	};
} // namespace aether
