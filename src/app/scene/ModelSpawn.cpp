#include "scene/ModelSpawn.hpp"

#include <algorithm>
#include <vector>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "gpu/GpuEnums.hpp"
#include "material/MaterialSystem.hpp"
#include "material/MaterialTemplate.hpp"
#include "material/PipelineCache.hpp"
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
			world.EmplaceOrReplace<NameComponent>(meshEntity, NameComponent{.name = stem + " mesh"});
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
		// Chokepoint for every model-assign path (drag-drop, palette, scripts):
		// 3D geometry needs the Meshes3D feature active in this scene.
		if (!HasSceneFeature(world.GetSceneFeatures(), SceneFeatureFlags::Meshes3D))
		{
			AE_WARN(LogCategory::App, "Ignoring model assign '{}': the 3D Meshes feature is not active in this scene", path);
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

		if (modelPtr->primitives.size() == 1 && modelPtr->primitives[0].skinIndex < 0)
		{
			return AssignModelMeshToEntity(world, assets, ctx, root, path, 0);
		}

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

	int ModelPrimitiveCount(AssetManager& assets, scripting::SceneContext& ctx, const std::string& path)
	{
		const LoadedModel* modelPtr = LoadModelCached(assets, ctx, path);
		return modelPtr != nullptr ? static_cast<int>(modelPtr->primitives.size()) : 0;
	}

	const Mesh* ResolveModelPrimitiveMesh(AssetManager& assets, scripting::SceneContext& ctx, const std::string& path, int primitiveIndex)
	{
		LoadedModel* modelPtr = LoadModelCached(assets, ctx, path);
		if (modelPtr == nullptr || primitiveIndex < 0 || static_cast<std::size_t>(primitiveIndex) >= modelPtr->primitives.size())
		{
			return nullptr;
		}
		return &modelPtr->primitives[static_cast<std::size_t>(primitiveIndex)].mesh;
	}

	void RegisterModelAssets(AssetDatabase& db, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path)
	{
		const LoadedModel* modelPtr = LoadModelCached(assets, ctx, path);
		if (modelPtr == nullptr)
		{
			return;
		}
		const std::string stem = ModelDisplayName(path);
		db.Register(MakeModelSource(path), stem);
		const std::size_t count = modelPtr->primitives.size();
		for (std::size_t i = 0; i < count; ++i)
		{
			std::string name = count > 1 ? stem + " #" + std::to_string(i) : stem;
			db.Register(MakeModelMeshSource(path, static_cast<int>(i)), std::move(name));
		}
	}

	bool AssignModelMeshToEntity(World& world, AssetManager& assets, scripting::SceneContext& ctx, Entity entity, const std::string& path, int primitiveIndex)
	{
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return false;
		}
		LoadedModel* modelPtr = LoadModelCached(assets, ctx, path);
		if (modelPtr == nullptr || primitiveIndex < 0 || static_cast<std::size_t>(primitiveIndex) >= modelPtr->primitives.size())
		{
			return false;
		}
		const LoadedModelPrimitive& primitive = modelPtr->primitives[static_cast<std::size_t>(primitiveIndex)];

		if (!world.Has<TransformComponent>(entity))
		{
			world.Emplace<TransformComponent>(entity);
		}
		if (!world.Has<NameComponent>(entity))
		{
			world.Emplace<NameComponent>(entity, NameComponent{.name = ModelDisplayName(path)});
		}

		world.EmplaceOrReplace<MeshComponent>(entity, MeshComponent{.mesh = &primitive.mesh});
		world.EmplaceOrReplace<MeshSourceComponent>(entity, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Model, .path = path, .primitiveIndex = static_cast<std::uint32_t>(primitiveIndex)});

		if (primitive.hasMaterial)
		{
			MaterialSystem::AssignMaterial(world, entity, assets.GetMaterialRegistry(), assets.GetPipelineCache(), primitive.material);
		}
		else
		{
			MaterialTemplate tmpl{.shaderVfsPath = "shaders://gltf_mesh.spv"};
			tmpl.cullMode = gpu::CullMode::None;
			const GraphicsPipeline* pipeline = assets.GetPipelineCache().Acquire(tmpl);
			world.EmplaceOrReplace<PipelineComponent>(entity, PipelineComponent{.pipeline = pipeline});
			RemoveIfPresent<MaterialComponent>(world, entity);
			RemoveIfPresent<MaterialInstanceComponent>(world, entity);
		}

		if (modelPtr->animationDb.IsValid() && primitive.skinIndex >= 0)
		{
			const auto skinIdx = static_cast<std::uint32_t>(primitive.skinIndex);
			const std::uint32_t joints = modelPtr->animationDb.GetSkinJointCount(skinIdx);
			if (joints > 0)
			{
				world.EmplaceOrReplace<SkinnedMeshComponent>(entity, SkinnedMeshComponent{.animDb = &modelPtr->animationDb, .skinIndex = skinIdx, .jointCount = joints});
			}
		}
		else if (world.Has<SkinnedMeshComponent>(entity))
		{
			world.Remove<SkinnedMeshComponent>(entity);
		}

		RememberSceneEntity(ctx, entity);
		return true;
	}

	bool ReloadModelAssets(AssetDatabase& db, AssetManager& assets, scripting::SceneContext& ctx, const std::string& path)
	{
		// live MeshComponent.mesh pointers into the old slot never dangle.
		auto result = assets.LoadModel(path);
		if (!result)
		{
			AE_WARN(LogCategory::App, "ModelSpawn: reload of '{}' failed: {}", path, result.error());
			return false;
		}
		ctx.loadedModels.push_back(std::move(result.value()));
		ctx.loadedModelMap[path] = ctx.loadedModels.size() - 1;

		RegisterModelAssets(db, assets, ctx, path);
		db.TouchByPath(path);
		return true;
	}
} // namespace aether::app::scene
