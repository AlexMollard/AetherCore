#include "UiWidgets.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "Input.hpp"
#include "UIRenderer.hpp"
#include "UiComponents.hpp"
#include "UiContext.hpp"
#include "UiLayout.hpp"
#include "UiWorld.hpp"

namespace aether::ui
{
	// ── Theme singleton ───────────────────────────────────────────────────────

	const UiTheme& UiTheme::Default()
	{
		static const UiTheme s_default;
		return s_default;
	}

	// ── Private pixel-space helpers ───────────────────────────────────────────

	// Resolved pixel rect of an entity's transform.
	static glm::vec4 PixelRect(const UiTransformComponent& t, VkExtent2D ext)
	{
		return ResolveUiRectPx(ext, t.rect);
	}

	// Builds a UiRect that lands on the given pixel rect, using the entity's
	// anchor so subsequent UIRenderer draws are consistent.
	static UiRect PixelToUiRect(const UiTransformComponent& t, glm::vec4 px, VkExtent2D ext)
	{
		const glm::vec2 sizePx{ static_cast<float>(ext.width), static_cast<float>(ext.height) };
		const glm::vec2 anchorPx = t.rect.anchorMin * sizePx;
		return UiRect{
			.anchorMin   = t.rect.anchorMin,
			.anchorMax   = t.rect.anchorMin,
			.offsetMinPx = { px.x - anchorPx.x, px.y - anchorPx.y },
			.offsetMaxPx = { px.x + px.z - anchorPx.x, px.y + px.w - anchorPx.y },
		};
	}

	// A UiPoint centred on a pixel rect, using the entity's anchor.
	static UiPoint CentrePoint(const UiTransformComponent& t, glm::vec4 px, VkExtent2D ext)
	{
		const glm::vec2 sizePx{ static_cast<float>(ext.width), static_cast<float>(ext.height) };
		const glm::vec2 anchorPx = t.rect.anchorMin * sizePx;
		return UiPoint{
			.anchor   = t.rect.anchorMin,
			.offsetPx = { px.x + px.z * 0.5f - anchorPx.x, px.y + px.w * 0.5f - anchorPx.y },
		};
	}

	// UiPoint at a specific pixel position, using the entity's anchor.
	static UiPoint PixelPoint(const UiTransformComponent& t, glm::vec2 px, VkExtent2D ext)
	{
		const glm::vec2 sizePx{ static_cast<float>(ext.width), static_cast<float>(ext.height) };
		const glm::vec2 anchorPx = t.rect.anchorMin * sizePx;
		return UiPoint{
			.anchor   = t.rect.anchorMin,
			.offsetPx = px - anchorPx,
		};
	}

	// ── Button ────────────────────────────────────────────────────────────────

	bool DrawButton(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t   = world.TryGet<UiTransformComponent>(entity);
		auto* btn = world.TryGet<UiButtonComponent>(entity);
		auto* inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !btn || !inp)
		{
			return false;
		}

		// Choose background colour from interaction state.
		glm::vec4 bgColor = btn->normalColor;
		if (inp->pressed)
		{
			bgColor = btn->pressColor;
		}
		else if (inp->hovered)
		{
			bgColor = btn->hoverColor;
		}

		const glm::vec4 px = PixelRect(*t, extent);
		ui.DrawRect(t->rect, bgColor, theme.cornerRadius);

		// Centred label.
		if (!btn->label.empty())
		{
			const UiPoint centre = CentrePoint(*t, px, extent);
			// Offset slightly up from the visual centre to account for glyph baseline.
			const UiPoint labelPos{
				.anchor   = centre.anchor,
				.offsetPx = { centre.offsetPx.x, centre.offsetPx.y - theme.buttonFontSize * 0.35f },
			};
			ui.DrawText(btn->label, labelPos, theme.buttonFontSize, btn->textColor);
		}

		return inp->clicked;
	}

	// ── Slider ────────────────────────────────────────────────────────────────

	float DrawSlider(UiWorld& world, Entity entity, UIRenderer& ui, const Input& input, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t      = world.TryGet<UiTransformComponent>(entity);
		auto* slider = world.TryGet<UiSliderComponent>(entity);
		auto* inp    = world.TryGet<UiInputComponent>(entity);
		if (!t || !slider || !inp)
		{
			return 0.f;
		}

		const glm::vec4 px = PixelRect(*t, extent);
		const float trackH = px.w;
		const float range  = std::max(slider->max - slider->min, 1e-6f);

		// Update isDragging and value from mouse.
		if (inp->pressed)
		{
			slider->isDragging = true;
		}
		if (!input.IsMouseButtonDown(MouseButton::Left))
		{
			slider->isDragging = false;
		}
		if (slider->isDragging)
		{
			const float mouseX = input.GetMousePos().x;
			const float t01    = std::clamp((mouseX - px.x) / px.z, 0.f, 1.f);
			slider->value      = slider->min + t01 * range;
		}

		// Draw track background.
		ui.DrawRect(t->rect, theme.sliderTrack, theme.cornerRadius * 0.5f);

		// Draw fill up to current value.
		const float fillFraction = (slider->value - slider->min) / range;
		const float fillW        = px.z * fillFraction;
		if (fillW > 0.f)
		{
			ui.DrawRect(PixelToUiRect(*t, { px.x, px.y, fillW, trackH }, extent), theme.sliderFill, theme.cornerRadius * 0.5f);
		}

		// Draw knob circle.
		const float knobCX = px.x + fillW;
		const float knobCY = px.y + trackH * 0.5f;
		ui.DrawCircle(PixelPoint(*t, { knobCX, knobCY }, extent), theme.knobRadius, slider->isDragging ? theme.accent : theme.sliderKnob);

		return slider->value;
	}

	// ── Checkbox ─────────────────────────────────────────────────────────────

	bool DrawCheckbox(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t   = world.TryGet<UiTransformComponent>(entity);
		auto* cb  = world.TryGet<UiCheckboxComponent>(entity);
		auto* inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !cb || !inp)
		{
			return false;
		}

		if (inp->clicked)
		{
			cb->checked = !cb->checked;
		}

		const glm::vec4 px    = PixelRect(*t, extent);
		const float boxSize   = std::min(px.w, 16.f);
		const float boxY      = px.y + (px.w - boxSize) * 0.5f;
		const glm::vec4 boxPx = { px.x, boxY, boxSize, boxSize };

		// Box background.
		const glm::vec4 boxColor = cb->checked ? theme.checkboxOn : theme.checkboxOff;
		ui.DrawRect(PixelToUiRect(*t, boxPx, extent), boxColor, 3.f);

		// Tick mark (two line segments) when checked.
		if (cb->checked)
		{
			const float m = boxSize * 0.15f;
			const glm::vec2 p0{ boxPx.x + m,              boxPx.y + boxPx.w * 0.5f };
			const glm::vec2 p1{ boxPx.x + boxPx.z * 0.4f, boxPx.y + boxPx.w - m * 1.5f };
			const glm::vec2 p2{ boxPx.x + boxPx.z - m,   boxPx.y + m * 1.5f };
			ui.DrawLine(PixelPoint(*t, p0, extent), PixelPoint(*t, p1, extent), 2.f, theme.text);
			ui.DrawLine(PixelPoint(*t, p1, extent), PixelPoint(*t, p2, extent), 2.f, theme.text);
		}

		// Inline label to the right of the box.
		if (!cb->label.empty())
		{
			const float labelX = boxPx.x + boxSize + 6.f;
			const float labelY = px.y + (px.w - theme.bodyFontSize) * 0.5f;
			ui.DrawText(cb->label, PixelPoint(*t, { labelX, labelY }, extent), theme.bodyFontSize, theme.text);
		}

		return cb->checked;
	}

	// ── Progress bar ──────────────────────────────────────────────────────────

	void DrawProgressBar(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t      = world.TryGet<UiTransformComponent>(entity);
		auto* slider = world.TryGet<UiSliderComponent>(entity);
		if (!t || !slider)
		{
			return;
		}

		const glm::vec4 px       = PixelRect(*t, extent);
		const float range        = std::max(slider->max - slider->min, 1e-6f);
		const float fillFraction = std::clamp((slider->value - slider->min) / range, 0.f, 1.f);
		const float fillW        = px.z * fillFraction;

		ui.DrawRect(t->rect, theme.sliderTrack, theme.cornerRadius * 0.5f);
		if (fillW > 0.f)
		{
			ui.DrawRect(PixelToUiRect(*t, { px.x, px.y, fillW, px.w }, extent), theme.sliderFill, theme.cornerRadius * 0.5f);
		}
	}

	// ── Panel ─────────────────────────────────────────────────────────────────

	bool DrawPanel(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t     = world.TryGet<UiTransformComponent>(entity);
		auto* panel = world.TryGet<UiPanelComponent>(entity);
		auto* inp   = world.TryGet<UiInputComponent>(entity);
		if (!t || !panel)
		{
			return true;
		}

		// Toggle collapse on click of the header area.
		if (inp && inp->clicked)
		{
			const glm::vec4 px = PixelRect(*t, extent);
			const glm::vec2 mp = inp->hovered ? glm::vec2{} : glm::vec2{};
			// Collapse handled by UiSystem's drag-start check; click anywhere in header toggles.
			if (panel->collapsible)
			{
				panel->collapsed = !panel->collapsed;
			}
		}

		const glm::vec4 px = PixelRect(*t, extent);

		// Panel background.
		ui.SetLayer(0);
		ui.DrawRect(t->rect, theme.panelBg, theme.cornerRadius);

		// Header background.
		ui.SetLayer(1);
		const float hdrH    = theme.headerHeight;
		const glm::vec4 hdrPx = { px.x, px.y, px.z, hdrH };
		ui.DrawRect(PixelToUiRect(*t, hdrPx, extent), theme.panelHeaderBg, theme.cornerRadius);

		// Accent bar on left edge of header.
		const glm::vec4 accentPx = { px.x + 2.f, px.y + hdrH * 0.2f, 3.f, hdrH * 0.6f };
		ui.DrawRect(PixelToUiRect(*t, accentPx, extent), theme.accent);

		// Title text.
		const glm::vec2 sizePx{ static_cast<float>(extent.width), static_cast<float>(extent.height) };
		const glm::vec2 anchorPx = t->rect.anchorMin * sizePx;
		if (!panel->title.empty())
		{
			ui.DrawText(panel->title,
			        UiPoint{
			                .anchor   = t->rect.anchorMin,
			                .offsetPx = { px.x + theme.padding - anchorPx.x, px.y + hdrH * 0.5f - theme.titleFontSize * 0.35f - anchorPx.y },
			        },
			        theme.titleFontSize,
			        theme.textTitle);
		}

		// Collapse arrow indicator.
		if (panel->collapsible)
		{
			const float arrowX = px.x + px.z - theme.padding;
			const float arrowY = px.y + hdrH * 0.5f;
			if (panel->collapsed)
			{
				// Right-pointing triangle (collapsed).
				ui.DrawLine(PixelPoint(*t, { arrowX - 5.f, arrowY - 5.f }, extent), PixelPoint(*t, { arrowX, arrowY }, extent), 2.f, theme.textLabel);
				ui.DrawLine(PixelPoint(*t, { arrowX, arrowY }, extent), PixelPoint(*t, { arrowX - 5.f, arrowY + 5.f }, extent), 2.f, theme.textLabel);
			}
			else
			{
				// Down-pointing triangle (expanded).
				ui.DrawLine(PixelPoint(*t, { arrowX - 5.f, arrowY - 3.f }, extent), PixelPoint(*t, { arrowX, arrowY + 3.f }, extent), 2.f, theme.textLabel);
				ui.DrawLine(PixelPoint(*t, { arrowX, arrowY + 3.f }, extent), PixelPoint(*t, { arrowX + 5.f, arrowY - 3.f }, extent), 2.f, theme.textLabel);
			}
		}

		// Separator line below header.
		ui.SetLayer(2);
		const float sepY = px.y + hdrH;
		ui.DrawLine(
		        UiPoint{ .anchor = t->rect.anchorMin, .offsetPx = { px.x + theme.padding - anchorPx.x, sepY - anchorPx.y } },
		        UiPoint{ .anchor = t->rect.anchorMin, .offsetPx = { px.x + px.z - theme.padding - anchorPx.x, sepY - anchorPx.y } },
		        1.f,
		        theme.separator);

		return !panel->collapsed;
	}

	// ── Z-order management ────────────────────────────────────────────────────

	void BringToFront(UiWorld& world, Entity entity)
	{
		float maxZ = 0.f;
		for (auto [e, t]: world.View<UiTransformComponent>().each())
		{
			maxZ = std::max(maxZ, t.zOrder);
		}
		if (auto* t = world.TryGet<UiTransformComponent>(entity))
		{
			if (t->zOrder < maxZ)
			{
				t->zOrder = maxZ + 1.f;
			}
		}
	}

	// ── Layout ───────────────────────────────────────────────────────────────

	void ApplyLayout(UiWorld& world, Entity container, VkExtent2D extent)
	{
		auto* layout   = world.TryGet<UiLayoutComponent>(container);
		auto* children = world.TryGet<UiChildrenComponent>(container);
		auto* parent   = world.TryGet<UiTransformComponent>(container);
		if (!layout || !children || !parent)
		{
			return;
		}

		const glm::vec4 parentPx = PixelRect(*parent, extent);
		const float pad          = layout->padding;
		const float spacing      = layout->spacing;
		const bool isVertical    = (layout->direction == UiLayoutComponent::Direction::Vertical);

		float cursor = isVertical ? (parentPx.y + pad) : (parentPx.x + pad);

		for (const Entity child: children->children)
		{
			auto* ct = world.TryGet<UiTransformComponent>(child);
			if (!ct)
			{
				continue;
			}

			const glm::vec4 childPx = PixelRect(*ct, extent);

			if (isVertical)
			{
				// Stack top-to-bottom: preserve child width/height, move Y.
				const float newY = cursor;
				ct->rect         = PixelToUiRect(*ct, { parentPx.x + pad, newY, parentPx.z - 2.f * pad, childPx.w }, extent);
				cursor           += childPx.w + spacing;
			}
			else
			{
				// Stack left-to-right: preserve child width/height, move X.
				const float newX = cursor;
				ct->rect         = PixelToUiRect(*ct, { newX, parentPx.y + pad, childPx.z, parentPx.w - 2.f * pad }, extent);
				cursor           += childPx.z + spacing;
			}
		}
	}

	void RunLayouts(UiWorld& world, VkExtent2D extent)
	{
		for (auto [e, layout, children]: world.View<UiLayoutComponent, UiChildrenComponent>().each())
		{
			ApplyLayout(world, UiWorld::FromEntt(e), extent);
		}
	}

	// ── Spawn helpers ─────────────────────────────────────────────────────────

	Entity SpawnButton(UiWorld& world, UiRect rect, std::string_view label, float zOrder)
	{
		return world.Spawn()
		        .Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder })
		        .Add<UiInputComponent>()
		        .Add<UiButtonComponent>(UiButtonComponent{ .label = std::string(label) })
		        .entity();
	}

	Entity SpawnSlider(UiWorld& world, UiRect rect, float min, float max, float value, float zOrder)
	{
		return world.Spawn()
		        .Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder })
		        .Add<UiInputComponent>()
		        .Add<UiSliderComponent>(UiSliderComponent{ .min = min, .max = max, .value = value })
		        .entity();
	}

	Entity SpawnCheckbox(UiWorld& world, UiRect rect, std::string_view label, bool checked, float zOrder)
	{
		return world.Spawn()
		        .Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder })
		        .Add<UiInputComponent>()
		        .Add<UiCheckboxComponent>(UiCheckboxComponent{ .checked = checked, .label = std::string(label) })
		        .entity();
	}

	Entity SpawnProgressBar(UiWorld& world, UiRect rect, float min, float max, float value, float zOrder)
	{
		return world.Spawn()
		        .Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder })
		        .Add<UiSliderComponent>(UiSliderComponent{ .min = min, .max = max, .value = value })
		        .entity();
	}

	Entity SpawnPanel(UiWorld& world, UiRect rect, std::string_view title, bool draggable, bool collapsible, float zOrder)
	{
		return world.Spawn()
		        .Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder })
		        .Add<UiInputComponent>()
		        .Add<UiPanelComponent>(UiPanelComponent{
		                .title       = std::string(title),
		                .draggable   = draggable,
		                .collapsible = collapsible,
		        })
		        .entity();
	}

} // namespace aether::ui
