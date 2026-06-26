#include "UiWidgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include "platform/Input.hpp"
#include "ui/UIRenderer.hpp"
#include "UiComponents.hpp"
#include "UiContext.hpp"
#include "utils/Profiler.hpp"
#include "ui/UiLayout.hpp"

namespace aether::ui
{
	// -- Theme singleton -------------------------------------------------------

	const UiTheme& UiTheme::Default()
	{
		static const UiTheme s_default;
		return s_default;
	}

	// -- Private pixel-space helpers -------------------------------------------

	// Resolved pixel rect of an entity's transform.
	static glm::vec4 PixelRect(const UiTransformComponent& t, gpu::Extent2D ext)
	{
		return ResolveUiRectPx(ext, t.rect);
	}

	// Builds a UiRect that lands on the given pixel rect, using the entity's
	// anchor so subsequent UIRenderer draws are consistent.
	static UiRect PixelToUiRect(const UiTransformComponent& t, glm::vec4 px, gpu::Extent2D ext)
	{
		const glm::vec2 sizePx{static_cast<float>(ext.width), static_cast<float>(ext.height)};
		const glm::vec2 anchorPx = t.rect.anchorMin * sizePx;
		return UiRect{
		        .anchorMin = t.rect.anchorMin,
		        .anchorMax = t.rect.anchorMin,
		        .offsetMinPx = {px.x - anchorPx.x, px.y - anchorPx.y},
		        .offsetMaxPx = {px.x + px.z - anchorPx.x, px.y + px.w - anchorPx.y},
		};
	}

	static std::string FitTextToWidth(UIRenderer& ui, std::string_view text, float fontSize, float maxWidth)
	{
		if (maxWidth <= 0.f || text.empty() || ui.MeasureText(text, fontSize) <= maxWidth)
		{
			return std::string(text);
		}

		constexpr std::string_view kEllipsis = "...";
		const float ellipsisW = ui.MeasureText(kEllipsis, fontSize);
		if (ellipsisW > maxWidth)
		{
			return {};
		}

		std::size_t lo = 0;
		std::size_t hi = text.size();
		while (lo < hi)
		{
			const std::size_t mid = (lo + hi + 1) / 2;
			std::string candidate{text.substr(0, mid)};
			candidate += kEllipsis;
			if (ui.MeasureText(candidate, fontSize) <= maxWidth)
			{
				lo = mid;
			}
			else
			{
				hi = mid - 1;
			}
		}

		std::string fitted{text.substr(0, lo)};
		fitted += kEllipsis;
		return fitted;
	}

	// A UiPoint centred on a pixel rect, using the entity's anchor.
	static UiPoint CentrePoint(const UiTransformComponent& t, glm::vec4 px, gpu::Extent2D ext)
	{
		const glm::vec2 sizePx{static_cast<float>(ext.width), static_cast<float>(ext.height)};
		const glm::vec2 anchorPx = t.rect.anchorMin * sizePx;
		return UiPoint{
		        .anchor = t.rect.anchorMin,
		        .offsetPx = {px.x + px.z * 0.5f - anchorPx.x, px.y + px.w * 0.5f - anchorPx.y},
		};
	}

	// UiPoint at a specific pixel position, using the entity's anchor.
	static UiPoint PixelPoint(const UiTransformComponent& t, glm::vec2 px, gpu::Extent2D ext)
	{
		const glm::vec2 sizePx{static_cast<float>(ext.width), static_cast<float>(ext.height)};
		const glm::vec2 anchorPx = t.rect.anchorMin * sizePx;
		return UiPoint{
		        .anchor = t.rect.anchorMin,
		        .offsetPx = px - anchorPx,
		};
	}

	std::int32_t ComputeEffectiveLayer(aether::World& world, Entity entity, int subLayer)
	{
		Entity current = entity;
		while (auto parent = world.TryGet<UiParentComponent>(current))
		{
			current = parent->parent;
		}
		float rootZ = 0.f;
		if (auto rt = world.TryGet<UiTransformComponent>(current))
		{
			rootZ = rt->zOrder;
		}
		float entityZ = 0.f;
		if (auto et = world.TryGet<UiTransformComponent>(entity))
		{
			entityZ = et->zOrder;
		}
		const float offset = std::clamp(entityZ - rootZ, 0.f, 9.999f);
		const std::int32_t rootLayer = static_cast<std::int32_t>(std::lround(rootZ)) * 100000;
		const auto offsetLayer = static_cast<std::int32_t>(std::lround(offset * 10000.f));
		return rootLayer + offsetLayer + subLayer;
	}

	// -- Button ----------------------------------------------------------------

	bool DrawButton(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		AE_PROFILE_ZONE();
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto btn = world.TryGet<UiButtonComponent>(entity);
		auto inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !btn || !inp)
		{
			return false;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		// Smooth colour blend: mix normal->hover, then normal->press using animated weights.
		// pressT takes priority over hoverT (press is the innermost state).
		glm::vec4 bgColor = glm::mix(glm::mix(btn->normalColor, btn->hoverColor, inp->hoverT), btn->pressColor, inp->pressT);

		const glm::vec4 px = PixelRect(*t, extent);
		ui.DrawRect(t->rect, bgColor, theme.cornerRadius);

		// Centred label.
		// label.position is the baseline in screen space; the glyph cap-height sits
		// at baseline - bearingY*fontSize (~ baseline - 0.7*fs).  Visual centre of
		// caps ~ baseline - 0.35*fs, so baseline = widgetCentreY + 0.35*fontSize.
		// Horizontally: MeasureText gives total advance; shift left by half so the
		// string is centred rather than starting at the widget centre.
		if (!btn->label.empty())
		{
			const UiPoint centre = CentrePoint(*t, px, extent);
			const float textW = ui.MeasureText(btn->label, theme.buttonFontSize);
			ui.DrawText(btn->label, UiPoint{.anchor = centre.anchor, .offsetPx = {centre.offsetPx.x - textW * 0.5f, centre.offsetPx.y + theme.buttonFontSize * 0.35f}}, theme.buttonFontSize, btn->textColor);
		}

		ui.SetLayer(prevLayer);
		return inp->clicked;
	}

	// -- Slider ----------------------------------------------------------------

	float DrawSlider(aether::World& world, Entity entity, UIRenderer& ui, const Input& input, gpu::Extent2D extent, const UiTheme& theme)
	{
		AE_PROFILE_ZONE();
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto slider = world.TryGet<UiSliderComponent>(entity);
		auto inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !slider || !inp)
		{
			return 0.f;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		const glm::vec4 px = PixelRect(*t, extent);
		const bool hasLabel = !slider->label.empty();
		const float labelH = hasLabel ? theme.labelFontSize + 6.f : 0.f;
		const float trackY = px.y + labelH;
		const float trackH = hasLabel ? std::max(8.f, px.w - labelH) : px.w;
		const float range = std::max(slider->max - slider->min, 1e-6f);

		if (hasLabel)
		{
			ui.DrawText(slider->label, PixelPoint(*t, {px.x, px.y + theme.labelFontSize}, extent), theme.labelFontSize, theme.textLabel);
		}

		// Update isDragging and value from mouse.
		if (inp->pressed)
		{
			slider->isDragging = true;
		}
		if (!input.IsMouseButtonDown(MouseButton::Left) || !inp->hovered)
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
		const UiRect trackRect = PixelToUiRect(*t, {px.x, trackY, px.z, trackH}, extent);
		ui.DrawRect(trackRect, theme.sliderTrack, theme.cornerRadius * 0.5f);

		// Draw fill up to current value.
		const float fillFraction = (slider->value - slider->min) / range;
		const float fillW = px.z * fillFraction;
		if (fillW > 0.f)
		{
			ui.DrawRect(PixelToUiRect(*t, {px.x, trackY, fillW, trackH}, extent), theme.sliderFill, theme.cornerRadius * 0.5f);
		}

		// Draw knob circle.
		const float knobCX = px.x + fillW;
		const float knobCY = trackY + trackH * 0.5f;
		ui.DrawCircle(PixelPoint(*t, {knobCX, knobCY}, extent), theme.knobRadius, slider->isDragging ? theme.accent : theme.sliderKnob);

		ui.SetLayer(prevLayer);
		return slider->value;
	}

	// -- Checkbox -------------------------------------------------------------

	bool DrawCheckbox(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		AE_PROFILE_ZONE();
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto cb = world.TryGet<UiCheckboxComponent>(entity);
		auto inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !cb || !inp)
		{
			return false;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		if (inp->clicked)
		{
			cb->checked = !cb->checked;
		}

		const glm::vec4 px = PixelRect(*t, extent);
		const float boxSize = std::min(px.w, 16.f);
		const float boxY = px.y + (px.w - boxSize) * 0.5f;
		const glm::vec4 boxPx = {px.x, boxY, boxSize, boxSize};

		// Box background.
		const glm::vec4 boxColor = cb->checked ? theme.checkboxOn : theme.checkboxOff;
		ui.DrawRect(PixelToUiRect(*t, boxPx, extent), boxColor, 3.f);

		// Tick mark (two line segments) when checked.
		if (cb->checked)
		{
			const float m = boxSize * 0.15f;
			const glm::vec2 p0{boxPx.x + m, boxPx.y + boxPx.w * 0.5f};
			const glm::vec2 p1{boxPx.x + boxPx.z * 0.4f, boxPx.y + boxPx.w - m * 1.5f};
			const glm::vec2 p2{boxPx.x + boxPx.z - m, boxPx.y + m * 1.5f};
			ui.DrawLine(PixelPoint(*t, p0, extent), PixelPoint(*t, p1, extent), 2.f, theme.text);
			ui.DrawLine(PixelPoint(*t, p1, extent), PixelPoint(*t, p2, extent), 2.f, theme.text);
		}

		// Inline label to the right of the box.
		if (!cb->label.empty())
		{
			const float labelX = boxPx.x + boxSize + 6.f;
			const float labelY = px.y + px.w * 0.5f + theme.bodyFontSize * 0.35f;
			ui.DrawText(cb->label, PixelPoint(*t, {labelX, labelY}, extent), theme.bodyFontSize, theme.text);
		}

		ui.SetLayer(prevLayer);
		return cb->checked;
	}

	// -- Progress bar ----------------------------------------------------------

	void DrawProgressBar(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto slider = world.TryGet<UiSliderComponent>(entity);
		if (!t || !slider)
		{
			return;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		const glm::vec4 px = PixelRect(*t, extent);
		const float range = std::max(slider->max - slider->min, 1e-6f);
		const float fillFraction = std::clamp((slider->value - slider->min) / range, 0.f, 1.f);
		const float fillW = px.z * fillFraction;

		ui.DrawRect(t->rect, theme.sliderTrack, theme.cornerRadius * 0.5f);
		if (fillW > 0.f)
		{
			ui.DrawRect(PixelToUiRect(*t, {px.x, px.y, fillW, px.w}, extent), theme.sliderFill, theme.cornerRadius * 0.5f);
		}
		ui.SetLayer(prevLayer);
	}

	// -- Panel -----------------------------------------------------------------

	bool DrawPanel(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		AE_PROFILE_ZONE();
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto panel = world.TryGet<UiPanelComponent>(entity);
		auto inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !panel)
		{
			return true;
		}

		// Toggle collapse on header click: save/restore full rect so the body
		// disappears when collapsed and reappears at the original size on expand.
		if (inp && inp->clicked && panel->collapsible)
		{
			const glm::vec4 px0 = PixelRect(*t, extent);
			const float headerH = theme.headerHeight + panel->headerExtensionHeight;
			const bool inHeader = (inp->clickPos.y >= px0.y && inp->clickPos.y < px0.y + headerH);
			if (inHeader)
			{
				if (!panel->collapsed)
				{
					panel->expandedRect = t->rect;
					t->rect = PixelToUiRect(*t, {px0.x, px0.y, px0.z, headerH}, extent);
				}
				else
				{
					// Restore size from the saved rect but keep the current offsetMinPx so
					// dragging while collapsed does not teleport the panel back on expand.
					const glm::vec2 size = panel->expandedRect.offsetMaxPx - panel->expandedRect.offsetMinPx;
					UiRect restored = panel->expandedRect;
					restored.offsetMinPx = t->rect.offsetMinPx;
					restored.offsetMaxPx = t->rect.offsetMinPx + size;
					t->rect = restored;
				}
				panel->collapsed = !panel->collapsed;
			}
		}

		const std::int32_t prevLayer = ui.GetLayer();
		const std::int32_t baseLayer = ComputeEffectiveLayer(world, entity);

		const glm::vec4 px = PixelRect(*t, extent);
		const float hdrH = theme.headerHeight + panel->headerExtensionHeight;
		const glm::vec2 sizePx{static_cast<float>(extent.width), static_cast<float>(extent.height)};
		const glm::vec2 anchorPx = t->rect.anchorMin * sizePx;

		// Panel background - only drawn when expanded.
		ui.SetLayer(baseLayer);
		if (!panel->collapsed)
		{
			ui.DrawRect(t->rect, theme.panelBg, theme.cornerRadius);
		}

		// Header background (always drawn; covers the full rect when collapsed).
		ui.SetLayer(baseLayer + 1);
		const glm::vec4 hdrPx = {px.x, px.y, px.z, hdrH};
		ui.DrawRect(PixelToUiRect(*t, hdrPx, extent), theme.panelHeaderBg, theme.cornerRadius);

		// Accent bar on left edge of header.
		const glm::vec4 accentPx = {px.x + 2.f, px.y + theme.headerHeight * 0.2f, 3.f, theme.headerHeight * 0.6f};
		ui.DrawRect(PixelToUiRect(*t, accentPx, extent), theme.accent);

		// Title text.
		if (!panel->title.empty())
		{
			ui.DrawText(panel->title,
			        UiPoint{
			                .anchor = t->rect.anchorMin,
			                .offsetPx = {px.x + theme.padding - anchorPx.x, px.y + theme.headerHeight * 0.5f + theme.titleFontSize * 0.35f - anchorPx.y},
			        },
			        theme.titleFontSize,
			        theme.textTitle);
		}

		// Collapse arrow indicator.
		if (panel->collapsible)
		{
			const float arrowX = px.x + px.z - theme.padding;
			const float arrowY = px.y + theme.headerHeight * 0.5f;
			if (panel->collapsed)
			{
				// Right-pointing triangle (collapsed).
				ui.DrawLine(PixelPoint(*t, {arrowX - 5.f, arrowY - 5.f}, extent), PixelPoint(*t, {arrowX, arrowY}, extent), 2.f, theme.textLabel);
				ui.DrawLine(PixelPoint(*t, {arrowX, arrowY}, extent), PixelPoint(*t, {arrowX - 5.f, arrowY + 5.f}, extent), 2.f, theme.textLabel);
			}
			else
			{
				// Down-pointing triangle (expanded).
				ui.DrawLine(PixelPoint(*t, {arrowX - 5.f, arrowY - 3.f}, extent), PixelPoint(*t, {arrowX, arrowY + 3.f}, extent), 2.f, theme.textLabel);
				ui.DrawLine(PixelPoint(*t, {arrowX, arrowY + 3.f}, extent), PixelPoint(*t, {arrowX + 5.f, arrowY - 3.f}, extent), 2.f, theme.textLabel);
			}
		}

		// Separator line below header - only when expanded.
		if (!panel->collapsed)
		{
			ui.SetLayer(baseLayer + 2);
			const float sepY = px.y + hdrH;
			ui.DrawLine(UiPoint{.anchor = t->rect.anchorMin, .offsetPx = {px.x + theme.padding - anchorPx.x, sepY - anchorPx.y}},
			        UiPoint{.anchor = t->rect.anchorMin, .offsetPx = {px.x + px.z - theme.padding - anchorPx.x, sepY - anchorPx.y}},
			        1.f,
			        theme.separator);
		}

		ui.SetLayer(prevLayer);
		return !panel->collapsed;
	}

	// -- Z-order management ----------------------------------------------------

	// Recursively adds `delta` to the z-order of `entity` and every descendant
	// reachable through UiChildrenComponent.
	static void RaiseSubtree(aether::World& world, Entity entity, float delta)
	{
		if (auto t = world.TryGet<UiTransformComponent>(entity))
		{
			t->zOrder += delta;
		}
		if (const auto ch = world.TryGet<UiChildrenComponent>(entity))
		{
			for (const Entity child: ch->children)
			{
				RaiseSubtree(world, child, delta);
			}
		}
	}

	void BringToFront(aether::World& world, Entity entity)
	{
		Entity root = entity;
		while (auto parent = world.TryGet<UiParentComponent>(root))
		{
			root = parent->parent;
		}

		float maxZ = 0.f;
		for (const auto& [e, t]: world.View<UiTransformComponent>().each())
		{
			const Entity ent = aether::World::FromEntt(e);
			if (!world.Has<UiParentComponent>(ent))
			{
				maxZ = std::max(maxZ, t.zOrder);
			}
		}

		if (maxZ > 1'000'000.f)
		{
			for (const auto& [e, t]: world.View<UiTransformComponent>().each())
			{
				t.zOrder *= 0.5f;
			}
			maxZ *= 0.5f;
		}

		auto t = world.TryGet<UiTransformComponent>(root);
		if (!t || t->zOrder > maxZ)
		{
			return;
		}

		const float delta = maxZ + 1.f - t->zOrder;
		t->zOrder = maxZ + 1.f;

		if (const auto ch = world.TryGet<UiChildrenComponent>(root))
		{
			for (const Entity child: ch->children)
			{
				RaiseSubtree(world, child, delta);
			}
		}
	}

	// -- Layout ---------------------------------------------------------------

	void ApplyLayout(aether::World& world, Entity container, gpu::Extent2D extent)
	{
		AE_PROFILE_ZONE();
		auto layout = world.TryGet<UiLayoutComponent>(container);
		auto children = world.TryGet<UiChildrenComponent>(container);
		auto parent = world.TryGet<UiTransformComponent>(container);
		if (!layout || !children || !parent)
		{
			return;
		}

		const glm::vec4 parentPx = PixelRect(*parent, extent);
		const float pad = layout->padding;
		const float spacing = layout->spacing;
		const bool isVertical = (layout->direction == UiLayoutComponent::Direction::Vertical);

		// -- Pass 1: measure fixed children, sum flex weights -----------------
		float fixedTotal = 0.f;
		float flexWeightTotal = 0.f;
		int childCount = 0;

		for (const Entity child: children->children)
		{
			const auto ct = world.TryGet<UiTransformComponent>(child);
			if (!ct)
			{
				continue;
			}

			if (ct->flexGrow > 0.f)
			{
				++childCount;
				flexWeightTotal += ct->flexGrow;
			}
			else
			{
				const glm::vec4 childPx = PixelRect(*ct, extent);
				const float mainSize = isVertical ? childPx.w : childPx.z;
				if (mainSize <= 0.f)
				{
					continue;
				}
				++childCount;
				fixedTotal += mainSize;
			}
		}

		// Space remaining after fixed children and inter-item gaps.
		const float totalGaps = childCount > 1 ? static_cast<float>(childCount - 1) * spacing : 0.f;
		const float available = isVertical ? (parentPx.w - 2.f * pad) : (parentPx.z - 2.f * pad);
		const float flexPool = std::max(0.f, available - fixedTotal - totalGaps);

		// -- Pass 2: position all children ------------------------------------
		const float crossSize = isVertical ? (parentPx.z - 2.f * pad) : (parentPx.w - 2.f * pad);
		float cursor = isVertical ? (parentPx.y + pad) : (parentPx.x + pad);

		int positionedChildren = 0;
		for (const Entity child: children->children)
		{
			auto ct = world.TryGet<UiTransformComponent>(child);
			if (!ct)
			{
				continue;
			}

			const glm::vec4 childPx = PixelRect(*ct, extent);
			const float mainSize = (ct->flexGrow > 0.f && flexWeightTotal > 0.f) ? flexPool * (ct->flexGrow / flexWeightTotal) : (isVertical ? childPx.w : childPx.z);
			if (ct->flexGrow <= 0.f && mainSize <= 0.f)
			{
				continue;
			}

			if (isVertical)
			{
				float childX = parentPx.x + pad;
				float childW = crossSize;
				if (layout->crossAlignment != UiLayoutComponent::Alignment::Stretch)
				{
					childW = childPx.z; // natural width
					if (layout->crossAlignment == UiLayoutComponent::Alignment::Center)
					{
						childX += (crossSize - childW) * 0.5f;
					}
					else if (layout->crossAlignment == UiLayoutComponent::Alignment::End)
					{
						childX += crossSize - childW;
					}
				}
				ct->rect = PixelToUiRect(*ct, {childX, cursor, childW, mainSize}, extent);
			}
			else
			{
				float childY = parentPx.y + pad;
				float childH = crossSize;
				if (layout->crossAlignment != UiLayoutComponent::Alignment::Stretch)
				{
					childH = childPx.w; // natural height
					if (layout->crossAlignment == UiLayoutComponent::Alignment::Center)
					{
						childY += (crossSize - childH) * 0.5f;
					}
					else if (layout->crossAlignment == UiLayoutComponent::Alignment::End)
					{
						childY += crossSize - childH;
					}
				}
				ct->rect = PixelToUiRect(*ct, {cursor, childY, mainSize, childH}, extent);
			}
			cursor += mainSize;
			++positionedChildren;
			if (positionedChildren < childCount)
			{
				cursor += spacing;
			}
		}

		// -- Auto-size: shrink/grow container to wrap content ------------------
		// cursor is now: start + pad + sum(sizes) + (N-1)*spacing.
		// Desired container size: 2*pad + sum(sizes) + (N-1)*spacing = cursor - start + pad.
		if (layout->autoSize && childCount > 0)
		{
			const float start = isVertical ? parentPx.y : parentPx.x;
			const float newMainSize = cursor - start + pad;
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

	void ApplyGridLayout(aether::World& world, Entity container, gpu::Extent2D extent)
	{
		auto grid = world.TryGet<UiGridLayoutComponent>(container);
		auto children = world.TryGet<UiChildrenComponent>(container);
		auto parent = world.TryGet<UiTransformComponent>(container);
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
			auto ct = world.TryGet<UiTransformComponent>(child);
			if (!ct)
			{
				++i;
				continue;
			}

			const int col = i % cols;
			const int row = i / cols;
			const float x = parentPx.x + pad + static_cast<float>(col) * (slot + sp);
			const float y = parentPx.y + pad + static_cast<float>(row) * (slot + sp);
			ct->rect = PixelToUiRect(*ct, {x, y, slot, slot}, extent);
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

	void RunLayouts(aether::World& world, gpu::Extent2D extent)
	{
		AE_PROFILE_ZONE();
		// Three-pass layout to handle nested containers correctly regardless of
		// EnTT view iteration order.
		//
		// Pass 1: child containers (auto-size to content height).
		for (const auto& [e, layout, children]: world.View<UiLayoutComponent, UiChildrenComponent>().each())
		{
			const Entity entity = aether::World::FromEntt(e);
			if (world.Has<UiParentComponent>(entity))
			{
				ApplyLayout(world, entity, extent);
			}
		}
		// Pass 2: root containers (position children now that their sizes are
		// known, then auto-size to fit).
		for (const auto& [e, layout, children]: world.View<UiLayoutComponent, UiChildrenComponent>().each())
		{
			const Entity entity = aether::World::FromEntt(e);
			if (!world.Has<UiParentComponent>(entity))
			{
				ApplyLayout(world, entity, extent);
			}
		}
		// Pass 3: child containers again (re-position grandchildren now that the
		// parent has moved the container to its final position).
		for (const auto& [e, layout, children]: world.View<UiLayoutComponent, UiChildrenComponent>().each())
		{
			const Entity entity = aether::World::FromEntt(e);
			if (world.Has<UiParentComponent>(entity))
			{
				ApplyLayout(world, entity, extent);
			}
		}
		for (const auto& [e, grid, children]: world.View<UiGridLayoutComponent, UiChildrenComponent>().each())
		{
			ApplyGridLayout(world, aether::World::FromEntt(e), extent);
		}
	}

	// -- Spawn helpers ---------------------------------------------------------

	Entity SpawnButton(aether::World& world, UiRect rect, std::string_view label, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiInputComponent>(e);
		world.Emplace<UiButtonComponent>(e, UiButtonComponent{.label = std::string(label)});
		return e;
	}

	Entity SpawnSlider(aether::World& world, UiRect rect, float min, float max, float value, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiInputComponent>(e);
		world.Emplace<UiSliderComponent>(e, UiSliderComponent{.min = min, .max = max, .value = value});
		return e;
	}

	Entity SpawnCheckbox(aether::World& world, UiRect rect, std::string_view label, bool checked, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiInputComponent>(e);
		world.Emplace<UiCheckboxComponent>(e, UiCheckboxComponent{.checked = checked, .label = std::string(label)});
		return e;
	}

	Entity SpawnProgressBar(aether::World& world, UiRect rect, float min, float max, float value, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiSliderComponent>(e, UiSliderComponent{.min = min, .max = max, .value = value});
		return e;
	}

	Entity SpawnPanel(aether::World& world, UiRect rect, std::string_view title, bool draggable, bool collapsible, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiInputComponent>(e);
		world.Emplace<UiPanelComponent>(e,
		        UiPanelComponent{
		                .title = std::string(title),
		                .draggable = draggable,
		                .collapsible = collapsible,
		        });
		return e;
	}

	// -- Text Input ------------------------------------------------------------

	bool DrawTextInput(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto ti = world.TryGet<UiTextInputComponent>(entity);
		auto inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !ti || !inp)
		{
			return false;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		const glm::vec4 px = PixelRect(*t, extent);

		// Background: blend normal->hover when not focused, snap to focus color when focused.
		const glm::vec4 bgColor = inp->focused ? theme.inputFocusBg : glm::mix(theme.inputBg, theme.inputHoverBg, inp->hoverT);
		ui.DrawRect(t->rect, bgColor, theme.cornerRadius);

		// Accent bar on left edge indicates keyboard focus.
		if (inp->focused)
		{
			const glm::vec4 accentPx = {px.x, px.y + 3.f, 2.f, px.w - 6.f};
			ui.DrawRect(PixelToUiRect(*t, accentPx, extent), theme.accent);
		}

		// Clip all text/cursor drawing to the input box.
		ui.PushClipRect(t->rect);

		const float textX = px.x + theme.padding * 0.5f;
		const float textY = px.y + px.w * 0.5f + theme.bodyFontSize * 0.35f;

		if (ti->text.empty() && !ti->placeholder.empty())
		{
			ui.DrawText(ti->placeholder, PixelPoint(*t, {textX, textY}, extent), theme.bodyFontSize, theme.placeholder);
		}
		else if (!ti->text.empty())
		{
			ui.DrawText(ti->text, PixelPoint(*t, {textX, textY}, extent), theme.bodyFontSize, theme.text);
		}

		// Blinking cursor - only when focused.
		if (inp->focused && ti->cursorVisible)
		{
			const std::string_view prefix(ti->text.data(), static_cast<std::size_t>(ti->cursorPos));
			const float cursorX = textX + ui.MeasureText(prefix, theme.bodyFontSize);
			ui.DrawLine(PixelPoint(*t, {cursorX, px.y + 3.f}, extent), PixelPoint(*t, {cursorX, px.y + px.w - 3.f}, extent), 1.5f, theme.accent);
		}

		ui.PopClipRect();

		// submitted is set by UiSystem::ProcessTextInput for one frame on Enter.
		// Cleared in UiSystem::EndFrame so layer code can read it during OnGui/OnUpdate.
		const bool wasSubmitted = ti->submitted;
		ui.SetLayer(prevLayer);
		return wasSubmitted;
	}

	Entity SpawnTextInput(aether::World& world, UiRect rect, std::string_view placeholder, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiInputComponent>(e);
		world.Emplace<UiTextInputComponent>(e, UiTextInputComponent{.placeholder = std::string(placeholder)});
		return e;
	}

	// -- Vec3 drag --------------------------------------------------------------

	static std::string FormatFloat(float value, int decimals)
	{
		char buf[64]{};
		const int clampedDecimals = std::clamp(decimals, 0, 6);
		std::snprintf(buf, sizeof(buf), "%.*f", clampedDecimals, value);
		return buf;
	}

	static int Vec3AxisAt(const UiTransformComponent& t, const UiVec3DragComponent& vec, glm::vec4 px, glm::vec2 mousePos)
	{
		if (vec.readOnly)
		{
			return -1;
		}

		const float labelW = std::min(92.f, px.z * 0.33f);
		const float gap = 4.f;
		const float fieldX = px.x + labelW + 6.f;
		const float fieldW = std::max(0.f, (px.z - labelW - 6.f - gap * 2.f) / 3.f);
		(void) t;

		for (int axis = 0; axis < 3; ++axis)
		{
			const float x = fieldX + static_cast<float>(axis) * (fieldW + gap);
			if (mousePos.x >= x && mousePos.x <= x + fieldW && mousePos.y >= px.y && mousePos.y <= px.y + px.w)
			{
				return axis;
			}
		}
		return -1;
	}

	bool DrawVec3Drag(aether::World& world, Entity entity, UIRenderer& ui, const Input& input, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto vec = world.TryGet<UiVec3DragComponent>(entity);
		auto inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !vec || !inp || !vec->visible)
		{
			return false;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		vec->changed = false;
		const glm::vec4 px = PixelRect(*t, extent);
		const float labelW = std::min(92.f, px.z * 0.33f);
		const float gap = 4.f;
		const float fieldX = px.x + labelW + 6.f;
		const float fieldW = std::max(0.f, (px.z - labelW - 6.f - gap * 2.f) / 3.f);
		const float textY = px.y + px.w * 0.5f + theme.bodyFontSize * 0.35f;
		const char* axisNames[] = {"X", "Y", "Z"};
		const glm::vec4 axisColors[] = {
		        {0.86f, 0.31f, 0.24f, 1.f},
		        {0.42f, 0.65f, 0.31f, 1.f},
		        {0.34f, 0.52f, 0.78f, 1.f},
		};

		ui.DrawText(vec->label, PixelPoint(*t, {px.x, textY}, extent), theme.labelFontSize, theme.textLabel);

		if (inp->pressed && !vec->readOnly)
		{
			const int axis = Vec3AxisAt(*t, *vec, px, input.GetMousePos());
			if (axis >= 0 && vec->activeAxis < 0)
			{
				vec->activeAxis = axis;
				vec->dragStartMouse = input.GetMousePos();
				vec->dragStartValue = vec->value;
				vec->dragging = false;
				vec->editingAxis = -1;
			}
		}

		if (vec->activeAxis >= 0 && input.IsMouseButtonDown(MouseButton::Left))
		{
			const glm::vec2 delta = input.GetMousePos() - vec->dragStartMouse;
			if (!vec->dragging && glm::dot(delta, delta) > 9.f)
			{
				vec->dragging = true;
			}
			if (vec->dragging)
			{
				vec->value[vec->activeAxis] = std::clamp(vec->dragStartValue[vec->activeAxis] + delta.x * vec->speed, vec->min[vec->activeAxis], vec->max[vec->activeAxis]);
				vec->changed = true;
			}
		}

		if (vec->activeAxis >= 0 && input.IsMouseButtonReleased(MouseButton::Left))
		{
			if (!vec->dragging && inp->hovered)
			{
				vec->editingAxis = vec->activeAxis;
				vec->editText = FormatFloat(vec->value[vec->editingAxis], vec->decimals);
			}
			vec->activeAxis = -1;
			vec->dragging = false;
		}

		for (int axis = 0; axis < 3; ++axis)
		{
			const float x = fieldX + static_cast<float>(axis) * (fieldW + gap);
			const glm::vec4 fieldPx{x, px.y, fieldW, px.w};
			const bool active = axis == vec->activeAxis || axis == vec->editingAxis;
			glm::vec4 bg = vec->readOnly ? theme.tabInactive : glm::mix(theme.inputBg, theme.inputHoverBg, inp->hoverT);
			if (active)
			{
				bg = theme.inputFocusBg;
			}
			ui.DrawRect(PixelToUiRect(*t, fieldPx, extent), bg, theme.cornerRadius * 0.5f);

			const glm::vec4 badgePx{x + 3.f, px.y + 3.f, 18.f, px.w - 6.f};
			ui.DrawRect(PixelToUiRect(*t, badgePx, extent), axisColors[axis], theme.cornerRadius * 0.5f);

			const float axisTextW = ui.MeasureText(axisNames[axis], theme.buttonFontSize);
			ui.DrawText(axisNames[axis], PixelPoint(*t, {badgePx.x + badgePx.z * 0.5f - axisTextW * 0.5f, textY}, extent), theme.buttonFontSize, theme.text);

			const std::string valueText = (axis == vec->editingAxis) ? vec->editText : FormatFloat(vec->value[axis], vec->decimals);
			const float valueMaxW = std::max(0.f, fieldW - 28.f);
			const std::string fitted = FitTextToWidth(ui, valueText, theme.bodyFontSize, valueMaxW);
			ui.DrawText(fitted, PixelPoint(*t, {x + 25.f, textY}, extent), theme.bodyFontSize, vec->readOnly ? theme.textLabel : theme.text);
		}

		ui.SetLayer(prevLayer);
		return vec->changed;
	}

	Entity SpawnVec3Drag(aether::World& world, UiRect rect, std::string_view label, glm::vec3 value, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiInputComponent>(e);
		world.Emplace<UiVec3DragComponent>(e, UiVec3DragComponent{.label = std::string(label), .value = value});
		return e;
	}

	// -- Hierarchy helper ------------------------------------------------------

	void AddChild(aether::World& world, Entity parent, Entity child)
	{
		auto children = world.TryGet<UiChildrenComponent>(parent);
		children->children.push_back(child);
		world.EmplaceOrReplace<UiParentComponent>(child, UiParentComponent{parent});

		if (auto parentTransform = world.TryGet<UiTransformComponent>(parent))
		{
			if (auto childTransform = world.TryGet<UiTransformComponent>(child))
			{
				const float parentZ = parentTransform->zOrder;
				const float offset = 0.01f * static_cast<float>(children->children.size());
				childTransform->zOrder = parentZ + offset;
			}
		}
	}

	// -- Item Slot -------------------------------------------------------------

	bool DrawItemSlot(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto slot = world.TryGet<UiItemSlotComponent>(entity);
		auto inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !slot || !inp)
		{
			return false;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

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
		const glm::vec4 innerPx = {px.x + kBorderW, px.y + kBorderW, px.z - 2.f * kBorderW, px.w - 2.f * kBorderW};
		ui.DrawRect(PixelToUiRect(*t, innerPx, extent), theme.slotBg, theme.slotCornerRadius - kBorderW);

		// Item contents when the slot is not empty.
		if (slot->quantity > 0)
		{
			const float iconInset = kBorderW + 2.f;
			const glm::vec4 iconPx = {px.x + iconInset, px.y + iconInset, px.z - 2.f * iconInset, px.w - 2.f * iconInset};
			const UiRect iconRect = PixelToUiRect(*t, iconPx, extent);

			if (slot->textureSlot > 0)
			{
				ui.DrawTexturedRect(iconRect, slot->textureSlot, {0.f, 0.f, 1.f, 1.f}, glm::vec4(1.f));
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

				const glm::vec2 sizePx{static_cast<float>(extent.width), static_cast<float>(extent.height)};
				const glm::vec2 anchorPx = t->rect.anchorMin * sizePx;

				// Drop shadow for readability.
				ui.DrawText(qty, UiPoint{.anchor = t->rect.anchorMin, .offsetPx = {tx - anchorPx.x + 1.f, ty - anchorPx.y + 1.f}}, theme.slotFontSize, {0.f, 0.f, 0.f, 0.75f});
				ui.DrawText(qty, UiPoint{.anchor = t->rect.anchorMin, .offsetPx = {tx - anchorPx.x, ty - anchorPx.y}}, theme.slotFontSize, theme.quantityText);
			}
		}

		// Hover overlay on top of everything.
		if (inp->hoverT > 0.f)
		{
			glm::vec4 overlay = theme.slotHoverOverlay;
			overlay.a *= inp->hoverT;
			ui.DrawRect(t->rect, overlay, theme.slotCornerRadius);
		}

		ui.SetLayer(prevLayer);
		return inp->clicked;
	}

	Entity SpawnItemSlot(aether::World& world, UiRect rect, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiInputComponent>(e);
		world.Emplace<UiItemSlotComponent>(e);
		return e;
	}

	Entity SpawnItemGrid(aether::World& world, UiRect containerRect, int columns, float slotSize, float spacing, float padding, int slotCount, float slotZOrder, Entity* slotsOut, float containerZOrder)
	{
		Entity container = world.Create();
		world.Emplace<UiTransformComponent>(container, UiTransformComponent{.rect = containerRect, .zOrder = containerZOrder});
		world.Emplace<UiGridLayoutComponent>(container,
		        UiGridLayoutComponent{
		                .columns = columns,
		                .slotSize = slotSize,
		                .spacing = spacing,
		                .padding = padding,
		        });
		world.Emplace<UiChildrenComponent>(container);

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

	// -- Label row --------------------------------------------------------------

	void DrawLabelRow(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto row = world.TryGet<UiLabelRowComponent>(entity);
		if (!t || !row)
		{
			return;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		const glm::vec4 px = PixelRect(*t, extent);
		const float y = px.y + px.w * 0.5f + theme.labelFontSize * 0.35f;
		const float valueColumnOffset = std::clamp(row->valueColumnOffsetPx, 0.f, px.z);
		const float valueGap = 8.f;
		const float autoLabelMax = row->value.empty() ? px.z : std::max(0.f, valueColumnOffset - valueGap);
		const float autoValueMax = std::max(0.f, px.z - valueColumnOffset);
		const float labelMax = row->labelMaxWidthPx > 0.f ? std::min(row->labelMaxWidthPx, px.z) : autoLabelMax;
		const float valueMax = row->valueMaxWidthPx > 0.f ? std::min(row->valueMaxWidthPx, autoValueMax) : autoValueMax;
		const std::string label = row->truncateLabel ? FitTextToWidth(ui, row->label, theme.labelFontSize, labelMax) : row->label;
		const std::string value = row->truncateValue ? FitTextToWidth(ui, row->value, theme.labelFontSize, valueMax) : row->value;

		ui.DrawText(label, PixelPoint(*t, {px.x, y}, extent), theme.labelFontSize, theme.textLabel);
		if (!value.empty())
		{
			ui.DrawText(value, PixelPoint(*t, {px.x + valueColumnOffset, y}, extent), theme.labelFontSize, row->valueColor);
		}

		ui.SetLayer(prevLayer);
	}

	Entity SpawnLabelRow(aether::World& world, UiRect rect, std::string_view label, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiLabelRowComponent>(e, UiLabelRowComponent{.label = std::string(label)});
		return e;
	}

	// -- Selectable row ---------------------------------------------------------

	bool DrawSelectable(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto row = world.TryGet<UiSelectableComponent>(entity);
		auto inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !row || !inp || !row->visible)
		{
			return false;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		const glm::vec4 px = PixelRect(*t, extent);
		const float bgAlpha = row->selected ? 0.30f : (0.12f * inp->hoverT);
		if (bgAlpha > 0.f)
		{
			glm::vec4 bg = row->selected ? theme.accent : theme.tabHover;
			bg.a = bgAlpha;
			ui.DrawRect(t->rect, bg, theme.cornerRadius * 0.5f);
		}

		if (row->selected)
		{
			ui.DrawRect(PixelToUiRect(*t, {px.x, px.y + 3.f, 2.f, px.w - 6.f}, extent), theme.accent, 0.f);
		}

		const float textX = px.x + 8.f;
		const float textY = px.y + px.w * 0.5f + theme.bodyFontSize * 0.35f;
		const float maxW = row->labelMaxWidthPx > 0.f ? std::min(row->labelMaxWidthPx, px.z - 12.f) : std::max(0.f, px.z - 16.f);
		const std::string label = FitTextToWidth(ui, row->label, theme.bodyFontSize, maxW);
		ui.DrawText(label, PixelPoint(*t, {textX, textY}, extent), theme.bodyFontSize, row->selected ? theme.text : theme.textLabel);

		ui.SetLayer(prevLayer);
		return inp->clicked;
	}

	Entity SpawnSelectable(aether::World& world, UiRect rect, std::string_view label, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiInputComponent>(e);
		world.Emplace<UiSelectableComponent>(e, UiSelectableComponent{.label = std::string(label)});
		return e;
	}

	// -- Tree node --------------------------------------------------------------

	bool DrawTreeNode(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto node = world.TryGet<UiTreeNodeComponent>(entity);
		auto inp = world.TryGet<UiInputComponent>(entity);
		if (!t || !node || !inp || !node->visible)
		{
			return false;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		const glm::vec4 px = PixelRect(*t, extent);
		const float bgAlpha = node->selected ? 0.32f : (0.14f * inp->hoverT);
		if (bgAlpha > 0.f)
		{
			glm::vec4 bg = node->selected ? theme.accent : theme.tabHover;
			bg.a = bgAlpha;
			ui.DrawRect(t->rect, bg, theme.cornerRadius * 0.5f);
		}

		if (node->selected)
		{
			ui.DrawRect(PixelToUiRect(*t, {px.x, px.y + 3.f, 2.f, px.w - 6.f}, extent), theme.accent, 0.f);
		}

		const float indent = static_cast<float>(node->depth) * node->indentPx;
		const float markerX = px.x + 8.f + indent;
		const float textX = markerX + 14.f;
		const float textY = px.y + px.w * 0.5f + theme.bodyFontSize * 0.35f;

		if (node->hasChildren)
		{
			ui.DrawText(node->expanded ? "v" : ">", PixelPoint(*t, {markerX, textY}, extent), theme.bodyFontSize, node->selected ? theme.text : theme.textSection);
		}

		const float autoMax = std::max(0.f, px.x + px.z - textX - 8.f);
		const float maxW = node->labelMaxWidthPx > 0.f ? std::min(node->labelMaxWidthPx, autoMax) : autoMax;
		const std::string label = FitTextToWidth(ui, node->label, theme.bodyFontSize, maxW);
		ui.DrawText(label, PixelPoint(*t, {textX, textY}, extent), theme.bodyFontSize, node->selected ? theme.text : theme.textLabel);

		ui.SetLayer(prevLayer);
		return inp->clicked;
	}

	Entity SpawnTreeNode(aether::World& world, UiRect rect, std::string_view label, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiInputComponent>(e);
		world.Emplace<UiTreeNodeComponent>(e, UiTreeNodeComponent{.label = std::string(label)});
		return e;
	}

	// -- Section separator ------------------------------------------------------

	void DrawSection(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto t = world.TryGet<UiTransformComponent>(entity);
		if (!t)
		{
			return;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		const glm::vec4 px = PixelRect(*t, extent);
		const float lineY = px.y + px.w * 0.5f;
		ui.DrawLine(PixelPoint(*t, {px.x, lineY}, extent), PixelPoint(*t, {px.x + px.z, lineY}, extent), 1.f, theme.separator);

		ui.SetLayer(prevLayer);
	}

	Entity SpawnSection(aether::World& world, UiRect rect, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiSectionComponent>(e);
		return e;
	}

	// -- Tab bar ----------------------------------------------------------------

	void DrawTabBar(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto tabComp = world.TryGet<UiTabComponent>(entity);
		auto transform = world.TryGet<UiTransformComponent>(entity);
		auto children = world.TryGet<UiChildrenComponent>(entity);
		if (!tabComp || !transform || !children)
		{
			return;
		}

		// State management (click detection, page show/hide) is handled by
		// UiSystem::ProcessTabBars which runs before RunLayouts. DrawTabBar
		// only draws the visual tab strip and buttons.

		const std::size_t tabCount = std::min(tabComp->tabNames.size(), children->children.size());

		// Draw tab strip.
		const glm::vec4 tabPx = PixelRect(*transform, extent);
		const std::int32_t baseLayer = ComputeEffectiveLayer(world, entity);
		const std::int32_t prevLayer = ui.GetLayer();

		ui.SetLayer(baseLayer);
		ui.DrawRect(transform->rect, theme.panelHeaderBg);

		// Draw each tab button.
		for (std::size_t i = 0; i < tabCount; ++i)
		{
			Entity btnEntity = children->children[i];
			auto btnT = world.TryGet<UiTransformComponent>(btnEntity);
			auto btnComp = world.TryGet<UiButtonComponent>(btnEntity);
			auto btnInp = world.TryGet<UiInputComponent>(btnEntity);
			if (!btnT)
			{
				continue;
			}

			const glm::vec4 btnPx = PixelRect(*btnT, extent);

			const bool selected = i == tabComp->selectedTab;
			const bool hovered = btnInp != nullptr && btnInp->hovered;
			const UiRect insetRect = PixelToUiRect(*btnT, {btnPx.x + 3.f, btnPx.y + 3.f, std::max(0.f, btnPx.z - 6.f), std::max(0.f, btnPx.w - 6.f)}, extent);

			if (selected || hovered)
			{
				const glm::vec4 bgColor = selected ? glm::vec4{0.09f, 0.12f, 0.12f, 0.92f} : theme.tabHover;
				ui.SetLayer(baseLayer + 1 + static_cast<std::int32_t>(i));
				ui.DrawRect(insetRect, bgColor, 3.f);
			}

			if (selected)
			{
				const float underlineY = btnPx.y + btnPx.w - 4.f;
				ui.SetLayer(baseLayer + 2 + static_cast<std::int32_t>(i));
				ui.DrawLine(PixelPoint(*btnT, {btnPx.x + 9.f, underlineY}, extent), PixelPoint(*btnT, {btnPx.x + btnPx.z - 9.f, underlineY}, extent), 2.f, theme.accent);
			}

			// Tab label.
			if (btnComp && !btnComp->label.empty())
			{
				const UiPoint centre = CentrePoint(*btnT, btnPx, extent);
				const float tabFontSize = 12.f;
				const std::string label = FitTextToWidth(ui, btnComp->label, tabFontSize, std::max(0.f, btnPx.z - 10.f));
				const float textW = ui.MeasureText(label, tabFontSize);
				const glm::vec4 textColor = (i == tabComp->selectedTab) ? theme.textTitle : theme.textLabel;
				ui.SetLayer(baseLayer + 3 + static_cast<std::int32_t>(i));
				ui.DrawText(label, UiPoint{.anchor = centre.anchor, .offsetPx = {centre.offsetPx.x - textW * 0.5f, centre.offsetPx.y + tabFontSize * 0.35f}}, tabFontSize, textColor);
			}
		}

		ui.SetLayer(prevLayer);
	}

	Entity SpawnTabPage(aether::World& world, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = UiRect{}, .zOrder = zOrder});
		world.Emplace<UiLayoutComponent>(e,
		        UiLayoutComponent{
		                .direction = UiLayoutComponent::Direction::Vertical,
		                .spacing = 0.f,
		                .padding = 0.f,
		                .autoSize = true,
		        });
		world.Emplace<UiChildrenComponent>(e);
		return e;
	}

	Entity SpawnTabBar(aether::World& world, UiRect rect, const std::vector<std::string>& tabNames, const std::vector<Entity>& tabPages, float zOrder)
	{
		Entity bar = world.Create();
		world.Emplace<UiTransformComponent>(bar, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiLayoutComponent>(bar,
		        UiLayoutComponent{
		                .direction = UiLayoutComponent::Direction::Horizontal,
		                .spacing = 2.f,
		                .padding = 4.f,
		        });
		world.Emplace<UiTabComponent>(bar,
		        UiTabComponent{
		                .tabNames = tabNames,
		                .selectedTab = 0,
		                .tabPages = tabPages,
		        });
		world.Emplace<UiChildrenComponent>(bar);

		for (std::size_t i = 0; i < tabNames.size(); ++i)
		{
			Entity btn = SpawnButton(world, UiRect{.offsetMaxPx = {64.f, 22.f}}, tabNames[i], zOrder + 0.01f * static_cast<float>(i + 1));
			if (auto transform = world.TryGet<UiTransformComponent>(btn))
			{
				transform->flexGrow = 1.0f;
			}
			AddChild(world, bar, btn);
		}

		return bar;
	}

	// -- Graph ------------------------------------------------------------------

	void DrawGraph(aether::World& world, Entity entity, UIRenderer& ui, gpu::Extent2D extent, const UiTheme& theme)
	{
		auto t = world.TryGet<UiTransformComponent>(entity);
		auto graph = world.TryGet<UiGraphComponent>(entity);
		if (!t || !graph)
		{
			return;
		}

		const std::int32_t prevLayer = ui.GetLayer();
		ui.SetLayer(ComputeEffectiveLayer(world, entity));

		const glm::vec4 px = PixelRect(*t, extent);
		const float chartX = px.x;
		constexpr float kTopPad = 6.f;
		const float chartY = px.y + kTopPad;
		const float chartW = px.z;
		constexpr float kChartH = 76.f;
		constexpr float kLegendArea = 26.f;
		const float chartH = std::min(px.w - kLegendArea, kChartH);
		const float range = std::max(graph->rangeMax - graph->rangeMin, 1e-6f);

		// Background (full entity rect).
		ui.DrawRect(t->rect, {0.12f, 0.12f, 0.15f, 1.f}, 3.f);

		// Bars in the upper chart region.
		if (graph->count > 0)
		{
			const float barW = chartW / static_cast<float>(UiGraphComponent::kMaxSamples);
			for (std::size_t i = 0; i < UiGraphComponent::kMaxSamples; ++i)
			{
				if (i >= graph->count)
				{
					continue;
				}
				const std::size_t idx = (graph->head + UiGraphComponent::kMaxSamples - graph->count + i) % UiGraphComponent::kMaxSamples;
				const float val = graph->samples[idx];
				const float barH = (std::clamp(val, graph->rangeMin, graph->rangeMax) / range) * chartH;
				const float bx = chartX + static_cast<float>(i) * barW;
				const float by = chartY + chartH - barH;

				glm::vec4 barColor = theme.accent;
				const float normalMs = (graph->rangeMax - graph->rangeMin) / 2.f;
				if (val > graph->rangeMin + normalMs * 1.5f)
				{
					barColor = theme.bad;
				}
				else if (val > graph->rangeMin + normalMs)
				{
					barColor = theme.warn;
				}

				ui.DrawRect(PixelToUiRect(*t, {bx, by, std::max(barW - 1.f, 1.f), barH}, extent), barColor);
			}
		}

		// Reference lines.
		auto drawRef = [&](float refValue, const glm::vec4& color)
		{
			if (refValue < graph->rangeMin || refValue > graph->rangeMax)
			{
				return;
			}
			const float refY = chartY + chartH - ((refValue - graph->rangeMin) / range) * chartH;
			ui.DrawLine(PixelPoint(*t, {chartX, refY}, extent), PixelPoint(*t, {chartX + chartW, refY}, extent), 1.f, color * glm::vec4{1.f, 1.f, 1.f, 0.4f});
		};
		drawRef(16.667f, theme.good);
		drawRef(33.333f, theme.warn);

		// Legend text in the lower area.
		if (!graph->label.empty())
		{
			const float legendY = px.y + px.w - 6.f;
			ui.DrawText(graph->label, PixelPoint(*t, {chartX, legendY}, extent), 11.f, theme.textLabel * glm::vec4{1.f, 1.f, 1.f, 0.8f});
		}

		ui.SetLayer(prevLayer);
	}

	Entity SpawnGraph(aether::World& world, UiRect rect, std::string_view label, float rangeMin, float rangeMax, float zOrder)
	{
		Entity e = world.Create();
		world.Emplace<UiTransformComponent>(e, UiTransformComponent{.rect = rect, .zOrder = zOrder});
		world.Emplace<UiGraphComponent>(e,
		        UiGraphComponent{
		                .rangeMin = rangeMin,
		                .rangeMax = rangeMax,
		                .label = std::string(label),
		        });
		return e;
	}

} // namespace aether::ui
