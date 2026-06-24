#pragma once

#include <glm/glm.hpp>

namespace aether::ui
{
	// Centralised visual styling for the ECS UI system.
	// Pass a UiTheme by const-ref to every widget draw function so all panels
	// share a consistent look by default, but individual components can be
	// overridden per-call or per-entity via a custom theme.
	struct UiTheme
	{
		// -- Colours ------------------------------------------------------------
		glm::vec4 panelBg{0.07f, 0.09f, 0.10f, 0.93f};
		glm::vec4 panelHeaderBg{0.11f, 0.14f, 0.15f, 0.98f};
		glm::vec4 accent{0.93f, 0.55f, 0.20f, 1.00f};
		glm::vec4 separator{0.23f, 0.28f, 0.29f, 0.92f};
		glm::vec4 text{0.90f, 0.91f, 0.86f, 1.00f};
		glm::vec4 textLabel{0.61f, 0.66f, 0.62f, 1.00f};
		glm::vec4 textSection{0.79f, 0.75f, 0.58f, 1.00f};
		glm::vec4 textTitle{0.98f, 0.78f, 0.42f, 1.00f};
		glm::vec4 good{0.45f, 0.68f, 0.35f, 0.95f};
		glm::vec4 warn{0.92f, 0.61f, 0.24f, 0.95f};
		glm::vec4 bad{0.78f, 0.29f, 0.22f, 0.95f};

		glm::vec4 btnNormal{0.16f, 0.20f, 0.21f, 1.f};
		glm::vec4 btnHover{0.25f, 0.29f, 0.27f, 1.f};
		glm::vec4 btnPress{0.11f, 0.14f, 0.15f, 1.f};
		glm::vec4 btnText{0.91f, 0.89f, 0.80f, 1.f};

		glm::vec4 sliderTrack{0.12f, 0.15f, 0.15f, 1.f};
		glm::vec4 sliderFill{0.88f, 0.47f, 0.18f, 1.f};
		glm::vec4 sliderKnob{0.97f, 0.71f, 0.33f, 1.f};

		glm::vec4 checkboxOff{0.12f, 0.15f, 0.15f, 1.f};
		glm::vec4 checkboxOn{0.42f, 0.58f, 0.28f, 1.f};
		glm::vec4 checkboxBorder{0.44f, 0.49f, 0.42f, 1.f};

		glm::vec4 inputBg{0.09f, 0.12f, 0.12f, 1.f};
		glm::vec4 inputHoverBg{0.13f, 0.16f, 0.15f, 1.f};
		glm::vec4 inputFocusBg{0.13f, 0.14f, 0.11f, 1.f};
		glm::vec4 placeholder{0.48f, 0.54f, 0.50f, 0.72f};

		// -- Tab bar colours -----------------------------------------------------
		glm::vec4 tabStripBg{0.06f, 0.08f, 0.08f, 1.00f};
		glm::vec4 tabActive{0.14f, 0.17f, 0.15f, 0.98f};
		glm::vec4 tabInactive{0.07f, 0.09f, 0.09f, 0.88f};
		glm::vec4 tabHover{0.12f, 0.15f, 0.14f, 0.96f};

		// -- Inventory slot colours ----------------------------------------------
		glm::vec4 slotBg{0.09f, 0.11f, 0.11f, 0.96f};
		glm::vec4 slotBorder{0.31f, 0.36f, 0.33f, 1.f};
		glm::vec4 slotHoverBorder{0.84f, 0.55f, 0.24f, 1.f};
		glm::vec4 slotSelectedBorder{0.95f, 0.72f, 0.35f, 1.f};
		glm::vec4 slotHoverOverlay{1.f, 0.69f, 0.28f, 0.10f};
		glm::vec4 quantityText{0.96f, 0.80f, 0.46f, 1.f};
		float slotCornerRadius = 2.f;
		float slotFontSize = 10.f;

		// -- Layout metrics -----------------------------------------------------
		float cornerRadius = 3.f;
		float headerHeight = 44.f;
		float padding = 16.f;
		float rowHeight = 20.f;
		float knobRadius = 6.f;
		float tabHeight = 30.f;

		// -- Font sizes ---------------------------------------------------------
		float titleFontSize = 18.f;
		float sectionFontSize = 12.f;
		float bodyFontSize = 14.f;
		float labelFontSize = 14.f;
		float buttonFontSize = 13.f;

		// Returns the single shared default theme instance.
		[[nodiscard]] static const UiTheme& Default();
	};

} // namespace aether::ui
