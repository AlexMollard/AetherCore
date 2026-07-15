#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

#include <glm/glm.hpp>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "rendering/LightingManager.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"

namespace aether
{
	class BindlessManager;
	class CullPass;
	class LightingManager;
	class PostProcessStack;
	class Renderer2D;
	class VulkanContext;
	class World;

	// additive - it never touches the main scene queue, passes or targets - a bug
	class CameraPreviewService
	{
	public:
		static constexpr std::uint32_t kWidth = 480u;
		static constexpr std::uint32_t kHeight = 270u;

		void Initialize(VulkanContext& context, BindlessManager& bindless, const RenderQueueSharedPipelines& pipelines, gpu::Format colorFormat, gpu::Format depthFormat);
		void Shutdown();

		// Producer thread: aim the preview at a camera's view/projection (enabled=false
		void SetRequest(bool enabled, const glm::mat4& view, const glm::mat4& proj, glm::vec3 cameraPos, float nearPlane = 0.1f, bool drawSkybox = true);

		[[nodiscard]] bool IsEnabled() const
		{
			return m_enabled.load(std::memory_order_relaxed);
		}

		// Game thread: mirror the scene's mesh draws into the preview queue.
		void PrepareQueue(std::uint32_t drawSlot, World& world);

		// Render thread: build the preview frame constants (copy the main frame
		void BuildFrameConstants(const FrameConstants& mainFc, std::uint32_t frameIdx, LightingManager* lighting);

		void RegisterImages(RenderGraph& graph);

		void RegisterComputePasses(RenderGraph& graph, CullPass& cullPass);
		void RegisterGraphicsPasses(RenderGraph& graph,
		        LightingManager* lighting,
		        BindlessManager& bindless,
		        const PostProcessStack& postProcess,
		        gpu::PipelineView skyboxPipeline,
		        Renderer2D& renderer2D);

		[[nodiscard]] gpu::ImageView GetColorView() const
		{
			return m_colorLdrView;
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

		gpu::TextureHandle m_colorHandle{};
		gpu::TextureHandle m_depthHandle{};
		gpu::ImageView m_colorView = nullptr;
		RGImage m_color{};
		RGImage m_depth{};
		std::uint32_t m_colorBindlessSlot = 0xFFFFFFFFu;

		gpu::TextureHandle m_colorLdrHandle{};
		gpu::ImageView m_colorLdrView = nullptr;
		RGImage m_colorLdr{};

		LightingManager* m_lighting = nullptr;
		LightViewId m_lightView = kInvalidLightView;

		gpu::Format m_colorFormat = gpu::Format::Undefined;
		gpu::Format m_depthFormat = gpu::Format::Undefined;
		bool m_initialized = false;

		// Thread-safe request: the producer writes the camera each frame, the render
		std::mutex m_requestMutex;
		glm::mat4 m_reqView{1.0f};
		glm::mat4 m_reqProj{1.0f};
		glm::vec3 m_reqCameraPos{0.0f};
		float m_reqNearPlane = 0.1f;
		std::atomic<bool> m_enabled{false};
		std::atomic<bool> m_drawSkybox{true};
	};
} // namespace aether
