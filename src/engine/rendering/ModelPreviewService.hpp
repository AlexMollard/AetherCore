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

	// Renders a model file as a turntable thumbnail into a small offscreen target
	// (File Explorer preview card). Same second-POV skeleton as
	// CameraPreviewService - own queue, frame constants, cull + lit forward +
	// tonemap passes, fully additive - but the scene source is a PRIVATE world
	// holding one entity per model primitive, and the service OWNS the
	// LoadedModel (meshes by value), so nothing couples to the live scene or the
	// scripting SceneContext.
	//
	// Threading: ShowModel / ClearModel / PrepareQueue run on the game thread;
	// BuildFrameConstants runs on the render thread and reads the turntable
	// camera under a mutex (the CameraPreviewService request pattern).
	class ModelPreviewService
	{
	public:
		static constexpr std::uint32_t kSize = 384u; // square thumbnail

		void Initialize(VulkanContext& context, BindlessManager& bindless, const RenderQueueSharedPipelines& pipelines, gpu::Format colorFormat, gpu::Format depthFormat);
		void Shutdown(AssetManager* assets);

		// Game thread: load `path` (VFS-aware; the caller bakes raw glTF first)
		// and stage it for the turntable. Replaces any previous model.
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
		// camera, disable scene shadows/local lights - they belong to the live
		// scene, not this isolated model).
		void BuildFrameConstants(const FrameConstants& mainFc, std::uint32_t frameIdx);

		void RegisterComputePasses(RenderGraph& graph, CullPass& cullPass);
		void RegisterGraphicsPasses(RenderGraph& graph, BindlessManager& bindless, const PostProcessStack& postProcess);

		// Tonemapped LDR view for ImGui display.
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
		glm::vec4 m_bounds{0.0f, 0.0f, 0.0f, 1.0f}; // merged sphere: xyz centre, w radius
		float m_turntableAngle = 0.0f;
		std::atomic<bool> m_hasModel{false};

		// Turntable camera, produced on the game thread, consumed on the render thread.
		std::mutex m_cameraMutex;
		glm::mat4 m_camView{1.0f};
		glm::mat4 m_camProj{1.0f};
		glm::vec3 m_camPos{0.0f};
	};
} // namespace aether
