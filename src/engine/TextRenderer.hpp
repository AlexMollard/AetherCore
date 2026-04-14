#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "FontAtlas.hpp"
#include "GraphicsPipeline.hpp"
#include "UiLayout.hpp"

namespace aether
{
	class AetherCore;

	// Manages an SDF text rendering pipeline and a font atlas.
	//
	// Typical usage:
	//   m_textRenderer.Init(engine, "assets://fonts/Roboto-Regular.ttf", "UIPass");
	//   // each frame, during OnGui:
	//   m_textRenderer.DrawText("Hello", UiPoint{ .anchor={0.f,0.f}, .offsetPx={16.f,40.f} }, 32.f);
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
		void Init(AetherCore& engine,
			std::string_view fontVfsPath,
			std::string_view passName,
			int              glyphSize = 48);

		// Remove the render-graph pass, destroy the pipeline and atlas.
		void Shutdown(AetherCore& engine);

		// Queue anchor-based text for this frame.
		// Call during a layer's OnGui(); labels are flushed by the registered
		// pass inside EndFrame().
		void DrawText(std::string_view text,
			const UiPoint& point,
			float            fontSize,
			glm::vec4        color = glm::vec4(1.f));

		[[nodiscard]] bool IsReady() const { return m_ready; }

	private:
		// Push constants for text_sdf.slang (72 bytes, matches shader layout exactly).
		// screenSize is vec4 so float4 in SPIR-V is 16-byte aligned — no hidden padding.
		struct GlyphPush
		{
			glm::vec4 screenSize;   // .xy = viewport pixels, .zw unused
			glm::vec4 glyphRect;    // x, y, w, h in screen pixels (top-left origin)
			glm::vec4 uvRect;       // u0, v0, u1, v1 in [0,1] atlas space
			glm::vec4 color;        // RGBA tint (linear)
			uint32_t  atlasSlot;    // bindless sampled-image index
			uint32_t  _pad0 = 0;
		};
		static_assert(sizeof(GlyphPush) == 72,
			"GlyphPush must match text_sdf.slang push constant block.");

		struct PendingLabel
		{
			std::string text;
			glm::vec2   position;
			float       fontSize;
			glm::vec4   color;
		};

		void EnsurePassRegistered();
		void RegisterPass();

		std::string               m_passName;
		AetherCore* m_engine = nullptr;
		FontAtlas                 m_fontAtlas;
		GraphicsPipeline          m_pipeline;
		std::vector<PendingLabel> m_pendingLabels;
		bool                      m_ready = false;
	};
}
