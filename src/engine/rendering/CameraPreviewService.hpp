#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include <glm/glm.hpp>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"

namespace aether
{
	class BindlessManager;
	class CullPass;
	class LightingManager;
	class VulkanContext;
	class World;

	// Renders a selected editor camera's view into a small offscreen colour target
	// for a picture-in-picture thumbnail (Unity-style camera preview). It owns its
	// OWN render queue, frame-constants, cull + forward passes and target, mirroring
	// the LocalShadowService pattern (a separate POV render). Because it is entirely
	// additive - it never touches the main scene queue, passes or targets - a bug
	// here can degrade only the thumbnail, never the main viewport.
	//
	// Data flow per frame:
	//   producer : SetRequest(view, proj, pos, enabled)   (editor sets the camera)
	//   game     : PrepareQueue(slot, world)              (mirror scene draws)
	//   render   : BuildFrameConstants(mainFc, slot)      (copy main fc, override cam)
	//   render   : cull + forward passes draw into m_color, tonemapped for display
	class CameraPreviewService
	{
	public:
		// Thumbnail resolution (16:9). Small - it is a preview, not the main view.
		static constexpr std::uint32_t kWidth = 480u;
		static constexpr std::uint32_t kHeight = 270u;

		void Initialize(VulkanContext& context, BindlessManager& bindless, const RenderQueueSharedPipelines& pipelines, gpu::Format colorFormat, gpu::Format depthFormat);
		void Shutdown();

		// Producer thread: aim the preview at a camera's view/projection (enabled=false
		// disables it so its passes early-out and it stops mirroring draws).
		void SetRequest(bool enabled, const glm::mat4& view, const glm::mat4& proj, glm::vec3 cameraPos);

		[[nodiscard]] bool IsEnabled() const
		{
			return m_enabled.load(std::memory_order_relaxed);
		}

		// Game thread: mirror the scene's mesh draws into the preview queue.
		void PrepareQueue(std::uint32_t drawSlot, World& world);

		// Render thread: build the preview frame constants (copy the main frame
		// constants for lighting/shadows/resource tables, override the camera view so
		// culling + shading run from the preview POV).
		void BuildFrameConstants(const FrameConstants& mainFc, std::uint32_t frameIdx);

		// Re-register the offscreen target as a render-graph external image; call once
		// per graph rebuild (RenderGraph::Clear drops external registrations).
		void RegisterImages(RenderGraph& graph);

		// Render-graph passes, registered each frame from RenderingSubsystem.
		void RegisterComputePasses(RenderGraph& graph, CullPass& cullPass);
		void RegisterGraphicsPasses(RenderGraph& graph, LightingManager* lighting, BindlessManager& bindless);

		// The ImGui-sampleable preview colour (bindless slot + view). Rendered in the
		// forward HDR format; a UI panel samples it directly (bright areas clip, which
		// is fine for a thumbnail).
		[[nodiscard]] std::uint32_t GetColorBindlessSlot() const
		{
			return m_colorBindlessSlot;
		}

		[[nodiscard]] gpu::ImageView GetColorView() const
		{
			return m_colorView;
		}

		void ClearAllQueues()
		{
			m_queue.DiscardAllPending();
		}

		void DiscardPendingQueue(std::uint32_t slot)
		{
			m_queue.DiscardPending(slot);
		}

	private:
		RenderQueue m_queue;
		FrameConstantsBuffer m_constants;
		PreparedDrawList m_draws{};

		// HDR scene target (rendered lit, forward colour format) + depth. The colour
		// is bindless-sampled so a UI panel can show it.
		gpu::TextureHandle m_colorHandle{};
		gpu::TextureHandle m_depthHandle{};
		gpu::ImageView m_colorView = nullptr;
		RGImage m_color{};
		RGImage m_depth{};
		std::uint32_t m_colorBindlessSlot = 0xFFFFFFFFu;

		gpu::Format m_colorFormat = gpu::Format::Undefined;
		gpu::Format m_depthFormat = gpu::Format::Undefined;
		bool m_initialized = false;

		// Thread-safe request: the producer writes the camera each frame, the render
		// thread reads it once in BuildFrameConstants.
		std::mutex m_requestMutex;
		glm::mat4 m_reqView{1.0f};
		glm::mat4 m_reqProj{1.0f};
		glm::vec3 m_reqCameraPos{0.0f};
		std::atomic<bool> m_enabled{false};
	};
} // namespace aether
