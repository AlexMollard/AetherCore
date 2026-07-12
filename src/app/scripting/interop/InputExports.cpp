#include "scripting/interop/InteropCommon.hpp"

#include "platform/Input.hpp"

// Input state exported to C#. Key codes are the engine's aether::Key values
// (GLFW codes), mirrored by the managed Key enum.

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

// ── Mouse (button codes match managed MouseButton: 0/1/2 = Left/Right/Middle) ──

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

// Cursor position in render-target pixels (top-left origin) - same space as a
// UI element's resolved rect, so it hit-tests UI directly.
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

AE_SCRIPT_API Vec2 aether_input_scroll_delta()
{
	const glm::vec2 p = ActiveContext().input->GetScrollDelta();
	return {p.x, p.y};
}
