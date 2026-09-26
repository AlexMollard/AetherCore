#include "twinsanity/LevelBake.hpp"

#include "assets/AssetManager.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

namespace aether::editor::twinsanity
{
	namespace
	{
		// The prefab's save name and the bake root's entity name - both also known to the C#
		// side (TwinsanityBake.PrefabName / TwinsanityBake.BakeRootName); keep them in step.
		constexpr const char* kPrefabName = "beach";
		constexpr const char* kBakeRootName = "Twinsanity Beach Bake";

		// The bake is deterministic (same level data -> same subtree walk), so the fresh
		// capture's entity order equals the previous prefab's. Re-stamping the fresh entities
		// with the old guids keeps every instance override pointed at the same baked entity.
		// The name check is the cheap tripwire for "the order drifted": on a mismatch the old
		// guids are useless and the overrides are dropped with a warning instead of being
		// applied to the wrong entities.
		bool GuidsStillAlign(const app::scene::SceneDescription& fresh, const app::scene::SceneDescription& old)
		{
			if (fresh.entities.size() != old.entities.size())
			{
				return false;
			}
			for (std::size_t i = 0; i < fresh.entities.size(); ++i)
			{
				if (fresh.entities[i].name != old.entities[i].name)
				{
					return false;
				}
			}
			return true;
		}
	} // namespace

	LevelBakeReport BakeBeachLevel(ServiceContainer& services)
	{
		LevelBakeReport report;
		report.prefab = kPrefabName;

		auto* scenes = services.TryGet<SceneSubsystem>();
		auto* assets = services.TryGet<AssetManager>();
		auto* scripting = services.TryGet<app::scripting::CSharpScriptingSubsystem>();
		if (scenes == nullptr || assets == nullptr || scripting == nullptr)
		{
			report.error = "editor subsystems unavailable (no scene, assets or C# host)";
			return report;
		}
		World& world = scenes->GetWorld();

		// The scene's current instance record, if any: its overrides survive this re-bake.
		const app::scene::ApplySceneDeps deps = app::scene::MakeApplySceneDeps(services);
		Entity oldRoot{};
		for (const auto captured: world.View<PrefabInstanceComponent>())
		{
			const Entity e = World::FromEntt(captured);
			const auto* inst = world.TryGet<PrefabInstanceComponent>(e);
			if (inst != nullptr && inst->prefabPath == kPrefabName)
			{
				oldRoot = e;
				break;
			}
		}
		app::scene::PrefabInstanceRecord oldRecord;
		const bool hadInstance = oldRoot.IsValid();
		if (hadInstance)
		{
			oldRecord = app::scene::CapturePrefabInstance(world, oldRoot, assets->GetMaterialRegistry(), assets->GetTextureRegistry());
			oldRecord.prefabPath = kPrefabName;
		}

		// Build the level into the live scene. The C# command reports through the log; a thrown
		// bake (bad level data, missing assets) lands here as a failure with no root to capture.
		//
		// The command body calls Scene.*/Entity.*/Assets.* like any script callback, and every
		// one of those exports dereferences the thread's active scene context - nothing else
		// published one on the control endpoint's thread, so this scope is the difference
		// between the bake running and a segfault in its first P/Invoke (the exact trap
		// CSharpRpcBridge::Invoke documents).
		auto* sceneCtx = services.TryGet<app::scripting::SceneContext>();
		if (sceneCtx == nullptr)
		{
			report.error = "no scene context registered - the editor scene layer is not active";
			return report;
		}
		const app::scripting::ActiveContextScope scope(*sceneCtx);
		std::string scriptError;
		if (!scripting->InvokeScriptCommand("TwinsanityBake", scriptError))
		{
			report.error = scriptError;
			return report;
		}
		Entity root{};
		for (const auto captured: world.View<NameComponent>())
		{
			const Entity e = World::FromEntt(captured);
			const auto* name = world.TryGet<NameComponent>(e);
			if (name != nullptr && name->name == kBakeRootName)
			{
				root = e;
				break;
			}
		}
		if (!root.IsValid())
		{
			report.error = "the bake produced no '" + std::string(kBakeRootName) + "' root - see the console";
			return report;
		}

		// Capture the fresh bake as the prefab. The subtree was built parented under the root,
		// so one capture takes all of it.
		app::scene::SceneDescription fresh = app::scene::CapturePrefab(world, root, assets->GetMaterialRegistry(), assets->GetTextureRegistry());
		report.prefabEntities = static_cast<int>(fresh.entities.size());

		// Guid stability across bakes (see GuidsStillAlign): keep the old guids when the bake
		// reproduced the same subtree, else re-key and drop the old overrides with a warning.
		const auto oldPrefab = app::scene::ReadPrefabFile(kPrefabName);
		bool keepOverrides = false;
		if (oldPrefab && GuidsStillAlign(fresh, *oldPrefab))
		{
			for (std::size_t i = 0; i < fresh.entities.size(); ++i)
			{
				fresh.entities[i].guid = oldPrefab->entities[i].guid;
			}
			keepOverrides = true;
		}
		else if (oldPrefab)
		{
			app::scene::AssignPrefabGuids(fresh);
			AE_WARN(LogCategory::App,
			        "TwinsanityBake: the new bake does not match the previous prefab ({} vs {} entities, or the order drifted), so the instance's overrides cannot be re-aligned and are dropped",
			        oldPrefab->entities.size(), fresh.entities.size());
		}
		else
		{
			app::scene::AssignPrefabGuids(fresh);
		}

		if (!app::scene::SavePrefabFile(kPrefabName, fresh))
		{
			report.error = "SavePrefabFile failed - the prefab file could not be written";
			return report;
		}

		// The fresh subtree was only for the capture; the scene keeps the instance reference.
		ecs::DestroyHierarchy(world, root);

		if (hadInstance)
		{
			if (keepOverrides)
			{
				// Re-expand the same record against the new prefab: placement, overrides,
				// removed and added entities all carry over (ExpandPrefabInstance skips an
				// override whose guid the prefab no longer holds).
				ecs::DestroyHierarchy(world, oldRoot);
				report.overridesKept = static_cast<int>(oldRecord.overrides.size());
				app::scene::PrefabInstanceRecord carried = oldRecord;
				const Entity newRoot = app::scene::ExpandPrefabInstance(carried, world, deps);
				report.ok = newRoot.IsValid();
				report.replacedInstance = true;
				if (!report.ok)
				{
					report.error = "failed to re-expand the prefab instance";
				}
				return report;
			}
			report.overridesDropped = static_cast<int>(oldRecord.overrides.size());
			ecs::DestroyHierarchy(world, oldRoot);
		}

		// First bake (or a level change that voided the old overrides): expand a fresh record.
		app::scene::PrefabInstanceRecord record;
		record.prefabPath = kPrefabName;
		const Entity newRoot = app::scene::ExpandPrefabInstance(record, world, deps);
		report.ok = newRoot.IsValid();
		if (!report.ok)
		{
			report.error = "failed to expand the prefab instance";
		}
		return report;
	}
} // namespace aether::editor::twinsanity
