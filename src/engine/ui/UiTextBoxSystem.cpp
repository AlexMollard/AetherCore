#include "ui/UiTextBoxSystem.hpp"

#include <algorithm>
#include <cmath>

#include <entt/entt.hpp>

#include "platform/Input.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiTextEdit.hpp"

namespace aether::ui
{
	namespace
	{
		// Held keys repeat like an OS key-repeat: one immediate action, a pause, then a stream.
		constexpr float kRepeatDelay = 0.4f;
		constexpr float kRepeatRate = 0.03f;
		constexpr double kDoubleClickSeconds = 0.35;

		TextEditState ToEditState(const UITextBox& box)
		{
			TextEditState s;
			s.text = box.text;
			s.caret = box.caret;
			s.selectionAnchor = box.selectionAnchor;
			s.scrollX = box.scrollX;
			return s;
		}

		void FromEditState(const TextEditState& s, UITextBox& box)
		{
			box.text = s.text;
			box.caret = s.caret;
			box.selectionAnchor = s.selectionAnchor;
			box.scrollX = s.scrollX;
		}

		TextEditLimits LimitsOf(const UITextBox& box)
		{
			TextEditLimits limits;
			limits.contentType = box.contentType;
			limits.allowedChars = box.allowedChars;
			limits.maxLength = box.maxLength;
			return limits;
		}

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

		void SetCapture(World& world, Entity e, bool capture)
		{
			const bool has = world.Has<UIKeyboardCapture>(e);
			if (capture && !has)
			{
				world.Emplace<UIKeyboardCapture>(e);
			}
			else if (!capture && has)
			{
				world.Remove<UIKeyboardCapture>(e);
			}
		}

		// True on the down-edge, and again on the repeat schedule while held. One key repeats at a
		// time - the last one pressed - which is what a keyboard does.
		bool KeyAction(Input& input, UITextBox& box, Key key, float dt)
		{
			const auto code = static_cast<int>(key);
			if (input.IsKeyPressed(key))
			{
				box.repeatKey = code;
				box.repeatTimer = kRepeatDelay;
				return true;
			}
			if (box.repeatKey != code || !input.IsKeyDown(key))
			{
				return false;
			}
			box.repeatTimer -= dt;
			if (box.repeatTimer <= 0.f)
			{
				box.repeatTimer = kRepeatRate;
				return true;
			}
			return false;
		}
	} // namespace

	void UiTextBoxSystem::Update(World& world, Input& input, FontRegistry* fonts, float time)
	{
		// Frame delta from the absolute time this system is handed, matching UiWidgetSystem.
		// Clamped so a pause or scene switch cannot jump the caret blink or key repeat.
		static float s_prevTime = time;
		const float dt = std::clamp(time - s_prevTime, 0.f, 0.1f);
		s_prevTime = time;

		const glm::vec2 mouse = input.GetMousePos();
		const bool mouseDown = input.IsMouseButtonDown(MouseButton::Left);
		const bool mousePressed = input.IsMouseButtonPressed(MouseButton::Left);
		const bool shift = input.IsKeyDown(Key::LeftShift) || input.IsKeyDown(Key::RightShift);
		const bool ctrl = input.IsKeyDown(Key::LeftCtrl) || input.IsKeyDown(Key::RightCtrl);

		world.View<UITextBox, UIRect>().each(
		        [&](entt::entity ent, UITextBox& box, UIRect& rect)
		        {
			        const Entity e = World::FromEntt(ent);
			        box.changed = false;
			        box.submitted = false;
			        box.cancelled = false;

			        if (ecs::HasDisabledAncestor(world, e))
			        {
				        box.editing = false;
				        box.dragging = false;
				        SetCapture(world, e, false);
				        return;
			        }

			        const glm::vec4 r = rect.resolvedRect;
			        const float innerX = r.x + box.padding;
			        const float innerW = std::max(r.z - 2.f * box.padding, 1.f);
			        const bool hovered = mouse.x >= r.x && mouse.x <= r.x + r.z && mouse.y >= r.y && mouse.y <= r.y + r.w;

			        // Losing focus commits and releases the keyboard.
			        if (box.editing && !Focused(world, e))
			        {
				        box.editing = false;
				        box.dragging = false;
				        SetCapture(world, e, false);
			        }

			        // Activation (click, or Enter/Space while focused) starts editing.
			        if (!box.editing && Activated(world, e))
			        {
				        box.editing = true;
				        box.committedText = box.text;
				        box.caretTimer = 0.f;
				        box.repeatKey = 0;
				        SetCapture(world, e, true);
				        TextEditState s = ToEditState(box);
				        SelectAll(s); // entering a field selects it, so typing replaces
				        FromEditState(s, box);
			        }

			        if (!box.editing)
			        {
				        return;
			        }

			        const FontAsset* font = fonts != nullptr ? fonts->Load(box.fontName) : nullptr;
			        TextEditState s = ToEditState(box);
			        bool textChanged = false;

			        // ── Mouse: click places the caret, drag extends, double-click selects a word ──
			        if (font != nullptr && mousePressed && hovered)
			        {
				        const std::string display = DisplayText(s.text, box.password);
				        const float localX = mouse.x - innerX + s.scrollX;
				        const int index = CaretFromPixelX(*font, display, box.pixelSize, localX);
				        const double now = static_cast<double>(time);
				        if (box.lastClickTime >= 0.0 && now - box.lastClickTime < kDoubleClickSeconds)
				        {
					        SelectWordAt(s, index);
				        }
				        else
				        {
					        s.caret = index;
					        if (!shift)
					        {
						        ClearSelection(s);
					        }
					        box.dragging = true;
				        }
				        box.lastClickTime = now;
				        box.caretTimer = 0.f;
			        }
			        else if (font != nullptr && box.dragging && mouseDown)
			        {
				        const std::string display = DisplayText(s.text, box.password);
				        s.caret = CaretFromPixelX(*font, display, box.pixelSize, mouse.x - innerX + s.scrollX);
			        }
			        if (!mouseDown)
			        {
				        box.dragging = false;
			        }

			        // ── Clipboard ───────────────────────────────────────────────────────────────
			        if (ctrl && input.IsKeyPressed(Key::A))
			        {
				        SelectAll(s);
			        }
			        if (ctrl && input.IsKeyPressed(Key::C) && HasSelection(s))
			        {
				        input.SetClipboardText(SelectedText(s));
			        }
			        if (ctrl && input.IsKeyPressed(Key::X) && HasSelection(s))
			        {
				        input.SetClipboardText(SelectedText(s));
				        textChanged = DeleteSelection(s) || textChanged;
			        }
			        if (ctrl && input.IsKeyPressed(Key::V))
			        {
				        textChanged = InsertText(s, LimitsOf(box), input.GetClipboardText()) || textChanged;
			        }

			        // ── Typed characters ────────────────────────────────────────────────────────
			        // Ctrl chords are shortcuts, not text; GLFW does not emit chars for them anyway,
			        // but skipping keeps a stray char from a chord out of the field.
			        if (!ctrl && !input.GetTypedChars().empty())
			        {
				        textChanged = InsertText(s, LimitsOf(box), input.GetTypedChars()) || textChanged;
			        }

			        // ── Caret and deletion ──────────────────────────────────────────────────────
			        if (KeyAction(input, box, Key::Left, dt))
			        {
				        MoveCaret(s, ctrl ? CaretMove::WordLeft : CaretMove::Left, shift);
			        }
			        if (KeyAction(input, box, Key::Right, dt))
			        {
				        MoveCaret(s, ctrl ? CaretMove::WordRight : CaretMove::Right, shift);
			        }
			        if (KeyAction(input, box, Key::Home, dt))
			        {
				        MoveCaret(s, CaretMove::Home, shift);
			        }
			        if (KeyAction(input, box, Key::End, dt))
			        {
				        MoveCaret(s, CaretMove::End, shift);
			        }
			        if (KeyAction(input, box, Key::Backspace, dt))
			        {
				        textChanged = DeleteBackward(s, ctrl) || textChanged;
			        }
			        if (KeyAction(input, box, Key::Delete, dt))
			        {
				        textChanged = DeleteForward(s, ctrl) || textChanged;
			        }

			        // ── Commit / cancel ─────────────────────────────────────────────────────────
			        bool leaveEditing = false;
			        if (input.IsKeyPressed(Key::Enter) || input.IsKeyPressed(Key::KpEnter))
			        {
				        box.submitted = true;
				        leaveEditing = true;
			        }
			        else if (input.IsKeyPressed(Key::Escape))
			        {
				        if (s.text != box.committedText)
				        {
					        s.text = box.committedText;
					        s.caret = static_cast<int>(s.text.size());
					        ClearSelection(s);
					        textChanged = true;
				        }
				        box.cancelled = true;
				        leaveEditing = true;
			        }
			        else if (input.IsKeyPressed(Key::Tab))
			        {
				        leaveEditing = true; // navigation already moved focus on this key
			        }

			        if (font != nullptr)
			        {
				        ScrollToCaret(s, *font, box.pixelSize, innerW, box.password);
			        }
			        FromEditState(s, box);

			        box.changed = textChanged;
			        box.caretTimer += dt;
			        if (leaveEditing)
			        {
				        box.editing = false;
				        box.dragging = false;
				        box.repeatKey = 0;
				        box.committedText = box.text;
				        SetCapture(world, e, false);
			        }
		        });
	}
} // namespace aether::ui
