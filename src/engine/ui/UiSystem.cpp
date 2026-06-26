#include "UiSystem.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "platform/Input.hpp"
#include "UiComponents.hpp"
#include "UiContext.hpp"
#include "ui/UiLayout.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiTheme.hpp"
#include "scene/World.hpp"
#include "UiWidgets.hpp"
#include "utils/Profiler.hpp"

namespace aether::ui
{
	static constexpr float kTitleBarHeight = 48.f;
	static constexpr float kScrollStepPx = 36.f;

	static void ProcessTabBars(aether::World& world);

	static void MoveSubtree(aether::World& world, Entity entity, glm::vec2 delta)
	{
		if (auto t = world.TryGet<UiTransformComponent>(entity))
		{
			t->rect.offsetMinPx += delta;
			t->rect.offsetMaxPx += delta;
		}
		if (const auto ch = world.TryGet<UiChildrenComponent>(entity))
		{
			for (const Entity child: ch->children)
			{
				MoveSubtree(world, child, delta);
			}
		}
	}

	static UiRect PixelRectToUiRect(const glm::vec4& px)
	{
		return UiRect{
		        .anchorMin = {0.f, 0.f},
		        .anchorMax = {0.f, 0.f},
		        .offsetMinPx = {px.x, px.y},
		        .offsetMaxPx = {px.x + px.z, px.y + px.w},
		};
	}

	static bool IsPointInRect(glm::vec2 point, const glm::vec4& rect)
	{
		return point.x >= rect.x && point.x <= rect.x + rect.z && point.y >= rect.y && point.y <= rect.y + rect.w;
	}

	static bool IsSubtreeVisible(aether::World& world, Entity entity)
	{
		if (const auto layout = world.TryGet<UiLayoutComponent>(entity))
		{
			if (!layout->visible)
			{
				return false;
			}
		}
		if (const auto render = world.TryGet<UiRenderComponent>(entity))
		{
			if (!render->visible)
			{
				return false;
			}
		}
		if (const auto vec3 = world.TryGet<UiVec3DragComponent>(entity))
		{
			if (!vec3->visible)
			{
				return false;
			}
		}
		if (const auto selectable = world.TryGet<UiSelectableComponent>(entity))
		{
			if (!selectable->visible)
			{
				return false;
			}
		}
		if (const auto treeNode = world.TryGet<UiTreeNodeComponent>(entity))
		{
			if (!treeNode->visible)
			{
				return false;
			}
		}
		return true;
	}

	static float VisibleSubtreeBottom(aether::World& world, Entity entity, gpu::Extent2D extent)
	{
		if (!IsSubtreeVisible(world, entity))
		{
			return -std::numeric_limits<float>::infinity();
		}

		float bottom = -std::numeric_limits<float>::infinity();
		bool hasVisibleChild = false;
		if (const auto children = world.TryGet<UiChildrenComponent>(entity))
		{
			for (const Entity child: children->children)
			{
				const float childBottom = VisibleSubtreeBottom(world, child, extent);
				if (std::isfinite(childBottom))
				{
					hasVisibleChild = true;
					bottom = std::max(bottom, childBottom);
				}
			}
		}

		if (!hasVisibleChild)
		{
			if (const auto transform = world.TryGet<UiTransformComponent>(entity))
			{
				const glm::vec4 px = ResolveUiRectPx(extent, transform->rect);
				bottom = px.y + px.w;
			}
		}

		return bottom;
	}

	static void ResetAncestorPanelScroll(aether::World& world, Entity entity)
	{
		auto link = world.TryGet<UiParentComponent>(entity);
		while (link && link->parent.IsValid())
		{
			const Entity parent = link->parent;
			if (auto panel = world.TryGet<UiPanelComponent>(parent))
			{
				panel->bodyScrollY = 0.f;
				panel->bodyMaxScrollY = 0.f;
			}
			link = world.TryGet<UiParentComponent>(parent);
		}
	}

	static void MoveBodyScrollContent(aether::World& world, Entity entity, glm::vec2 delta)
	{
		if (world.Has<UiLayoutComponent>(entity))
		{
			if (const auto children = world.TryGet<UiChildrenComponent>(entity))
			{
				for (const Entity child: children->children)
				{
					MoveSubtree(world, child, delta);
				}
				return;
			}
		}
		MoveSubtree(world, entity, delta);
	}

	static void ApplyPanelBodyScroll(aether::World& world, Entity panelEntity, const Input* input, gpu::Extent2D extent)
	{
		auto panel = world.TryGet<UiPanelComponent>(panelEntity);
		auto panelTransform = world.TryGet<UiTransformComponent>(panelEntity);
		const auto children = world.TryGet<UiChildrenComponent>(panelEntity);
		if (!panel || !panelTransform || !children || panel->collapsed)
		{
			return;
		}

		const UiTheme& theme = UiTheme::Default();
		const glm::vec4 panelPx = ResolveUiRectPx(extent, panelTransform->rect);
		const float bodyTop = panelPx.y + theme.headerHeight + panel->headerExtensionHeight;
		const float bodyBottom = panelPx.y + panelPx.w - theme.padding;
		const float bodyHeight = std::max(0.f, bodyBottom - bodyTop);

		float contentBottom = bodyTop;
		for (const Entity child: children->children)
		{
			if (!IsSubtreeVisible(world, child))
			{
				continue;
			}
			const float childBottom = VisibleSubtreeBottom(world, child, extent);
			if (!std::isfinite(childBottom))
			{
				continue;
			}
			const auto childTransform = world.TryGet<UiTransformComponent>(child);
			if (!childTransform)
			{
				contentBottom = std::max(contentBottom, childBottom);
				continue;
			}
			const glm::vec4 childPx = ResolveUiRectPx(extent, childTransform->rect);
			if (childPx.y + 0.5f < bodyTop)
			{
				continue;
			}
			contentBottom = std::max(contentBottom, childBottom);
		}

		panel->bodyMaxScrollY = std::max(0.f, contentBottom - bodyBottom);
		if (input != nullptr && panel->bodyMaxScrollY > 0.f && IsPointInRect(input->GetMousePos(), {panelPx.x, bodyTop, panelPx.z, bodyHeight}))
		{
			const float wheelY = input->GetScrollDelta().y;
			if (wheelY != 0.f)
			{
				panel->bodyScrollY = std::clamp(panel->bodyScrollY - wheelY * kScrollStepPx, 0.f, panel->bodyMaxScrollY);
			}
		}
		panel->bodyScrollY = std::clamp(panel->bodyScrollY, 0.f, panel->bodyMaxScrollY);

		if (panel->bodyScrollY <= 0.f)
		{
			return;
		}

		const glm::vec2 scrollDelta{0.f, -panel->bodyScrollY};
		for (const Entity child: children->children)
		{
			if (!IsSubtreeVisible(world, child))
			{
				continue;
			}
			const auto childTransform = world.TryGet<UiTransformComponent>(child);
			if (!childTransform)
			{
				continue;
			}
			const glm::vec4 childPx = ResolveUiRectPx(extent, childTransform->rect);
			if (childPx.y + 0.5f >= bodyTop)
			{
				MoveBodyScrollContent(world, child, scrollDelta);
			}
		}
	}

	// Runs before RunLayouts. Checks tab button clicks, updates selection,
	// and toggles UiLayoutComponent::autoSize + visible per page.
	static void ProcessTabBars(aether::World& world)
	{
		for (const auto& [e, tabComp, children]: world.View<UiTabComponent, UiChildrenComponent>().each())
		{
			const Entity tabEntity = aether::World::FromEntt(e);
			const std::size_t tabCount = std::min(tabComp.tabNames.size(), children.children.size());
			std::size_t newSelection = tabComp.selectedTab;
			for (std::size_t i = 0; i < tabCount; ++i)
			{
				if (const auto inp = world.TryGet<UiInputComponent>(children.children[i]))
				{
					if (inp->clicked)
					{
						newSelection = i;
					}
				}
			}
			if (tabCount > 0)
			{
				newSelection = std::min(newSelection, tabCount - 1);
			}
			tabComp.selectedTab = newSelection;

			if (tabComp.appliedSelectedTab != tabComp.selectedTab)
			{
				ResetAncestorPanelScroll(world, tabEntity);
				tabComp.appliedSelectedTab = tabComp.selectedTab;
			}

			const std::size_t pageCount = std::min(tabComp.tabPages.size(), tabComp.tabNames.size());
			for (std::size_t i = 0; i < pageCount; ++i)
			{
				auto layout = world.TryGet<UiLayoutComponent>(tabComp.tabPages[i]);
				auto pt = world.TryGet<UiTransformComponent>(tabComp.tabPages[i]);

				if (i == tabComp.selectedTab)
				{
					if (layout)
					{
						if (!layout->autoSize) // was hidden, just became visible
						{
							if (pt)
							{
								if (auto parentLink = world.TryGet<UiParentComponent>(tabComp.tabPages[i]))
								{
									if (auto parentTransform = world.TryGet<UiTransformComponent>(parentLink->parent))
									{
										pt->rect = parentTransform->rect;
									}
								}
							}
						}
						layout->autoSize = true;
						layout->visible = true;
					}
				}
				else
				{
					if (layout)
					{
						layout->autoSize = false;
						layout->visible = false;
					}
					if (pt)
					{
						pt->rect.offsetMaxPx.y = pt->rect.offsetMinPx.y; // zero height
					}
				}
			}
		}
	}

	static void LayoutAndScroll(aether::World& world, const Input* input, gpu::Extent2D extent)
	{
		ProcessTabBars(world);
		RunLayouts(world, extent);

		for (const auto& [e, panel, transform]: world.View<UiPanelComponent, UiTransformComponent>().each())
		{
			ApplyPanelBodyScroll(world, aether::World::FromEntt(e), input, extent);
		}
	}

	void UiSystem::BeginFrame(aether::World& world, Input& input, UiContext& ctx, gpu::Extent2D extent, float deltaTime)
	{
		AE_PROFILE_ZONE();
		// -- Mouse state --------------------------------------------------------
		ctx.mousePos = input.GetMousePos();
		ctx.mouseDelta = input.GetMouseDelta();
		ctx.mousePressed = input.IsMouseButtonPressed(MouseButton::Left);
		ctx.mouseDown = input.IsMouseButtonDown(MouseButton::Left);
		ctx.mouseReleased = input.IsMouseButtonReleased(MouseButton::Left);

		// -- Feed keyboard events into focused text input -----------------------
		// Must run before hit-test so Enter/Escape can clear focusedEntity before
		// the new hot entity is resolved.
		ProcessTextInput(world, ctx, input, deltaTime);

		// -- Drag --------------------------------------------------------------
		// UpdateDrag runs BEFORE we clear hotEntity so it can see the previous
		// frame's hot entity for drag-start detection.  It must also run before
		// HitTest so the dragged rect is at its new position when hit-tested.
		UpdateDrag(world, ctx, extent);

		LayoutAndScroll(world, &input, extent);

		// -- Clear per-frame transient flags -----------------------------------
		// Cleared AFTER UpdateDrag (which needs last frame's hotEntity) and
		// BEFORE HitTest (which will repopulate hotEntity for this frame).
		ctx.hotEntity = {};

		for (const auto& [e, inp]: world.View<UiInputComponent>().each())
		{
			inp.clicked = false;
			inp.hovered = false;
			inp.pressed = false;
			// Refresh focused from the persistent context entity so widgets always see
			// the current keyboard focus state without an extra loop later.
			inp.focused = (aether::World::FromEntt(e) == ctx.focusedEntity);
		}

		HitTest(world, ctx, extent);
		FlushWidgetStates(world, ctx);
		UpdateTransitions(world, deltaTime);

		// Capture mouse while any UI element is actively pressed/dragged.
		// Camera (and other systems) can check Input::IsMouseCaptured() to skip
		// their own mouse processing and avoid conflicting with UI interactions.
		input.SetMouseCaptured(ctx.activeEntity.IsValid());
	}

	// -- Private helpers used by BeginFrame and RenderAll ---------------------

	// Returns true if any ancestor panel of `entity` is collapsed, meaning the
	// entity is in the hidden body of that panel and should not receive input.
	static bool InsideCollapsedPanel(const aether::World& world, Entity entity)
	{
		auto link = world.TryGet<UiParentComponent>(entity);
		while (link && link->parent.IsValid())
		{
			const auto panel = world.TryGet<UiPanelComponent>(link->parent);
			if (panel && panel->collapsed)
			{
				return true;
			}
			link = world.TryGet<UiParentComponent>(link->parent);
		}
		return false;
	}

	static bool IsVisibleThroughAncestorPanels(aether::World& world, Entity entity, glm::vec2 mousePos, gpu::Extent2D extent)
	{
		Entity child = entity;
		auto link = world.TryGet<UiParentComponent>(child);
		while (link && link->parent.IsValid())
		{
			const Entity parent = link->parent;
			if (const auto panel = world.TryGet<UiPanelComponent>(parent))
			{
				const auto panelTransform = world.TryGet<UiTransformComponent>(parent);
				if (!panelTransform)
				{
					return false;
				}

				const UiTheme& theme = UiTheme::Default();
				const glm::vec4 panelPx = ResolveUiRectPx(extent, panelTransform->rect);
				if (!IsPointInRect(mousePos, panelPx))
				{
					return false;
				}

				if (!world.Has<UiTabComponent>(child))
				{
					const float bodyTop = panelPx.y + theme.headerHeight + panel->headerExtensionHeight;
					const float bodyBottom = panelPx.y + panelPx.w - theme.padding;
					if (mousePos.y < bodyTop || mousePos.y > bodyBottom)
					{
						return false;
					}
				}
			}

			child = parent;
			link = world.TryGet<UiParentComponent>(child);
		}
		return true;
	}

	namespace
	{
		// Dispatch drawing of a non-panel widget by its component type.
		void DrawWidget(aether::World& world, Entity entity, UIRenderer& ui, const Input& input, gpu::Extent2D extent, const UiTheme& theme)
		{
			if (world.Has<UiPanelComponent>(entity))
			{
				return;
			}
			if (InsideCollapsedPanel(world, entity))
			{
				return;
			}

			if (world.Has<UiButtonComponent>(entity) && world.Has<UiInputComponent>(entity))
			{
				DrawButton(world, entity, ui, extent, theme);
			}
			else if (world.Has<UiCheckboxComponent>(entity))
			{
				DrawCheckbox(world, entity, ui, extent, theme);
			}
			else if (world.Has<UiSliderComponent>(entity))
			{
				if (world.Has<UiInputComponent>(entity))
				{
					DrawSlider(world, entity, ui, input, extent, theme);
				}
				else
				{
					DrawProgressBar(world, entity, ui, extent, theme);
				}
			}
			else if (world.Has<UiTextInputComponent>(entity) && world.Has<UiInputComponent>(entity))
			{
				DrawTextInput(world, entity, ui, extent, theme);
			}
			else if (world.Has<UiVec3DragComponent>(entity) && world.Has<UiInputComponent>(entity))
			{
				DrawVec3Drag(world, entity, ui, input, extent, theme);
			}
			else if (world.Has<UiItemSlotComponent>(entity))
			{
				DrawItemSlot(world, entity, ui, extent, theme);
			}
			else if (world.Has<UiLabelRowComponent>(entity))
			{
				DrawLabelRow(world, entity, ui, extent, theme);
			}
			else if (world.Has<UiTreeNodeComponent>(entity) && world.Has<UiInputComponent>(entity))
			{
				DrawTreeNode(world, entity, ui, extent, theme);
			}
			else if (world.Has<UiSelectableComponent>(entity) && world.Has<UiInputComponent>(entity))
			{
				DrawSelectable(world, entity, ui, extent, theme);
			}
			else if (world.Has<UiSectionComponent>(entity))
			{
				DrawSection(world, entity, ui, extent, theme);
			}
			else if (world.Has<UiGraphComponent>(entity))
			{
				DrawGraph(world, entity, ui, extent, theme);
			}
		}

		// Recursively draw all children of a parent entity.
		void DrawChildren(aether::World& world, Entity parent, UIRenderer& ui, const Input& input, gpu::Extent2D extent, const UiTheme& theme)
		{
			const auto children = world.TryGet<UiChildrenComponent>(parent);
			if (!children)
			{
				return;
			}
			const auto parentTransform = world.TryGet<UiTransformComponent>(parent);
			const auto parentPanel = world.TryGet<UiPanelComponent>(parent);
			const bool clipToPanel = parentPanel != nullptr && parentTransform != nullptr;
			const glm::vec4 panelPx = parentTransform != nullptr ? ResolveUiRectPx(extent, parentTransform->rect) : glm::vec4{};
			const float bodyTop = (parentPanel != nullptr) ? panelPx.y + theme.headerHeight + parentPanel->headerExtensionHeight : 0.f;
			const float bodyBottom = (parentPanel != nullptr) ? panelPx.y + panelPx.w - theme.padding : 0.f;
			const UiRect panelClip = (parentTransform != nullptr) ? parentTransform->rect : UiRect{};
			const UiRect bodyClip = PixelRectToUiRect({panelPx.x, bodyTop, panelPx.z, std::max(0.f, bodyBottom - bodyTop)});
			for (Entity child: children->children)
			{
				bool pushedClip = false;
				if (clipToPanel)
				{
					const auto childTransform = world.TryGet<UiTransformComponent>(child);
					const glm::vec4 childPx = childTransform != nullptr ? ResolveUiRectPx(extent, childTransform->rect) : glm::vec4{};
					const bool bodyChild = !world.Has<UiTabComponent>(child) && (world.Has<UiChildrenComponent>(child) || (childTransform != nullptr && childPx.y + 0.5f >= bodyTop));
					ui.PushClipRect(bodyChild ? bodyClip : panelClip);
					pushedClip = true;
				}

				if (world.Has<UiPanelComponent>(child))
				{
					const bool bodyVisible = DrawPanel(world, child, ui, extent, theme);
					if (bodyVisible)
					{
						DrawChildren(world, child, ui, input, extent, theme);
					}
				}
				else if (world.Has<UiTabComponent>(child))
				{
					DrawTabBar(world, child, ui, extent, theme);
				}
				else if (world.Has<UiChildrenComponent>(child))
				{
					if (auto layout = world.TryGet<UiLayoutComponent>(child))
					{
						if (!layout->visible)
						{
							if (pushedClip)
							{
								ui.PopClipRect();
							}
							continue;
						}
					}
					DrawChildren(world, child, ui, input, extent, theme);
				}
				else
				{
					DrawWidget(world, child, ui, input, extent, theme);
				}
				if (pushedClip)
				{
					ui.PopClipRect();
				}
			}
		}
	} // namespace

	void UiSystem::RenderAll(aether::World& world, UIRenderer& ui, const Input& input, gpu::Extent2D extent)
	{
		const UiTheme& theme = UiTheme::Default();

		// Refresh layout after layers have updated row visibility/text in OnGui().
		LayoutAndScroll(world, nullptr, extent);

		// 2. Draw root-level panels (no parent) and their children.
		for (const auto& [e, panel, transform]: world.View<UiPanelComponent, UiTransformComponent>().each())
		{
			const Entity entity = aether::World::FromEntt(e);
			if (world.Has<UiParentComponent>(entity))
			{
				continue;
			}
			const bool bodyVisible = DrawPanel(world, entity, ui, extent, theme);
			if (bodyVisible)
			{
				DrawChildren(world, entity, ui, input, extent, theme);
			}
		}

		// 3. Draw root-level standalone widgets (no parent, no panel).
		for (const auto& [e, transform]: world.View<UiTransformComponent>().each())
		{
			const Entity entity = aether::World::FromEntt(e);
			if (world.Has<UiPanelComponent>(entity) || world.Has<UiParentComponent>(entity))
			{
				continue;
			}
			DrawWidget(world, entity, ui, input, extent, theme);
		}
	}

	void UiSystem::EndFrame(aether::World& world)
	{
		// Clear per-frame submitted flag on all text inputs (was previously done
		// inside DrawTextInput but is now deferred so layers can read it during
		// OnUpdate / OnGui before the flag is consumed).
		for (const auto& [e, ti]: world.View<UiTextInputComponent>().each())
		{
			ti.submitted = false;
		}
	}

	void UiSystem::HitTest(aether::World& world, UiContext& ctx, gpu::Extent2D extent)
	{
		const glm::vec2 mp = ctx.mousePos;
		Entity bestEntity;
		std::int32_t bestLayer = std::numeric_limits<std::int32_t>::min();

		for (const auto& [e, transform, inp]: world.View<UiTransformComponent, UiInputComponent>().each())
		{
			if (!inp.blockInput)
			{
				continue;
			}

			// Skip widgets that live inside a collapsed panel's body.
			if (InsideCollapsedPanel(world, aether::World::FromEntt(e)))
			{
				continue;
			}

			const glm::vec4 r = ResolveUiRectPx(extent, transform.rect);
			if (mp.x >= r.x && mp.x <= r.x + r.z && mp.y >= r.y && mp.y <= r.y + r.w)
			{
				if (!IsVisibleThroughAncestorPanels(world, aether::World::FromEntt(e), mp, extent))
				{
					continue;
				}
				const std::int32_t layer = ComputeEffectiveLayer(world, aether::World::FromEntt(e));
				if (layer > bestLayer)
				{
					bestLayer = layer;
					bestEntity = aether::World::FromEntt(e);
				}
			}
		}

		ctx.hotEntity = bestEntity;
	}

	void UiSystem::UpdateDrag(aether::World& world, UiContext& ctx, gpu::Extent2D extent)
	{
		// Start drag: left button just pressed over a draggable panel's title bar.
		if (ctx.mousePressed && ctx.hotEntity.IsValid())
		{
			auto panel = world.TryGet<UiPanelComponent>(ctx.hotEntity);
			auto transform = world.TryGet<UiTransformComponent>(ctx.hotEntity);

			if (panel && panel->draggable && transform)
			{
				const glm::vec4 r = ResolveUiRectPx(extent, transform->rect);
				const bool inTitleBar = (ctx.mousePos.y >= r.y && ctx.mousePos.y <= r.y + kTitleBarHeight);

				if (inTitleBar)
				{
					ctx.isDragging = true;
					ctx.draggedEntity = ctx.hotEntity;
					ctx.dragStartMousePos = ctx.mousePos;
					ctx.dragStartRectMin = transform->rect.offsetMinPx;
					ctx.dragStartRectMax = transform->rect.offsetMaxPx;
					BringToFront(world, ctx.draggedEntity);
				}
			}
		}

		// Continue drag: apply mouse delta to the panel and all descendants.
		if (ctx.isDragging && ctx.mouseDown && ctx.draggedEntity.IsValid())
		{
			if (auto transform = world.TryGet<UiTransformComponent>(ctx.draggedEntity))
			{
				const glm::vec2 delta = ctx.mousePos - ctx.dragStartMousePos;
				const glm::vec2 newMin = ctx.dragStartRectMin + delta;
				const glm::vec2 newMax = ctx.dragStartRectMax + delta;
				const glm::vec2 frameDelta = newMin - transform->rect.offsetMinPx;
				transform->rect.offsetMinPx = newMin;
				transform->rect.offsetMaxPx = newMax;

				if (const auto ch = world.TryGet<UiChildrenComponent>(ctx.draggedEntity))
				{
					for (const Entity child: ch->children)
					{
						MoveSubtree(world, child, frameDelta);
					}
				}
			}
		}

		// End drag.  Only suppress the click if the mouse actually moved;
		// a press-and-release in place is a genuine click (e.g. collapse toggle).
		if (ctx.mouseReleased)
		{
			if (ctx.isDragging)
			{
				const glm::vec2 d = ctx.mousePos - ctx.dragStartMousePos;
				if (d.x * d.x + d.y * d.y > 4.f) // moved more than 2 px
				{
					ctx.activeEntity = {};
				}
			}
			ctx.isDragging = false;
			ctx.draggedEntity = {};
		}
	}

	void UiSystem::FlushWidgetStates(aether::World& world, UiContext& ctx)
	{
		if (!ctx.hotEntity.IsValid())
		{
			// Mouse pressed on empty area: cancel active and lose keyboard focus.
			if (ctx.mousePressed)
			{
				ctx.focusedEntity = {};
			}
			if (ctx.mouseReleased)
			{
				ctx.activeEntity = {};
			}
			return;
		}

		auto inp = world.TryGet<UiInputComponent>(ctx.hotEntity);
		if (inp == nullptr)
		{
			return;
		}

		inp->hovered = true;

		if (ctx.mousePressed)
		{
			ctx.activeEntity = ctx.hotEntity;
			inp->pressed = true;

			// Raise clicked entity above all others so panels stack correctly.
			BringToFront(world, ctx.hotEntity);

			// Focus management: text editors gain keyboard focus on click; anything
			// else clicked removes focus so the field stops consuming events.
			if (world.Has<UiTextInputComponent>(ctx.hotEntity) || world.Has<UiVec3DragComponent>(ctx.hotEntity))
			{
				ctx.focusedEntity = ctx.hotEntity;
				// Reset blink so cursor is immediately visible on focus.
				if (auto ti = world.TryGet<UiTextInputComponent>(ctx.hotEntity))
				{
					ti->cursorBlinkTime = 0.f;
					ti->cursorVisible = true;
				}
			}
			else
			{
				ctx.focusedEntity = {};
			}
		}

		if (ctx.mouseDown && ctx.activeEntity == ctx.hotEntity)
		{
			inp->pressed = true;
		}

		if (ctx.mouseReleased && ctx.activeEntity == ctx.hotEntity)
		{
			inp->clicked = true;
			inp->clickPos = ctx.mousePos; // position at the moment of release
			ctx.activeEntity = {};
		}
	}

	void UiSystem::ProcessTextInput(aether::World& world, UiContext& ctx, const Input& input, float deltaTime)
	{
		if (!ctx.focusedEntity.IsValid())
		{
			return;
		}

		auto ti = world.TryGet<UiTextInputComponent>(ctx.focusedEntity);
		auto vec3 = world.TryGet<UiVec3DragComponent>(ctx.focusedEntity);
		if (!ti && !vec3)
		{
			return;
		}

		if (vec3)
		{
			if (vec3->editingAxis < 0)
			{
				return;
			}

			if (input.IsKeyPressed(Key::Enter) || input.IsKeyPressed(Key::KpEnter))
			{
				try
				{
					const float parsed = std::stof(vec3->editText);
					vec3->value[vec3->editingAxis] = std::clamp(parsed, vec3->min[vec3->editingAxis], vec3->max[vec3->editingAxis]);
					vec3->changed = true;
				}
				catch (...)
				{
				}
				vec3->editingAxis = -1;
				ctx.focusedEntity = {};
				return;
			}

			if (input.IsKeyPressed(Key::Escape))
			{
				vec3->editingAxis = -1;
				ctx.focusedEntity = {};
				return;
			}

			for (const char c: input.GetTypedChars())
			{
				const bool numeric = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
				if (numeric && vec3->editText.size() < 32)
				{
					vec3->editText.push_back(c);
				}
			}

			if (input.IsKeyPressed(Key::Backspace) && !vec3->editText.empty())
			{
				vec3->editText.pop_back();
			}

			return;
		}

		// Enter / KpEnter: submit and unfocus.
		const bool enterPressed = input.IsKeyPressed(Key::Enter) || input.IsKeyPressed(Key::KpEnter);
		if (enterPressed)
		{
			ti->submitted = true;
			ctx.focusedEntity = {};
			return;
		}

		// Escape: unfocus without submitting.
		if (input.IsKeyPressed(Key::Escape))
		{
			ctx.focusedEntity = {};
			return;
		}

		// Printable characters from the GLFW char callback.
		for (const char c: input.GetTypedChars())
		{
			if (ti->maxLength > 0 && std::cmp_greater_equal(ti->text.size(), ti->maxLength))
			{
				break;
			}
			ti->text.insert(static_cast<std::string::size_type>(ti->cursorPos), 1u, c);
			++ti->cursorPos;
		}

		if (input.IsKeyPressed(Key::Backspace) && ti->cursorPos > 0)
		{
			ti->text.erase(static_cast<std::string::size_type>(ti->cursorPos - 1), 1u);
			--ti->cursorPos;
		}

		if (input.IsKeyPressed(Key::Delete) && std::cmp_less(ti->cursorPos, ti->text.size()))
		{
			ti->text.erase(static_cast<std::string::size_type>(ti->cursorPos), 1u);
		}

		if (input.IsKeyPressed(Key::Left) && ti->cursorPos > 0)
		{
			--ti->cursorPos;
		}
		if (input.IsKeyPressed(Key::Right) && std::cmp_less(ti->cursorPos, ti->text.size()))
		{
			++ti->cursorPos;
		}
		if (input.IsKeyPressed(Key::Home))
		{
			ti->cursorPos = 0;
		}
		if (input.IsKeyPressed(Key::End))
		{
			ti->cursorPos = static_cast<int>(ti->text.size());
		}

		ti->cursorBlinkTime += deltaTime;
		if (ti->cursorBlinkTime >= 0.5f)
		{
			ti->cursorBlinkTime -= 0.5f;
			ti->cursorVisible = !ti->cursorVisible;
		}
	}

	void UiSystem::UpdateTransitions(aether::World& world, float deltaTime)
	{
		// 80 ms target: rate = 1 / 0.08 = 12.5 per second.
		static constexpr float kTransRate = 12.5f;
		const float step = kTransRate * deltaTime;

		for (const auto& [e, inp]: world.View<UiInputComponent>().each())
		{
			inp.hoverT = inp.hovered ? std::min(1.f, inp.hoverT + step) : std::max(0.f, inp.hoverT - step);
			inp.pressT = inp.pressed ? std::min(1.f, inp.pressT + step) : std::max(0.f, inp.pressT - step);
		}
	}

} // namespace aether::ui
