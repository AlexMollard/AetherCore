#include "LoadingLayer.hpp"

#include "ui/UIRenderer.hpp"
#include "ui/UiLayout.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::app
{
	void LoadingLayer::OnGui(LayerContext& context)
	{
		if (!m_visible)
		{
			return;
		}

		auto& ui = context.Get<UIRenderer>();
		const gpu::Extent2D extent = context.Get<Swapchain>().GetExtent();
		const float w = static_cast<float>(extent.width);
		const float h = static_cast<float>(extent.height);

		// Low layer so the overlay sits behind any script UI.
		ui.SetLayer(-100);

		// Dim full-screen backdrop.
		ui.DrawRect(UiAnchors::StretchFull(), glm::vec4(0.05f, 0.07f, 0.10f, 0.92f));

		// Centered "Loading..." text.
		const float textSize = 28.0f;
		const float approxTextWidth = 180.0f;
		ui.DrawText("Loading...", UiPoint{.anchor = {0.f, 0.f}, .offsetPx = {(w - approxTextWidth) * 0.5f, h * 0.45f}}, textSize, glm::vec4(0.88f, 0.91f, 0.93f, 1.f));
	}
} // namespace aether::app
