// Custom scene serde for ScriptComponent: script property values can reference other
// entities, which capture rewrites to scene-local indices and apply resolves back
// through the created-entities vector - both need the serde context, not a plain field.

#include "scene/SceneComponentSerde.hpp"

#include "scene/Components.hpp"
#include "scene/SceneSerializerDetail.hpp"
#include "scene/World.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::app::scene
{
	using namespace detail;

	namespace
	{
		void CaptureScripts(SceneCaptureContext& c)
		{
			auto* script = c.world.TryGet<ScriptComponent>(c.entity);
			if (script == nullptr)
			{
				return;
			}
			// Absent for headless/no-scripting Worlds (AssetPacker, the bare
			// serializer test fixtures) - the graceful-absence idiom FindSystem
			// already uses everywhere else (see e.g. NetworkContext.cpp's
			// SetMotionType3D). properties then captures exactly as before this
			// sync existed: whatever the cache already held.
			auto* runner = static_cast<ScriptComponentSystem*>(c.world.FindSystem("ScriptComponentSystem"));
			for (std::size_t i = 0; i < script->scripts.size(); ++i)
			{
				ScriptEntry& entry = script->scripts[i];
				if (entry.path.empty())
				{
					continue;
				}
				if (runner != nullptr)
				{
					// A field the script itself assigned in C# never touches
					// ScriptRegistry.SetProperty (the Inspector's write path that
					// otherwise keeps this cache current) - see the method's own
					// comment for the full contract.
					runner->SyncPropertiesFromLiveInstance(c.entity, static_cast<std::uint32_t>(i), entry);
				}
				// Self is not a persisted property: EntityScript.Self is set at construction
				// from the entity itself (ScriptRegistry.CreateInstance -> EntityScript.Bind)
				// and is [HideInInspector] on the managed side precisely so it is never
				// reflected, captured, or reapplied. Erase any entry a pre-fix scene left in
				// the live cache (or that SyncPropertiesFromLiveInstance re-persisted before
				// that fix landed) directly - not just from the copy below - so a resave
				// permanently drops it instead of re-writing a stale/wrong index forever.
				entry.properties.erase("Self");
				c.rec.scripts.push_back(ScriptRecord{.type = entry.path, .properties = ScriptPropsToSceneRefs(entry.properties, c.indexOf)});
			}
		}

		void ApplyScripts(SceneApplyContext& c)
		{
			if (c.rec.scripts.empty())
			{
				return;
			}
			ScriptComponent component;
			component.scripts.reserve(c.rec.scripts.size());
			for (const ScriptRecord& script: c.rec.scripts)
			{
				auto properties = ScriptPropsFromSceneRefs(script.properties, c.created);
				// Same exclusion as capture: a scene saved before this fix may still carry a
				// (possibly wrong, positionally-remapped) Self entry. Self is always correct
				// at construction (EntityScript.Bind), so never seed the live property cache
				// with a persisted value for it - ApplyProperties would ignore it anyway now
				// that Self isn't reflected, but this keeps the cache itself clean too.
				properties.erase("Self");
				component.scripts.push_back(ScriptEntry{.path = script.type, .attached = false, .properties = std::move(properties)});
			}
			c.world.Emplace<ScriptComponent>(c.entity, std::move(component));
		}

		AE_SCENE_SERDE(Scripts, "Scripts", 80, CaptureScripts, ApplyScripts)
	} // namespace
} // namespace aether::app::scene
