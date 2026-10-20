#pragma once

namespace aether
{
	class World;
	namespace scripting
	{
		class GameplayAccess;
	}
}

namespace aether::app::web
{
	void BindManagedBrowserWorld(World* world, aether::scripting::GameplayAccess* access);
}
