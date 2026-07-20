#include "scene/SceneComponentSerde.hpp"

#include <algorithm>

namespace aether::app::scene
{
	namespace
	{
		std::vector<SceneComponentSerde>& Registry()
		{
			static std::vector<SceneComponentSerde> registry;
			return registry;
		}
	} // namespace

	void RegisterSceneComponentSerde(SceneComponentSerde serde)
	{
		std::vector<SceneComponentSerde>& registry = Registry();
		// Keep the vector sorted by order so capture/apply iterate deterministically;
		// stable_sort preserves registration order within an equal-order group.
		registry.push_back(std::move(serde));
		std::stable_sort(registry.begin(), registry.end(), [](const SceneComponentSerde& a, const SceneComponentSerde& b) { return a.order < b.order; });
	}

	const std::vector<SceneComponentSerde>& SceneComponentSerdes()
	{
		return Registry();
	}

	void RunCaptureSerdes(SceneCaptureContext& ctx)
	{
		for (const SceneComponentSerde& serde: Registry())
		{
			if (serde.capture)
			{
				serde.capture(ctx);
			}
		}
	}

	void RunApplySerdes(SceneApplyContext& ctx)
	{
		for (const SceneComponentSerde& serde: Registry())
		{
			if (serde.apply)
			{
				serde.apply(ctx);
			}
		}
	}
} // namespace aether::app::scene
