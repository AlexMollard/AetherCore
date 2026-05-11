#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "ui/QuadRenderer.hpp"
#include "text/TextRenderer.hpp"

namespace aether
{
	class AetherCore;

	// High-level UI renderer that composes dedicated text and quad renderers.
	class UIRenderer
	{
	public:
		void Init(AetherCore& engine, std::string_view fontVfsPath, std::string_view passNamePrefix = "UIPass", int glyphSize = 48);

		void Shutdown(AetherCore& engine);

		// Must be called once per frame on the game thread BEFORE any DrawText /
		// DrawRect calls. Routes pending submissions into the correct double-buffer slot.
		void SetWriteSlot(std::uint32_t slot)
		{
			m_quadRenderer.SetWriteSlot(slot);
		}

		void DrawText(std::string_view text, const UiPoint& point, float fontSize, glm::vec4 color = glm::vec4(1.f));

		// Returns the pixel width of `text` rendered at `fontSize`.
		[[nodiscard]] float MeasureText(std::string_view text, float fontSize) const;

		void DrawRect(const UiRect& rect, glm::vec4 color = glm::vec4(1.f), float cornerRadiusPx = 0.0f);
		void DrawLine(const UiPoint& start, const UiPoint& end, float thicknessPx, glm::vec4 color = glm::vec4(1.f));
		void DrawCircle(const UiPoint& center, float radiusPx, glm::vec4 color = glm::vec4(1.f));

		// Draws a bindless-sampled texture. uvRect = (u0, v0, u1, v1).
		void DrawTexturedRect(const UiRect& rect, std::uint32_t textureSlot, glm::vec4 uvRect = glm::vec4(0.f, 0.f, 1.f, 1.f), glm::vec4 tint = glm::vec4(1.f), float cornerRadiusPx = 0.f);

		// Clip rect stack - draw calls that fall entirely outside the active clip
		// are discarded on the CPU before reaching the GPU.
		void PushClipRect(const UiRect& rect);
		void PopClipRect();

		void SetLayer(std::int32_t layer)
		{
			m_currentLayer = layer;
		}

		[[nodiscard]] std::int32_t GetLayer() const
		{
			return m_currentLayer;
		}

		void PushLayer(std::int32_t delta = 1)
		{
			m_layerStack.push_back(m_currentLayer + delta);
			m_currentLayer = m_layerStack.back();
		}

		void PopLayer()
		{
			if (m_layerStack.size() > 1)
			{
				m_layerStack.pop_back();
				m_currentLayer = m_layerStack.back();
			}
		}

	private:
		AetherCore* m_engine = nullptr;
		std::int32_t m_currentLayer = 0;
		std::vector<std::int32_t> m_layerStack{ 0 };
		std::vector<glm::vec4> m_clipStack; // resolved pixel rects (x,y,w,h)
		TextRenderer m_textRenderer;
		QuadRenderer m_quadRenderer;
	};
} // namespace aether
