#include "ui/UIRenderer.hpp"

#include <string>

#include "scene/AetherCore.hpp"

namespace aether
{
	void UIRenderer::Init(AetherCore& engine, std::string_view fontVfsPath, std::string_view passNamePrefix, int glyphSize)
	{
		m_engine = &engine;
		m_currentLayer = 0;
		m_layerStack.clear();
		m_layerStack.push_back(0);

		const std::string prefix(passNamePrefix);

		m_quadRenderer.Init(engine, prefix + ".Quad");
		m_textRenderer.Init(engine, fontVfsPath, glyphSize);
	}

	void UIRenderer::Shutdown(AetherCore& engine)
	{
		m_textRenderer.Shutdown();
		m_quadRenderer.Shutdown(engine);
		m_engine = nullptr;
		m_currentLayer = 0;
		m_layerStack.clear();
		m_layerStack.push_back(0);
		m_clipStack.clear();
		m_quadRenderer.ClearClipRect();
	}

	void UIRenderer::DrawText(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color)
	{
		m_quadRenderer.EnsurePassRegistered();
		m_textRenderer.DrawTextLayered(text, point, fontSize, color, m_currentLayer, m_quadRenderer);
	}

	float UIRenderer::MeasureText(std::string_view text, float fontSize) const
	{
		return m_textRenderer.MeasureText(text, fontSize);
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

	void UIRenderer::DrawTexturedRect(const UiRect& rect, std::uint32_t textureSlot, glm::vec4 uvRect, glm::vec4 tint, float cornerRadiusPx)
	{
		m_quadRenderer.DrawTexturedRect(rect, textureSlot, uvRect, tint, m_currentLayer, cornerRadiusPx);
	}

	void UIRenderer::PushClipRect(const UiRect& rect)
	{
		if (m_engine == nullptr)
		{
			return;
		}
		const glm::vec4 px = ResolveUiRectPx(m_engine->GetSwapchainExtent(), rect);
		m_clipStack.push_back(px);
		m_quadRenderer.SetClipRect(px);
	}

	void UIRenderer::PopClipRect()
	{
		if (m_clipStack.empty())
		{
			return;
		}
		m_clipStack.pop_back();
		if (m_clipStack.empty())
		{
			m_quadRenderer.ClearClipRect();
		}
		else
		{
			m_quadRenderer.SetClipRect(m_clipStack.back());
		}
	}
} // namespace aether
