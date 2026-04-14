#include "UILayer.hpp"

#include "Logger.hpp"

namespace aether::app
{
	static constexpr std::string_view kFontPath = "assets://fonts/Roboto-Regular.ttf";

	void UILayer::OnAttach(LayerContext& context)
	{
		INFO(LogCategory::App, "UI layer attached.");
		m_uiRenderer.Init(context.engine, kFontPath, "UIPass");
	}

	void UILayer::OnDetach(LayerContext& context)
	{
		m_uiRenderer.Shutdown(context.engine);
		INFO(LogCategory::App, "UI layer detached.");
	}

	void UILayer::OnUpdate(LayerContext& context)
	{
		(void)context;
	}

	void UILayer::OnGui(LayerContext& context)
	{
		(void)context;

		// Panel anchored to top-left with fixed margins in pixels.
		m_uiRenderer.DrawQuad(
			aether::UiRect{
				.anchorMin = { 0.0f, 0.0f },
				.anchorMax = { 0.0f, 0.0f },
				.offsetMinPx = { 12.0f, 12.0f },
				.offsetMaxPx = { 360.0f, 110.0f },
			},
			glm::vec4(0.08f, 0.11f, 0.14f, 0.78f));

		m_uiRenderer.DrawText(
			"AetherCore",
			aether::UiPoint{
				.anchor = { 0.0f, 0.0f },
				.offsetPx = { 24.0f, 48.0f },
			},
			32.0f,
			glm::vec4(1.f, 0.9f, 0.3f, 1.f));

		m_uiRenderer.DrawText(
			"SDF Text + Quad UI",
			aether::UiPoint{
				.anchor = { 0.0f, 0.0f },
				.offsetPx = { 24.0f, 92.0f },
			},
			20.0f,
			glm::vec4(1.0f));
	}
}
