#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

#include "platform/Input.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

AE_SCRIPT_API std::int32_t aether_input_key_down(std::int32_t keyCode)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsKeyDown(static_cast<aether::Key>(keyCode)) ? 1 : 0; }); }

AE_SCRIPT_API std::int32_t aether_input_key_pressed(std::int32_t keyCode)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsKeyPressed(static_cast<aether::Key>(keyCode)) ? 1 : 0; }); }

// The primitive a "press any key to rebind" prompt needs - the alternative was a
// per-frame IsKeyPressed scan over every Key enum value from script, which works but
// costs kMaxKeys FFI calls a frame while the prompt is open. Returns Key.None (-1)
// when nothing was pressed - never mistaken for a real key, since the lowest real Key value (Space) is 32.
AE_SCRIPT_API std::int32_t aether_input_next_key_pressed()
{ return SafeExport([&] -> std::int32_t { return static_cast<std::int32_t>(ActiveContext().input->GetKeyPressedThisFrame()); }); }

AE_SCRIPT_API std::int32_t aether_input_key_released(std::int32_t keyCode)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsKeyReleased(static_cast<aether::Key>(keyCode)) ? 1 : 0; }); }

AE_SCRIPT_API float aether_input_delta_time()
{ return SafeExport([&] -> float { return ActiveContext().deltaTime; }); }

AE_SCRIPT_API std::int32_t aether_input_mouse_down(std::int32_t btn)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsMouseButtonDown(static_cast<aether::MouseButton>(btn)) ? 1 : 0; }); }

AE_SCRIPT_API std::int32_t aether_input_mouse_pressed(std::int32_t btn)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsMouseButtonPressed(static_cast<aether::MouseButton>(btn)) ? 1 : 0; }); }

AE_SCRIPT_API std::int32_t aether_input_mouse_released(std::int32_t btn)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsMouseButtonReleased(static_cast<aether::MouseButton>(btn)) ? 1 : 0; }); }

AE_SCRIPT_API Vec2 aether_input_mouse_pos()
{
	return SafeExport([&] -> Vec2
	{
	const glm::vec2 p = ActiveContext().input->GetMousePos();
	return {p.x, p.y};
	});
}

AE_SCRIPT_API Vec2 aether_input_mouse_delta()
{
	return SafeExport([&] -> Vec2
	{
	const glm::vec2 p = ActiveContext().input->GetMouseDelta();
	return {p.x, p.y};
	});
}

// A game that draws its own pointer has to be able to put the OS one away. Lives here rather than in
// a window API because GLFW models cursor visibility as an input mode, and Input already owns the
// window handle - no new plumbing, and nothing new to reach through from a script.
AE_SCRIPT_API void aether_input_set_os_cursor_visible(std::int32_t visible)
{ SafeExport([&] -> void { ActiveContext().input->SetOsCursorVisible(visible != 0); }); }

AE_SCRIPT_API std::int32_t aether_input_get_os_cursor_visible()
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsOsCursorVisible() ? 1 : 0; }); }

// -- Cursor lock (FPS-style pointer lock) ------------------------------------------
// Distinct from SetOsCursorVisible above: a locked cursor is hidden AND confined to the
// window, with aether_input_mouse_delta reporting unbounded relative motion instead of
// an absolute position that stops at the screen edge. Only takes effect while the game
// is actually playing and the window is focused (see Input::UpdateCursorLock /
// Application::OnUpdate) - a request made in edit mode, or one still standing when Play
// stops, has no effect. Released automatically on losing focus, on Stop, or when the
// user presses Escape; request it again to lock back up.
AE_SCRIPT_API void aether_input_request_cursor_lock(std::int32_t requested)
{ SafeExport([&] -> void { ActiveContext().input->RequestCursorLock(requested != 0); }); }

AE_SCRIPT_API std::int32_t aether_input_get_cursor_lock_requested()
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsCursorLockRequested() ? 1 : 0; }); }

// Whether the pointer is actually locked right now - the request AND focus AND not
// escaped. What a script should check before trusting aether_input_mouse_delta as
// unbounded look input.
AE_SCRIPT_API std::int32_t aether_input_is_cursor_locked()
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsCursorLocked() ? 1 : 0; }); }

AE_SCRIPT_API Vec2 aether_input_scroll_delta()
{
	return SafeExport([&] -> Vec2
	{
	const glm::vec2 p = ActiveContext().input->GetScrollDelta();
	return {p.x, p.y};
	});
}

// ── Clipboard ─────────────────────────────────────────────────────────────
AE_SCRIPT_API std::int32_t aether_input_get_clipboard(char* buf, std::int32_t bufLen)
{
	return SafeExport([&] -> std::int32_t
	{
	if (buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const std::string text = ActiveContext().input->GetClipboardText();
	const std::int32_t n = std::min<std::int32_t>(bufLen, static_cast<std::int32_t>(text.size()));
	std::memcpy(buf, text.data(), static_cast<std::size_t>(n));
	return n;
	});
}

AE_SCRIPT_API void aether_input_set_clipboard(const char* text)
{ SafeExport([&] -> void { ActiveContext().input->SetClipboardText(text != nullptr ? text : ""); }); }

// -- Gamepads --------------------------------------------------------------
// `pad` is a slot index, or Input::kAnyGamepad (-1) for "whichever pad is connected
// first" - the default the script layer passes, because GLFW slots are not compacted
// and a lone controller often is not in slot 0.
AE_SCRIPT_API std::int32_t aether_input_gamepad_connected(std::int32_t pad)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsGamepadConnected(pad) ? 1 : 0; }); }

AE_SCRIPT_API std::int32_t aether_input_gamepad_resolve(std::int32_t pad)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->ResolveGamepad(pad); }); }

AE_SCRIPT_API std::int32_t aether_input_gamepad_button_down(std::int32_t button, std::int32_t pad)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsGamepadButtonDown(static_cast<aether::GamepadButton>(button), pad) ? 1 : 0; }); }

AE_SCRIPT_API std::int32_t aether_input_gamepad_button_pressed(std::int32_t button, std::int32_t pad)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsGamepadButtonPressed(static_cast<aether::GamepadButton>(button), pad) ? 1 : 0; }); }

AE_SCRIPT_API std::int32_t aether_input_gamepad_button_released(std::int32_t button, std::int32_t pad)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsGamepadButtonReleased(static_cast<aether::GamepadButton>(button), pad) ? 1 : 0; }); }

AE_SCRIPT_API Vec2 aether_input_gamepad_stick(std::int32_t stick, std::int32_t pad)
{
	return SafeExport([&] -> Vec2
	{
	const glm::vec2 v = ActiveContext().input->GetGamepadStick(static_cast<aether::GamepadStick>(stick), pad);
	return {v.x, v.y};
	});
}

AE_SCRIPT_API float aether_input_gamepad_trigger(std::int32_t trigger, std::int32_t pad)
{ return SafeExport([&] -> float { return ActiveContext().input->GetGamepadTrigger(static_cast<aether::GamepadTrigger>(trigger), pad); }); }

AE_SCRIPT_API float aether_input_gamepad_axis_raw(std::int32_t axis, std::int32_t pad)
{ return SafeExport([&] -> float { return ActiveContext().input->GetGamepadAxisRaw(static_cast<aether::GamepadAxis>(axis), pad); }); }

AE_SCRIPT_API std::int32_t aether_input_gamepad_name(std::int32_t pad, char* buf, std::int32_t bufLen)
{
	return SafeExport([&] -> std::int32_t
	{
	if (buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const std::string_view name = ActiveContext().input->GetGamepadName(pad);
	const std::int32_t n = std::min<std::int32_t>(bufLen, static_cast<std::int32_t>(name.size()));
	std::memcpy(buf, name.data(), static_cast<std::size_t>(n));
	return n;
	});
}

AE_SCRIPT_API std::int32_t aether_input_gamepad_stick_flicked(std::int32_t stick, std::int32_t dir, std::int32_t pad)
{ return SafeExport([&] -> std::int32_t { return ActiveContext().input->IsGamepadStickFlicked(static_cast<aether::GamepadStick>(stick), static_cast<aether::GamepadDirection>(dir), pad) ? 1 : 0; }); }

// ConsumeGamepadButton is deliberately NOT exported: its keyboard counterpart is not either,
// and consumption is how the UI layer keeps a menu press away from the game underneath. A
// script reaching into that would be taking input away from itself.
AE_SCRIPT_API void aether_input_gamepad_set_deadzones(float stick, float trigger)
{ SafeExport([&] -> void { ActiveContext().input->SetGamepadDeadzones(stick, trigger); }); }
