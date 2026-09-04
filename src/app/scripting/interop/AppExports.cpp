#include "scripting/interop/InteropCommon.hpp"

#include "platform/PlatformSubsystem.hpp"
#include "platform/Window.hpp"
#include "utils/ServiceContainer.hpp"

// The application itself, exported to C#: the one thing a game needs from the process it is
// running in that is not the world, the renderer or the input. A game with a main menu has a
// LEAVE on it, and until now there was no way for a script to say so - the only exit from a
// shipped Hollowtide was the title bar or Alt+F4, which is a hole in the front door of the
// game rather than a missing convenience.

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// Asks the main loop to stop at the end of the frame, exactly as the title-bar X does. Not a
// process exit: the loop still unwinds, the scene is torn down in order, and anything with a
// shutdown to run gets to run it. A script that called std::exit here would skip the save.
AE_SCRIPT_API void aether_app_quit()
{
	SafeExport([&] -> void
	{
	auto* services = ActiveContext().services;
	if (services == nullptr)
	{
		return;
	}
	if (auto* platform = services->TryGet<aether::PlatformSubsystem>())
	{
		platform->GetWindow().RequestClose();
	}
	});
}
