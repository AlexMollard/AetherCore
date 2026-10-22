#pragma once

namespace aether
{
	class World;
	class PhysicsSystem;
	namespace scripting
	{
		class GameplayAccess;
		namespace gameplay3d
		{
			class ClipCatalog;
		}
	}
}

namespace aether::app::web
{
	void BindManagedBrowserWorld(World* world, aether::scripting::GameplayAccess* access);
	// The simulation the shared native interop exports (Physics/Character/Ragdoll/Animation) act on.
	// Null when the loaded scene has no 3D physics; the catalog names the cooked glTF clips.
	void BindManagedBrowserSimulation(PhysicsSystem* physics, const aether::scripting::gameplay3d::ClipCatalog* clips);
}
