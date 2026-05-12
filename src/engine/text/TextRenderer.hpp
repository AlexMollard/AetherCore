#pragma once

#include <glm/glm.hpp>
#include <string_view>

#include "text/FontAtlas.hpp"
#include "ui/UiLayout.hpp"

class ServiceContainer;

namespace aether
{
	class QuadRenderer;
	class VulkanContext;
	class BindlessManager;
	class Swapchain;

	// Manages an SDF font atlas and provides glyph decomposition for layer-sorted
	// text rendering via QuadRenderer.  No dedicated render pass or pipeline —
	// glyphs are submitted as ShapeType::SdfGlyph commands so they sort correctly
	// with all other UI quads in a single draw call.
	class TextRenderer
	{
	public:
		TextRenderer() = default;
		~TextRenderer() = default;

		TextRenderer(const TextRenderer&) = delete;
		TextRenderer& operator=(const TextRenderer&) = delete;

		// Load the font and build the SDF atlas.  `glyphSize` is the atlas cell
		// height in pixels (default 48).
		void Init(ServiceContainer& services, std::string_view fontVfsPath, int glyphSize = 48);

		void Shutdown();

		// Decompose `text` into per-glyph DrawGlyph calls on `qr` at the given layer.
		void DrawTextLayered(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color, std::int32_t layer, QuadRenderer& qr);

		// Returns the pixel width of `text` rendered at `fontSize`.
		[[nodiscard]] float MeasureText(std::string_view text, float fontSize) const;

		[[nodiscard]] bool IsReady() const
		{
			return m_ready;
		}

	private:
		VulkanContext* m_vkCtx = nullptr;
		BindlessManager* m_bindlessMgr = nullptr;
		Swapchain* m_swapchain = nullptr;
		FontAtlas m_fontAtlas;
		bool m_ready = false;
	};
} // namespace aether
