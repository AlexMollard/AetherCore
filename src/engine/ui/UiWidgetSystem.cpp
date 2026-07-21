#include "ui/UiWidgetSystem.hpp"

#include <algorithm>
#include <cmath>

#include <entt/entt.hpp>

#include "platform/Input.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

namespace aether::ui
{
	float SliderNormalized(float value, float minValue, float maxValue)
	{
		const float range = maxValue - minValue;
		if (range <= 1e-6f)
		{
			return 0.f;
		}
		return std::clamp((value - minValue) / range, 0.f, 1.f);
	}

	float SliderQuantize(float value, float minValue, float maxValue, float step)
	{
		float v = std::clamp(value, minValue, maxValue);
		if (step > 1e-6f)
		{
			v = minValue + std::round((v - minValue) / step) * step;
			v = std::clamp(v, minValue, maxValue);
		}
		return v;
	}

	float SliderValueFromMouseX(float mouseX, const glm::vec4& trackRect, float minValue, float maxValue, float step)
	{
		const float pad = 2.f;
		const float innerX = trackRect.x + pad;
		const float innerW = std::max(trackRect.z - 2.f * pad, 1e-6f);
		const float t = std::clamp((mouseX - innerX) / innerW, 0.f, 1.f);
		return SliderQuantize(minValue + t * (maxValue - minValue), minValue, maxValue, step);
	}

	namespace
	{
		bool Focused(World& world, Entity e)
		{
			const auto* s = world.TryGet<UISelectable>(e);
			return s != nullptr && s->focused;
		}

		bool Activated(World& world, Entity e)
		{
			const auto* s = world.TryGet<UISelectable>(e);
			return s != nullptr && s->activated;
		}

		float FocusPulse(float time)
		{
			return 0.7f + 0.3f * std::sin(time * 4.5f);
		}
	} // namespace

	void UiWidgetSystem::Update(World& world, Input& input, float time)
	{
		const glm::vec2 mouse = input.GetMousePos();
		const bool mouseDown = input.IsMouseButtonDown(MouseButton::Left);
		const bool right = input.IsKeyPressed(Key::Right);
		const bool left = input.IsKeyPressed(Key::Left);

		world.View<UISlider, UIRect>().each(
		        [&](entt::entity ent, UISlider& s, UIRect& rect)
		        {
			        const Entity e = World::FromEntt(ent);
			        s.changed = false;
			        if (ecs::HasDisabledAncestor(world, e))
			        {
				        s.dragging = false;
				        return;
			        }
			        const bool focused = Focused(world, e);
			        s.pulse = focused ? FocusPulse(time) : 1.f;

			        if (focused && (right || left))
			        {
				        const float delta = (s.step > 1e-6f ? s.step : (s.maxValue - s.minValue) * 0.02f) * (right ? 1.f : -1.f);
				        const float nv = SliderQuantize(s.value + delta, s.minValue, s.maxValue, s.step);
				        if (nv != s.value)
				        {
					        s.value = nv;
					        s.changed = true;
				        }
			        }

			        const glm::vec4 r = rect.resolvedRect;
			        const bool hover = mouse.x >= r.x && mouse.x <= r.x + r.z && mouse.y >= r.y && mouse.y <= r.y + r.w;
			        if (mouseDown && (s.dragging || hover))
			        {
				        s.dragging = true;
				        const float nv = SliderValueFromMouseX(mouse.x, r, s.minValue, s.maxValue, s.step);
				        if (nv != s.value)
				        {
					        s.value = nv;
					        s.changed = true;
				        }
			        }
			        if (!mouseDown)
			        {
				        s.dragging = false;
			        }
		        });

		world.View<UIToggle, UIRect>().each(
		        [&](entt::entity ent, UIToggle& tg, UIRect&)
		        {
			        const Entity e = World::FromEntt(ent);
			        tg.changed = false;
			        if (ecs::HasDisabledAncestor(world, e))
			        {
				        return;
			        }
			        tg.pulse = Focused(world, e) ? FocusPulse(time) : 1.f;
			        if (Activated(world, e))
			        {
				        tg.on = !tg.on;
				        tg.changed = true;
			        }
		        });
	}
} // namespace aether::ui
