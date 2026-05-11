#include "UiWidgets.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "platform/Input.hpp"
#include "ui/UIRenderer.hpp"
#include "UiComponents.hpp"
#include "UiContext.hpp"
#include "ui/UiLayout.hpp"
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
			.anchorMin = t.rect.anchorMin,
			.anchorMax = t.rect.anchorMin,
			.offsetMinPx = {        px.x - anchorPx.x,        px.y - anchorPx.y },
			.offsetMaxPx = { px.x + px.z - anchorPx.x, px.y + px.w - anchorPx.y },
		};
	}

	// A UiPoint centred on a pixel rect, using the entity's anchor.
	static UiPoint CentrePoint(const UiTransformComponent& t, glm::vec4 px, VkExtent2D ext)
	{
		const glm::vec2 sizePx{ static_cast<float>(ext.width), static_cast<float>(ext.height) };
		const glm::vec2 anchorPx = t.rect.anchorMin * sizePx;
		return UiPoint{
			.anchor = t.rect.anchorMin,
			.offsetPx = { px.x + px.z * 0.5f - anchorPx.x, px.y + px.w * 0.5f - anchorPx.y },
		};
	}

	// UiPoint at a specific pixel position, using the entity's anchor.
	static UiPoint PixelPoint(const UiTransformComponent& t, glm::vec2 px, VkExtent2D ext)
	{
		const glm::vec2 sizePx{ static_cast<float>(ext.width), static_cast<float>(ext.height) };
		const glm::vec2 anchorPx = t.rect.anchorMin * sizePx;
		return UiPoint{
			.anchor = t.rect.anchorMin,
			.offsetPx = px - anchorPx,
		};
	}

	// ── Button ────────────────────────────────────────────────────────────────

	bool DrawButton(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t = world.TryGet<UiTransformComponent>(entity);
		auto* btn = world.TryGet<UiButtonComponent>(entity);
		auto* inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !btn || !inp)
		{
			return false;
		}

		// Smooth colour blend: mix normal->hover, then normal->press using animated weights.
		// pressT takes priority over hoverT (press is the innermost state).
		glm::vec4 bgColor = glm::mix(glm::mix(btn->normalColor, btn->hoverColor, inp->hoverT), btn->pressColor, inp->pressT);

		const glm::vec4 px = PixelRect(*t, extent);
		ui.DrawRect(t->rect, bgColor, theme.cornerRadius);

		// Centred label.
		// label.position is the baseline in screen space; the glyph cap-height sits
		// at baseline - bearingY*fontSize (≈ baseline - 0.7*fs).  Visual centre of
		// caps ≈ baseline - 0.35*fs, so baseline = widgetCentreY + 0.35*fontSize.
		// Horizontally: MeasureText gives total advance; shift left by half so the
		// string is centred rather than starting at the widget centre.
		if (!btn->label.empty())
		{
			const UiPoint centre = CentrePoint(*t, px, extent);
			const float textW = ui.MeasureText(btn->label, theme.buttonFontSize);
			ui.DrawText(btn->label,
			        UiPoint{
			                centre.anchor, { centre.offsetPx.x - textW * 0.5f, centre.offsetPx.y + theme.buttonFontSize * 0.35f }
            },
			        theme.buttonFontSize,
			        btn->textColor);
		}

		return inp->clicked;
	}

	// ── Slider ────────────────────────────────────────────────────────────────

	float DrawSlider(UiWorld& world, Entity entity, UIRenderer& ui, const Input& input, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t = world.TryGet<UiTransformComponent>(entity);
		auto* slider = world.TryGet<UiSliderComponent>(entity);
		auto* inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !slider || !inp)
		{
			return 0.f;
		}

		const glm::vec4 px = PixelRect(*t, extent);
		const float trackH = px.w;
		const float range = std::max(slider->max - slider->min, 1e-6f);

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
			const float t01 = std::clamp((mouseX - px.x) / px.z, 0.f, 1.f);
			slider->value = slider->min + t01 * range;
		}

		// Draw track background.
		ui.DrawRect(t->rect, theme.sliderTrack, theme.cornerRadius * 0.5f);

		// Draw fill up to current value.
		const float fillFraction = (slider->value - slider->min) / range;
		const float fillW = px.z * fillFraction;
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
		auto* t = world.TryGet<UiTransformComponent>(entity);
		auto* cb = world.TryGet<UiCheckboxComponent>(entity);
		auto* inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !cb || !inp)
		{
			return false;
		}

		if (inp->clicked)
		{
			cb->checked = !cb->checked;
		}

		const glm::vec4 px = PixelRect(*t, extent);
		const float boxSize = std::min(px.w, 16.f);
		const float boxY = px.y + (px.w - boxSize) * 0.5f;
		const glm::vec4 boxPx = { px.x, boxY, boxSize, boxSize };

		// Box background.
		const glm::vec4 boxColor = cb->checked ? theme.checkboxOn : theme.checkboxOff;
		ui.DrawRect(PixelToUiRect(*t, boxPx, extent), boxColor, 3.f);

		// Tick mark (two line segments) when checked.
		if (cb->checked)
		{
			const float m = boxSize * 0.15f;
			const glm::vec2 p0{ boxPx.x + m, boxPx.y + boxPx.w * 0.5f };
			const glm::vec2 p1{ boxPx.x + boxPx.z * 0.4f, boxPx.y + boxPx.w - m * 1.5f };
			const glm::vec2 p2{ boxPx.x + boxPx.z - m, boxPx.y + m * 1.5f };
			ui.DrawLine(PixelPoint(*t, p0, extent), PixelPoint(*t, p1, extent), 2.f, theme.text);
			ui.DrawLine(PixelPoint(*t, p1, extent), PixelPoint(*t, p2, extent), 2.f, theme.text);
		}

		// Inline label to the right of the box.
		if (!cb->label.empty())
		{
			const float labelX = boxPx.x + boxSize + 6.f;
			const float labelY = px.y + px.w * 0.5f + theme.bodyFontSize * 0.35f;
			ui.DrawText(cb->label, PixelPoint(*t, { labelX, labelY }, extent), theme.bodyFontSize, theme.text);
		}

		return cb->checked;
	}

	// ── Progress bar ──────────────────────────────────────────────────────────

	void DrawProgressBar(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t = world.TryGet<UiTransformComponent>(entity);
		auto* slider = world.TryGet<UiSliderComponent>(entity);
		if (!t || !slider)
		{
			return;
		}

		const glm::vec4 px = PixelRect(*t, extent);
		const float range = std::max(slider->max - slider->min, 1e-6f);
		const float fillFraction = std::clamp((slider->value - slider->min) / range, 0.f, 1.f);
		const float fillW = px.z * fillFraction;

		ui.DrawRect(t->rect, theme.sliderTrack, theme.cornerRadius * 0.5f);
		if (fillW > 0.f)
		{
			ui.DrawRect(PixelToUiRect(*t, { px.x, px.y, fillW, px.w }, extent), theme.sliderFill, theme.cornerRadius * 0.5f);
		}
	}

	// ── Panel ─────────────────────────────────────────────────────────────────

	bool DrawPanel(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t = world.TryGet<UiTransformComponent>(entity);
		auto* panel = world.TryGet<UiPanelComponent>(entity);
		auto* inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !panel)
		{
			return true;
		}

		// Toggle collapse on header click: save/restore full rect so the body
		// disappears when collapsed and reappears at the original size on expand.
		if (inp && inp->clicked && panel->collapsible)
		{
			const glm::vec4 px0 = PixelRect(*t, extent);
			const bool inHeader = (inp->clickPos.y >= px0.y && inp->clickPos.y < px0.y + theme.headerHeight);
			if (inHeader)
			{
				if (!panel->collapsed)
				{
					panel->expandedRect = t->rect;
					t->rect = PixelToUiRect(*t, { px0.x, px0.y, px0.z, theme.headerHeight }, extent);
				}
				else
				{
					t->rect = panel->expandedRect;
				}
				panel->collapsed = !panel->collapsed;
			}
		}

		const glm::vec4 px = PixelRect(*t, extent);
		const float hdrH = theme.headerHeight;
		const glm::vec2 sizePx{ static_cast<float>(extent.width), static_cast<float>(extent.height) };
		const glm::vec2 anchorPx = t->rect.anchorMin * sizePx;

		// Panel background - only drawn when expanded.
		ui.SetLayer(0);
		if (!panel->collapsed)
		{
			ui.DrawRect(t->rect, theme.panelBg, theme.cornerRadius);
		}

		// Header background (always drawn; covers the full rect when collapsed).
		ui.SetLayer(1);
		const glm::vec4 hdrPx = { px.x, px.y, px.z, hdrH };
		ui.DrawRect(PixelToUiRect(*t, hdrPx, extent), theme.panelHeaderBg, theme.cornerRadius);

		// Accent bar on left edge of header.
		const glm::vec4 accentPx = { px.x + 2.f, px.y + hdrH * 0.2f, 3.f, hdrH * 0.6f };
		ui.DrawRect(PixelToUiRect(*t, accentPx, extent), theme.accent);

		// Title text.
		if (!panel->title.empty())
		{
			ui.DrawText(panel->title,
			        UiPoint{
			                .anchor = t->rect.anchorMin,
			                .offsetPx = { px.x + theme.padding - anchorPx.x, px.y + hdrH * 0.5f + theme.titleFontSize * 0.35f - anchorPx.y },
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

		// Separator line below header - only when expanded.
		if (!panel->collapsed)
		{
			ui.SetLayer(2);
			const float sepY = px.y + hdrH;
			ui.DrawLine(
			        UiPoint{
			                .anchor = t->rect.anchorMin, .offsetPx = { px.x + theme.padding - anchorPx.x, sepY - anchorPx.y }
            },
			        UiPoint{ .anchor = t->rect.anchorMin, .offsetPx = { px.x + px.z - theme.padding - anchorPx.x, sepY - anchorPx.y } },
			        1.f,
			        theme.separator);
		}

		return !panel->collapsed;
	}

	// ── Z-order management ────────────────────────────────────────────────────

	// Recursively adds `delta` to the z-order of `entity` and every descendant
	// reachable through UiChildrenComponent.
	static void RaiseSubtree(UiWorld& world, Entity entity, float delta)
	{
		if (auto* t = world.TryGet<UiTransformComponent>(entity))
		{
			t->zOrder += delta;
		}
		if (const auto* ch = world.TryGet<UiChildrenComponent>(entity))
		{
			for (const Entity child: ch->children)
			{
				RaiseSubtree(world, child, delta);
			}
		}
	}

	void BringToFront(UiWorld& world, Entity entity)
	{
		float maxZ = 0.f;
		for (auto [e, t]: world.View<UiTransformComponent>().each())
		{
			maxZ = std::max(maxZ, t.zOrder);
		}

		auto* t = world.TryGet<UiTransformComponent>(entity);
		if (!t || t->zOrder > maxZ)
		{
			return;
		}

		// Raise by enough to land one step above the current maximum.
		// The same delta is applied to every descendant so children always remain
		// above their parent panel, preserving their relative z ordering.
		const float delta = maxZ + 1.f - t->zOrder;
		t->zOrder = maxZ + 1.f;

		if (const auto* ch = world.TryGet<UiChildrenComponent>(entity))
		{
			for (const Entity child: ch->children)
			{
				RaiseSubtree(world, child, delta);
			}
		}
	}

	// ── Layout ───────────────────────────────────────────────────────────────

	void ApplyLayout(UiWorld& world, Entity container, VkExtent2D extent)
	{
		auto* layout = world.TryGet<UiLayoutComponent>(container);
		auto* children = world.TryGet<UiChildrenComponent>(container);
		auto* parent = world.TryGet<UiTransformComponent>(container);
		if (!layout || !children || !parent)
		{
			return;
		}

		const glm::vec4 parentPx = PixelRect(*parent, extent);
		const float pad = layout->padding;
		const float spacing = layout->spacing;
		const bool isVertical = (layout->direction == UiLayoutComponent::Direction::Vertical);

		// ── Pass 1: measure fixed children, sum flex weights ─────────────────
		float fixedTotal = 0.f;
		float flexWeightTotal = 0.f;
		int childCount = 0;

		for (const Entity child: children->children)
		{
			const auto* ct = world.TryGet<UiTransformComponent>(child);
			if (!ct)
			{
				continue;
			}
			++childCount;

			if (ct->flexGrow > 0.f)
			{
				flexWeightTotal += ct->flexGrow;
			}
			else
			{
				const glm::vec4 childPx = PixelRect(*ct, extent);
				fixedTotal += isVertical ? childPx.w : childPx.z;
			}
		}

		// Space remaining after fixed children and inter-item gaps.
		const float totalGaps = childCount > 1 ? static_cast<float>(childCount - 1) * spacing : 0.f;
		const float available = isVertical ? (parentPx.w - 2.f * pad) : (parentPx.z - 2.f * pad);
		const float flexPool = std::max(0.f, available - fixedTotal - totalGaps);

		// ── Pass 2: position all children ────────────────────────────────────
		const float crossSize = isVertical ? (parentPx.z - 2.f * pad) : (parentPx.w - 2.f * pad);
		float cursor = isVertical ? (parentPx.y + pad) : (parentPx.x + pad);

		for (const Entity child: children->children)
		{
			auto* ct = world.TryGet<UiTransformComponent>(child);
			if (!ct)
			{
				continue;
			}

			const glm::vec4 childPx = PixelRect(*ct, extent);
			const float mainSize = (ct->flexGrow > 0.f && flexWeightTotal > 0.f) ? flexPool * (ct->flexGrow / flexWeightTotal) : (isVertical ? childPx.w : childPx.z);

			if (isVertical)
			{
				ct->rect = PixelToUiRect(*ct, { parentPx.x + pad, cursor, crossSize, mainSize }, extent);
			}
			else
			{
				ct->rect = PixelToUiRect(*ct, { cursor, parentPx.y + pad, mainSize, crossSize }, extent);
			}
			cursor += mainSize + spacing;
		}

		// ── Auto-size: shrink/grow container to wrap content ──────────────────
		// cursor is now: start + pad + sum(sizes) + N*spacing.
		// Desired container size: 2*pad + sum(sizes) + (N-1)*spacing = cursor - start - spacing + pad.
		if (layout->autoSize && childCount > 0)
		{
			const float start = isVertical ? parentPx.y : parentPx.x;
			const float newMainSize = cursor - start - spacing + pad;
			glm::vec4 newPx = parentPx;
			if (isVertical)
			{
				newPx.w = newMainSize;
			}
			else
			{
				newPx.z = newMainSize;
			}
			parent->rect = PixelToUiRect(*parent, newPx, extent);
		}
	}

	void ApplyGridLayout(UiWorld& world, Entity container, VkExtent2D extent)
	{
		auto* grid = world.TryGet<UiGridLayoutComponent>(container);
		auto* children = world.TryGet<UiChildrenComponent>(container);
		auto* parent = world.TryGet<UiTransformComponent>(container);
		if (!grid || !children || !parent)
		{
			return;
		}

		const glm::vec4 parentPx = PixelRect(*parent, extent);
		const float pad = grid->padding;
		const float slot = grid->slotSize;
		const float sp = grid->spacing;
		const int cols = grid->columns;

		int i = 0;
		for (const Entity child: children->children)
		{
			auto* ct = world.TryGet<UiTransformComponent>(child);
			if (!ct)
			{
				++i;
				continue;
			}

			const int col = i % cols;
			const int row = i / cols;
			const float x = parentPx.x + pad + static_cast<float>(col) * (slot + sp);
			const float y = parentPx.y + pad + static_cast<float>(row) * (slot + sp);
			ct->rect = PixelToUiRect(*ct, { x, y, slot, slot }, extent);
			++i;
		}

		// Auto-size container height to wrap all rows.
		const int childCount = static_cast<int>(children->children.size());
		if (childCount > 0)
		{
			const int rows = (childCount + cols - 1) / cols;
			const float newH = 2.f * pad + static_cast<float>(rows) * slot + static_cast<float>(std::max(0, rows - 1)) * sp;
			glm::vec4 newPx = parentPx;
			newPx.w = newH;
			parent->rect = PixelToUiRect(*parent, newPx, extent);
		}
	}

	void RunLayouts(UiWorld& world, VkExtent2D extent)
	{
		for (auto [e, layout, children]: world.View<UiLayoutComponent, UiChildrenComponent>().each())
		{
			ApplyLayout(world, UiWorld::FromEntt(e), extent);
		}
		for (auto [e, grid, children]: world.View<UiGridLayoutComponent, UiChildrenComponent>().each())
		{
			ApplyGridLayout(world, UiWorld::FromEntt(e), extent);
		}
	}

	// ── Spawn helpers ─────────────────────────────────────────────────────────

	Entity SpawnButton(UiWorld& world, UiRect rect, std::string_view label, float zOrder)
	{
		return world.Spawn().Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder }).Add<UiInputComponent>().Add<UiButtonComponent>(UiButtonComponent{ .label = std::string(label) }).entity();
	}

	Entity SpawnSlider(UiWorld& world, UiRect rect, float min, float max, float value, float zOrder)
	{
		return world.Spawn().Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder }).Add<UiInputComponent>().Add<UiSliderComponent>(UiSliderComponent{ .min = min, .max = max, .value = value }).entity();
	}

	Entity SpawnCheckbox(UiWorld& world, UiRect rect, std::string_view label, bool checked, float zOrder)
	{
		return world.Spawn().Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder }).Add<UiInputComponent>().Add<UiCheckboxComponent>(UiCheckboxComponent{ .checked = checked, .label = std::string(label) }).entity();
	}

	Entity SpawnProgressBar(UiWorld& world, UiRect rect, float min, float max, float value, float zOrder)
	{
		return world.Spawn().Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder }).Add<UiSliderComponent>(UiSliderComponent{ .min = min, .max = max, .value = value }).entity();
	}

	Entity SpawnPanel(UiWorld& world, UiRect rect, std::string_view title, bool draggable, bool collapsible, float zOrder)
	{
		return world.Spawn()
		        .Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder })
		        .Add<UiInputComponent>()
		        .Add<UiPanelComponent>(UiPanelComponent{
		                .title = std::string(title),
		                .draggable = draggable,
		                .collapsible = collapsible,
		        })
		        .entity();
	}

	// ── Text Input ────────────────────────────────────────────────────────────

	bool DrawTextInput(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t = world.TryGet<UiTransformComponent>(entity);
		auto* ti = world.TryGet<UiTextInputComponent>(entity);
		auto* inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !ti || !inp)
		{
			return false;
		}

		const glm::vec4 px = PixelRect(*t, extent);

		// Background: blend normal->hover when not focused, snap to focus color when focused.
		const glm::vec4 bgColor = inp->focused ? theme.inputFocusBg : glm::mix(theme.inputBg, theme.inputHoverBg, inp->hoverT);
		ui.DrawRect(t->rect, bgColor, theme.cornerRadius);

		// Accent bar on left edge indicates keyboard focus.
		if (inp->focused)
		{
			const glm::vec4 accentPx = { px.x, px.y + 3.f, 2.f, px.w - 6.f };
			ui.DrawRect(PixelToUiRect(*t, accentPx, extent), theme.accent);
		}

		// Clip all text/cursor drawing to the input box.
		ui.PushClipRect(t->rect);

		const float textX = px.x + theme.padding * 0.5f;
		const float textY = px.y + px.w * 0.5f + theme.bodyFontSize * 0.35f;

		if (ti->text.empty() && !ti->placeholder.empty())
		{
			ui.DrawText(ti->placeholder, PixelPoint(*t, { textX, textY }, extent), theme.bodyFontSize, theme.placeholder);
		}
		else if (!ti->text.empty())
		{
			ui.DrawText(ti->text, PixelPoint(*t, { textX, textY }, extent), theme.bodyFontSize, theme.text);
		}

		// Blinking cursor - only when focused.
		if (inp->focused && ti->cursorVisible)
		{
			const std::string_view prefix(ti->text.data(), static_cast<std::size_t>(ti->cursorPos));
			const float cursorX = textX + ui.MeasureText(prefix, theme.bodyFontSize);
			ui.DrawLine(PixelPoint(*t, { cursorX, px.y + 3.f }, extent), PixelPoint(*t, { cursorX, px.y + px.w - 3.f }, extent), 1.5f, theme.accent);
		}

		ui.PopClipRect();

		// submitted is set by UiSystem::ProcessTextInput for one frame on Enter.
		const bool wasSubmitted = ti->submitted;
		ti->submitted = false;
		return wasSubmitted;
	}

	Entity SpawnTextInput(UiWorld& world, UiRect rect, std::string_view placeholder, float zOrder)
	{
		return world.Spawn().Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder }).Add<UiInputComponent>().Add<UiTextInputComponent>(UiTextInputComponent{ .placeholder = std::string(placeholder) }).entity();
	}

	// ── Hierarchy helper ──────────────────────────────────────────────────────

	void AddChild(UiWorld& world, Entity parent, Entity child)
	{
		world.TryGet<UiChildrenComponent>(parent)->children.push_back(child);
		world.Emplace<UiParentComponent>(child, UiParentComponent{ parent });
	}

	// ── Item Slot ─────────────────────────────────────────────────────────────

	bool DrawItemSlot(UiWorld& world, Entity entity, UIRenderer& ui, VkExtent2D extent, const UiTheme& theme)
	{
		auto* t = world.TryGet<UiTransformComponent>(entity);
		auto* slot = world.TryGet<UiItemSlotComponent>(entity);
		auto* inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !slot || !inp)
		{
			return false;
		}

		const glm::vec4 px = PixelRect(*t, extent);
		static constexpr float kBorderW = 2.f;

		// Border colour: selected > rarity (when filled) > hover blend > default.
		glm::vec4 borderColor = theme.slotBorder;
		if (slot->quantity > 0 && slot->rarityColor.a > 0.f)
		{
			borderColor = slot->rarityColor;
		}
		if (inp->hoverT > 0.f)
		{
			borderColor = glm::mix(borderColor, theme.slotHoverBorder, inp->hoverT);
		}
		if (slot->selected)
		{
			borderColor = theme.slotSelectedBorder;
		}

		// Outer border rect.
		ui.DrawRect(t->rect, borderColor, theme.slotCornerRadius);

		// Inner background.
		const glm::vec4 innerPx = { px.x + kBorderW, px.y + kBorderW, px.z - 2.f * kBorderW, px.w - 2.f * kBorderW };
		ui.DrawRect(PixelToUiRect(*t, innerPx, extent), theme.slotBg, theme.slotCornerRadius - kBorderW);

		// Item contents when the slot is not empty.
		if (slot->quantity > 0)
		{
			const float iconInset = kBorderW + 2.f;
			const glm::vec4 iconPx = { px.x + iconInset, px.y + iconInset, px.z - 2.f * iconInset, px.w - 2.f * iconInset };
			const UiRect iconRect = PixelToUiRect(*t, iconPx, extent);

			if (slot->textureSlot > 0)
			{
				ui.DrawTexturedRect(iconRect, slot->textureSlot, { 0.f, 0.f, 1.f, 1.f }, glm::vec4(1.f), theme.slotCornerRadius - iconInset);
			}
			else if (slot->rarityColor.a > 0.f)
			{
				// No texture loaded: draw a tinted placeholder so the item is visible.
				glm::vec4 tint = slot->rarityColor;
				tint.a *= 0.30f;
				ui.DrawRect(iconRect, tint, theme.slotCornerRadius - iconInset);
			}

			// Quantity badge (bottom-right, only when more than one).
			if (slot->quantity > 1)
			{
				const std::string qty = std::to_string(slot->quantity);
				const float tw = ui.MeasureText(qty, theme.slotFontSize);
				const float tx = px.x + px.z - tw - 3.f;
				const float ty = px.y + px.w - theme.slotFontSize - 2.f;

				const glm::vec2 sizePx{ static_cast<float>(extent.width), static_cast<float>(extent.height) };
				const glm::vec2 anchorPx = t->rect.anchorMin * sizePx;

				// Drop shadow for readability.
				ui.DrawText(qty,
				        UiPoint{
				                t->rect.anchorMin, { tx - anchorPx.x + 1.f, ty - anchorPx.y + 1.f }
                },
				        theme.slotFontSize,
				        { 0.f, 0.f, 0.f, 0.75f });
				ui.DrawText(qty,
				        UiPoint{
				                t->rect.anchorMin, { tx - anchorPx.x, ty - anchorPx.y }
                },
				        theme.slotFontSize,
				        theme.quantityText);
			}
		}

		// Hover overlay on top of everything.
		if (inp->hoverT > 0.f)
		{
			glm::vec4 overlay = theme.slotHoverOverlay;
			overlay.a *= inp->hoverT;
			ui.DrawRect(t->rect, overlay, theme.slotCornerRadius);
		}

		return inp->clicked;
	}

	Entity SpawnItemSlot(UiWorld& world, UiRect rect, float zOrder)
	{
		return world.Spawn().Add<UiTransformComponent>(UiTransformComponent{ .rect = rect, .zOrder = zOrder }).Add<UiInputComponent>().Add<UiItemSlotComponent>().entity();
	}

	Entity SpawnItemGrid(UiWorld& world, UiRect containerRect, int columns, float slotSize, float spacing, float padding, int slotCount, float slotZOrder, Entity* slotsOut, float containerZOrder)
	{
		Entity container = world.Spawn()
		                           .Add<UiTransformComponent>(UiTransformComponent{ .rect = containerRect, .zOrder = containerZOrder })
		                           .Add<UiGridLayoutComponent>(UiGridLayoutComponent{
		                                   .columns = columns,
		                                   .slotSize = slotSize,
		                                   .spacing = spacing,
		                                   .padding = padding,
		                           })
		                           .Add<UiChildrenComponent>()
		                           .entity();

		for (int i = 0; i < slotCount; ++i)
		{
			Entity slot = SpawnItemSlot(world, {}, slotZOrder);
			AddChild(world, container, slot);
			if (slotsOut)
			{
				slotsOut[i] = slot;
			}
		}

		return container;
	}

} // namespace aether::ui
