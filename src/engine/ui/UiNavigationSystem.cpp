#include "ui/UiNavigationSystem.hpp"

#include <cfloat>
#include <cmath>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "platform/Input.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

namespace aether::ui
{
	namespace
	{
		struct Candidate
		{
			Entity entity;
			glm::vec4 rect;
			bool interactable;
		};

		glm::vec2 Center(const glm::vec4& r)
		{
			return {r.x + r.z * 0.5f, r.y + r.w * 0.5f};
		}

		bool PointInRect(glm::vec2 p, const glm::vec4& r)
		{
			return p.x >= r.x && p.x <= r.x + r.z && p.y >= r.y && p.y <= r.y + r.w;
		}

		// Nearest interactable candidate whose centre lies in direction `dir` from `fromRect`.
		// Cost favours small distance along the axis and small perpendicular offset (alignment).
		Entity NearestInDirection(const std::vector<Candidate>& cands, Entity from, const glm::vec4& fromRect, glm::vec2 dir)
		{
			const glm::vec2 fromCenter = Center(fromRect);
			Entity best{};
			float bestCost = FLT_MAX;
			for (const Candidate& c: cands)
			{
				if (c.entity == from || !c.interactable)
				{
					continue;
				}
				const glm::vec2 d = Center(c.rect) - fromCenter;
				const float along = d.x * dir.x + d.y * dir.y;
				if (along <= 1.0f)
				{
					continue; // not in the pressed direction
				}
				const float perp = std::fabs(d.x * dir.y - d.y * dir.x);
				const float cost = along + perp * 2.0f;
				if (cost < bestCost)
				{
					bestCost = cost;
					best = c.entity;
				}
			}
			return best;
		}
	} // namespace

	void UiNavigationSystem::Update(World& world, Input& input)
	{
		std::vector<Candidate> cands;
		Entity prevFocused{};
		world.View<UISelectable, UIRect>().each(
		        [&](entt::entity ent, UISelectable& sel, UIRect& rect)
		        {
			        const Entity e = World::FromEntt(ent);
			        if (ecs::HasDisabledAncestor(world, e))
			        {
				        return; // only the active screen's selectables participate
			        }
			        cands.push_back({e, rect.resolvedRect, sel.interactable});
			        if (sel.focused)
			        {
				        prevFocused = e;
			        }
		        });

		if (cands.empty())
		{
			return;
		}

		Entity focused = prevFocused;
		if (!focused.IsValid())
		{
			for (const Candidate& c: cands)
			{
				if (c.interactable)
				{
					focused = c.entity;
					break;
				}
			}
		}

		// Mouse hover focuses on movement; a hovered element is the click target.
		const glm::vec2 mouse = input.GetMousePos();
		Entity hovered{};
		for (const Candidate& c: cands)
		{
			if (c.interactable && PointInRect(mouse, c.rect))
			{
				hovered = c.entity;
				break;
			}
		}
		const glm::vec2 mouseDelta = input.GetMouseDelta();
		if ((mouseDelta.x != 0.0f || mouseDelta.y != 0.0f) && hovered.IsValid())
		{
			focused = hovered;
		}

		// Keyboard: spatial move to the nearest selectable in the pressed direction.
		if (focused.IsValid())
		{
			glm::vec4 focusedRect{};
			for (const Candidate& c: cands)
			{
				if (c.entity == focused)
				{
					focusedRect = c.rect;
					break;
				}
			}
			glm::vec2 dir{0.0f, 0.0f};
			if (input.IsKeyPressed(Key::Down))
			{
				dir = {0.0f, 1.0f};
			}
			else if (input.IsKeyPressed(Key::Up))
			{
				dir = {0.0f, -1.0f};
			}
			else if (input.IsKeyPressed(Key::Right))
			{
				dir = {1.0f, 0.0f};
			}
			else if (input.IsKeyPressed(Key::Left))
			{
				dir = {-1.0f, 0.0f};
			}
			if (dir.x != 0.0f || dir.y != 0.0f)
			{
				const Entity target = NearestInDirection(cands, focused, focusedRect, dir);
				if (target.IsValid())
				{
					focused = target;
				}
			}
		}

		// Activation: Enter/Space on the focused element, or a left-click on the hovered one.
		Entity toActivate{};
		if ((input.IsKeyPressed(Key::Enter) || input.IsKeyPressed(Key::Space)) && focused.IsValid())
		{
			toActivate = focused;
		}
		if (input.IsMouseButtonPressed(MouseButton::Left) && hovered.IsValid())
		{
			toActivate = hovered;
			focused = hovered;
		}

		// Commit focus and activation to every selectable (inactive ones clear naturally).
		world.View<UISelectable>().each(
		        [&](entt::entity ent, UISelectable& sel)
		        {
			        const Entity e = World::FromEntt(ent);
			        sel.focused = (e == focused);
			        sel.activated = (e == toActivate);
		        });
	}
} // namespace aether::ui
