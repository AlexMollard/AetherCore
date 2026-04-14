#include "UIRenderer.hpp"

#include <string>

#include "AetherCore.hpp"

namespace aether
{
	void UIRenderer::Init(
		AetherCore& engine,
		std::string_view fontVfsPath,
		std::string_view passNamePrefix,
		int glyphSize)
	{
		const std::string prefix(passNamePrefix);

		// Keep quads behind text by registering quad pass first.
		m_quadRenderer.Init(engine, prefix + ".Quad");
		m_textRenderer.Init(engine, fontVfsPath, prefix + ".Text", glyphSize);
	}

	void UIRenderer::Shutdown(AetherCore& engine)
	{
		m_textRenderer.Shutdown(engine);
		m_quadRenderer.Shutdown(engine);
	}

	void UIRenderer::DrawText(
		std::string_view text,
		const UiPoint& point,
		float fontSize,
		glm::vec4 color)
	{
		m_textRenderer.DrawText(text, point, fontSize, color);
	}

	void UIRenderer::DrawQuad(const UiRect& rect, glm::vec4 color)
	{
		m_quadRenderer.DrawQuad(rect, color);
	}
}
