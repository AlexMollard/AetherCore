#pragma once

#include <string>

#include <glm/glm.hpp>

#include "scene/Entity.hpp"

namespace aether
{
	class AssetManager;
	class AssetDatabase;
	class Mesh;
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

	// Applies a model file to an existing entity. A single-primitive static model
	// is referenced directly on the entity as a shared mesh (Unity-style, no child
	// spawned); multi-primitive or skinned models keep the container-of-children
	// layout. This is the inspector/file-explorer workflow: create an entity, then
	// drag a model file onto it instead of spawning models from hierarchy menus.
	bool AssignModelToEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, Entity root, const std::string& path);

	// Number of primitives in a model (0 if it fails to load). Backs the
	// inspector's primitive picker for model-referenced meshes.
	int ModelPrimitiveCount(AssetManager& assets, scripting::SceneContext& ctx, const std::string& path);

	// Resolves a model primitive to its shared live Mesh pointer (loading the model
	// into the SceneContext cache if needed); nullptr on failure. This is the
	// AssetDatabase's model-mesh resolver.
	const Mesh* ResolveModelPrimitiveMesh(AssetManager& assets, scripting::SceneContext& ctx, const std::string& path, int primitiveIndex);

	// Registers a model and each of its primitives as assets in the database
	// (idempotent), so they appear in the mesh asset picker. Loads the model if
	// needed; a no-op if it fails to load.
	void RegisterModelAssets(AssetDatabase& db, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path);

	// Hot-reloads a model from disk into a FRESH cache slot (the old slot stays
	// alive so live MeshComponent.mesh pointers never dangle - it is re-pointed by
	// AssetDatabase::ResolveWorldMeshes next frame), then bumps the database
	// generation for the model's primitives so the resolve pass re-points them.
	// Returns false if the reload fails (the old model stays in use).
	bool ReloadModelAssets(AssetDatabase& db, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path);

	// References a single primitive of a model directly on `entity` as a SHARED
	// mesh: MeshComponent points into the cached LoadedModel (the same pointer
	// scene load resolves), so many entities can reference one model's meshes with
	// no duplication and no child entities. Sets the material + pipeline, and
	// SkinnedMeshComponent when the primitive is skinned. Returns false if the
	// model or primitive index can't be resolved.
	bool AssignModelMeshToEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, Entity entity, const std::string& path, int primitiveIndex);
} // namespace aether::app::scene
