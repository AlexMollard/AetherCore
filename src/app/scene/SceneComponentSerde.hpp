#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "scene/Entity.hpp"
#include "scene/SceneSerializer.hpp"

namespace aether
{
	class World;
	class MaterialRegistry;
	class TextureRegistry;
}

namespace aether::app::scene
{
	// Context handed to a component's custom capture: everything the world-independent
	// EntityRecord cannot itself hold (the live world, the asset registries used to
	// resolve handles->paths, and the entity->scene-index map for cross-entity refs).
	// A capture serde reads its component(s) off `world`/`entity` and fills `rec`.
	struct SceneCaptureContext
	{
		World& world;
		Entity entity;
		const MaterialRegistry& materials;
		const TextureRegistry& textures;
		const std::unordered_map<std::uint32_t, int>& indexOf;
		EntityRecord& rec;
	};

	// Context handed to a component's custom apply: the live world, the load-time deps
	// (asset managers, effect manager, ...), the created-entities vector for resolving
	// scene-index refs, and the shared per-entity state the two 700-line loops used to
	// thread by hand (implied features, the physics-domain tie-break, log counters).
	// An apply serde reads `rec` and emplaces onto `world`/`entity`.
	struct SceneApplyContext
	{
		World& world;
		Entity entity;
		const ApplySceneDeps& deps;
		const std::vector<Entity>& created;
		const EntityRecord& rec;
		SceneKind sceneKind;
		SceneFeatureFlags& impliedFeatures;
		bool apply2DPhysics;
		bool apply3DPhysics;
		std::size_t& behaviorCount;
		std::size_t& effectCount;
	};

	// One component's bespoke scene serialization, registered next to the component
	// instead of inlined in the capture/apply loops. `order` sequences apply only
	// (capture fills independent record fields, so its order does not matter); lower
	// runs first - e.g. mesh (10) before material (20), physics decision before joints.
	struct SceneComponentSerde
	{
		std::string name;
		int order = 0;
		std::function<void(SceneCaptureContext&)> capture;
		std::function<void(SceneApplyContext&)> apply;
	};

	void RegisterSceneComponentSerde(SceneComponentSerde serde);

	// All registered serdes, ascending by `order` (stable within equal order).
	const std::vector<SceneComponentSerde>& SceneComponentSerdes();

	// Run every registered capture / apply serde for one entity.
	void RunCaptureSerdes(SceneCaptureContext& ctx);
	void RunApplySerdes(SceneApplyContext& ctx);
} // namespace aether::app::scene

// Registers a component's capture/apply serde at static-init (like AE_COMPONENT).
// Invoke at file scope inside namespace aether::app::scene (so captureFn/applyFn resolve
// unqualified); `tag` is any unique identifier for the registration variable.
#define AE_SCENE_SERDE(tag, displayName, order, captureFn, applyFn)                    \
	static const bool tag##_serde_registered = []                                     \
	{                                                                                 \
		::aether::app::scene::RegisterSceneComponentSerde(                             \
		        ::aether::app::scene::SceneComponentSerde{displayName, (order), (captureFn), (applyFn)}); \
		return true;                                                                  \
	}();
