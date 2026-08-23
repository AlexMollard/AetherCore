#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "gpu/ResourceRegistry.hpp"
#include "material/TextureHandle.hpp"
#include "ui/CursorService.hpp"
#include "rendering/RenderGraphTypes.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiDrawCommand.hpp"

namespace aether
{
	class TextureRegistry;
	class World;
	class RenderGraph;
	class BindlessManager;
	class GpuDevice;
	struct PassContext;

	namespace gpu
	{
		class UploadContext;
	}
} // namespace aether

namespace aether::ui
{
	//      layout against m_world, builds the command list, uploads it into the
	class UiRenderer
	{
	public:
		void Init(GpuDevice& gpu, gpu::UploadContext& upload, TextureRegistry& textures, gpu::Format colorFormat);
		void Shutdown();

		// The engine pointer (see ui::CursorService). Set once at startup; drawn as the last command of
		// every frame so it is over every canvas, every overlay and every scene load.
		void SetCursorService(CursorService* cursor)
		{
			m_cursor = cursor;
		}

		void SetWorld(World* world)
		{
			m_world = world;
		}

		// The atlases this renderer bakes are the only ones in the process. UI systems that
		// measure text (text-box hit-testing, scrolling) need them, so hand out a reference
		// rather than letting anyone build a second, empty registry.
		[[nodiscard]] FontRegistry& Fonts()
		{
			return m_fontRegistry;
		}

		// Resolves layout + builds draw commands for m_world, uploads into frame
		void BuildFrame(glm::vec2 outputExtent, std::uint32_t frameSlot);

		void RegisterPass(RenderGraph& graph, RGImage color, gpu::Extent2D extent, BindlessManager& bindless);

	private:
		static constexpr std::uint32_t kFrames = aether::kMaxFramesInFlight;

		// A UI element rendered by its own pipeline (UIEffect), on top of the batched shapes.
		struct EffectDraw
		{
			gpu::PipelineHandle pipeline{};
			glm::vec4 rect{0.f};
			glm::vec4 params{0.f};
			glm::vec4 color0{0.f};
			glm::vec4 color1{0.f};
			bool background = false;
			int sortOrder = 0; // draw order within the background/overlay group (higher = on top)
		};

		// A contiguous run of batched draw commands sharing one shaderId (0 = default fragment).
		struct DrawGroup
		{
			std::uint32_t first = 0;
			std::uint32_t count = 0;
			std::uint32_t shaderId = 0;
			gpu::PipelineHandle pipeline{}; // resolved on the producer thread (default or material)
		};

		// One GPU-visible command buffer. A frame slot owns several and rotates between them.
		struct SlotBuffer
		{
			gpu::BufferHandle buffer{};
			void* mapped = nullptr;
			std::uint64_t address = 0;
			std::uint32_t capacity = 0;
		};

		struct Frame
		{
			// A slot is reused every kFrames frames, but the GPU is not necessarily finished
			// with it by then. The pass bakes each DrawGroup's device address into the recorded
			// command buffer as a push constant, so the GPU reads this MEMORY well after the
			// render thread has moved on - and the producer, which is throttled against frame
			// SUBMISSION rather than GPU completion, was free to memcpy over it in the meantime.
			// That is what turned rites into flat fills and stretched a material across the
			// panel: correct commands, overwritten underneath the GPU.
			//
			// Rotating between several buffers means a rebuild never touches the memory the GPU
			// is still reading. Three of them tolerates the GPU running nine frames behind the
			// producer, which is far past anything the frame throttle permits.
			//
			// Neither of the usual tools can see a hazard of this shape, which is worth knowing
			// before trusting them on the next one. Sync validation models hazards between GPU
			// ACCESSES; a host memcpy into a persistently mapped CpuToGpu buffer is outside that
			// model, so a clean sync-validation run says nothing about it. Loading the validation
			// layer at all serialises enough to hide it outright. The lever that does answer the
			// question is graphics framesInFlight: at the default of 2 it reproduces, at 1 the
			// slot cannot be reused early and it stops.
			static constexpr std::uint32_t kBuffersPerSlot = 3;
			std::array<SlotBuffer, kBuffersPerSlot> buffers{};
			std::uint32_t cursor = 0;

			// The buffer chosen for the frame currently being built, and then recorded. Safe to
			// read at record time: the producer is throttled to fewer frames ahead than it takes
			// to come back round to this slot, so it cannot have re-pointed these yet.
			void* mapped = nullptr;
			std::uint64_t address = 0;
			std::uint32_t count = 0;
			// Screen extent the commands in this slot's buffer were resolved against == the
			// shader's screenSize. Paired per-slot with `address` (not a shared member) so a
			// producer-thread BuildFrame for the next frame cannot overwrite the extent the
			// render thread still needs for this slot's not-yet-executed pass (e.g. across a
			// viewport resize).
			glm::vec2 extent{0.f};
			std::vector<EffectDraw> effects;
			// Custom-material batches: the material table (shaderId-1 == index) and the split of the
			// command buffer into runs, each drawn with its material's fragment shader.
			std::vector<UiMaterialDraw> materials;
			std::vector<DrawGroup> groups;

			// Which absolute frame slot BuildFrame filled this in for, and how many times this
			// slot has been rebuilt. The pass records from m_frames[ctx.frameSlot % kFrames] and
			// the build indexes m_frames[packet.drawSlot % kFrames]; if those two ever disagree,
			// a pass draws one frame's groups against another frame's command buffer, which
			// shows up as a material's fragment stretched across an unrelated element. Checked
			// rather than assumed - see the check in RegisterPass's Execute.
			std::uint32_t builtForSlot = 0xFFFFFFFFu;
			std::uint64_t buildSeq = 0;
		};

		void AppendCursor();
		static void EnsureCapacity(SlotBuffer& buf, std::uint32_t count);
		bool EnsureFontCurvesUploaded(std::string_view name);
		// Per-frame gate in front of the upload: cheap when the font already holds a live slot,
		// uploads when it does not, and reports a font that cannot be uploaded exactly once.
		void EnsureFontReady(std::string_view name);
		// Lazily create + cache a pipeline for a UIEffect shader ("shaders://<shader>.spv").
		gpu::PipelineHandle EffectPipeline(const std::string& shader);
		// Lazily create + cache a per-element material pipeline: the shared ui_shapes vertex shader
		// paired with the material's custom fragment ("shaders://<shader>.spv").
		gpu::PipelineHandle MaterialPipeline(const std::string& shader);

		World* m_world = nullptr;
		CursorService* m_cursor = nullptr;
		std::string m_cursorTexturePath;
		TextureHandle m_cursorTexture{};
		GpuDevice* m_gpu = nullptr;
		gpu::UploadContext* m_upload = nullptr;
		TextureRegistry* m_textures = nullptr;
		FontRegistry m_fontRegistry;
		// One curve texture per font, keyed by font name, and the ONLY owning reference to
		// each. A single handle here used to be overwritten by every upload, which dropped the
		// previous font's texture: its bindless slot could then be recycled by another texture
		// while the FontAsset still pointed at it, and glyphs sampled whatever had taken the
		// slot - text that corrupted and then vanished across play/stop cycles.
		std::unordered_map<std::string, gpu::TextureHandle> m_fontCurveTextures;
		bool m_defaultFontReady = false;
		// Font names whose atlas upload has already FAILED, so the error is reported once
		// rather than every frame. Successes are not memoised: EnsureFontAtlasUploaded early-
		// outs on a font that still holds a valid slot, which is also what lets a font whose
		// slot was invalidated re-upload instead of staying broken for the process.
		std::unordered_set<std::string> m_fontUploadFailed;
		// Effect pipelines retired after a shader overlay change; destroyed a few frames later
		// (they may still be referenced by in-flight command buffers).
		struct RetiringPipeline
		{
			gpu::PipelineHandle pipeline{};
			int framesLeft = 0;
		};

		gpu::PipelineHandle m_pipeline{};
		gpu::Format m_colorFormat{};
		std::unordered_map<std::string, gpu::PipelineHandle> m_effectPipelines;
		std::unordered_map<std::string, gpu::PipelineHandle> m_materialPipelines;
		std::vector<RetiringPipeline> m_effectPipelinesRetiring;
		std::uint64_t m_shaderGen = 0;
		std::array<Frame, kFrames> m_frames{};
		/// Monotonic count of BuildFrame calls, stamped into each frame so a drift report says
		/// how stale the data it recorded against actually was.
		std::uint64_t m_buildSeq = 0;
		std::vector<UiDrawCommand> m_scratch;
	};
} // namespace aether::ui
