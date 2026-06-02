#include "UiSystem.hpp"

#include <limits>

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

	static void MoveSubtree(aether::World& world, Entity entity, glm::vec2 delta)
	{
		if (auto* t = world.TryGet<UiTransformComponent>(entity))
		{
			t->rect.offsetMinPx += delta;
			t->rect.offsetMaxPx += delta;
		}
		if (const auto* ch = world.TryGet<UiChildrenComponent>(entity))
		{
			for (const Entity child: ch->children)
			{
				MoveSubtree(world, child, delta);
			}
		}
	}

	void UiSystem::BeginFrame(aether::World& world, Input& input, UiContext& ctx, VkExtent2D extent, float deltaTime)
	{
		AE_PROFILE_ZONE();
		// ── Mouse state ────────────────────────────────────────────────────────
		ctx.mousePos = input.GetMousePos();
		ctx.mouseDelta = input.GetMouseDelta();
		ctx.mousePressed = input.IsMouseButtonPressed(MouseButton::Left);
		ctx.mouseDown = input.IsMouseButtonDown(MouseButton::Left);
		ctx.mouseReleased = input.IsMouseButtonReleased(MouseButton::Left);

		// ── Feed keyboard events into focused text input ───────────────────────
		// Must run before hit-test so Enter/Escape can clear focusedEntity before
		// the new hot entity is resolved.
		ProcessTextInput(world, ctx, input, deltaTime);

		// ── Drag ──────────────────────────────────────────────────────────────
		// UpdateDrag runs BEFORE we clear hotEntity so it can see the previous
		// frame's hot entity for drag-start detection.  It must also run before
		// HitTest so the dragged rect is at its new position when hit-tested.
		UpdateDrag(world, ctx, extent);

		// ── Clear per-frame transient flags ───────────────────────────────────
		// Cleared AFTER UpdateDrag (which needs last frame's hotEntity) and
		// BEFORE HitTest (which will repopulate hotEntity for this frame).
		ctx.hotEntity = {};

		for (auto [e, inp]: world.View<UiInputComponent>().each())
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

	// ── Private helpers used by BeginFrame and RenderAll ─────────────────────

	// Returns true if any ancestor panel of `entity` is collapsed, meaning the
	// entity is in the hidden body of that panel and should not receive input.
	static bool InsideCollapsedPanel(const aether::World& world, Entity entity)
	{
		const UiParentComponent* link = world.TryGet<UiParentComponent>(entity);
		while (link && link->parent.IsValid())
		{
			const auto* panel = world.TryGet<UiPanelComponent>(link->parent);
			if (panel && panel->collapsed)
			{
				return true;
			}
			link = world.TryGet<UiParentComponent>(link->parent);
		}
		return false;
	}

	namespace
	{
		// Dispatch drawing of a non-panel widget by its component type.
		void DrawWidget(aether::World& world, Entity entity, UIRenderer& ui, const Input& input, VkExtent2D extent, const UiTheme& theme)
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
			else if (world.Has<UiItemSlotComponent>(entity))
			{
				DrawItemSlot(world, entity, ui, extent, theme);
			}
			else if (world.Has<UiLabelRowComponent>(entity))
			{
				DrawLabelRow(world, entity, ui, extent, theme);
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
		void DrawChildren(aether::World& world, Entity parent, UIRenderer& ui, const Input& input, VkExtent2D extent, const UiTheme& theme)
		{
			const auto* children = world.TryGet<UiChildrenComponent>(parent);
			if (!children)
			{
				return;
			}
			for (Entity child: children->children)
			{
				if (world.Has<UiPanelComponent>(child))
				{
					const bool bodyVisible = DrawPanel(world, child, ui, extent, theme);
					if (bodyVisible)
					{
						DrawChildren(world, child, ui, input, extent, theme);
					}
				}
				else
				{
					DrawWidget(world, child, ui, input, extent, theme);
				}
			}
		}
	} // namespace

	void UiSystem::RenderAll(aether::World& world, UIRenderer& ui, const Input& input, VkExtent2D extent)
	{
		const UiTheme& theme = UiTheme::Default();

		// 1. Position all children of layout containers.
		RunLayouts(world, extent);

		// 2. Draw root-level panels (no parent) and their children.
		for (auto [e, panel, transform]: world.View<UiPanelComponent, UiTransformComponent>().each())
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
		for (auto [e, transform]: world.View<UiTransformComponent>().each())
		{
			const Entity entity = aether::World::FromEntt(e);
			if (world.Has<UiPanelComponent>(entity) || world.Has<UiParentComponent>(entity))
			{
				continue;
			}
			DrawWidget(world, entity, ui, input, extent, theme);
		}
	}

	void UiSystem::EndFrame(aether::World& world, UiContext& ctx)
	{
		(void) ctx;

		// Clear per-frame submitted flag on all text inputs (was previously done
		// inside DrawTextInput but is now deferred so layers can read it during
		// OnUpdate / OnGui before the flag is consumed).
		for (auto [e, ti]: world.View<UiTextInputComponent>().each())
		{
			ti.submitted = false;
		}
	}

	void UiSystem::HitTest(aether::World& world, UiContext& ctx, VkExtent2D extent)
	{
		const glm::vec2 mp = ctx.mousePos;
		Entity bestEntity;
		std::int32_t bestLayer = std::numeric_limits<std::int32_t>::min();

		for (auto [e, transform, inp]: world.View<UiTransformComponent, UiInputComponent>().each())
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

	void UiSystem::UpdateDrag(aether::World& world, UiContext& ctx, VkExtent2D extent)
	{
		// Start drag: left button just pressed over a draggable panel's title bar.
		if (ctx.mousePressed && ctx.hotEntity.IsValid())
		{
			auto* panel = world.TryGet<UiPanelComponent>(ctx.hotEntity);
			auto* transform = world.TryGet<UiTransformComponent>(ctx.hotEntity);

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
			if (auto* transform = world.TryGet<UiTransformComponent>(ctx.draggedEntity))
			{
				const glm::vec2 delta = ctx.mousePos - ctx.dragStartMousePos;
				const glm::vec2 newMin = ctx.dragStartRectMin + delta;
				const glm::vec2 newMax = ctx.dragStartRectMax + delta;
				const glm::vec2 frameDelta = newMin - transform->rect.offsetMinPx;
				transform->rect.offsetMinPx = newMin;
				transform->rect.offsetMaxPx = newMax;

				if (const auto* ch = world.TryGet<UiChildrenComponent>(ctx.draggedEntity))
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

		auto* inp = world.TryGet<UiInputComponent>(ctx.hotEntity);
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

			// Focus management: text inputs gain keyboard focus on click; anything
			// else clicked removes focus so the text field stops consuming events.
			if (world.Has<UiTextInputComponent>(ctx.hotEntity))
			{
				ctx.focusedEntity = ctx.hotEntity;
				// Reset blink so cursor is immediately visible on focus.
				if (auto* ti = world.TryGet<UiTextInputComponent>(ctx.hotEntity))
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

		auto* ti = world.TryGet<UiTextInputComponent>(ctx.focusedEntity);
		if (!ti)
		{
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
			if (ti->maxLength > 0 && static_cast<int>(ti->text.size()) >= ti->maxLength)
			{
				break;
			}
			ti->text.insert(static_cast<std::string::size_type>(ti->cursorPos), 1u, c);
			++ti->cursorPos;
		}

		// Backspace: delete character before cursor.
		if (input.IsKeyPressed(Key::Backspace) && ti->cursorPos > 0)
		{
			ti->text.erase(static_cast<std::string::size_type>(ti->cursorPos - 1), 1u);
			--ti->cursorPos;
		}

		// Delete: delete character at cursor.
		if (input.IsKeyPressed(Key::Delete) && ti->cursorPos < static_cast<int>(ti->text.size()))
		{
			ti->text.erase(static_cast<std::string::size_type>(ti->cursorPos), 1u);
		}

		// Arrow keys.
		if (input.IsKeyPressed(Key::Left) && ti->cursorPos > 0)
		{
			--ti->cursorPos;
		}
		if (input.IsKeyPressed(Key::Right) && ti->cursorPos < static_cast<int>(ti->text.size()))
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

		// Cursor blink.
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

		for (auto [e, inp]: world.View<UiInputComponent>().each())
		{
			inp.hoverT = inp.hovered ? std::min(1.f, inp.hoverT + step) : std::max(0.f, inp.hoverT - step);
			inp.pressT = inp.pressed ? std::min(1.f, inp.pressT + step) : std::max(0.f, inp.pressT - step);
		}
	}

} // namespace aether::ui
