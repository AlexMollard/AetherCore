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

#include "scene/SceneComponentSerde.hpp"
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
			RemoveIf<Physics2DStateComponent>(world, entity);
			RemoveIf<ui::UIImage>(world, entity);
			RemoveIf<EffectRefComponent>(world, entity);
			RemoveIf<EffectParamsComponent>(world, entity);
			// Every genericSerialize component (Bob/Spin/Orbit/lights/tile map/day night/
			// orbit camera/...) is cleared straight from the registry via the same flag
			// that drives its capture/apply, so a new one never needs a matching RemoveIf
			// line here to be reset before re-apply.
			for (const reflect::ComponentType& ct: reflect::ComponentTypes())
			{
				if (ct.genericSerialize && ct.remove)
				{
					ct.remove(world, entity);
				}
			}
			RemoveIf<CameraComponent>(world, entity);
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

		// True if the record carries any 2D physics: the generic Rigid Body 2D / Collider
		// 2D (any reflected component requiring the Physics2D feature) or the bespoke Joint
		// 2D. Drives the per-entity 2D/3D physics exclusivity tie-break.
		bool RecordHas2DPhysics(const EntityRecord& rec)
		{
			if (rec.joint2D.has_value())
			{
				return true;
			}
			for (const GenericComponent& g: rec.reflected)
			{
				const reflect::ComponentType* ct = reflect::FindComponentType(g.type);
				if (ct != nullptr && (static_cast<std::uint32_t>(ct->requiredFeatures) & static_cast<std::uint32_t>(SceneFeatureFlags::Physics2D)) != 0)
				{
					return true;
				}
			}
			return false;
		}

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

				bool apply2DPhysics = RecordHas2DPhysics(rec);
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

				// Stable scene-node id: keep the one from the .toml, or mint one for legacy scenes that
				// predate node ids, so a subsequent save writes stable parent-by-node references.
				// EmplaceOrReplace (not Emplace) so re-applying onto a reused entity stays idempotent.
				world.EmplaceOrReplace<SceneNodeComponent>(e, SceneNodeComponent{rec.nodeId != 0 ? rec.nodeId : GenerateSceneNodeId()});

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
				// Sprite Renderer / Sprite Animator / Mesh Renderer apply through the serde
				// table (below).
				if (rec.hasTransform)
				{
					world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = ComposeTransform(rec.position, rec.eulerDeg, rec.scale)});
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
					// Physics-domain exclusivity: when this entity's 2D physics lost the
					// tie-break to 3D, skip its 2D-physics components (never emplace both).
					if (!apply2DPhysics && (static_cast<std::uint32_t>(ct->requiredFeatures) & static_cast<std::uint32_t>(SceneFeatureFlags::Physics2D)) != 0)
					{
						continue;
					}
					void* comp = ct->emplaceDefault(world, e);
					if (comp == nullptr)
					{
						continue;
					}
					// A component implies its declared scene features (e.g. a light implies
					// Lighting3D, a tile map implies Tilemaps) - this drives the feature
					// reconcile generically, replacing the old per-component impliedFeatures.
					impliedFeatures |= ct->requiredFeatures;
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
				// Particle Emitter, Point/Spot lights, Day Night, Tile Map and Orbit Camera
				// are applied generically through the rec.reflected loop above. Camera and
				// Scripts apply through the serde table (below).

				// Components with custom serialization (asset resolution, cross-entity refs,
				// merged records) apply through the serde table at the end of the per-entity
				// pass, ordered so dependencies hold (mesh before material, etc.); each is
				// registered next to its component instead of inlined here.
				SceneApplyContext applyCtx{world, e, deps, created, rec, scene.kind, impliedFeatures, apply2DPhysics, apply3DPhysics, behaviorCount, effectCount};
				RunApplySerdes(applyCtx);
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
				// DontDestroyOnLoad subtrees survive gameplay switches only; authoring
				// loads reset the world completely. (SceneTransient alone is NOT enough -
				// prefab-instance roots are SceneTransient but must be re-expanded, not kept.)
				if (mode == SceneLoadMode::GameplaySwitch && ecs::HasDontDestroyOnLoadAncestor(world, e))
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
