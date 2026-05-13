#include "LoadingLayer.hpp"

#include <algorithm>
#include <cstdint>

#include "ui/UIRenderer.hpp"
#include "ui/UiLayout.hpp"
#include "utils/LoadingManager.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether::app
{

	void LoadingLayer::OnGui(LayerContext& context)
	{
		auto* loadingManager = context.TryGet<LoadingManager>();
		if (!loadingManager || loadingManager->IsComplete())
		{
			return;
		}

		UIRenderer& ui = context.Get<UIRenderer>();
		const VkExtent2D extent = context.Get<Swapchain>().GetExtent();

		const float w = static_cast<float>(extent.width);
		const float h = static_cast<float>(extent.height);

		constexpr float kBarWidth = 280.0f;
		constexpr float kBarHeight = 14.0f;
		constexpr float kCornerRadius = 6.0f;

		const float barX = (w - kBarWidth) * 0.5f;
		const float centerY = h * 0.45f;

		// Dark full-screen overlay (lowest layer so other UI draws on top).
		ui.SetLayer(-100);
		ui.DrawRect(UiAnchors::StretchFull(), glm::vec4(0.05f, 0.07f, 0.10f, 0.92f));

		// "Loading..." heading.
		ui.SetLayer(-99);
		ui.DrawText("Loading...",
		        UiPoint{
		                {  0.f,            0.f },
                        { barX, centerY - 36.f }
        },
		        28.f,
		        glm::vec4(0.88f, 0.91f, 0.93f, 1.f));

		// Progress bar background track.
		ui.DrawRect(UiAnchors::Fixed({ 0.f, 0.f }, { barX, centerY }, { kBarWidth, kBarHeight }), glm::vec4(0.12f, 0.15f, 0.20f, 1.f), kCornerRadius);

		// Progress bar fill.
		const float progress = loadingManager->GetProgress();
		const float fillWidth = std::max(4.0f, (kBarWidth - 4.0f) * progress);
		ui.DrawRect(UiAnchors::Fixed({ 0.f, 0.f }, { barX + 2.f, centerY + 2.f }, { fillWidth, kBarHeight - 4.f }), glm::vec4(0.42f, 0.62f, 0.74f, 1.f), kCornerRadius - 2.f);

		// Current task label beneath the bar.
		const std::string_view task = loadingManager->GetCurrentTask();
		if (!task.empty())
		{
			ui.DrawText(task,
			        UiPoint{
			                {  0.f,                         0.f },
                            { barX, centerY + kBarHeight + 10.f }
            },
			        15.f,
			        glm::vec4(0.50f, 0.55f, 0.60f, 1.f));
		}
	}

} // namespace aether::app
