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
