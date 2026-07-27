#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "platform/Input.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

AE_SCRIPT_API std::int32_t aether_input_key_down(std::int32_t keyCode)
{
	return ActiveContext().input->IsKeyDown(static_cast<aether::Key>(keyCode)) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_input_key_pressed(std::int32_t keyCode)
{
	return ActiveContext().input->IsKeyPressed(static_cast<aether::Key>(keyCode)) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_input_key_released(std::int32_t keyCode)
{
	return ActiveContext().input->IsKeyReleased(static_cast<aether::Key>(keyCode)) ? 1 : 0;
}

AE_SCRIPT_API float aether_input_delta_time()
{
	return ActiveContext().deltaTime;
}

AE_SCRIPT_API std::int32_t aether_input_mouse_down(std::int32_t btn)
{
	return ActiveContext().input->IsMouseButtonDown(static_cast<aether::MouseButton>(btn)) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_input_mouse_pressed(std::int32_t btn)
{
	return ActiveContext().input->IsMouseButtonPressed(static_cast<aether::MouseButton>(btn)) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_input_mouse_released(std::int32_t btn)
{
	return ActiveContext().input->IsMouseButtonReleased(static_cast<aether::MouseButton>(btn)) ? 1 : 0;
}

AE_SCRIPT_API Vec2 aether_input_mouse_pos()
{
	const glm::vec2 p = ActiveContext().input->GetMousePos();
	return {p.x, p.y};
}

AE_SCRIPT_API Vec2 aether_input_mouse_delta()
{
	const glm::vec2 p = ActiveContext().input->GetMouseDelta();
	return {p.x, p.y};
}

// A game that draws its own pointer has to be able to put the OS one away. Lives here rather than in
// a window API because GLFW models cursor visibility as an input mode, and Input already owns the
// window handle - no new plumbing, and nothing new to reach through from a script.
AE_SCRIPT_API void aether_input_set_os_cursor_visible(std::int32_t visible)
{
	ActiveContext().input->SetOsCursorVisible(visible != 0);
}

AE_SCRIPT_API std::int32_t aether_input_get_os_cursor_visible()
{
	return ActiveContext().input->IsOsCursorVisible() ? 1 : 0;
}

AE_SCRIPT_API Vec2 aether_input_scroll_delta()
{
	const glm::vec2 p = ActiveContext().input->GetScrollDelta();
	return {p.x, p.y};
}

// ── Clipboard ─────────────────────────────────────────────────────────────
AE_SCRIPT_API std::int32_t aether_input_get_clipboard(char* buf, std::int32_t bufLen)
{
	if (buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const std::string text = ActiveContext().input->GetClipboardText();
	const std::int32_t n = std::min<std::int32_t>(bufLen, static_cast<std::int32_t>(text.size()));
	std::memcpy(buf, text.data(), static_cast<std::size_t>(n));
	return n;
}

AE_SCRIPT_API void aether_input_set_clipboard(const char* text)
{
	ActiveContext().input->SetClipboardText(text != nullptr ? text : "");
}
