#pragma once

#include <array>
#include <cstdint>
#include <span>
#include "volk.hpp"

#include "FrameConstants.hpp"
#include "FrameConstantsBuffer.hpp"
#include "GraphicsPipeline.hpp"
#include "RenderGraph.hpp"
#include "RenderQueue.hpp"
#include "RenderThread.hpp"

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
	class ShadowService
	{
	public:
		void Initialize(VulkanContext& context, const Swapchain& swapchain);
		void Shutdown(VkDevice device);

		void RecreatePipeline(VkDevice device, VkFormat depthFormat);

		// Set the double-buffer write slot and clear it on every cascade queue.
		// Must be called before any SubmitShadowCaster calls for this frame
		// (i.e. at the same point the main RenderQueue is set up and cleared).
		void PrepareWriteSlot(std::uint32_t drawSlot);

		void PrepareQueues(std::uint32_t drawSlot, Scene& scene, World& world);
		void SetAnimationDatabase(const AnimationDatabase* animationDb);

		// Submit a draw command as a shadow caster to every cascade queue.
		// Call after PrepareQueues, before the render thread consumes the queues.
		void SubmitShadowCaster(const DrawCommand& cmd);

		void RegisterPasses(RenderGraph& graph, BindlessManager& bindlessManager, VkDevice device, const CullPass& cullPass, VkFormat depthFormat);
		void BuildFrameShadowData(const RenderFramePacket& packet, std::uint32_t frameIdx, CameraManager& cameraManager, FrameConstants& fc);

		[[nodiscard]] std::span<const RGImage> GetShadowDepthImages() const
		{
			return std::span<const RGImage>(m_shadowDepth.data(), m_shadowDepth.size());
		}

	private:
		std::array<RenderQueue, kShadowCascadeCount> m_shadowRenderQueues;
		std::array<RenderQueue, kShadowCascadeCount> m_voxelShadowRenderQueues;
		std::array<FrameConstantsBuffer, kShadowCascadeCount> m_shadowFrameConstants;
		GraphicsPipeline m_shadowPipeline;
		GraphicsPipeline m_voxelShadowPipeline;
		std::array<RGImage, kShadowCascadeCount> m_shadowDepth{};
		std::array<VkExtent2D, kShadowCascadeCount> m_shadowMapExtents{
			VkExtent2D{ 4096u, 4096u },
			VkExtent2D{ 2048u, 2048u },
			VkExtent2D{ 1024u, 1024u },
		};
		std::array<std::uint32_t, kShadowCascadeCount> m_shadowMapSlots{ 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	};
} // namespace aether
