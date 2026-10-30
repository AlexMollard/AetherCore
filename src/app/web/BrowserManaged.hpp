#pragma once

namespace aether
{
	class World;
	class ServiceContainer;
	class PhysicsSystem;
	namespace scripting
	{
		class GameplayAccess;
		struct ManagedScriptApi;
		namespace gameplay3d
		{
			class ClipCatalog;
		}
	}
}
namespace aether::app::scripting { struct SceneContext; }


namespace aether::app::web
{
	void BindManagedBrowserWorld(World* world, aether::scripting::GameplayAccess* access);
	void BindManagedBrowserServices(ServiceContainer* services);
	[[nodiscard]] const aether::scripting::ManagedScriptApi& ManagedBrowserApi();
	[[nodiscard]] aether::app::scripting::SceneContext& ManagedBrowserSceneContext();
	// The simulation the shared native interop exports (Physics/Character/Ragdoll/Animation) act on.
	// Null when the loaded scene has no 3D physics; the catalog names the cooked glTF clips.
	void BindManagedBrowserSimulation(PhysicsSystem* physics, const aether::scripting::gameplay3d::ClipCatalog* clips);
}
