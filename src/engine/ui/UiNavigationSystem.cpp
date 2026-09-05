#include "ui/UiNavigationSystem.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string_view>
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
			std::string_view group;
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
		//
		// `fromGroup` scopes the search: a selectable carrying a group only reaches others in
		// the same one, which is how two clusters of controls on one screen keep their own
		// arrow-key navigation. An empty group considers everything, so a screen that never
		// sets one behaves exactly as before. Tab is deliberately left unscoped - it cycles the
		// whole screen in reading order, and that is the way out of a group.
		Entity NearestInDirection(const std::vector<Candidate>& cands, Entity from, const glm::vec4& fromRect, glm::vec2 dir, std::string_view fromGroup)
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
				if (!fromGroup.empty() && c.group != fromGroup)
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

		// Reading order: top-to-bottom, then left-to-right. Rows are QUANTIZED rather than
		// compared with a pairwise tolerance: "within 8px counts as the same row" is not a
		// strict weak ordering (y = 0, 8, 16 makes a cycle), and std::sort on such a
		// comparator is undefined behaviour. Bucketing is transitive, so the sort is safe;
		// the cost is that two controls in one bucket but a few px apart in y order by x.
		bool BeforeInReadingOrder(const glm::vec4& a, const glm::vec4& b)
		{
			constexpr float kRowHeight = 16.f;
			const int rowA = static_cast<int>(std::floor(a.y / kRowHeight));
			const int rowB = static_cast<int>(std::floor(b.y / kRowHeight));
			if (rowA != rowB)
			{
				return rowA < rowB;
			}
			return a.x < b.x;
		}

		// The element the keyboard enters a screen on: first in reading order, so it is
		// also the first element Tab would reach.
		std::string_view GroupOf(const std::vector<Candidate>& cands, Entity e)
		{
			for (const Candidate& c: cands)
			{
				if (c.entity == e)
				{
					return c.group;
				}
			}
			return {};
		}

		Entity FirstInReadingOrder(const std::vector<Candidate>& cands)
		{
			const Candidate* best = nullptr;
			for (const Candidate& c: cands)
			{
				if (!c.interactable)
				{
					continue;
				}
				if (best == nullptr || BeforeInReadingOrder(c.rect, best->rect))
				{
					best = &c;
				}
			}
			return best != nullptr ? best->entity : Entity{};
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
			        cands.push_back({e, rect.resolvedRect, sel.interactable, sel.group});
			        if (sel.focused)
			        {
				        prevFocused = e;
			        }
		        });

		if (cands.empty())
		{
			// Nothing navigable this frame - but the flags written on the LAST frame that had
			// candidates are still sitting on the components, and `activated` is a one-frame
			// event, so leaving it set makes it fire again whenever the screen comes back.
			//
			// This bites precisely the screens that close themselves: a Resume button hides
			// the pause menu as it is activated, which empties the candidate list on the very
			// next frame - so the clear below never ran, and re-opening the menu read a stale
			// activation and resumed instantly, forever.
			//
			// `focused` is deliberately left alone. It is persistent state by design, and a
			// screen that is briefly hidden should come back with the player's place intact.
			world.View<UISelectable>().each([](entt::entity, UISelectable& sel) { sel.activated = false; });
			return;
		}

		Entity focused = prevFocused;
		// Focus always names a usable element, or nothing at all. The nav system never
		// INVENTS one, and that second half is load-bearing:
		//
		//  - Focus that has become unusable is dropped. The element may have been removed,
		//    or a script may have turned it non-interactable this frame (e.g. a Level
		//    Select node that locked once the active save slot resolved - the nodes
		//    default to interactable in the scene, so the nav system can briefly focus one
		//    before the script disables it). Left stuck there, keyboard Enter targets
		//    something that can't be activated and the reachable elements never can be.
		//  - Nothing focused STAYS nothing focused. Defaulting to the first interactable
		//    would arm Enter/Space against an element the player has never touched, which
		//    silently takes those keys away from the GAME for as long as any canvas exists:
		//    a HUD chat box turns the jump button into "open the chat", and every key after
		//    it into typing. Focus is established deliberately - by the mouse, by Tab or a
		//    direction key below, or by a screen calling Ui.SetFocus - never by default.
		bool focusedUsable = false;
		for (const Candidate& c: cands)
		{
			if (c.entity == focused && c.interactable)
			{
				focusedUsable = true;
				break;
			}
		}
		if (!focusedUsable)
		{
			focused = Entity{};
		}

		// The cursor position feeds exactly one thing below: which element a CLICK would
		// land on (`hovered`). Merely resting the mouse over a selectable must never move
		// keyboard focus by itself - Ui.HasFocus documents "the engine never focuses
		// anything on its own", and a screen with one selectable sitting mid-screen (a
		// single text box, say) would otherwise have its keyboard permanently and
		// silently stolen the instant the cursor drifted across it, with no click, no key
		// press, and nothing on screen to explain it. A script that wants to STYLE the
		// element under the cursor - independent of keyboard focus entirely - already has
		// Ui.IsHovered for exactly that; hover here exists only to resolve a click.
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
		// An element that has claimed the keyboard (a text field being edited) keeps the keys the
		// nav system would otherwise spend: arrows must move its caret, Enter must submit to it.
		// Tab is deliberately exempt - it is the guaranteed keyboard exit from a captured field.
		const bool captured = focused.IsValid() && world.Has<UIKeyboardCapture>(focused);

		// Keyboard: spatial move to the nearest selectable in the pressed direction.
		if (!captured)
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
			// A focused horizontal slider captures Left/Right for value adjustment
			// (UiWidgetSystem handles them); vertical nav still works.
			const bool focusedIsSlider = focused.IsValid() && world.TryGet<UISlider>(focused) != nullptr;
			// Gamepad reads as a third source of the same four directions, so every screen
			// that already navigates by arrow keys navigates by controller with no change to
			// it. The d-pad gives real press edges; the stick is latched by Input into the
			// same shape, so a held stick steps one element rather than one per frame.
			const auto padDir = [&input](GamepadButton button, GamepadDirection dir)
			{
				return input.IsGamepadButtonPressed(button) || input.IsGamepadStickFlicked(GamepadStick::Left, dir);
			};
			const bool wantDown = input.IsKeyPressed(Key::Down) || padDir(GamepadButton::DpadDown, GamepadDirection::Down);
			const bool wantUp = input.IsKeyPressed(Key::Up) || padDir(GamepadButton::DpadUp, GamepadDirection::Up);
			const bool wantRight = input.IsKeyPressed(Key::Right) || padDir(GamepadButton::DpadRight, GamepadDirection::Right);
			const bool wantLeft = input.IsKeyPressed(Key::Left) || padDir(GamepadButton::DpadLeft, GamepadDirection::Left);

			glm::vec2 dir{0.0f, 0.0f};
			if (wantDown)
			{
				dir = {0.0f, 1.0f};
			}
			else if (wantUp)
			{
				dir = {0.0f, -1.0f};
			}
			else if (wantRight && !focusedIsSlider)
			{
				dir = {1.0f, 0.0f};
			}
			else if (wantLeft && !focusedIsSlider)
			{
				dir = {-1.0f, 0.0f};
			}
			if (dir.x != 0.0f || dir.y != 0.0f)
			{
				if (!focused.IsValid())
				{
					// A direction key with nothing focused is how the keyboard ENTERS a
					// screen: it lights the first element up rather than moving from one.
					// Seeding here instead of by default is what leaves Enter/Space with
					// the game until the player actually reaches for the UI.
					focused = FirstInReadingOrder(cands);
				}
				else
				{
					const std::string_view focusedGroup = GroupOf(cands, focused);
					if (const Entity target = NearestInDirection(cands, focused, focusedRect, dir, focusedGroup); target.IsValid())
					{
						focused = target;
					}
				}
			}
		}

		// Tab cycles focus in reading order - what a connect form needs (address, port, connect).
		if (input.IsKeyPressed(Key::Tab) && !cands.empty())
		{
			const bool backwards = input.IsKeyDown(Key::LeftShift) || input.IsKeyDown(Key::RightShift);
			std::vector<Candidate> ordered;
			for (const Candidate& c: cands)
			{
				if (c.interactable)
				{
					ordered.push_back(c);
				}
			}
			std::sort(ordered.begin(), ordered.end(),
			        [](const Candidate& a, const Candidate& b) { return BeforeInReadingOrder(a.rect, b.rect); });
			if (!ordered.empty())
			{
				if (!focused.IsValid())
				{
					// Same "the keyboard enters the screen" rule as the direction keys: the
					// first Tab lands on an end of the order rather than stepping off an
					// element nobody chose.
					focused = backwards ? ordered.back().entity : ordered.front().entity;
				}
				else
				{
					// `focused` is one of these: it survived the usability check above, and
					// `ordered` holds exactly the interactable candidates.
					std::size_t index = 0;
					for (std::size_t i = 0; i < ordered.size(); ++i)
					{
						if (ordered[i].entity == focused)
						{
							index = i;
							break;
						}
					}
					const std::size_t count = ordered.size();
					index = backwards ? (index + count - 1) % count : (index + 1) % count;
					focused = ordered[index].entity;
				}
			}
		}

		// Activation: Enter/Space on the focused element, or a left-click on the hovered one.
		Entity toActivate{};
		if (!captured && (input.IsKeyPressed(Key::Enter) || input.IsKeyPressed(Key::Space)) && focused.IsValid())
		{
			toActivate = focused;
			// The key pressed a button, and that is all it did. Without this, the Space
			// that chose "Resume" is also read by the character controller a moment later
			// and the player jumps as the menu closes; the Enter that chose a menu item is
			// also read by a chat box as "open the chat". Consumed here, before any script
			// runs, so no script has to know a menu exists.
			input.ConsumeKey(Key::Enter);
			input.ConsumeKey(Key::Space);
		}
		// The controller has exactly the same problem, and worse odds of avoiding it: A is
		// both the universal menu-confirm and the universal jump, so without consuming it the
		// A that resumes a game is guaranteed to also make the character jump on the frame
		// the menu closes.
		if (!captured && input.IsGamepadButtonPressed(GamepadButton::A) && focused.IsValid())
		{
			toActivate = focused;
			input.ConsumeGamepadButton(GamepadButton::A);
		}
		if (input.IsMouseButtonPressed(MouseButton::Left))
		{
			if (hovered.IsValid())
			{
				toActivate = hovered;
				focused = hovered;
			}
			else
			{
				// The other half of "clicking away is the documented way out" (see the
				// hover remarks above): a click on a DIFFERENT selectable already moves
				// focus away from whatever had it, but a click on nothing selectable at
				// all - the game world around the UI, empty canvas space - left focus
				// untouched before this. A field clicked into deliberately, then clicked
				// away from into the game rather than onto another widget, kept the
				// keyboard forever: UiTextBoxSystem ends editing precisely when the
				// focused element stops matching it, which clearing focus here is what
				// actually drives.
				focused = Entity{};
			}
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
