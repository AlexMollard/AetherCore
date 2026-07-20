// Custom scene serde for ScriptComponent: script property values can reference other
// entities, which capture rewrites to scene-local indices and apply resolves back
// through the created-entities vector - both need the serde context, not a plain field.

#include "scene/SceneComponentSerde.hpp"

#include "scene/Components.hpp"
#include "scene/SceneSerializerDetail.hpp"
#include "scene/World.hpp"

namespace aether::app::scene
{
	using namespace detail;

	namespace
	{
		void CaptureScripts(SceneCaptureContext& c)
		{
			const auto* script = c.world.TryGet<ScriptComponent>(c.entity);
			if (script == nullptr)
			{
				return;
			}
			for (const ScriptEntry& entry: script->scripts)
			{
				if (entry.path.empty())
				{
					continue;
				}
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
				component.scripts.push_back(ScriptEntry{.path = script.type, .attached = false, .properties = ScriptPropsFromSceneRefs(script.properties, c.created)});
			}
			c.world.Emplace<ScriptComponent>(c.entity, std::move(component));
		}

		AE_SCENE_SERDE(Scripts, "Scripts", 80, CaptureScripts, ApplyScripts)
	} // namespace
} // namespace aether::app::scene
