#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "layers/LoadingLayer.hpp"
#include "scripting/SceneContext.hpp"

namespace
{
	using namespace aether::app::scripting;

	// set_loading_visible(true)  -> show "Loading..." overlay
	// set_loading_visible(false) -> hide it
	// Script does its own loading; this is just a UI toggle.
	void das_set_loading_visible(bool visible)
	{
		if (auto* overlay = ActiveContext().loadingOverlay)
		{
			overlay->SetVisible(visible);
		}
	}

} // namespace

namespace aether::app::scripting
{
	struct LoadingModule : DasModuleBase
	{
		LoadingModule()
		      : DasModuleBase("loading")
		{
			das::ModuleLibrary lib(this);

			Bind<das_set_loading_visible>(lib, "set_loading_visible", SE::modifyExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(LoadingModule, aether::app::scripting)
