#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <glm/glm.hpp>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "scene/LoadedModel.hpp"
#include "scene/World.hpp"

namespace aether
{
	class AssetManager;
	class BindlessManager;
	class CullPass;
	class PostProcessStack;
	class VulkanContext;

	// Threading: ShowModel / ClearModel / PrepareQueue run on the game thread;
	class ModelPreviewService
	{
	public:
		static constexpr std::uint32_t kSize = 384u;

		void Initialize(VulkanContext& context, BindlessManager& bindless, const RenderQueueSharedPipelines& pipelines, gpu::Format colorFormat, gpu::Format depthFormat);
		void Shutdown(AssetManager* assets);

		// Game thread: load `path` (VFS-aware; the caller bakes raw glTF first)
		bool ShowModel(AssetManager& assets, const std::string& path, std::string& outError);
		void ClearModel(AssetManager& assets);

		[[nodiscard]] bool HasModel() const
		{
			return m_hasModel.load(std::memory_order_relaxed);
		}

		[[nodiscard]] std::size_t PrimitiveCount() const
		{
			return m_model.primitives.size();
		}

		// Game thread: advance the turntable and mirror the private world's draws.
		void PrepareQueue(std::uint32_t drawSlot);

		// Render thread: preview frame constants (copy the main fc, override the
		void BuildFrameConstants(const FrameConstants& mainFc, std::uint32_t frameIdx);

		void RegisterComputePasses(RenderGraph& graph, CullPass& cullPass);
		void RegisterGraphicsPasses(RenderGraph& graph, BindlessManager& bindless, const PostProcessStack& postProcess);

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

		[[nodiscard]] RenderQueue& GetRenderQueue()
		{
			return m_queue;
		}

	private:
		void RegisterImages(RenderGraph& graph);
		void DestroyModelEntities(AssetManager& assets);

		RenderQueue m_queue;
		FrameConstantsBuffer m_constants;
		PreparedDrawList m_draws{};

		gpu::TextureHandle m_colorHandle{};
		gpu::TextureHandle m_depthHandle{};
		gpu::TextureHandle m_colorLdrHandle{};
		gpu::ImageView m_colorView = nullptr;
		gpu::ImageView m_colorLdrView = nullptr;
		RGImage m_color{};
		RGImage m_colorLdr{};
		RGImage m_depth{};
		std::uint32_t m_colorBindlessSlot = 0xFFFFFFFFu;

		gpu::Format m_colorFormat = gpu::Format::Undefined;
		gpu::Format m_depthFormat = gpu::Format::Undefined;
		bool m_initialized = false;

		// The isolated model scene (game thread only).
		World m_world;
		LoadedModel m_model;
		glm::vec4 m_bounds{0.0f, 0.0f, 0.0f, 1.0f};
		float m_turntableAngle = 0.0f;
		std::atomic<bool> m_hasModel{false};

		// Turntable camera, produced on the game thread, consumed on the render thread.
		std::mutex m_cameraMutex;
		glm::mat4 m_camView{1.0f};
		glm::mat4 m_camProj{1.0f};
		glm::vec3 m_camPos{0.0f};
	};
} // namespace aether
