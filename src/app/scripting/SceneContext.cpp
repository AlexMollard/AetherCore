#include "scripting/SceneContext.hpp"

namespace aether::app::scripting
{
	thread_local SceneContext* g_activeContext = nullptr;
}
