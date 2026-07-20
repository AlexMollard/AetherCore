#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include <entt/entt.hpp>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "material/EffectManager.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsSystem.hpp"
#include "rendering/Renderer.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"
#include "scripting/SceneContext.hpp"
#include "ui/UiComponents.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

#include "scene/SceneSerializerDetail.hpp"

namespace aether::app::scene
{
	using namespace detail;

	namespace
	{
		template<typename T>
		void RemoveIf(World& world, Entity entity)
		{
			if (world.Has<T>(entity))
			{
				world.Remove<T>(entity);
			}
		}

		void ClearTags(World& world, Entity entity)
		{
			ForEachTag(
			        [&](const std::string&, std::uint32_t tagId)
			        {
				        if (TagHas(&world, entity.id, tagId))
				        {
					        TagRemove(&world, entity.id, tagId);
				        }
			        });
		}

		void ResetRestorableEntity(World& world, Entity entity)
		{
			ecs::DetachFromParent(world, entity);
			ClearTags(world, entity);

			RemoveIf<NameComponent>(world, entity);
			RemoveIf<TransformComponent>(world, entity);
			RemoveIf<HierarchyComponent>(world, entity);
			RemoveIf<MeshComponent>(world, entity);
			RemoveIf<MeshSourceComponent>(world, entity);
			RemoveIf<MaterialComponent>(world, entity);
			RemoveIf<MaterialInstanceComponent>(world, entity);
			RemoveIf<SkinnedMeshComponent>(world, entity);
			RemoveIf<JointComponent>(world, entity);
			RemoveIf<CollisionEventsComponent>(world, entity);
			RemoveIf<ColliderComponent>(world, entity);
			RemoveIf<RigidBodyComponent>(world, entity);
			RemoveIf<PhysicsStateComponent>(world, entity);
			RemoveIf<Joint2DComponent>(world, entity);
			RemoveIf<CollisionEvents2DComponent>(world, entity);
			RemoveIf<Collider2DComponent>(world, entity);
			RemoveIf<RigidBody2DComponent>(world, entity);
			RemoveIf<Physics2DStateComponent>(world, entity);
			RemoveIf<ui::UICanvas>(world, entity);
			RemoveIf<ui::UIRect>(world, entity);
			RemoveIf<ui::UIImage>(world, entity);
			RemoveIf<ui::UIText>(world, entity);
			RemoveIf<EffectRefComponent>(world, entity);
			RemoveIf<EffectParamsComponent>(world, entity);
			RemoveIf<BobComponent>(world, entity);
			RemoveIf<SpinComponent>(world, entity);
			RemoveIf<OrbitComponent>(world, entity);
			RemoveIf<MaterialPulseComponent>(world, entity);
			RemoveIf<ScalePulseComponent>(world, entity);
			RemoveIf<LookAtComponent>(world, entity);
			RemoveIf<ParallaxComponent>(world, entity);
			RemoveIf<ParticleEmitterComponent>(world, entity);
			RemoveIf<PointLightComponent>(world, entity);
			RemoveIf<SpotLightComponent>(world, entity);
			RemoveIf<DayNightComponent>(world, entity);
			RemoveIf<TileMapComponent>(world, entity);
			RemoveIf<CameraComponent>(world, entity);
			RemoveIf<OrbitCameraComponent>(world, entity);
			RemoveIf<MainCameraComponent>(world, entity);
			RemoveIf<ScriptComponent>(world, entity);
			RemoveIf<MeshRendererComponent>(world, entity);
			RemoveIf<SpriteRendererComponent>(world, entity);
			RemoveIf<SpriteAnimatorComponent>(world, entity);
			RemoveIf<DisabledComponent>(world, entity);
		}

		// Forward decl: override application (below) applies a one-entity mini-scene
		// onto an already-expanded entity, and the expander is called from this core.
		std::vector<Entity> ApplySceneToEntities(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, std::vector<Entity> created, bool registerSceneEntities);

		// Link every expanded entity back to its instance root, stamping the source
		// prefab entity's STABLE guid (created[i] aligns with prefab.entities[i]).
		// Returns a guid -> entity map so overrides route by guid, not by index -
		// reordering the prefab's entities never misaligns existing overrides.
		std::unordered_map<std::uint64_t, Entity> LinkPrefabSubtree(World& world, const std::vector<Entity>& created, Entity root, const SceneDescription& prefab)
		{
			std::unordered_map<std::uint64_t, Entity> byGuid;
			for (std::size_t i = 0; i < created.size(); ++i)
			{
				if (!created[i].IsValid())
				{
					continue;
				}
				const std::uint64_t g = i < prefab.entities.size() ? EffectiveGuid(prefab.entities[i], i) : static_cast<std::uint64_t>(i + 1);
				world.Emplace<PrefabLinkComponent>(created[i], PrefabLinkComponent{.instanceRoot = root, .prefabGuid = g});
				byGuid[g] = created[i];
			}
			return byGuid;
		}

		// Re-apply each per-entity override onto the matching expanded entity (by
		// stable guid). Uses RestoreSubtreeInPlace (the proven undo restore path): it
		// strips the entity, re-applies the override record onto the same handle, and
		// re-attaches it to its original parent - safe to run onto a populated entity
		// (a plain re-apply double-fires component-construct signals). PrefabLink is
		// preserved (not a restorable component), so the entity stays linked.
		void ApplyPrefabOverrides(World& world, const ApplySceneDeps& deps, const std::unordered_map<std::uint64_t, Entity>& byGuid,
		        const std::unordered_map<std::uint64_t, const EntityRecord*>& prefabByGuid, const std::vector<PrefabEntityOverride>& overrides)
		{
			for (const PrefabEntityOverride& ov: overrides)
			{
				const auto it = byGuid.find(ov.guid);
				if (it == byGuid.end() || !it->second.IsValid())
				{
					continue;
				}
				const auto pit = prefabByGuid.find(ov.guid);
				if (pit == prefabByGuid.end() || pit->second == nullptr)
				{
					continue; // override references a guid no longer in the prefab
				}
				const Entity target = it->second;
				Entity parent{};
				if (const auto* h = world.TryGet<HierarchyComponent>(target))
				{
					parent = h->parent;
				}
				// Merge the instance's changed keys onto a fresh copy of the *current*
				// prefab record, so un-overridden keys (incl. later prefab edits) win.
				EntityRecord merged = MergePrefabOverride(*pit->second, ov.partialToml);
				merged.parentIndex = -1;
				merged.entityId = target.id;
				SceneDescription mini;
				mini.entities.push_back(std::move(merged));
				RestoreSubtreeInPlace(mini, world, deps, parent);
			}
		}

		// Expand every linked prefab instance in `scene` into live entities. Each
		// instance root carries a PrefabInstanceComponent (so capture re-emits the
		// reference) and a SceneTransientComponent (so the expanded subtree is never
		// written back as flat entities); the whole subtree is PrefabLink-tagged and
		// per-entity overrides are re-applied.
		void ExpandPrefabInstances(const SceneDescription& scene, World& world, const ApplySceneDeps& deps)
		{
			for (const PrefabInstanceRecord& rec: scene.prefabInstances)
			{
				if (rec.prefabPath.empty())
				{
					continue;
				}
				std::optional<SceneDescription> prefab = ReadPrefabFile(rec.prefabPath);
				if (!prefab)
				{
					AE_WARN(LogCategory::App, "Prefab instance references missing prefab '{}'", rec.prefabPath);
					continue;
				}
				const glm::mat4 xform = ComposeTransform(rec.position, rec.eulerDeg, rec.scale);
				std::vector<Entity> created;
				const Entity root = InstantiatePrefab(*prefab, world, deps, xform, &created);
				if (!root.IsValid())
				{
					continue;
				}
				world.Emplace<PrefabInstanceComponent>(root, PrefabInstanceComponent{.prefabPath = rec.prefabPath});
				world.Emplace<SceneTransientComponent>(root);
				if (!rec.name.empty())
				{
					if (auto* nc = world.TryGet<NameComponent>(root))
					{
						nc->name = rec.name;
					}
				}
				const auto byGuid = LinkPrefabSubtree(world, created, root, *prefab);

				// Remove prefab entities that were deleted in this instance. Never remove
				// the instance root itself: it maps to the prefab's root entity, and an
				// earlier capture bug could write the root's guid into `removed`; deleting
				// it would wipe the whole instance.
				for (const std::uint64_t g: rec.removedGuids)
				{
					const auto it = byGuid.find(g);
					if (it != byGuid.end() && it->second != root && world.GetRegistry().valid(World::ToEntt(it->second)))
					{
						ecs::DestroyHierarchy(world, it->second);
					}
				}

				// Map prefab records by stable guid so overrides merge onto the current
				// prefab (field-level: only overridden keys come from the instance).
				std::unordered_map<std::uint64_t, const EntityRecord*> prefabByGuid;
				for (std::size_t i = 0; i < prefab->entities.size(); ++i)
				{
					prefabByGuid[EffectiveGuid(prefab->entities[i], i)] = &prefab->entities[i];
				}
				ApplyPrefabOverrides(world, deps, byGuid, prefabByGuid, rec.overrides);

				// Re-create instance-local added entities; their roots attach to the
				// instance root (which is SceneTransient, so they capture as added again).
				if (!rec.addedEntities.empty())
				{
					SceneDescription addScene;
					addScene.entities = rec.addedEntities;
					std::vector<Entity> addCreated;
					addCreated.reserve(addScene.entities.size());
					for (std::size_t i = 0; i < addScene.entities.size(); ++i)
					{
						addCreated.push_back(world.Create());
					}
					ApplySceneToEntities(addScene, world, deps, addCreated, false);
					for (std::size_t i = 0; i < addScene.entities.size() && i < addCreated.size(); ++i)
					{
						if (addScene.entities[i].parentIndex < 0)
						{
							ecs::SetParent(world, addCreated[i], root);
						}
					}
				}
			}
		}

		std::vector<Entity> ApplySceneToEntities(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, std::vector<Entity> created, bool registerSceneEntities)
		{
			if (deps.assetDatabase != nullptr)
			{
				for (const AssetManifestEntry& a: scene.assetManifest)
				{
					AssetSource src;
					src.type = (a.type == "mesh") ? AssetType::Mesh : AssetType::Texture;
					src.path = a.path;
					src.subIndex = a.subIndex;
					src.builtin = a.builtin;
					deps.assetDatabase->Register(src);
				}
			}

			if (deps.renderer != nullptr && scene.environment)
			{
				const EnvironmentRecord& env = *scene.environment;
				deps.renderer->SetAmbientLight(env.ambient);
				deps.renderer->SetDirectionalLight(env.sunDirection, env.sunIntensity);
				deps.renderer->SetSunColor(env.sunColor);
				deps.renderer->SetSkyGradient(env.skyHorizon, env.skyZenith);
				deps.renderer->SetSkyVoidColor(env.skyVoid);
			}

			std::size_t behaviorCount = 0;
			std::size_t effectCount = 0;

			// Feature reconcile: records imply scene features (old files predate
			// some flags; hand-edited files may disagree). Every feature is
			// legal in every scene kind (Unity-style); the only domain rule is
			// per-entity - one entity never simulates 2D and 3D physics at once.
			SceneFeatureFlags impliedFeatures = SceneFeatureFlags::None;

			for (std::size_t i = 0; i < scene.entities.size(); ++i)
			{
				const EntityRecord& rec = scene.entities[i];
				const Entity e = created[i];

				bool apply2DPhysics = rec.rigidBody2D || rec.collider2D || rec.joint2D;
				bool apply3DPhysics = rec.physics.has_value() || rec.joint.has_value();
				if (apply2DPhysics && apply3DPhysics)
				{
					// One domain per entity; keep the one matching the scene kind.
					if (scene.kind == SceneKind::Scene2D)
					{
						apply3DPhysics = false;
					}
					else
					{
						apply2DPhysics = false;
					}
					AE_WARN(LogCategory::App, "Scene load: entity '{}' has both 2D and 3D physics records; keeping the {} set", rec.name, apply2DPhysics ? "2D" : "3D");
				}
				if (apply3DPhysics)
				{
					impliedFeatures |= SceneFeatureFlags::Physics3D;
				}
				if (apply2DPhysics)
				{
					impliedFeatures |= SceneFeatureFlags::Physics2D;
				}
				if (rec.sprite || rec.spriteAnimator)
				{
					impliedFeatures |= SceneFeatureFlags::Sprites;
				}
				if (rec.mesh || rec.skinned)
				{
					impliedFeatures |= SceneFeatureFlags::Meshes3D;
				}
				if (rec.pointLight || rec.spotLight)
				{
					impliedFeatures |= SceneFeatureFlags::Lighting3D;
				}

				if (!rec.name.empty())
				{
					world.Emplace<NameComponent>(e, NameComponent{.name = rec.name});
				}
				for (const std::string& tag: rec.tags)
				{
					const std::uint32_t tagId = TagCreate(tag);
					if (tagId != UINT32_MAX)
					{
						TagAdd(&world, e.id, tagId);
					}
				}
				if (rec.disabled)
				{
					world.EmplaceOrReplace<DisabledComponent>(e);
				}
				if (rec.sprite)
				{
					world.EmplaceOrReplace<SpriteRendererComponent>(e, *rec.sprite);
				}
				if (rec.spriteAnimator)
				{
					world.EmplaceOrReplace<SpriteAnimatorComponent>(e, *rec.spriteAnimator);
				}
				if (rec.meshRenderer)
				{
					world.EmplaceOrReplace<MeshRendererComponent>(e, MeshRendererComponent{.visible = rec.meshRendererVisible, .castShadows = rec.meshRendererCastShadows});
				}
				if (rec.hasTransform)
				{
					world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = ComposeTransform(rec.position, rec.eulerDeg, rec.scale)});
				}

				if (rec.physics && apply3DPhysics)
				{
					const PhysicsRecord& phys = *rec.physics;
					world.Emplace<ColliderComponent>(e,
					        ColliderComponent{
					                .shape = phys.shapeType,
					                .halfExtents = phys.halfExtents,
					                .radius = phys.radius,
					                .halfHeight = phys.halfHeight,
					                .center = phys.center,
					                .friction = phys.friction,
					                .restitution = phys.restitution,
					                .isSensor = phys.isSensor,
					                .layer = phys.isSensor ? PhysicsLayer::Sensor : PhysicsLayer::Moving,
					        });
					world.Emplace<RigidBodyComponent>(e,
					        RigidBodyComponent{
					                .motionType = phys.motionType,
					                .mass = phys.mass,
					                .linearDamping = phys.linearDamping,
					                .angularDamping = phys.angularDamping,
					                .gravityFactor = phys.gravityFactor,
					                .maxLinearVelocity = phys.maxLinearVelocity,
					                .maxAngularVelocity = phys.maxAngularVelocity,
					                .continuousCollision = phys.continuousCollision,
					                .allowSleeping = phys.allowSleeping,
					                .lockPosition = phys.lockPosition,
					                .lockRotation = phys.lockRotation,
					        });
				}
				if (rec.joint && apply3DPhysics)
				{
					const JointRecord& jr = *rec.joint;
					Entity targetEntity{};
					if (jr.targetIndex >= 0 && jr.targetIndex < static_cast<int>(created.size()))
					{
						targetEntity = created[static_cast<std::size_t>(jr.targetIndex)];
					}
					world.Emplace<JointComponent>(e,
					        JointComponent{
					                .type = jr.type,
					                .target = targetEntity,
					                .anchor = jr.anchor,
					                .axis = jr.axis,
					                .minLimit = jr.minLimit,
					                .maxLimit = jr.maxLimit,
					                .distance = jr.distance,
					                .collideConnected = jr.collideConnected,
					        });
				}
				if (rec.rigidBody2D && apply2DPhysics)
				{
					world.EmplaceOrReplace<RigidBody2DComponent>(e, *rec.rigidBody2D);
				}
				if (rec.collider2D && apply2DPhysics)
				{
					world.EmplaceOrReplace<Collider2DComponent>(e, *rec.collider2D);
				}
				if (rec.joint2D && apply2DPhysics)
				{
					Joint2DComponent component = *rec.joint2D;
					if (rec.joint2DTargetIndex >= 0 && rec.joint2DTargetIndex < static_cast<int>(created.size()))
					{
						component.target = created[static_cast<std::size_t>(rec.joint2DTargetIndex)];
					}
					world.EmplaceOrReplace<Joint2DComponent>(e, component);
				}

				if (rec.uiCanvas)
				{
					world.Emplace<ui::UICanvas>(e, ui::UICanvas{static_cast<ui::UICanvas::ScaleMode>(rec.uiCanvas->scaleMode), rec.uiCanvas->referenceResolution, rec.uiCanvas->sortBias});
				}
				if (rec.uiRect)
				{
					world.Emplace<ui::UIRect>(e, ui::UIRect{rec.uiRect->anchorMin, rec.uiRect->anchorMax, rec.uiRect->offsetMin, rec.uiRect->offsetMax, rec.uiRect->pivot, glm::vec4{0.f}});
				}
				if (rec.uiImage)
				{
					ui::UIImage im;
					im.color = rec.uiImage->color;
					im.cornerRadius = rec.uiImage->cornerRadius;
					im.pixelArt = rec.uiImage->pixelArt;
					if (!rec.uiImage->texturePath.empty() && deps.assets != nullptr)
					{
						im.texture = deps.assets->GetTextureRegistry().Acquire(rec.uiImage->texturePath);
						if (deps.assetDatabase != nullptr)
						{
							deps.assetDatabase->Register(MakeTextureSource(rec.uiImage->texturePath));
						}
					}
					world.Emplace<ui::UIImage>(e, im);
				}
				if (rec.uiText)
				{
					world.Emplace<ui::UIText>(
					        e, ui::UIText{rec.uiText->text, rec.uiText->fontName, rec.uiText->pixelSize, rec.uiText->color, static_cast<ui::UIText::HAlign>(rec.uiText->hAlign), static_cast<ui::UIText::VAlign>(rec.uiText->vAlign), rec.uiText->wrap});
				}

				if (rec.mesh)
				{
					const Mesh* resolved = nullptr;
					LoadedModel* model = nullptr;
					if (rec.mesh->kind == MeshSourceComponent::Kind::Primitive)
					{
						if (deps.assets != nullptr && deps.primitives != nullptr)
						{
							if (const auto prim = PrimitiveFromName(rec.mesh->path))
							{
								resolved = &deps.primitives->Get(*prim);
							}
						}
					}
					else if (deps.sceneContext != nullptr)
					{
						auto& ctx = *deps.sceneContext;
						if (const auto it = ctx.loadedModelMap.find(rec.mesh->path); it != ctx.loadedModelMap.end())
						{
							model = &ctx.loadedModels[it->second];
						}
						else if (deps.assets != nullptr)
						{
							auto result = deps.assets->LoadModel(rec.mesh->path);
							if (!result && deps.ensureModelBaked)
							{
								std::string bakeError;
								if (deps.ensureModelBaked(rec.mesh->path, bakeError))
								{
									AE_INFO(LogCategory::App, "Scene load: auto-imported model '{}'", rec.mesh->path);
									result = deps.assets->LoadModel(rec.mesh->path);
								}
								else
								{
									AE_WARN(LogCategory::App, "Scene load: auto-import of model '{}' failed: {}", rec.mesh->path, bakeError);
								}
							}
							if (result)
							{
								ctx.loadedModels.push_back(std::move(result.value()));
								ctx.loadedModelMap[rec.mesh->path] = ctx.loadedModels.size() - 1;
								model = &ctx.loadedModels.back();
							}
							else
							{
								AE_WARN(LogCategory::App, "Scene load: model '{}' failed: {}", rec.mesh->path, result.error());
							}
						}
						if (model != nullptr && rec.mesh->primitiveIndex < model->primitives.size())
						{
							resolved = &model->primitives[rec.mesh->primitiveIndex].mesh;
						}
					}

					if (resolved != nullptr)
					{
						world.Emplace<MeshComponent>(e, MeshComponent{.mesh = resolved});
						world.Emplace<MeshSourceComponent>(e, *rec.mesh);
						if (deps.assetDatabase != nullptr)
						{
							deps.assetDatabase->Register(rec.mesh->kind == MeshSourceComponent::Kind::Primitive ? MakePrimitiveMeshSource(rec.mesh->path) : MakeModelMeshSource(rec.mesh->path, static_cast<int>(rec.mesh->primitiveIndex)));
						}
					}
					else
					{
						AE_WARN(LogCategory::App, "Scene load: mesh source '{}' unresolved for '{}'", rec.mesh->path, rec.name);
					}

					if (rec.skinned && model != nullptr && model->animationDb.IsValid())
					{
						const auto& primitive = model->primitives[rec.mesh->primitiveIndex];
						if (primitive.skinIndex >= 0)
						{
							const auto skinIdx = static_cast<std::uint32_t>(primitive.skinIndex);
							const std::uint32_t joints = model->animationDb.GetSkinJointCount(skinIdx);
							const std::uint32_t clipCount = model->animationDb.GetClipCount();
							SkinnedMeshComponent smc{};
							smc.animDb = &model->animationDb;
							smc.skinIndex = skinIdx;
							smc.jointCount = joints;
							smc.clipIndex = clipCount > 0 ? std::min(rec.skinned->clipIndex, clipCount - 1) : 0;
							smc.animTime = rec.skinned->animTime;
							smc.playbackSpeed = rec.skinned->playbackSpeed;
							smc.looping = rec.skinned->looping;
							world.EmplaceOrReplace<SkinnedMeshComponent>(e, smc);
						}
					}
				}

				if (rec.material && deps.assets != nullptr)
				{
					MaterialAsset asset = rec.material->asset;
					TextureRegistry& textures = deps.assets->GetTextureRegistry();
					const auto acquire = [&textures](const std::string& path, TextureHandle& out)
					{
						out = path.empty() ? TextureHandle{} : textures.Acquire(path);
					};
					acquire(rec.material->albedoPath, asset.albedoTex);
					acquire(rec.material->normalPath, asset.normalTex);
					acquire(rec.material->metallicRoughnessPath, asset.metallicRoughnessTex);
					acquire(rec.material->occlusionPath, asset.occlusionTex);
					acquire(rec.material->emissivePath, asset.emissiveTex);
					if (deps.assetDatabase != nullptr)
					{
						for (const std::string& texPath: {rec.material->albedoPath, rec.material->normalPath, rec.material->metallicRoughnessPath, rec.material->occlusionPath, rec.material->emissivePath})
						{
							if (!texPath.empty())
							{
								deps.assetDatabase->Register(MakeTextureSource(texPath));
							}
						}
					}
					MaterialSystem::AssignMaterial(world, e, deps.assets->GetMaterialRegistry(), deps.assets->GetPipelineCache(), asset);
					for (const TextureHandle h: {asset.albedoTex, asset.normalTex, asset.metallicRoughnessTex, asset.occlusionTex, asset.emissiveTex})
					{
						if (h.IsValid())
						{
							textures.Release(h);
						}
					}
				}

				if (rec.effect && !rec.effect->name.empty() && deps.effectManager != nullptr && deps.effectParams != nullptr && deps.pipelines != nullptr)
				{
					if (effects::ApplyEntityEffect(world, e, rec.effect->name, *deps.effectManager, *deps.pipelines, *deps.effectParams, &rec.effect->params))
					{
						++effectCount;
					}
					else
					{
						AE_WARN(LogCategory::App, "Scene load: unknown effect '{}'", rec.effect->name);
					}
				}
				else if (rec.effect)
				{
					AE_WARN(LogCategory::App, "Scene load: effect '{}' on '{}' skipped (missing effect deps)", rec.effect->name, rec.name);
				}

				// Pure data-only components, applied generically: emplace a default
				// (so runtime fields start clean) then set the reflected fields.
				for (const GenericComponent& generic: rec.reflected)
				{
					const reflect::ComponentType* ct = reflect::FindComponentType(generic.type);
					if (ct == nullptr || ct->emplaceDefault == nullptr)
					{
						continue;
					}
					void* comp = ct->emplaceDefault(world, e);
					if (comp == nullptr)
					{
						continue;
					}
					for (const auto& [fieldName, value]: generic.fields)
					{
						if (const reflect::FieldDesc* f = ct->FindField(fieldName))
						{
							f->set(comp, value);
						}
					}
					if (ct->postSet)
					{
						ct->postSet(world, e);
					}
					++behaviorCount;
				}
				if (rec.particles)
				{
					world.Emplace<ParticleEmitterComponent>(e, *rec.particles);
					++behaviorCount;
				}
				if (rec.pointLight)
				{
					world.Emplace<PointLightComponent>(e, *rec.pointLight);
				}
				if (rec.spotLight)
				{
					world.Emplace<SpotLightComponent>(e, *rec.spotLight);
				}
				if (rec.dayNight)
				{
					world.Emplace<DayNightComponent>(e, *rec.dayNight);
					impliedFeatures |= SceneFeatureFlags::Lighting3D;
				}
				if (rec.tileMap)
				{
					world.EmplaceOrReplace<TileMapComponent>(e, *rec.tileMap);
					impliedFeatures |= SceneFeatureFlags::Tilemaps;
				}
				if (rec.camera)
				{
					world.Emplace<CameraComponent>(e, *rec.camera);
					if (rec.mainCamera)
					{
						ecs::SetMainCameraEntity(world, e);
					}
				}
				if (rec.orbitCamera)
				{
					world.EmplaceOrReplace<OrbitCameraComponent>(e, *rec.orbitCamera);
				}
				if (!rec.scripts.empty())
				{
					ScriptComponent component;
					component.scripts.reserve(rec.scripts.size());
					for (const ScriptRecord& script: rec.scripts)
					{
						component.scripts.push_back(ScriptEntry{.path = script.type, .attached = false, .properties = ScriptPropsFromSceneRefs(script.properties, created)});
					}
					world.Emplace<ScriptComponent>(e, std::move(component));
				}
			}

			std::vector<Entity> migratedLights;
			for (const LightRecord& light: scene.lights)
			{
				if (light.isSpot)
				{
					migratedLights.push_back(ecs::CreateSpotLightEntity(world,
					        light.position,
					        light.direction,
					        SpotLightComponent{.color = light.color, .intensity = light.intensity, .radius = light.radius, .innerAngleRad = light.innerAngleRad, .outerAngleRad = light.outerAngleRad, .castsShadow = light.castsShadow}));
				}
				else
				{
					migratedLights.push_back(ecs::CreatePointLightEntity(world, light.position, PointLightComponent{.color = light.color, .intensity = light.intensity, .radius = light.radius, .castsShadow = light.castsShadow}));
				}
			}
			if (!migratedLights.empty())
			{
				AE_INFO(LogCategory::App, "Scene load: migrated {} legacy light record(s) to light entities - re-save to upgrade the file", migratedLights.size());
			}

			for (std::size_t i = 0; i < scene.entities.size(); ++i)
			{
				const int parent = scene.entities[i].parentIndex;
				if (parent >= 0 && static_cast<std::size_t>(parent) < created.size())
				{
					ecs::SetParent(world, created[i], created[static_cast<std::size_t>(parent)]);
				}
			}

			if (deps.sceneContext != nullptr && registerSceneEntities)
			{
				for (const Entity e: created)
				{
					deps.sceneContext->sceneEntities.push_back(e);
				}
				for (const Entity e: migratedLights)
				{
					deps.sceneContext->sceneEntities.push_back(e);
				}
			}
			if (!migratedLights.empty())
			{
				impliedFeatures |= SceneFeatureFlags::Lighting3D;
			}
			const auto missingImplied = static_cast<SceneFeatureFlags>(static_cast<std::uint32_t>(impliedFeatures) & ~static_cast<std::uint32_t>(world.GetSceneFeatures()));
			if (missingImplied != SceneFeatureFlags::None)
			{
				world.SetSceneFeatures(world.GetSceneFeatures() | missingImplied);
			}

			// Expand linked prefab instances into live (SceneTransient) subtrees. Done
			// last so instance children never collide with the scene's own entities,
			// and shared by every apply path (load, restore, undo) via this core.
			ExpandPrefabInstances(scene, world, deps);

			AE_INFO(LogCategory::App, "Scene apply: {} entities, {} behaviors, {} effects, {} prefab instances (format v{})", created.size() + migratedLights.size(), behaviorCount, effectCount, scene.prefabInstances.size(), scene.version);
			return created;
		}
	} // namespace

	std::vector<Entity> ApplyScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps)
	{
		world.SetSceneKind(scene.kind);
		world.SetSceneFeatures(scene.features);
		std::vector<Entity> created;
		created.reserve(scene.entities.size());
		for (std::size_t i = 0; i < scene.entities.size(); ++i)
		{
			created.push_back(world.Create());
		}
		return ApplySceneToEntities(scene, world, deps, std::move(created), true);
	}

	std::vector<Entity> RestoreSceneInPlace(const SceneDescription& scene, World& world, const ApplySceneDeps& deps)
	{
		world.SetSceneKind(scene.kind);
		world.SetSceneFeatures(scene.features);
		if (deps.physics != nullptr)
		{
			deps.physics->WaitForStepIdle();
		}

		auto& reg = world.GetRegistry();
		std::vector<Entity> targets;
		targets.reserve(scene.entities.size());
		std::unordered_set<std::uint32_t> restoredIds;
		for (const EntityRecord& rec: scene.entities)
		{
			Entity target{rec.entityId};
			if (!target.IsValid() || !reg.valid(World::ToEntt(target)))
			{
				target = world.Create();
			}
			restoredIds.insert(target.id);
			targets.push_back(target);
		}

		std::vector<Entity> doomed;
		for (const auto handle: reg.storage<entt::entity>())
		{
			if (!reg.valid(handle))
			{
				continue;
			}
			const Entity e = World::FromEntt(handle);
			if (e.IsValid() && !restoredIds.contains(e.id))
			{
				doomed.push_back(e);
			}
		}
		for (const Entity e: doomed)
		{
			if (reg.valid(World::ToEntt(e)))
			{
				ecs::DetachFromParent(world, e);
				world.Destroy(e);
			}
		}

		for (const Entity e: targets)
		{
			if (reg.valid(World::ToEntt(e)))
			{
				ResetRestorableEntity(world, e);
			}
		}

		if (deps.sceneContext != nullptr)
		{
			deps.sceneContext->sceneEntities.clear();
		}
		std::vector<Entity> restored = targets;
		ApplySceneToEntities(scene, world, deps, std::move(targets), true);
		return restored;
	}

	std::vector<Entity> RestoreSubtreeInPlace(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, Entity attachParent)
	{
		if (deps.physics != nullptr)
		{
			deps.physics->WaitForStepIdle();
		}
		auto& reg = world.GetRegistry();

		// Reuse each record's original id where it is still live; recreate the rest
		// under the same id so references (selection, parenting, component refs) hold.
		std::vector<Entity> targets;
		targets.reserve(scene.entities.size());
		for (const EntityRecord& rec: scene.entities)
		{
			Entity target{rec.entityId};
			if (!target.IsValid() || !reg.valid(World::ToEntt(target)))
			{
				target = world.CreateWithId(Entity{rec.entityId});
			}
			targets.push_back(target);
		}

		// Clear the reused entities back to a blank slate before re-applying.
		for (const Entity target: targets)
		{
			if (reg.valid(World::ToEntt(target)))
			{
				ResetRestorableEntity(world, target);
			}
		}

		ApplySceneToEntities(scene, world, deps, targets, false);

		// Re-attach the subtree roots (parentIndex < 0) to their live parent. Internal
		// parenting was already resolved by ApplySceneToEntities via parentIndex.
		for (std::size_t i = 0; i < scene.entities.size() && i < targets.size(); ++i)
		{
			if (scene.entities[i].parentIndex >= 0 || !reg.valid(World::ToEntt(targets[i])))
			{
				continue;
			}
			if (attachParent.IsValid() && reg.valid(World::ToEntt(attachParent)))
			{
				ecs::SetParent(world, targets[i], attachParent);
			}
		}
		return targets;
	}

	void ReplaceScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, SceneLoadMode mode)
	{
		world.SetSceneKind(scene.kind);
		world.SetSceneFeatures(scene.features);
		// removal must not race the async physics step.
		if (deps.physics != nullptr)
		{
			deps.physics->WaitForStepIdle();
		}
		auto& reg = world.GetRegistry();
		std::vector<Entity> doomed;
		std::vector<Entity> spared;
		for (const auto handle: reg.storage<entt::entity>())
		{
			if (reg.valid(handle))
			{
				const Entity e = World::FromEntt(handle);
				if (!e.IsValid())
				{
					continue;
				}
				// Persistent subtrees survive gameplay switches only; authoring
				// loads reset the world completely.
				if (mode == SceneLoadMode::GameplaySwitch && ecs::HasSceneTransientAncestor(world, e))
				{
					spared.push_back(e);
				}
				else
				{
					doomed.push_back(e);
				}
			}
		}
		for (const Entity e: doomed)
		{
			if (reg.valid(World::ToEntt(e)))
			{
				world.Destroy(e);
			}
		}
		if (deps.sceneContext != nullptr)
		{
			deps.sceneContext->sceneEntities.clear();
			for (const Entity e: spared)
			{
				deps.sceneContext->sceneEntities.push_back(e);
			}
		}

		ApplyScene(scene, world, deps);
	}

	bool LoadSceneFile(const std::string& sceneName, World& world, const ApplySceneDeps& deps, SceneLoadMode mode)
	{
		const auto scene = ReadSceneFile(sceneName);
		if (!scene)
		{
			return false;
		}
		ReplaceScene(*scene, world, deps, mode);
		AE_INFO(LogCategory::App, "Scene loaded: {} ({} entities)", sceneName, scene->entities.size());
		return true;
	}

	Entity InstantiatePrefab(const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld, std::vector<Entity>* outCreated)
	{
		// Instantiating a prefab must NOT redefine the scene's domain. A prefab is
		// serialised as a mini-scene whose `kind` defaults to Scene3D; ApplyScene
		// otherwise assigns that kind to the world, so spawning a prefab from a
		// script at runtime silently flips a 2D world to 3D. That breaks every
		// kind-dependent path (2D grid/tile painting in the editor, and any
		// gameplay/render logic that keys off the world kind). Preserve the world's
		// existing kind and only ever *add* the features the prefab's entities imply.
		const SceneKind savedKind = world.GetSceneKind();
		const SceneFeatureFlags savedFeatures = world.GetSceneFeatures();
		std::vector<Entity> created = ApplyScene(prefab, world, deps);
		world.SetSceneKind(savedKind);
		world.SetSceneFeatures(savedFeatures | world.GetSceneFeatures());

		Entity root = created.empty() ? Entity{} : created.front();
		for (std::size_t i = 0; i < prefab.entities.size() && i < created.size(); ++i)
		{
			if (prefab.entities[i].parentIndex < 0)
			{
				ecs::SetWorldTransform(world, created[i], localToWorld);
				root = created[i];
				break;
			}
		}
		if (outCreated != nullptr)
		{
			*outCreated = std::move(created);
		}
		return root;
	}

	Entity InstantiatePrefabInstance(const std::string& prefabName, const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld)
	{
		std::vector<Entity> created;
		const Entity root = InstantiatePrefab(prefab, world, deps, localToWorld, &created);
		if (!root.IsValid())
		{
			return root;
		}
		world.Emplace<PrefabInstanceComponent>(root, PrefabInstanceComponent{.prefabPath = prefabName});
		world.Emplace<SceneTransientComponent>(root);
		LinkPrefabSubtree(world, created, root, prefab);
		return root;
	}
} // namespace aether::app::scene
