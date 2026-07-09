#include "scene/ModelSpawn.hpp"

#include <algorithm>
#include <vector>

#include "assets/AssetManager.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

namespace aether::app::scene
{
	namespace
	{
		LoadedModel* LoadModelCached(AssetManager& assets, scripting::SceneContext& ctx, const std::string& path)
		{
			if (const auto it = ctx.loadedModelMap.find(path); it != ctx.loadedModelMap.end())
			{
				return &ctx.loadedModels[it->second];
			}
			auto result = assets.LoadModel(path);
			if (!result)
			{
				AE_WARN(LogCategory::App, "ModelSpawn: failed to load '{}': {}", path, result.error());
				return nullptr;
			}
			ctx.loadedModels.push_back(std::move(result.value()));
			ctx.loadedModelMap[path] = ctx.loadedModels.size() - 1;
			return &ctx.loadedModels.back();
		}

		std::string ModelDisplayName(const std::string& path)
		{
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
			return stem;
		}

		void RememberSceneEntity(scripting::SceneContext& ctx, Entity entity)
		{
			if (std::find(ctx.sceneEntities.begin(), ctx.sceneEntities.end(), entity) == ctx.sceneEntities.end())
			{
				ctx.sceneEntities.push_back(entity);
			}
		}

		void ForgetSceneEntity(scripting::SceneContext& ctx, Entity entity)
		{
			std::erase(ctx.sceneEntities, entity);
		}

		template<typename T>
		void RemoveIfPresent(World& world, Entity entity)
		{
			if (world.Has<T>(entity))
			{
				world.Remove<T>(entity);
			}
		}
	} // namespace

	Entity SpawnModelEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path, const glm::mat4& localToWorld)
	{
		// Reuse the shared model cache (same one das load_model and the scene
		// loader use) so repeated spawns of one model load it once.
		LoadedModel* modelPtr = LoadModelCached(assets, ctx, path);
		if (modelPtr == nullptr)
		{
			return {};
		}

		const std::string stem = ModelDisplayName(path);

		const Entity root = world.Create();
		world.Emplace<NameComponent>(root, NameComponent{.name = stem});
		world.Emplace<TransformComponent>(root, TransformComponent{.localToWorld = localToWorld});
		RememberSceneEntity(ctx, root);

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
			RememberSceneEntity(ctx, meshEntity);
		}
		return root;
	}

	bool AssignModelToEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, Entity root, const std::string& path)
	{
		if (!root.IsValid() || !world.GetRegistry().valid(World::ToEntt(root)))
		{
			return false;
		}

		LoadedModel* modelPtr = LoadModelCached(assets, ctx, path);
		if (modelPtr == nullptr)
		{
			return false;
		}

		std::vector<Entity> generatedChildren;
		if (const auto* hierarchy = world.TryGet<HierarchyComponent>(root))
		{
			for (const Entity child: hierarchy->children)
			{
				const auto* source = world.TryGet<MeshSourceComponent>(child);
				if (source != nullptr && source->kind == MeshSourceComponent::Kind::Model)
				{
					generatedChildren.push_back(child);
				}
			}
		}
		for (const Entity child: generatedChildren)
		{
			ForgetSceneEntity(ctx, child);
			ecs::DestroyHierarchy(world, child);
		}

		RemoveIfPresent<MeshComponent>(world, root);
		RemoveIfPresent<SkinnedMeshComponent>(world, root);
		RemoveIfPresent<MeshSourceComponent>(world, root);
		RemoveIfPresent<MaterialComponent>(world, root);
		RemoveIfPresent<MaterialInstanceComponent>(world, root);
		RemoveIfPresent<PipelineComponent>(world, root);

		if (!world.Has<TransformComponent>(root))
		{
			world.Emplace<TransformComponent>(root);
		}
		if (!world.Has<NameComponent>(root))
		{
			world.Emplace<NameComponent>(root, NameComponent{.name = ModelDisplayName(path)});
		}
		RememberSceneEntity(ctx, root);

		const glm::mat4 rootTransform = world.TryGet<TransformComponent>(root) != nullptr ? world.Get<TransformComponent>(root).localToWorld : glm::mat4(1.0f);
		const std::string stem = ModelDisplayName(path);
		std::vector<Entity> meshEntities = assets.SpawnModel(*modelPtr, root.id);
		for (std::size_t i = 0; i < meshEntities.size(); ++i)
		{
			const Entity meshEntity = meshEntities[i];
			if (auto* tc = world.TryGet<TransformComponent>(meshEntity))
			{
				tc->localToWorld = rootTransform * tc->localToWorld;
			}
			world.EmplaceOrReplace<NameComponent>(meshEntity, NameComponent{.name = stem + " mesh"});
			world.EmplaceOrReplace<MeshSourceComponent>(meshEntity, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Model, .path = path, .primitiveIndex = static_cast<std::uint32_t>(i)});
			RememberSceneEntity(ctx, meshEntity);
		}
		return !meshEntities.empty();
	}
} // namespace aether::app::scene
