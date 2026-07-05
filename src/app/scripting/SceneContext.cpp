#include "scripting/SceneContext.hpp"

namespace aether::app::scripting
{
	// The active SceneContext for the current script call. Installed by the
	// ScriptComponentSystem / ScriptedSceneLayer immediately before invoking a
	// managed script function and cleared immediately after, so the C# interop
	// exports resolve engine services through ActiveContext().
	thread_local SceneContext* g_activeContext = nullptr;
} // namespace aether::app::scripting
