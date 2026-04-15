#pragma once

#include <glm/glm.hpp>
#include <string_view>

#include "QuadRenderer.hpp"
#include "TextRenderer.hpp"

namespace aether
{
	class AetherCore;

	// High-level UI renderer that composes dedicated text and quad renderers.
	class UIRenderer
	{
	public:
		void Init(AetherCore& engine, std::string_view fontVfsPath, std::string_view passNamePrefix = "UIPass", int glyphSize = 48);

		void Shutdown(AetherCore& engine);

		void DrawText(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color = glm::vec4(1.f));

		void DrawQuad(const UiRect& rect, glm::vec4 color = glm::vec4(1.f));

		[[nodiscard]] TextRenderer& GetTextRenderer()
		{
			return m_textRenderer;
		}

		[[nodiscard]] QuadRenderer& GetQuadRenderer()
		{
			return m_quadRenderer;
		}

	private:
		TextRenderer m_textRenderer;
		QuadRenderer m_quadRenderer;
	};
} // namespace aether
