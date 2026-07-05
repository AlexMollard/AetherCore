#pragma once

#include <string>

#include <glm/glm.hpp>

#include "scene/Entity.hpp"

namespace aether
{
	class AssetManager;
	class World;
} // namespace aether

namespace aether::app::scripting
{
	struct SceneContext;
} // namespace aether::app::scripting

namespace aether::app::scene
{
	// Spawns a model file as a named root entity with one mesh child per
	// primitive - the exact layout das load_model produces - for editor-side
	// spawning (the outliner's Create > Model menu). Loaded model data caches
	// in (and is owned by) the SceneContext, so all spawned entities register
	// as scene entities: F5 teardown frees the entities and the model data
	// together instead of leaving dangling mesh/animation pointers.
	// Returns the root, or an invalid Entity when the model fails to load.
	Entity SpawnModelEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path, const glm::mat4& localToWorld);
} // namespace aether::app::scene
