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
		// ── Colours ────────────────────────────────────────────────────────────
		glm::vec4 panelBg{0.07f, 0.09f, 0.12f, 0.91f};
		glm::vec4 panelHeaderBg{0.10f, 0.13f, 0.17f, 0.97f};
		glm::vec4 accent{0.42f, 0.62f, 0.74f, 1.00f};
		glm::vec4 separator{0.18f, 0.23f, 0.29f, 0.90f};
		glm::vec4 text{0.88f, 0.91f, 0.93f, 1.00f};
		glm::vec4 textLabel{0.50f, 0.61f, 0.69f, 1.00f};
		glm::vec4 textSection{0.70f, 0.76f, 0.81f, 1.00f};
		glm::vec4 textTitle{0.90f, 0.88f, 0.82f, 1.00f};
		glm::vec4 good{0.40f, 0.72f, 0.46f, 0.95f};
		glm::vec4 warn{0.86f, 0.71f, 0.30f, 0.95f};
		glm::vec4 bad{0.80f, 0.33f, 0.30f, 0.95f};

		glm::vec4 btnNormal{0.20f, 0.24f, 0.30f, 1.f};
		glm::vec4 btnHover{0.28f, 0.34f, 0.44f, 1.f};
		glm::vec4 btnPress{0.16f, 0.20f, 0.26f, 1.f};
		glm::vec4 btnText{0.88f, 0.91f, 0.93f, 1.f};

		glm::vec4 sliderTrack{0.12f, 0.15f, 0.20f, 1.f};
		glm::vec4 sliderFill{0.42f, 0.62f, 0.74f, 1.f};
		glm::vec4 sliderKnob{0.72f, 0.85f, 0.92f, 1.f};

		glm::vec4 checkboxOff{0.15f, 0.18f, 0.23f, 1.f};
		glm::vec4 checkboxOn{0.42f, 0.62f, 0.74f, 1.f};
		glm::vec4 checkboxBorder{0.30f, 0.38f, 0.48f, 1.f};

		glm::vec4 inputBg{0.09f, 0.11f, 0.15f, 1.f};
		glm::vec4 inputHoverBg{0.11f, 0.14f, 0.19f, 1.f};
		glm::vec4 inputFocusBg{0.07f, 0.10f, 0.16f, 1.f};
		glm::vec4 placeholder{0.42f, 0.51f, 0.59f, 0.70f};

		// ── Inventory slot colours ──────────────────────────────────────────────
		glm::vec4 slotBg{0.08f, 0.10f, 0.14f, 0.95f};
		glm::vec4 slotBorder{0.22f, 0.27f, 0.35f, 1.f};
		glm::vec4 slotHoverBorder{0.55f, 0.68f, 0.82f, 1.f};
		glm::vec4 slotSelectedBorder{0.82f, 0.92f, 1.00f, 1.f};
		glm::vec4 slotHoverOverlay{1.f, 1.f, 1.f, 0.09f};
		glm::vec4 quantityText{0.92f, 0.90f, 0.78f, 1.f};
		float slotCornerRadius = 4.f;
		float slotFontSize = 10.f;

		// ── Layout metrics ─────────────────────────────────────────────────────
		float cornerRadius = 6.f;
		float headerHeight = 48.f;
		float padding = 16.f;
		float rowHeight = 20.f;
		float knobRadius = 7.f;

		// ── Font sizes ─────────────────────────────────────────────────────────
		float titleFontSize = 18.f;
		float sectionFontSize = 12.f;
		float bodyFontSize = 14.f;
		float labelFontSize = 14.f;
		float buttonFontSize = 13.f;

		// Returns the single shared default theme instance.
		[[nodiscard]] static const UiTheme& Default();
	};

} // namespace aether::ui
