#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

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
	class TextureRegistry;
	class BindlessManager;
	class CullPass;
	class PostProcessStack;
	class VulkanContext;

	// Threading: ShowModel / ClearModel / PrepareQueue run on the game thread;
	class ModelPreviewService
	{
	public:
		static constexpr std::uint32_t kSize = 384u;

		// `size` is the square edge this instance renders at, and `passPrefix` names its graph
		// passes. Both exist so a second instance can bake small material thumbnails without
		// colliding with the interactive preview's pass names or borrowing its resolution.
		void Initialize(VulkanContext& context,
		        BindlessManager& bindless,
		        const RenderQueueSharedPipelines& pipelines,
		        gpu::Format colorFormat,
		        gpu::Format depthFormat,
		        std::uint32_t size = kSize,
		        std::string_view passPrefix = "$ModelPreview",
		        std::uint32_t atlasCols = 1);

		// Bake slots. This instance's image is an atlas of `atlasCols x atlasCols` squares of
		// `size`, and one thumbnail is rendered into one of those squares.
		//
		// An atlas rather than an image per material because the render graph is STATIC: passes
		// are registered once at startup, so the image a pass draws into is fixed for the life
		// of the graph. A per-material target is therefore impossible - but a per-material
		// RECTANGLE of one fixed target is not, and it costs a single allocation rather than one
		// per material. The tonemap loads rather than clears, so finished slots survive.
		//
		// -1 renders nothing, which is what an idle baker does between bakes.
		void SetBakeSlot(const int slot) noexcept
		{
			m_bakeSlot.store(slot, std::memory_order_relaxed);
		}

		// The last atlas slot actually DRAWN, as reported by the pass itself. A caller cannot
		// infer this from elapsed frames: if the preview had nothing to submit, the pass draws
		// nothing and the slot keeps whatever undefined contents it had. Showing a slot on a
		// frame count alone is how an unrendered thumbnail appears as a black square.
		[[nodiscard]] int LastDrawnSlot() const noexcept
		{
			return m_drawnSlot.load(std::memory_order_relaxed);
		}

		[[nodiscard]] std::uint32_t GetAtlasColumns() const noexcept
		{
			return m_atlasCols;
		}

		[[nodiscard]] std::uint32_t GetSlotCount() const noexcept
		{
			return m_atlasCols * m_atlasCols;
		}
		void Shutdown(AssetManager* assets);

		// Game thread: load `path` (VFS-aware; the caller bakes raw glTF first)
		bool ShowModel(AssetManager& assets, const std::string& path, std::string& outError);

		// Show one mesh wearing an arbitrary material, for previewing a material on its own.
		// The mesh is borrowed, not owned - callers pass a long-lived primitive.
		bool ShowMaterialOnMesh(AssetManager& assets, const Mesh& mesh, const MaterialAsset& material, std::string& outError);

		// Whether the preview is lit by the SCENE's sky or by a neutral studio environment.
		// A scene sky is what the material will actually sit in, but it also tints every
		// preview by whatever the level happens to look like, which makes two materials
		// impossible to compare.
		void SetSceneEnvironmentEnabled(bool enabled) noexcept
		{
			m_sceneEnvironment.store(enabled, std::memory_order_relaxed);
		}

		[[nodiscard]] bool IsSceneEnvironmentEnabled() const noexcept
		{
			return m_sceneEnvironment.load(std::memory_order_relaxed);
		}
		// Whether the turntable keeps rotating. A moving preview shows a material from every
		// angle, which is what you want while wiring one; it is also the one thing that makes
		// a small detail impossible to look at, which is what you want once it is nearly right.
		void SetTurntableEnabled(bool enabled) noexcept
		{
			m_turntable.store(enabled, std::memory_order_relaxed);
		}

		[[nodiscard]] bool IsTurntableEnabled() const noexcept
		{
			return m_turntable.load(std::memory_order_relaxed);
		}

		void ClearModel(AssetManager& assets);

		// Whether every texture the shown model needs has finished uploading. Baking a
		// thumbnail before that gives an untextured model, and a one-shot bake would keep it
		// forever - the same trap the material thumbnails fell into.
		[[nodiscard]] bool TexturesResident(const TextureRegistry& textures) const;

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

		// Only the LDR image is service-owned: its view is an ImGui texture id. For a baker it
		// is the whole atlas, and tiles sample sub-rectangles of it.
		gpu::TextureHandle m_colorLdrHandle{};
		gpu::ImageView m_colorLdrView = nullptr;
		// Which atlas slot the next render writes into, as requested on the game thread.
		std::atomic<int> m_bakeSlot{-1};
		// Written by the pass on the render thread once it has issued the draw.
		std::atomic<int> m_drawnSlot{-1};
		// ...and that request LATCHED PER FRAME. The render thread records a frame well after
		// the game thread has moved on, so reading the live request in the pass drew most bakes
		// into whatever slot was current by then - usually -1, which skipped the draw and left
		// the slot undefined. Every other piece of per-frame state here travels the same way.
		std::array<std::atomic<int>, kMaxFramesInFlight> m_bakeSlotForFrame{};
		std::uint32_t m_atlasCols = 1;
		std::uint32_t m_size = kSize;
		std::string m_passPrefix = "$ModelPreview";
		// Held as members because the graph keeps the pointer, and two instances must not
		// register passes under the same name.
		std::string m_cullPassName = "$ModelPreviewCull";
		std::string m_forwardPassName = "$ModelPreviewForward";
		std::string m_tonemapPassName = "$ModelPreviewTonemap";
		std::string m_readyPassName = "$ModelPreviewReady";
		std::string m_drawsProductName = "ModelPreviewDraws";
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
		std::atomic<bool> m_turntable{true};
		std::atomic<bool> m_sceneEnvironment{true};
		std::atomic<bool> m_hasModel{false};

		// Turntable camera, produced on the game thread, consumed on the render thread.
		std::mutex m_cameraMutex;
		glm::mat4 m_camView{1.0f};
		glm::mat4 m_camProj{1.0f};
		glm::vec3 m_camPos{0.0f};
	};
} // namespace aether
