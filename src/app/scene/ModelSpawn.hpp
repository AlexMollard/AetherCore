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
}

namespace aether::app::scene
{
	// primitive - the exact layout das load_model produces. Loaded model data caches
	Entity SpawnModelEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path, const glm::mat4& localToWorld);

	// layout. This is the inspector/file-explorer workflow: create an entity, then
	bool AssignModelToEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, Entity root, const std::string& path);

	int ModelPrimitiveCount(AssetManager& assets, scripting::SceneContext& ctx, const std::string& path);

	const Mesh* ResolveModelPrimitiveMesh(AssetManager& assets, scripting::SceneContext& ctx, const std::string& path, int primitiveIndex);

	void RegisterModelAssets(AssetDatabase& db, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path);

	// alive so live MeshComponent.mesh pointers never dangle - it is re-pointed by
	bool ReloadModelAssets(AssetDatabase& db, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path);

	bool AssignModelMeshToEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, Entity entity, const std::string& path, int primitiveIndex);
} // namespace aether::app::scene
