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
	// primitive - the exact layout das load_model produces. Loaded model data caches
	// in (and is owned by) the SceneContext, so all spawned entities register
	// as scene entities: F5 teardown frees the entities and the model data
	// together instead of leaving dangling mesh/animation pointers.
	// Returns the root, or an invalid Entity when the model fails to load.
	Entity SpawnModelEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path, const glm::mat4& localToWorld);

	// Applies a model file to an existing entity. The target remains the stable
	// editable model root while generated mesh children are replaced from `path`.
	// This is the inspector/file-explorer workflow: create an entity, then drag a
	// model file onto it instead of spawning models from hierarchy menus.
	bool AssignModelToEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, Entity root, const std::string& path);
} // namespace aether::app::scene
