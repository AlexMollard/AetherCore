#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "gpu/ResourceRegistry.hpp"        // BufferHandle, PipelineHandle, gpu:: enums (also pulls in aether::kMaxFramesInFlight via GpuTypes.hpp)
#include "rendering/RenderGraphTypes.hpp"  // RGImage, PassContext
#include "ui/UiDrawCommand.hpp"

namespace aether
{
	class World;
	class RenderGraph;
	class BindlessManager;
	class GpuDevice;
	struct PassContext;
} // namespace aether

namespace aether::ui
{
	// Draws the UI command buffer built by UiLayoutSystem::ResolveCanvases +
	// BuildDrawCommands using the ui_shapes.slang pipeline (rects / rounded
	// rects / circles / lines / textured rects / SDF glyphs, one instanced
	// quad per command, read from a device-address command buffer).
	//
	// Usage per frame:
	//   1. BuildFrame(outputExtent, frameSlot)  - game/render-prep time: resolves
	//      layout against m_world, builds the command list, uploads it into the
	//      frame-slot's mapped device buffer.
	//   2. RegisterPass(graph, color, extent, bindless) - render-graph build time:
	//      appends the "$UiOverlay" pass (once); each frame it draws whatever
	//      BuildFrame uploaded for the slot then in flight (live ctx.frameSlot).
	class UiRenderer
	{
	public:
		void Init(GpuDevice& gpu, gpu::Format colorFormat);
		void Shutdown();

		void SetWorld(World* world)
		{
			m_world = world;
		}

		// Resolves layout + builds draw commands for m_world, uploads into frame
		// `frameSlot`'s device buffer. Call once per frame before RegisterPass.
		// outputExtent is the pixels the UI resolves against (== the pass render
		// extent) and the shader's screenSize. No-op (clears the slot's draw
		// count) when no World is set.
		void BuildFrame(glm::vec2 outputExtent, std::uint32_t frameSlot);

		// Appends "$UiOverlay" into `color` (invalid -> swapchain), drawing
		// Draw(6, N) - one instanced fullscreen-mapped quad per command. Registered
		// once; its Execute reads the LIVE PassContext::frameSlot, so it always
		// draws the commands the matching BuildFrame uploaded that frame.
		void RegisterPass(RenderGraph& graph, RGImage color, gpu::Extent2D extent, BindlessManager& bindless);

	private:
		// aether::kMaxFramesInFlight (gpu/GpuTypes.hpp) - the engine-wide frames-in-flight count.
		static constexpr std::uint32_t kFrames = aether::kMaxFramesInFlight;

		struct Frame
		{
			gpu::BufferHandle buffer{};
			void* mapped = nullptr;
			std::uint64_t address = 0;
			std::uint32_t capacity = 0; // commands
			std::uint32_t count = 0;    // commands
			// Screen extent the commands in this slot's buffer were resolved
			// against == the shader's screenSize. Paired per-slot with `address`
			// (not a shared member) so a producer-thread BuildFrame for the next
			// frame cannot overwrite the extent the render thread still needs for
			// this slot's not-yet-executed pass (e.g. across a viewport resize).
			glm::vec2 extent{0.f};
		};

		// (Re)allocates frame.buffer when frame.capacity < count, destroying the
		// old handle first and re-resolving mapped/address. Geometric growth
		// from a 256-command floor.
		void EnsureCapacity(Frame& frame, std::uint32_t count);

		World* m_world = nullptr;
		gpu::PipelineHandle m_pipeline{};
		std::array<Frame, kFrames> m_frames{};
		std::vector<UiDrawCommand> m_scratch;
	};
} // namespace aether::ui
