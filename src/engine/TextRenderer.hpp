#pragma once

#include <array>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "FontAtlas.hpp"
#include "GraphicsPipeline.hpp"
#include "Swapchain.hpp"
#include "UiLayout.hpp"
#include "UniqueBuffer.hpp"

namespace aether
{
	class AetherCore;

	// Manages an SDF text rendering pipeline and a font atlas.
	//
	// Typical usage:
	//   m_textRenderer.Init(engine, "assets://fonts/Roboto-Regular.ttf", "UIPass");
	//   // each frame, during OnGui:
	//   m_textRenderer.DrawText("Hello", UiPoint{ .anchor={0.f,0.f},
	//   .offsetPx={16.f,40.f} }, 32.f);
	//   // on teardown:
	//   m_textRenderer.Shutdown(engine);
	class TextRenderer
	{
	public:
		TextRenderer() = default;
		~TextRenderer() = default;

		TextRenderer(const TextRenderer&) = delete;
		TextRenderer& operator=(const TextRenderer&) = delete;

		// Load the font, create the SDF pipeline, and register a render-graph
		// pass named `passName`.  The pass writes to the swapchain image using
		// LOAD_OP_LOAD so it composites over whatever was already rendered.
		// `glyphSize` is the atlas cell height in pixels (default 48).
		void Init(AetherCore& engine, std::string_view fontVfsPath, std::string_view passName, int glyphSize = 48);

		// Remove the render-graph pass, destroy the pipeline and atlas.
		void Shutdown(AetherCore& engine);

		// Must be called by the game thread before DrawText() each frame.
		void SetWriteSlot(std::uint32_t slot)
		{
			m_writeSlot = slot;
		}

		// Queue anchor-based text for this frame.
		// Call during a layer's OnGui(); labels are flushed by the registered
		// pass inside EndFrame().
		void DrawText(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color = glm::vec4(1.f));

		[[nodiscard]] bool IsReady() const
		{
			return m_ready;
		}

	private:
		// Per-glyph data consumed by the vertex shader through a device-addressable
		// buffer.
		struct GlyphInstance
		{
			glm::vec4 glyphRect; // x, y, w, h in screen pixels (top-left origin)
			glm::vec4 uvRect;    // u0, v0, u1, v1 in [0,1] atlas space
			glm::vec4 color;     // RGBA tint (linear)
		};

		static_assert(sizeof(GlyphInstance) == 48, "GlyphInstance layout must match text_sdf.slang.");

		// One push-constant block per text pass.
		struct BatchPush
		{
			glm::vec4 screenSize; // .xy = viewport pixels, .zw unused
			uint32_t atlasSlot;   // bindless sampled-image index
			uint32_t _pad0 = 0;
			uint64_t glyphDataAddr = 0;
		};

		static_assert(sizeof(BatchPush) == 32, "BatchPush must match text_sdf.slang push constant block.");

		struct PendingLabel
		{
			std::string text;
			glm::vec2 position;
			float fontSize;
			glm::vec4 color;
		};

		void EnsurePassRegistered();
		void RegisterPass();

		std::string m_passName;
		AetherCore* m_engine = nullptr;
		FontAtlas m_fontAtlas;
		GraphicsPipeline m_pipeline;
		// Double-buffered pending label list. Game thread writes to m_writeSlot;
		// render thread reads from ctx.frameIndex % kMaxFramesInFlight.
		std::array<std::vector<PendingLabel>, Swapchain::kMaxFramesInFlight> m_pendingLabels;
		std::uint32_t m_writeSlot = 0;
		std::array<UniqueBuffer, Swapchain::kMaxFramesInFlight> m_glyphBuffers;
		std::array<std::size_t, Swapchain::kMaxFramesInFlight> m_glyphBufferCapacities{};
		bool m_ready = false;
	};
} // namespace aether
