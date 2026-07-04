#pragma once

#include "scene/Entity.hpp"

namespace aether
{
	class World;
	class MaterialRegistry;
	struct MaterialAsset;

	namespace MaterialSystem
	{
		// Connect the entt on_destroy hook so material handles are released when a
		// MaterialComponent is destroyed with its entity or removed. Call once at
		// engine setup, after the registry exists.
		void ConnectLifecycle(World& world, MaterialRegistry& registry);

		// Disconnect the hook before the registry is torn down so late world
		// teardown cannot release into a dead registry.
		void DisconnectLifecycle(World& world);

		// Assign a material to an entity: release any previous handle, acquire the
		// new asset, store handle + cached slot. Emplaces the component if absent.
		void AssignMaterial(World& world, Entity entity, MaterialRegistry& registry, const MaterialAsset& asset);
	} // namespace MaterialSystem
} // namespace aether
