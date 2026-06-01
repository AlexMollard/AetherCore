#include "ui/UIRenderer.hpp"

#include <algorithm>
#include <string>

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether
{
	void UIRenderer::Init(ServiceContainer& services, std::string_view fontVfsPath, std::string_view passNamePrefix, int glyphSize)
	{
		AE_PROFILE_ZONE();
		m_swapchain = &services.Get<Swapchain>();
		m_currentLayer = 0;
		m_layerStack.clear();
		m_layerStack.push_back(0);

		const std::string prefix(passNamePrefix);

		m_quadRenderer.Init(services, prefix + ".Quad");
		m_textRenderer.Init(services, fontVfsPath, glyphSize);
	}

	void UIRenderer::Shutdown(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		m_textRenderer.Shutdown();
		m_quadRenderer.Shutdown(services);
		m_swapchain = nullptr;
		m_currentLayer = 0;
		m_layerStack.clear();
		m_layerStack.push_back(0);
		m_clipStack.clear();
		m_quadRenderer.ClearClipRect();
	}

	void UIRenderer::DrawText(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color)
	{
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

	void UIRenderer::DrawTexturedRect(const UiRect& rect, std::uint32_t textureSlot, glm::vec4 uvRect, glm::vec4 tint)
	{
		m_quadRenderer.DrawTexturedRect(rect, textureSlot, uvRect, tint, m_currentLayer);
	}

	void UIRenderer::PushClipRect(const UiRect& rect)
	{
		if (m_swapchain == nullptr)
		{
			return;
		}
		glm::vec4 px = ResolveUiRectPx(m_swapchain->GetExtent(), rect);

		if (!m_clipStack.empty())
		{
			const glm::vec4& parent = m_clipStack.back();
			const float minX = std::max(px.x, parent.x);
			const float minY = std::max(px.y, parent.y);
			const float maxX = std::min(px.x + px.z, parent.x + parent.z);
			const float maxY = std::min(px.y + px.w, parent.y + parent.w);
			px = (maxX > minX && maxY > minY) ? glm::vec4(minX, minY, maxX - minX, maxY - minY) : glm::vec4(0.f, 0.f, 0.f, 0.f);
		}

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
