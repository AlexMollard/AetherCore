#include "scene/SceneSerializer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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
#include "physics2d/Physics2DSystem.hpp"
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

	namespace detail
	{
		std::unordered_map<std::uint32_t, std::vector<DeferredScriptNodeRef>>& DeferredScriptNodeRefs()
		{
			thread_local std::unordered_map<std::uint32_t, std::vector<DeferredScriptNodeRef>> pending;
			return pending;
		}
	} // namespace detail

	namespace
	{
		// Non-zero while InstantiatePrefab is on the stack. It owns the physics-flush guard
		// (only the outermost instance may build bodies) and doubles as the "this apply is a
		// prefab expansion, not a scene load" signal - a prefab reaches ApplyScene through the
		// same public entry point a scene does, so the apply itself cannot tell them apart.
		thread_local int g_prefabInstantiateDepth = 0;

		// Nesting depth of ApplySceneToEntities. Script refs by scene-node id are resolved when
		// the outermost apply finishes, i.e. once every entity they could name exists.
		thread_local int g_applyDepth = 0;

		// Scenes whose [[includes]] are being applied right now (cycle guard).
		thread_local std::vector<std::string> g_includeStack;

		void ResolveDeferredScriptNodeRefs(World& world)
		{
			auto& pending = DeferredScriptNodeRefs();
			if (pending.empty())
			{
				return;
			}
			std::unordered_map<std::uint64_t, Entity> byNode;
			world.View<SceneNodeComponent>().each([&](entt::entity handle, const SceneNodeComponent& node) { byNode.emplace(node.id, World::FromEntt(handle)); });
			auto& reg = world.GetRegistry();
			for (const auto& [entityId, refs]: pending)
			{
				const Entity e{entityId};
				auto* scripts = reg.valid(World::ToEntt(e)) ? world.TryGet<ScriptComponent>(e) : nullptr;
				if (scripts == nullptr)
				{
					continue;
				}
				for (const DeferredScriptNodeRef& ref: refs)
				{
					if (ref.scriptIndex >= scripts->scripts.size())
					{
						continue;
					}
					ScriptEntry& entry = scripts->scripts[ref.scriptIndex];
					const auto prop = entry.properties.find(ref.property);
					if (prop == entry.properties.end())
					{
						continue;
					}
					if (const auto it = byNode.find(ref.nodeId); it != byNode.end())
					{
						prop->second.i64 = it->second.id;
					}
					else
					{
						AE_WARN(LogCategory::App, "Scene load: script '{}' property '{}' references node {}, which nothing in the loaded scene carries; clearing the reference", entry.path, ref.property, ref.nodeId);
						prop->second.i64 = 0;
					}
				}
			}
			pending.clear();
		}

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
			// Runtime-only marker, so it is never in the snapshot and re-applying cannot overwrite
			// it. Stopping while a text box is mid-edit would otherwise strand it: the sweep that
			// releases it lives in UiTextBoxSystem, which stops ticking the moment Play ends.
			RemoveIf<ui::UIKeyboardCapture>(world, entity);
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
		// outExtraRoots (optional) receives the roots this apply creates beyond `created`:
		// expanded prefab-instance roots and migrated legacy lights.
		std::vector<Entity> ApplySceneToEntities(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, std::vector<Entity> created, bool registerSceneEntities,
		        std::vector<Entity>* outExtraRoots = nullptr);

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

		// Point an override's intra-instance script refs at this instance's entities. The
		// capture stores such a ref as the target's index in the prefab (how the prefab file
		// itself stores it), but the override is applied as a one-entity mini-scene, whose
		// own index space cannot resolve it. `created` is the expansion, aligned with the
		// prefab's entities; a target removed in this instance resolves to no reference.
		void RebindPrefabScriptRefs(World& world, Entity target, const EntityRecord& record, const std::vector<Entity>& created)
		{
			auto* live = world.TryGet<ScriptComponent>(target);
			if (live == nullptr)
			{
				return;
			}
			const auto& reg = world.GetRegistry();
			for (std::size_t s = 0; s < record.scripts.size() && s < live->scripts.size(); ++s)
			{
				const ScriptRecord& script = record.scripts[s];
				for (const auto& [name, value]: script.properties)
				{
					const bool isRef = value.type == ScriptPropertyValue::Type::Entity || value.type == ScriptPropertyValue::Type::Component;
					if (!isRef || value.i64 < 0 || script.nodeRefs.contains(name))
					{
						continue;
					}
					const auto prop = live->scripts[s].properties.find(name);
					if (prop == live->scripts[s].properties.end())
					{
						continue;
					}
					const auto index = static_cast<std::size_t>(value.i64);
					const bool alive = index < created.size() && created[index].IsValid() && reg.valid(World::ToEntt(created[index]));
					prop->second.i64 = alive ? created[index].id : 0;
				}
			}
		}

		// Re-apply each per-entity override onto the matching expanded entity (by
		// stable guid). Uses RestoreSubtreeInPlace (the proven undo restore path): it
		// strips the entity, re-applies the override record onto the same handle, and
		// re-attaches it to its original parent - safe to run onto a populated entity
		// (a plain re-apply double-fires component-construct signals). PrefabLink is
		// preserved (not a restorable component), so the entity stays linked.
		//
		// The instance ROOT's override carries only its non-transform, non-name keys
		// (scripts, tags, components): its transform and name belong to the instance
		// record, and its children must stay attached, so all three are carried across.
		void ApplyPrefabOverrides(World& world, const ApplySceneDeps& deps, Entity root, const std::vector<Entity>& created, const std::unordered_map<std::uint64_t, Entity>& byGuid,
		        const std::unordered_map<std::uint64_t, const EntityRecord*>& prefabByGuid, const std::vector<PrefabEntityOverride>& overrides)
		{
			for (const PrefabEntityOverride& ov: overrides)
			{
				const auto it = byGuid.find(ov.guid);
				if (it == byGuid.end() || !it->second.IsValid() || !world.GetRegistry().valid(World::ToEntt(it->second)))
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
				std::vector<Entity> children;
				if (const auto* h = world.TryGet<HierarchyComponent>(target))
				{
					parent = h->parent;
					children = h->children;
				}
				// Merge the instance's changed keys onto a fresh copy of the *current*
				// prefab record, so un-overridden keys (incl. later prefab edits) win.
				SceneDescription mini;
				mini.entities.push_back(MergePrefabOverride(*pit->second, ov.partialToml));
				EntityRecord& merged = mini.entities.front();
				merged.parentIndex = -1;
				merged.entityId = target.id;
				const bool isRoot = target == root;
				std::optional<glm::mat4> rootPose;
				if (isRoot)
				{
					if (const auto* tc = world.TryGet<TransformComponent>(target))
					{
						rootPose = tc->localToWorld;
						merged.hasTransform = true;
						DecomposeTRS(*rootPose, merged.position, merged.eulerDeg, merged.scale);
					}
					if (const auto* nc = world.TryGet<NameComponent>(target))
					{
						merged.name = nc->name;
					}
				}
				RestoreSubtreeInPlace(mini, world, deps, parent);
				if (auto* tc = world.TryGet<TransformComponent>(target); tc != nullptr && rootPose)
				{
					tc->localToWorld = *rootPose; // exact, not a decompose/compose round trip
				}
				// The reset dropped the target's child list (the children still name it as
				// their parent); re-link them in their original order.
				for (const Entity child: children)
				{
					if (world.GetRegistry().valid(World::ToEntt(child)))
					{
						ecs::SetParent(world, child, target);
					}
				}
				RebindPrefabScriptRefs(world, target, merged, created);
			}
		}

		// Rebuild every 2D body in a subtree. Applying a collider/rigid-body component
		// already builds its body (component apply calls RebuildBody), which for a
		// prefab instance happens while the subtree still sits at the prefab's base
		// pose. Re-running it once the instance transform is set rebuilds each body
		// from the final world pose.
		void RebuildSubtreeBodies2D(World& world, Physics2DSystem& physics2D, Entity entity)
		{
			if (world.Has<RigidBody2DComponent>(entity))
			{
				physics2D.RebuildBody(world, entity);
			}
			if (const auto* hierarchy = world.TryGet<HierarchyComponent>(entity))
			{
				for (const Entity child: hierarchy->children)
				{
					RebuildSubtreeBodies2D(world, physics2D, child);
				}
			}
		}

		// Expand every linked prefab instance in `scene` into live entities. Each
		// instance root carries a PrefabInstanceComponent (so capture re-emits the
		// reference) and a SceneTransientComponent (so the expanded subtree is never
		// written back as flat entities); the whole subtree is PrefabLink-tagged and
		// per-entity overrides are re-applied.
		void ExpandPrefabInstances(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, std::vector<Entity>* outRoots)
		{
			for (const PrefabInstanceRecord& rec: scene.prefabInstances)
			{
				const Entity root = ExpandPrefabInstance(rec, world, deps);
				if (outRoots != nullptr && root.IsValid())
				{
					outRoots->push_back(root);
				}
			}
		}

		void ApplySceneIncludes(const SceneDescription& host, World& world, const ApplySceneDeps& deps);

		std::vector<Entity> ApplySceneToEntitiesImpl(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, std::vector<Entity> created, bool registerSceneEntities, std::vector<Entity>* outExtraRoots)
		{
			const auto applyStart = std::chrono::steady_clock::now();
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
				world.GetRegistry().ctx().insert_or_assign(AppliedEnvironment{env});
				deps.renderer->SetAmbientLight(env.ambient);
				deps.renderer->SetDirectionalLight(env.sunDirection, env.sunIntensity);
				deps.renderer->SetSunColor(env.sunColor);
				deps.renderer->SetSkyGradient(env.skyHorizon, env.skyZenith);
				deps.renderer->SetSkyVoidColor(env.skyVoid);
				deps.renderer->SetClouds(env.cloudCoverage, env.cloudSpeed);
				deps.renderer->SetObjectLights(env.objectAmbient, env.objectLight0Direction, env.objectLight0Color,
				                               env.objectLight1Direction, env.objectLight1Color);
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
				// A record without one (a subtree/undo capture, a prefab override) re-applied onto a
				// reused entity keeps the id it already has: script refs and saves name it by that id.
				if (rec.nodeId != 0)
				{
					world.EmplaceOrReplace<SceneNodeComponent>(e, SceneNodeComponent{rec.nodeId});
				}
				else if (!world.Has<SceneNodeComponent>(e))
				{
					world.Emplace<SceneNodeComponent>(e, SceneNodeComponent{GenerateSceneNodeId()});
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
			ExpandPrefabInstances(scene, world, deps, outExtraRoots);
			if (outExtraRoots != nullptr)
			{
				outExtraRoots->insert(outExtraRoots->end(), migratedLights.begin(), migratedLights.end());
			}

			// Only a real scene load is worth a line. Prefab expansion reaches this core through
			// the same public ApplyScene a scene load uses, so `registerSceneEntities` cannot tell
			// them apart - the instantiate depth can. A scene with ninety instances used to emit
			// ninety "1 entities" lines, burying the one line saying how much of it actually loaded.
			if (registerSceneEntities && g_prefabInstantiateDepth == 0)
			{
				// Includes are part of every scene-level apply (load, Play restore, scene undo), so
				// they live here in the shared core rather than in each caller.
				if (!scene.includes.empty())
				{
					if (g_includeStack.empty())
					{
						world.GetRegistry().ctx().insert_or_assign(ActiveSceneIncludes{scene.includes});
					}
					ApplySceneIncludes(scene, world, deps);
				}
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart).count();
				AE_INFO(LogCategory::App, "Scene apply: {} entities, {} behaviors, {} effects, {} prefab instances, {} includes (format v{}) in {:.1f} ms", created.size() + migratedLights.size(), behaviorCount, effectCount,
				        scene.prefabInstances.size(), scene.includes.size(), scene.version, ms);
			}
			else
			{
				AE_VERBOSE(LogCategory::App, "Scene apply (nested): {} entities (format v{})", created.size() + migratedLights.size(), scene.version);
			}
			return created;
		}

		std::vector<Entity> ApplySceneToEntities(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, std::vector<Entity> created, bool registerSceneEntities, std::vector<Entity>* outExtraRoots)
		{
			++g_applyDepth;
			std::vector<Entity> result = ApplySceneToEntitiesImpl(scene, world, deps, std::move(created), registerSceneEntities, outExtraRoots);
			if (--g_applyDepth == 0)
			{
				ResolveDeferredScriptNodeRefs(world);
			}
			return result;
		}

		// Apply `host`'s [[includes]] additively: each included scene's entities and prefab
		// instances load as SceneTransient roots marked IncludedFromComponent, so the host never
		// captures them (CaptureScene re-emits only the include list). The host keeps its own
		// environment and kind; an include only adds scene features.
		void ApplySceneIncludes(const SceneDescription& host, World& world, const ApplySceneDeps& deps)
		{
			std::unordered_set<std::string> applied;
			for (const std::string& name: host.includes)
			{
				if (!applied.insert(name).second || name == host.name || std::find(g_includeStack.begin(), g_includeStack.end(), name) != g_includeStack.end())
				{
					AE_WARN(LogCategory::App, "Scene include '{}' skipped: it is already applied (duplicate or include cycle)", name);
					continue;
				}
				std::optional<SceneDescription> included = ReadSceneFile(name);
				if (!included)
				{
					AE_WARN(LogCategory::App, "Scene include '{}' not found", name);
					continue;
				}
				included->environment.reset(); // the host owns the environment
				world.SetSceneFeatures(world.GetSceneFeatures() | included->features);
				std::vector<Entity> created;
				created.reserve(included->entities.size());
				for (std::size_t i = 0; i < included->entities.size(); ++i)
				{
					created.push_back(world.Create());
				}
				std::vector<Entity> roots;
				g_includeStack.push_back(name);
				created = ApplySceneToEntities(*included, world, deps, std::move(created), true, &roots);
				g_includeStack.pop_back();
				for (const Entity e: created)
				{
					const auto* h = world.TryGet<HierarchyComponent>(e);
					if (h == nullptr || !h->parent.IsValid())
					{
						roots.push_back(e);
					}
				}
				for (const Entity e: roots)
				{
					world.EmplaceOrReplace<SceneTransientComponent>(e);
					world.EmplaceOrReplace<IncludedFromComponent>(e, IncludedFromComponent{name});
				}
			}
		}
	} // namespace

	Entity ExpandPrefabInstance(const PrefabInstanceRecord& rec, World& world, const ApplySceneDeps& deps)
	{
		if (rec.prefabPath.empty())
		{
			return Entity{};
		}
		std::optional<SceneDescription> prefab = ReadPrefabFile(rec.prefabPath);
		if (!prefab)
		{
			AE_WARN(LogCategory::App, "Prefab instance references missing prefab '{}'", rec.prefabPath);
			return Entity{};
		}
		const glm::mat4 xform = ComposeTransform(rec.position, rec.eulerDeg, rec.scale);
		std::vector<Entity> created;
		const Entity root = InstantiatePrefab(*prefab, world, deps, xform, &created);
		if (!root.IsValid())
		{
			return Entity{};
		}
		world.Emplace<PrefabInstanceComponent>(root, PrefabInstanceComponent{.prefabPath = rec.prefabPath});
		world.Emplace<SceneTransientComponent>(root);
		if (rec.node != 0)
		{
			world.EmplaceOrReplace<SceneNodeComponent>(root, SceneNodeComponent{rec.node});
		}
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
		ApplyPrefabOverrides(world, deps, root, created, byGuid, prefabByGuid, rec.overrides);

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
	
		return root;
	}

	bool ApplyPrefabInstanceToPrefab(World& world, Entity root, const ApplySceneDeps& deps, const MaterialRegistry& materials, const TextureRegistry& textures, std::vector<Entity>* outRebuilt)
	{
		const auto* inst = world.TryGet<PrefabInstanceComponent>(root);
		if (inst == nullptr)
		{
			return false;
		}
		const std::string prefabName = inst->prefabPath;

		// Snapshot every OTHER instance of this prefab while the OLD prefab is still the one
		// on disk, so each delta records only what that instance genuinely overrides.
		std::vector<PrefabInstanceRecord> siblings;
		std::vector<Entity> siblingRoots;
		auto& reg = world.GetRegistry();
		for (const auto handle: reg.storage<entt::entity>())
		{
			if (!reg.valid(handle))
			{
				continue;
			}
			const Entity e = World::FromEntt(handle);
			if (e == root)
			{
				continue;
			}
			const auto* other = world.TryGet<PrefabInstanceComponent>(e);
			if (other == nullptr || other->prefabPath != prefabName)
			{
				continue;
			}
			siblings.push_back(CapturePrefabInstance(world, e, materials, textures));
			siblingRoots.push_back(e);
		}

		if (!SavePrefabFile(prefabName, CapturePrefab(world, root, materials, textures)))
		{
			return false;
		}

		// Script refs held OUTSIDE the siblings (a scene entity, the applied instance, any other
		// instance) that name a sibling's root must follow it to its rebuilt root. Refs held
		// INSIDE a sibling were captured by node id above and resolve on expansion.
		std::unordered_map<std::uint32_t, std::size_t> siblingOf;
		for (std::size_t i = 0; i < siblingRoots.size(); ++i)
		{
			siblingOf.emplace(siblingRoots[i].id, i);
		}
		const auto insideSibling = [&](Entity e)
		{
			for (Entity cur = e; cur.IsValid();)
			{
				if (siblingOf.contains(cur.id))
				{
					return true;
				}
				const auto* h = world.TryGet<HierarchyComponent>(cur);
				cur = h != nullptr ? h->parent : Entity{};
			}
			return false;
		};
		struct HeldRef
		{
			Entity holder;
			std::size_t script;
			std::string property;
			std::size_t sibling;
		};
		std::vector<HeldRef> heldRefs;
		world.View<ScriptComponent>().each(
		        [&](entt::entity handle, const ScriptComponent& sc)
		        {
			        const Entity holder = World::FromEntt(handle);
			        for (std::size_t s = 0; s < sc.scripts.size(); ++s)
			        {
				        for (const auto& [name, value]: sc.scripts[s].properties)
				        {
					        const bool isRef = value.type == ScriptPropertyValue::Type::Entity || value.type == ScriptPropertyValue::Type::Component;
					        const auto it = isRef && value.i64 > 0 ? siblingOf.find(static_cast<std::uint32_t>(value.i64)) : siblingOf.end();
					        if (it != siblingOf.end() && !insideSibling(holder))
					        {
						        heldRefs.push_back(HeldRef{holder, s, name, it->second});
					        }
				        }
			        }
		        });

		// Rebuild the siblings from the new prefab, re-applying the deltas captured above. All
		// are torn down first and expanded as one apply, so refs between siblings resolve.
		for (const Entity sibling: siblingRoots)
		{
			ecs::DestroyHierarchy(world, sibling);
		}
		std::vector<Entity> rebuilt;
		rebuilt.reserve(siblings.size());
		++g_applyDepth;
		for (const PrefabInstanceRecord& sibling: siblings)
		{
			rebuilt.push_back(ExpandPrefabInstance(sibling, world, deps));
		}
		if (--g_applyDepth == 0)
		{
			ResolveDeferredScriptNodeRefs(world);
		}
		for (const HeldRef& ref: heldRefs)
		{
			auto* sc = reg.valid(World::ToEntt(ref.holder)) ? world.TryGet<ScriptComponent>(ref.holder) : nullptr;
			if (sc == nullptr || ref.script >= sc->scripts.size())
			{
				continue;
			}
			if (const auto prop = sc->scripts[ref.script].properties.find(ref.property); prop != sc->scripts[ref.script].properties.end())
			{
				prop->second.i64 = rebuilt[ref.sibling].id; // 0 when the rebuild failed
			}
		}
		if (outRebuilt != nullptr)
		{
			for (const Entity e: rebuilt)
			{
				if (e.IsValid())
				{
					outRebuilt->push_back(e);
				}
			}
		}
		return true;
	}

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
		// Whole-world restore: the snapshot's include list (re-applied by the core) replaces the old one.
		world.GetRegistry().ctx().erase<ActiveSceneIncludes>();
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
		// A new scene: its own include list (set by the apply core, if any) replaces the old one.
		world.GetRegistry().ctx().erase<ActiveSceneIncludes>();
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
		// World::Destroy cascades to the whole subtree (see its own file comment) - a
		// spared entity (DontDestroyOnLoad, computed above) whose PARENT is an ordinary,
		// doomed entity would otherwise be swept away the instant that parent is
		// destroyed below, even though it was just classified as surviving this switch.
		// Re-parenting is free here (SetParent/InsertChildAt never touch
		// TransformComponent - see RuntimeContainers.cs's own file comment on the native
		// implementation), so detaching to root costs nothing and needs no per-entity
		// transform fixup. Not currently reachable by any script in this project (every
		// DontDestroyOnLoad call so far marks a root, whose children inherit sparing
		// through the SAME ancestor check spared/doomed used to classify them - see
		// HasDontDestroyOnLoadAncestor), but nothing stops a future one from marking a
		// non-root child instead.
		std::unordered_set<std::uint32_t> sparedIds;
		sparedIds.reserve(spared.size());
		for (const Entity e: spared)
		{
			sparedIds.insert(e.id);
		}
		for (const Entity e: spared)
		{
			const auto* h = reg.try_get<HierarchyComponent>(World::ToEntt(e));
			if (h != nullptr && h->parent.IsValid() && !sparedIds.contains(h->parent.id))
			{
				ecs::DetachFromParent(world, e);
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

	Entity InstantiatePrefab(const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld, std::vector<Entity>* outCreated, bool markTransient)
	{
		// Instantiating a prefab must NOT redefine the scene's domain. A prefab is
		// serialised as a mini-scene whose `kind` defaults to Scene3D; ApplyScene
		// otherwise assigns that kind to the world, so spawning a prefab from a
		// script at runtime silently flips a 2D world to 3D. That breaks every
		// kind-dependent path (2D grid/tile painting in the editor, and any
		// gameplay/render logic that keys off the world kind). Preserve the world's
		// existing kind and only ever *add* the features the prefab's entities imply.
		// Depth guard: ApplyScene below re-enters this function for nested prefabs, and
		// at that point the inner instance still sits at its prefab-local pose. Only the
		// outermost call may build physics bodies (see the flush at the end).
		++g_prefabInstantiateDepth;

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
		if (markTransient && root.IsValid())
		{
			world.Emplace<SceneTransientComponent>(root);
		}

		// Applying the prefab's components already built their 2D bodies, but that
		// happened above while the subtree still sat at the prefab's base pose - so a
		// static sensor stayed at the origin while its sprite moved here, and a dynamic
		// body spawned at the origin and fell out of the world. Now that the instance
		// carries its final pose, rebuild the bodies from it.
		--g_prefabInstantiateDepth;
		if (g_prefabInstantiateDepth == 0 && root.IsValid())
		{
			if (auto* physics2D = dynamic_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem")))
			{
				RebuildSubtreeBodies2D(world, *physics2D, root);
			}
		}
		return root;
	}

	Entity InstantiatePrefabInstance(const std::string& prefabName, const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld, std::uint64_t nodeId)
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
		if (nodeId != 0)
		{
			world.EmplaceOrReplace<SceneNodeComponent>(root, SceneNodeComponent{nodeId});
		}
		return root;
	}
} // namespace aether::app::scene
