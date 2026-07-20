// Custom scene serde for Mesh Source (+ Skinned Mesh). Apply resolves the mesh - a
// primitive, or a model loaded/auto-baked through the deps and cached on the scene
// context - and the skinned setup reads the loaded model's animation database, so the
// two are one coupled unit here (Skinned needs the model that Mesh just loaded). Order
// 10 keeps it before the material serde so the mesh renderer exists first.

#include "scene/SceneComponentSerde.hpp"

#include <algorithm>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "scene/SceneSerializerDetail.hpp"
#include "utils/Logger.hpp"

namespace aether::app::scene
{
	using namespace detail;

	namespace
	{
		void CaptureMesh(SceneCaptureContext& c)
		{
			if (const auto* ms = c.world.TryGet<MeshSourceComponent>(c.entity))
			{
				c.rec.mesh = *ms;
			}
			if (const auto* smc = c.world.TryGet<SkinnedMeshComponent>(c.entity))
			{
				c.rec.skinned = SkinnedRecord{.clipIndex = smc->clipIndex, .animTime = smc->animTime, .playbackSpeed = smc->playbackSpeed, .looping = smc->looping};
			}
		}

		void ApplyMesh(SceneApplyContext& c)
		{
			if (!c.rec.mesh)
			{
				return;
			}
			World& world = c.world;
			const Entity e = c.entity;
			const ApplySceneDeps& deps = c.deps;

			const Mesh* resolved = nullptr;
			LoadedModel* model = nullptr;
			if (c.rec.mesh->kind == MeshSourceComponent::Kind::Primitive)
			{
				if (deps.assets != nullptr && deps.primitives != nullptr)
				{
					if (const auto prim = PrimitiveFromName(c.rec.mesh->path))
					{
						resolved = &deps.primitives->Get(*prim);
					}
				}
			}
			else if (deps.sceneContext != nullptr)
			{
				auto& ctx = *deps.sceneContext;
				if (const auto it = ctx.loadedModelMap.find(c.rec.mesh->path); it != ctx.loadedModelMap.end())
				{
					model = &ctx.loadedModels[it->second];
				}
				else if (deps.assets != nullptr)
				{
					auto result = deps.assets->LoadModel(c.rec.mesh->path);
					if (!result && deps.ensureModelBaked)
					{
						std::string bakeError;
						if (deps.ensureModelBaked(c.rec.mesh->path, bakeError))
						{
							AE_INFO(LogCategory::App, "Scene load: auto-imported model '{}'", c.rec.mesh->path);
							result = deps.assets->LoadModel(c.rec.mesh->path);
						}
						else
						{
							AE_WARN(LogCategory::App, "Scene load: auto-import of model '{}' failed: {}", c.rec.mesh->path, bakeError);
						}
					}
					if (result)
					{
						ctx.loadedModels.push_back(std::move(result.value()));
						ctx.loadedModelMap[c.rec.mesh->path] = ctx.loadedModels.size() - 1;
						model = &ctx.loadedModels.back();
					}
					else
					{
						AE_WARN(LogCategory::App, "Scene load: model '{}' failed: {}", c.rec.mesh->path, result.error());
					}
				}
				if (model != nullptr && c.rec.mesh->primitiveIndex < model->primitives.size())
				{
					resolved = &model->primitives[c.rec.mesh->primitiveIndex].mesh;
				}
			}

			if (resolved != nullptr)
			{
				world.Emplace<MeshComponent>(e, MeshComponent{.mesh = resolved});
				world.Emplace<MeshSourceComponent>(e, *c.rec.mesh);
				if (deps.assetDatabase != nullptr)
				{
					deps.assetDatabase->Register(c.rec.mesh->kind == MeshSourceComponent::Kind::Primitive ? MakePrimitiveMeshSource(c.rec.mesh->path) : MakeModelMeshSource(c.rec.mesh->path, static_cast<int>(c.rec.mesh->primitiveIndex)));
				}
			}
			else
			{
				AE_WARN(LogCategory::App, "Scene load: mesh source '{}' unresolved for '{}'", c.rec.mesh->path, c.rec.name);
			}

			if (c.rec.skinned && model != nullptr && model->animationDb.IsValid())
			{
				const auto& primitive = model->primitives[c.rec.mesh->primitiveIndex];
				if (primitive.skinIndex >= 0)
				{
					const auto skinIdx = static_cast<std::uint32_t>(primitive.skinIndex);
					const std::uint32_t joints = model->animationDb.GetSkinJointCount(skinIdx);
					const std::uint32_t clipCount = model->animationDb.GetClipCount();
					SkinnedMeshComponent smc{};
					smc.animDb = &model->animationDb;
					smc.skinIndex = skinIdx;
					smc.jointCount = joints;
					smc.clipIndex = clipCount > 0 ? std::min(c.rec.skinned->clipIndex, clipCount - 1) : 0;
					smc.animTime = c.rec.skinned->animTime;
					smc.playbackSpeed = c.rec.skinned->playbackSpeed;
					smc.looping = c.rec.skinned->looping;
					world.EmplaceOrReplace<SkinnedMeshComponent>(e, smc);
				}
			}
		}

		AE_SCENE_SERDE(Mesh, "Mesh Source", 10, CaptureMesh, ApplyMesh)
	} // namespace
} // namespace aether::app::scene
