#pragma once

#include <string>
#include <vector>

#include "AppLayer.hpp"
#include "scene/Entity.hpp"

namespace aether::app
{
	// Sandbox layer that exercises every ECS UI feature:
	//   - Draggable / collapsible panels
	//   - Button, slider, checkbox, progress bar (with hover/press transitions)
	//   - Text input (GLFW char callback, cursor, blink, Enter/Escape)
	//   - Vertical auto-layout with flex spacers
	//   - Auto-size panel (shrinks to wrap its children)
	//   - Anchor preset corners (TopLeft / TopRight / BottomLeft / BottomRight)
	class UiSandboxLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		// ── Widget Gallery panel (draggable, collapsible) ─────────────────────
		Entity m_galleryPanel;
		Entity m_clickButton;
		Entity m_slider;
		Entity m_checkA;
		Entity m_checkB;
		Entity m_progressBar;
		int m_clickCount = 0;
		float m_sliderValue = 50.f;
		float m_progressTime = 0.f;

		// ── Text Input panel ──────────────────────────────────────────────────
		Entity m_inputPanel;
		Entity m_textInput;
		std::string m_lastSubmitted = "(none)";

		// ── Flex toolbar (standalone strip, no panel header) ──────────────────
		// Demonstrates UiTransformComponent::flexGrow pushing buttons to edges.
		Entity m_flexContainer;
		Entity m_flexLeft;
		Entity m_flexSpacer; // transform-only, flexGrow=1
		Entity m_flexRight;

		// ── Auto-size panel (draggable, collapsible) ──────────────────────────
		// UiLayoutComponent::autoSize=true shrinks the panel to wrap its children.
		Entity m_autoPanel;
		Entity m_autoItem1;
		Entity m_autoItem2;
		Entity m_autoItem3;

		// ── Corner anchor demonstrations ──────────────────────────────────────
		Entity m_cornerTL;
		Entity m_cornerTR;
		Entity m_cornerBL;
		Entity m_cornerBR;

		// All owned entities – destroyed en-masse in OnDetach.
		std::vector<Entity> m_entities;
	};
} // namespace aether::app
