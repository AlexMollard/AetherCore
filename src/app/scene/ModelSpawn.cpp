#include "scene/ModelSpawn.hpp"

#include <vector>

#include "assets/AssetManager.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

namespace aether::app::scene
{
	Entity SpawnModelEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path, const glm::mat4& localToWorld)
	{
		// Reuse the shared model cache (same one das load_model and the scene
		// loader use) so repeated spawns of one model load it once.
		LoadedModel* modelPtr = nullptr;
		if (const auto it = ctx.loadedModelMap.find(path); it != ctx.loadedModelMap.end())
		{
			modelPtr = &ctx.loadedModels[it->second];
		}
		else
		{
			auto result = assets.LoadModel(path);
			if (!result)
			{
				AE_WARN(LogCategory::App, "SpawnModelEntity: failed to load '{}': {}", path, result.error());
				return Entity{};
			}
			ctx.loadedModels.push_back(std::move(result.value()));
			ctx.loadedModelMap[path] = ctx.loadedModels.size() - 1;
			modelPtr = &ctx.loadedModels.back();
		}

		std::string stem = path;
		if (const auto slash = stem.find_last_of("/\\"); slash != std::string::npos)
		{
			stem = stem.substr(slash + 1);
		}
		if (const auto dot = stem.find_last_of('.'); dot != std::string::npos)
		{
			stem = stem.substr(0, dot);
		}
		if (stem.empty())
		{
			stem = "Model";
		}

		const Entity root = world.Create();
		world.Emplace<NameComponent>(root, NameComponent{.name = stem});
		world.Emplace<TransformComponent>(root, TransformComponent{.localToWorld = localToWorld});
		ctx.sceneEntities.push_back(root);

		std::vector<Entity> meshEntities = assets.SpawnModel(*modelPtr, root.id);
		for (std::size_t i = 0; i < meshEntities.size(); ++i)
		{
			const Entity meshEntity = meshEntities[i];
			if (auto* tc = world.TryGet<TransformComponent>(meshEntity))
			{
				tc->localToWorld = localToWorld * tc->localToWorld;
			}
			// SpawnModel already linked meshEntity under root via ecs::SetParent.
			world.EmplaceOrReplace<NameComponent>(meshEntity, NameComponent{.name = stem + " mesh"});
			// Stable identity for serialization: one entity per primitive, in order.
			world.EmplaceOrReplace<MeshSourceComponent>(meshEntity, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Model, .path = path, .primitiveIndex = static_cast<std::uint32_t>(i)});
			ctx.sceneEntities.push_back(meshEntity);
		}
		return root;
	}
} // namespace aether::app::scene
