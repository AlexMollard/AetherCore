#include "UiSystem.hpp"

#include "Input.hpp"
#include "UiComponents.hpp"
#include "UiContext.hpp"
#include "UiLayout.hpp"
#include "UiWorld.hpp"
#include "UiWidgets.hpp"

namespace aether::ui
{
	static constexpr float kTitleBarHeight = 48.f;

	void UiSystem::BeginFrame(UiWorld& world, const Input& input, UiContext& ctx, VkExtent2D extent)
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
		}

		// ── Drag must be updated before hit-test so the rect is current ───────
		UpdateDrag(world, ctx, extent);
		HitTest(world, ctx, extent);
		FlushWidgetStates(world, ctx);
	}

	void UiSystem::EndFrame(UiWorld& world, UiContext& ctx)
	{
		(void) world;
		(void) ctx;
	}

	// ── Private ───────────────────────────────────────────────────────────────

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
			// Button release outside the original target - cancel active.
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
		}

		if (ctx.mouseDown && ctx.activeEntity == ctx.hotEntity)
		{
			inp->pressed = true;
		}

		if (ctx.mouseReleased && ctx.activeEntity == ctx.hotEntity)
		{
			inp->clicked = true;
			ctx.activeEntity = {};
		}
	}

} // namespace aether::ui
