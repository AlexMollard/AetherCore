#include "UiSandboxLayer.hpp"

#include <cmath>
#include <format>

#include "scene/AetherCore.hpp"
#include "platform/Input.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiLayout.hpp"
#include "ui/UiTheme.hpp"
#include "ui/UiWidgets.hpp"
#include "ui/UiWorld.hpp"
#include "utils/Logger.hpp"

namespace aether::app
{
	namespace
	{
		// Minimal UiRect with the given pixel height and zero width.
		// ApplyLayout will override X, Y, and width; height is the only thing preserved.
		UiRect HeightRect(float h)
		{
			UiRect r{};
			r.offsetMaxPx.y = h;
			return r;
		}

	} // namespace

	// ── OnAttach ─────────────────────────────────────────────────────────────────

	void UiSandboxLayer::OnAttach(LayerContext& context)
	{
		auto& world = context.Get<ui::UiWorld>();

		// Helper: register every spawned entity for cleanup in OnDetach.
		auto reg = [this](Entity e) -> Entity
		{
			m_entities.push_back(e);
			return e;
		};

		// ── Widget Gallery ─────────────────────────────────────────────────────
		// padding=58 clears the 48px panel header and adds an 10px inner margin.
		// For a 300px-wide panel the content area is 300 - 2*58 = 184px wide.
		m_galleryPanel = reg(ui::SpawnPanel(world, UiAnchors::TopLeft({ 120.f, 20.f }, { 300.f, 355.f }), "Widget Gallery", /*draggable=*/true, /*collapsible=*/true, 1.f));

		world.Emplace<ui::UiLayoutComponent>(m_galleryPanel,
		        ui::UiLayoutComponent{
		                .direction = ui::UiLayoutComponent::Direction::Vertical,
		                .spacing = 10.f,
		                .padding = 58.f,
		        });
		world.Emplace<ui::UiChildrenComponent>(m_galleryPanel);

		m_clickButton = reg(ui::SpawnButton(world, HeightRect(32.f), "Click Me!", 2.f));
		m_slider = reg(ui::SpawnSlider(world, HeightRect(24.f), 0.f, 100.f, 50.f, 2.f));
		m_checkA = reg(ui::SpawnCheckbox(world, HeightRect(22.f), "Enable shadows", true, 2.f));
		m_checkB = reg(ui::SpawnCheckbox(world, HeightRect(22.f), "Bloom effect", false, 2.f));
		m_progressBar = reg(ui::SpawnProgressBar(world, HeightRect(16.f), 0.f, 1.f, 0.f, 2.f));

		ui::AddChild(world, m_galleryPanel, m_clickButton);
		ui::AddChild(world, m_galleryPanel, m_slider);
		ui::AddChild(world, m_galleryPanel, m_checkA);
		ui::AddChild(world, m_galleryPanel, m_checkB);
		ui::AddChild(world, m_galleryPanel, m_progressBar);

		// ── Text Input ─────────────────────────────────────────────────────────
		m_inputPanel = reg(ui::SpawnPanel(world, UiAnchors::TopLeft({ 120.f, 395.f }, { 300.f, 120.f }), "Text Input", /*draggable=*/false, /*collapsible=*/false, 1.f));

		world.Emplace<ui::UiLayoutComponent>(m_inputPanel,
		        ui::UiLayoutComponent{
		                .direction = ui::UiLayoutComponent::Direction::Vertical,
		                .spacing = 0.f,
		                .padding = 58.f,
		        });
		world.Emplace<ui::UiChildrenComponent>(m_inputPanel);

		m_textInput = reg(ui::SpawnTextInput(world, HeightRect(32.f), "Type and press Enter...", 2.f));
		ui::AddChild(world, m_inputPanel, m_textInput);

		// ── Flex Toolbar ───────────────────────────────────────────────────────
		// A standalone full-width horizontal strip near the bottom of the screen.
		// No panel header - a plain background rect is drawn in OnGui.
		// anchorMin/Max = {0,1}/{1,1} -> anchors both horizontal edges to the
		// bottom of the screen so the toolbar scales with the window width.
		// Padding=8 leaves crossSize = (56 - 16) = 40px for button height.
		m_flexContainer = reg(world.Create());
		world.Emplace<ui::UiTransformComponent>(m_flexContainer,
		        ui::UiTransformComponent{
		                .rect =
		                        UiRect{
                                       .anchorMin = { 0.f, 1.f },
                                       .anchorMax = { 1.f, 1.f },
                                       .offsetMinPx = { 20.f, -98.f },
                                       .offsetMaxPx = { -20.f, -52.f },
		                               },
		                .zOrder = 1.f,
        });
		world.Emplace<ui::UiLayoutComponent>(m_flexContainer,
		        ui::UiLayoutComponent{
		                .direction = ui::UiLayoutComponent::Direction::Horizontal,
		                .spacing = 8.f,
		                .padding = 8.f,
		        });
		world.Emplace<ui::UiChildrenComponent>(m_flexContainer);

		m_flexLeft = reg(ui::SpawnButton(world, HeightRect(40.f), "Left", 2.f));
		m_flexRight = reg(ui::SpawnButton(world, HeightRect(40.f), "Right", 2.f));

		// Give the buttons a fixed width. The layout overrides their X/Y but
		// preserves width when flexGrow is 0 (the default).
		world.TryGet<ui::UiTransformComponent>(m_flexLeft)->rect.offsetMaxPx.x = 120.f;
		world.TryGet<ui::UiTransformComponent>(m_flexRight)->rect.offsetMaxPx.x = 120.f;

		// Flex spacer: transform-only entity (no input, no visuals) with flexGrow=1.
		// ApplyLayout gives it all the remaining horizontal space, pushing the two
		// buttons to opposite ends of the toolbar.
		m_flexSpacer = reg(world.Create());
		world.Emplace<ui::UiTransformComponent>(m_flexSpacer,
		        ui::UiTransformComponent{
		                .rect = HeightRect(40.f),
		                .zOrder = 0.f,
		                .flexGrow = 1.f,
		        });
		// UiParentComponent is added by AddChild below - do not emplace here.

		ui::AddChild(world, m_flexContainer, m_flexLeft);
		ui::AddChild(world, m_flexContainer, m_flexSpacer);
		ui::AddChild(world, m_flexContainer, m_flexRight);

		// ── Auto-size panel ────────────────────────────────────────────────────
		// The initial height (80px) is a placeholder.  Each frame RunLayouts runs
		// ApplyLayout with autoSize=true, which shrinks/grows the panel to exactly
		// wrap its three children plus padding.
		m_autoPanel = reg(ui::SpawnPanel(world, UiAnchors::TopLeft({ 440.f, 20.f }, { 280.f, 80.f }), "Auto-Size", /*draggable=*/true, /*collapsible=*/true, 1.f));

		world.Emplace<ui::UiLayoutComponent>(m_autoPanel,
		        ui::UiLayoutComponent{
		                .direction = ui::UiLayoutComponent::Direction::Vertical,
		                .spacing = 8.f,
		                .padding = 58.f,
		                .autoSize = true,
		        });
		world.Emplace<ui::UiChildrenComponent>(m_autoPanel);

		m_autoItem1 = reg(ui::SpawnButton(world, HeightRect(28.f), "Button Alpha", 2.f));
		m_autoItem2 = reg(ui::SpawnButton(world, HeightRect(28.f), "Button Beta", 2.f));
		m_autoItem3 = reg(ui::SpawnCheckbox(world, HeightRect(22.f), "Auto checkbox", true, 2.f));

		ui::AddChild(world, m_autoPanel, m_autoItem1);
		ui::AddChild(world, m_autoPanel, m_autoItem2);
		ui::AddChild(world, m_autoPanel, m_autoItem3);

		// ── Corner anchor mini-panels ──────────────────────────────────────────
		// Each one demonstrates a different UiAnchors preset.
		// They are non-draggable / non-collapsible so they stay put as reference.
		m_cornerTL = reg(ui::SpawnPanel(world, UiAnchors::TopLeft({ 8.f, 8.f }, { 95.f, 36.f }), "TopLeft", false, false, 0.5f));
		m_cornerTR = reg(ui::SpawnPanel(world, UiAnchors::TopRight({ 8.f, 8.f }, { 100.f, 36.f }), "TopRight", false, false, 0.5f));
		m_cornerBL = reg(ui::SpawnPanel(world, UiAnchors::BottomLeft({ 8.f, 8.f }, { 105.f, 36.f }), "BottomLeft", false, false, 0.5f));
		m_cornerBR = reg(ui::SpawnPanel(world, UiAnchors::BottomRight({ 8.f, 8.f }, { 115.f, 36.f }), "BottomRight", false, false, 0.5f));

		INFO(LogCategory::App, "UiSandboxLayer attached ({} entities).", m_entities.size());
	}

	// ── OnDetach ─────────────────────────────────────────────────────────────────

	void UiSandboxLayer::OnDetach(LayerContext& context)
	{
		auto& world = context.Get<ui::UiWorld>();
		for (const Entity e: m_entities)
		{
			world.Destroy(e);
		}
		m_entities.clear();
		INFO(LogCategory::App, "UiSandboxLayer detached.");
	}

	// ── OnUpdate ─────────────────────────────────────────────────────────────────

	void UiSandboxLayer::OnUpdate(LayerContext& context)
	{
		m_progressTime += static_cast<float>(context.deltaTimeSeconds);

		// Animate the progress bar with a smooth sine wave so all values [0..1] are
		// exercised over time.
		if (auto* s = context.Get<ui::UiWorld>().TryGet<ui::UiSliderComponent>(m_progressBar))
		{
			s->value = std::sin(m_progressTime * 0.8f) * 0.5f + 0.5f;
		}
	}

	// ── OnGui ─────────────────────────────────────────────────────────────────────

	void UiSandboxLayer::OnGui(LayerContext& context)
	{
		auto& world = context.Get<ui::UiWorld>();
		UIRenderer& ui = context.Get<UIRenderer>();
		const VkExtent2D extent = context.Get<Swapchain>().GetExtent();
		const ui::UiTheme& theme = ui::UiTheme::Default();

		// ── Layout pass ───────────────────────────────────────────────────────
		// Positions every child of every UiLayoutComponent + UiChildrenComponent
		// entity. Also auto-sizes the Auto-Size panel to wrap its children.
		ui::RunLayouts(world, extent);

		// ── Widget Gallery ────────────────────────────────────────────────────
		// Update the button label each frame to reflect the current click count.
		if (auto* btn = world.TryGet<ui::UiButtonComponent>(m_clickButton))
		{
			btn->label = std::format("Click Me!  ({})", m_clickCount);
		}

		if (ui::DrawPanel(world, m_galleryPanel, ui, extent, theme))
		{
			if (ui::DrawButton(world, m_clickButton, ui, extent, theme))
			{
				++m_clickCount;
			}

			m_sliderValue = ui::DrawSlider(world, m_slider, ui, context.Get<Input>(), extent, theme);
			ui::DrawCheckbox(world, m_checkA, ui, extent, theme);
			ui::DrawCheckbox(world, m_checkB, ui, extent, theme);
			ui::DrawProgressBar(world, m_progressBar, ui, extent, theme);
		}

		// ── Text Input ────────────────────────────────────────────────────────
		// Embed the last submitted string in the panel title so it's visible even
		// when the field is currently empty.
		if (auto* panel = world.TryGet<ui::UiPanelComponent>(m_inputPanel))
		{
			panel->title = std::format("Text Input  \xC2\xB7  last: {}", m_lastSubmitted);
		}

		// Always open (not collapsible) - DrawPanel return value is ignored.
		ui::DrawPanel(world, m_inputPanel, ui, extent, theme);
		if (ui::DrawTextInput(world, m_textInput, ui, extent, theme))
		{
			// DrawTextInput returns true for one frame when Enter is pressed.
			if (const auto* ti = world.TryGet<ui::UiTextInputComponent>(m_textInput))
			{
				m_lastSubmitted = ti->text.empty() ? "(empty)" : ti->text;
			}
		}

		// ── Flex Toolbar ──────────────────────────────────────────────────────
		// The toolbar has no panel entity, so we draw its background manually.
		if (const auto* ct = world.TryGet<ui::UiTransformComponent>(m_flexContainer))
		{
			ui.DrawRect(ct->rect, theme.panelBg, theme.cornerRadius);

			// Thin separator line along the top edge.
			const glm::vec4 px = ResolveUiRectPx(extent, ct->rect);
			const glm::vec2 sz{ static_cast<float>(extent.width), static_cast<float>(extent.height) };
			const glm::vec2 anchPx = ct->rect.anchorMin * sz;

			ui.DrawLine(
			        UiPoint{
			                .anchor = ct->rect.anchorMin, .offsetPx = { px.x - anchPx.x, px.y - anchPx.y }
            },
			        UiPoint{ .anchor = ct->rect.anchorMin, .offsetPx = { px.x + px.z - anchPx.x, px.y - anchPx.y } },
			        1.f,
			        theme.separator);
		}

		// The spacer has no widget to draw - just render the two buttons.
		ui::DrawButton(world, m_flexLeft, ui, extent, theme);
		ui::DrawButton(world, m_flexRight, ui, extent, theme);

		// ── Auto-size panel ───────────────────────────────────────────────────
		if (ui::DrawPanel(world, m_autoPanel, ui, extent, theme))
		{
			ui::DrawButton(world, m_autoItem1, ui, extent, theme);
			ui::DrawButton(world, m_autoItem2, ui, extent, theme);
			ui::DrawCheckbox(world, m_autoItem3, ui, extent, theme);
		}

		// ── Corner anchor mini-panels ─────────────────────────────────────────
		// Each panel header label names its own UiAnchors preset.
		ui::DrawPanel(world, m_cornerTL, ui, extent, theme);
		ui::DrawPanel(world, m_cornerTR, ui, extent, theme);
		ui::DrawPanel(world, m_cornerBL, ui, extent, theme);
		ui::DrawPanel(world, m_cornerBR, ui, extent, theme);
	}

} // namespace aether::app
