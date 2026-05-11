#include "UiSystem.hpp"

#include "platform/Input.hpp"
#include "UiComponents.hpp"
#include "UiContext.hpp"
#include "ui/UiLayout.hpp"
#include "UiWorld.hpp"
#include "UiWidgets.hpp"

namespace aether::ui
{
	static constexpr float kTitleBarHeight = 48.f;

	void UiSystem::BeginFrame(UiWorld& world, const Input& input, UiContext& ctx, VkExtent2D extent, float deltaTime)
	{
		// ── Mouse state ────────────────────────────────────────────────────────
		ctx.mousePos = input.GetMousePos();
		ctx.mouseDelta = input.GetMouseDelta();
		ctx.mousePressed = input.IsMouseButtonPressed(MouseButton::Left);
		ctx.mouseDown = input.IsMouseButtonDown(MouseButton::Left);
		ctx.mouseReleased = input.IsMouseButtonReleased(MouseButton::Left);

		// ── Clear per-frame transient flags ───────────────────────────────────
		ctx.hotEntity = {};

		for (auto [e, inp]: world.View<UiInputComponent>().each())
		{
			inp.clicked = false;
			inp.hovered = false;
			inp.pressed = false;
			// Refresh focused from the persistent context entity so widgets always see
			// the current keyboard focus state without an extra loop later.
			inp.focused = (UiWorld::FromEntt(e) == ctx.focusedEntity);
		}

		// ── Feed keyboard events into focused text input ───────────────────────
		// Must run before hit-test so Enter/Escape can clear focusedEntity before
		// the new hot entity is resolved.
		ProcessTextInput(world, ctx, input, deltaTime);

		// ── Drag must be updated before hit-test so the rect is current ───────
		UpdateDrag(world, ctx, extent);
		HitTest(world, ctx, extent);
		FlushWidgetStates(world, ctx);
		UpdateTransitions(world, deltaTime);
	}

	void UiSystem::EndFrame(UiWorld& world, UiContext& ctx)
	{
		(void) world;
		(void) ctx;
	}

	// ── Private ───────────────────────────────────────────────────────────────

	// Returns true if any ancestor panel of `entity` is collapsed, meaning the
	// entity is in the hidden body of that panel and should not receive input.
	static bool InsideCollapsedPanel(const UiWorld& world, Entity entity)
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

	void UiSystem::HitTest(UiWorld& world, UiContext& ctx, VkExtent2D extent)
	{
		const glm::vec2 mp = ctx.mousePos;
		Entity bestEntity;
		float bestZ = -1e9f;

		for (auto [e, transform, inp]: world.View<UiTransformComponent, UiInputComponent>().each())
		{
			if (!inp.blockInput)
			{
				continue;
			}

			// Skip widgets that live inside a collapsed panel's body.
			if (InsideCollapsedPanel(world, UiWorld::FromEntt(e)))
			{
				continue;
			}

			const glm::vec4 r = ResolveUiRectPx(extent, transform.rect);
			if (mp.x >= r.x && mp.x <= r.x + r.z && mp.y >= r.y && mp.y <= r.y + r.w)
			{
				if (transform.zOrder > bestZ)
				{
					bestZ = transform.zOrder;
					bestEntity = UiWorld::FromEntt(e);
				}
			}
		}

		ctx.hotEntity = bestEntity;
	}

	void UiSystem::UpdateDrag(UiWorld& world, UiContext& ctx, VkExtent2D extent)
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
				}
			}
		}

		// Continue drag: apply mouse delta to the panel's pixel offsets.
		if (ctx.isDragging && ctx.mouseDown && ctx.draggedEntity.IsValid())
		{
			if (auto* transform = world.TryGet<UiTransformComponent>(ctx.draggedEntity))
			{
				const glm::vec2 delta = ctx.mousePos - ctx.dragStartMousePos;
				transform->rect.offsetMinPx = ctx.dragStartRectMin + delta;
				transform->rect.offsetMaxPx = ctx.dragStartRectMax + delta;
			}
		}

		// End drag.
		if (ctx.mouseReleased)
		{
			ctx.isDragging = false;
			ctx.draggedEntity = {};
		}
	}

	void UiSystem::FlushWidgetStates(UiWorld& world, UiContext& ctx)
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

	void UiSystem::ProcessTextInput(UiWorld& world, UiContext& ctx, const Input& input, float deltaTime)
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

	void UiSystem::UpdateTransitions(UiWorld& world, float deltaTime)
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
