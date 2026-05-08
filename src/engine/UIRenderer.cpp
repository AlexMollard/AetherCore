#include "UIRenderer.hpp"

#include <string>

#include "AetherCore.hpp"

namespace aether
{
	void UIRenderer::Init(AetherCore& engine, std::string_view fontVfsPath, std::string_view passNamePrefix, int glyphSize)
	{
		m_engine = &engine;
		m_currentLayer = 0;
		m_layerStack.clear();
		m_layerStack.push_back(0);

		const std::string prefix(passNamePrefix);

		// Keep quads behind text by registering quad pass first.
		m_quadRenderer.Init(engine, prefix + ".Quad");
		m_textRenderer.Init(engine, fontVfsPath, prefix + ".Text", glyphSize);
	}

	void UIRenderer::Shutdown(AetherCore& engine)
	{
		m_textRenderer.Shutdown(engine);
		m_quadRenderer.Shutdown(engine);
		m_engine = nullptr;
		m_currentLayer = 0;
		m_layerStack.clear();
		m_layerStack.push_back(0);
	}

	void UIRenderer::DrawText(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color)
	{
		m_quadRenderer.EnsurePassRegistered();
		m_textRenderer.DrawText(text, point, fontSize, color);
	}

	void UIRenderer::DrawRect(const UiRect& rect, glm::vec4 color, float cornerRadiusPx)
	{
		m_quadRenderer.DrawRect(rect, color, m_currentLayer, cornerRadiusPx);
	}

	void UIRenderer::DrawLine(const UiPoint& start, const UiPoint& end, float thicknessPx, glm::vec4 color)
	{
		m_quadRenderer.DrawLine(start, end, thicknessPx, color, m_currentLayer);
	}

	void UIRenderer::DrawCircle(const UiPoint& center, float radiusPx, glm::vec4 color)
	{
		m_quadRenderer.DrawCircle(center, radiusPx, color, m_currentLayer);
	}
} // namespace aether
