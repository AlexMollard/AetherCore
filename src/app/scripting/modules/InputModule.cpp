#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "platform/Input.hpp"
#include "scripting/SceneContext.hpp"

namespace
{
	using namespace aether::app::scripting;

	bool das_is_key_down(int keyCode)
	{
		return ActiveContext().input->IsKeyDown(static_cast<aether::Key>(keyCode));
	}

	bool das_is_key_pressed(int keyCode)
	{
		return ActiveContext().input->IsKeyPressed(static_cast<aether::Key>(keyCode));
	}

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
	struct InputModule : DasModuleBase
	{
		InputModule()
		      : DasModuleBase("input")
		{
			das::ModuleLibrary lib(this);

			Bind<das_is_key_down>(lib, "is_key_down", SE::accessExternal);
			Bind<das_is_key_pressed>(lib, "is_key_pressed", SE::accessExternal);
			Bind<das_is_key_released>(lib, "is_key_released", SE::accessExternal);
			Bind<das_get_delta_time>(lib, "get_delta_time", SE::accessExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(InputModule, aether::app::scripting)
