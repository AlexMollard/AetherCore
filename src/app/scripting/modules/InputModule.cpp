#include "InputModule.hpp"
#include "DasHelpers.hpp"

#include "daScript/daScript.h"

#include "platform/Input.hpp"

namespace
{
	using namespace aether::app::scripting;

	// is_key_down(key_code_int) -> bool
	bool das_is_key_down(int keyCode)
	{
		return ActiveContext().input->IsKeyDown(static_cast<aether::Key>(keyCode));
	}

	// is_key_pressed(key_code_int) -> bool  (true only on the frame it was first pressed)
	bool das_is_key_pressed(int keyCode)
	{
		return ActiveContext().input->IsKeyPressed(static_cast<aether::Key>(keyCode));
	}

	// is_key_released(key_code_int) -> bool
	bool das_is_key_released(int keyCode)
	{
		return ActiveContext().input->IsKeyReleased(static_cast<aether::Key>(keyCode));
	}

	float das_get_delta_time()
	{
		return ActiveContext().deltaTime;
	}

} // namespace

namespace aether::app::scripting
{
	struct InputModule : das::Module
	{
		InputModule()
		      : das::Module("input")
		{
			das::ModuleLibrary lib(this);

			addExtern<DAS_BIND_FUN(das_is_key_down)>(*this, lib, "is_key_down", das::SideEffects::accessExternal, "das_is_key_down");
			addExtern<DAS_BIND_FUN(das_is_key_pressed)>(*this, lib, "is_key_pressed", das::SideEffects::accessExternal, "das_is_key_pressed");
			addExtern<DAS_BIND_FUN(das_is_key_released)>(*this, lib, "is_key_released", das::SideEffects::accessExternal, "das_is_key_released");
			addExtern<DAS_BIND_FUN(das_get_delta_time)>(*this, lib, "get_delta_time", das::SideEffects::accessExternal, "das_get_delta_time");

			verifyAotReady();
		}
	};

} // namespace aether::app::scripting

REGISTER_MODULE_IN_NAMESPACE(InputModule, aether::app::scripting);

void RegisterInputModule()
{
	NEED_MODULE(InputModule);
}
