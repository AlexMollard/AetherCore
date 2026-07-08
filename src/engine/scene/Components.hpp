#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "animation/AnimationDatabase.hpp"
#include "assets/GltfAsset.hpp"
#include "material/EffectParams.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialHandle.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class GraphicsPipeline;
	class Mesh;

	// -- ECS component types ------------------------------------------------
	// Each is a plain-data struct; the World owns all component storage.

	// World-space transform (model matrix).
	struct TransformComponent
	{
		glm::mat4 localToWorld{1.0f};
	};

	// Human-readable display name (auto-assigned at spawn, editable in the inspector).
	struct NameComponent
	{
		std::string name;
	};

	// Canonical scene-graph link. Both sides are kept consistent exclusively
	// through aether::ecs::SetParent (see scene/Hierarchy.hpp) - never mutate
	// parent/children directly.
	struct HierarchyComponent
	{
		Entity parent{};              // {0} == root
		std::vector<Entity> children; // ordered
	};

	// Reference to a GPU vertex/index buffer.
	struct MeshComponent
	{
		const Mesh* mesh = nullptr;
	};

	// Reference to a registry material + the cached GPU slot for the render loop.
	// The handle is authoritative for lifetime; gpuSlot is refreshed on assignment
	// (MaterialSystem::AssignMaterial). Default-constructed = "no material".
	struct MaterialComponent
	{
		MaterialHandle handle{};
		std::uint32_t gpuSlot = 0xFFFFFFFFu;
	};

	// A per-entity editable material (copy-on-write over the immutable registry).
	// Holds the authoring asset between edits; the typed setters in MaterialSystem
	// mutate one field and re-acquire. Used by material instances and effects.
	struct MaterialInstanceComponent
	{
		MaterialAsset asset{};
	};

	// Per-entity effect-parameter slot, independent of MaterialComponent.gpuSlot.
	// Its presence also marks the entity as effect-driven for the effect-override
	// rule in MaterialSystem::AssignMaterial. Freed via EffectSystem's on_destroy hook.
	// `params` is the CPU-authoritative copy the effect setters read-modify-write
	// (so a single-field edit does not clobber the others) before one buffer Write.
	struct EffectParamsComponent
	{
		std::uint32_t paramSlot = 0xFFFFFFFFu;
		EffectParams params{};
	};

	// Pipeline (shader + raster state) used to draw the entity.
	struct PipelineComponent
	{
		const GraphicsPipeline* pipeline = nullptr;
	};

	// Stable identity of the entity's mesh, recorded at spawn time (there is no
	// Mesh* -> source reverse map). Scene serialization re-resolves the pointer
	// from this: a model primitive by (path, index) or a primitive by kind name.
	struct MeshSourceComponent
	{
		enum class Kind : std::uint8_t
		{
			Model,     // path = model VFS path, primitiveIndex = LoadedModel primitive
			Primitive, // path = primitive kind name ("cube", "sphere", ...)
		};

		Kind kind = Kind::Primitive;
		std::string path;
		std::uint32_t primitiveIndex = 0;
	};

	// Which named effect drives this entity (set_entity_effect). The effect
	// pipeline/param slot are runtime state; the name is the stable identity a
	// scene load re-applies through the EffectManager.
	struct EffectRefComponent
	{
		std::string name;
	};

	// Excludes an entity (and, via the hierarchy, its whole subtree) from scene
	// capture. Script-owned runtime entities (the player, its model children)
	// mark themselves transient so boot auto-generation and Play snapshots do
	// not duplicate them.
	struct SceneTransientComponent
	{
	};

	// Serialized C# script field override (Unity-style [SerializeField]). The tag
	// mirrors the scripting PropertyType wire enum.
	struct ScriptPropertyValue
	{
		enum class Type : std::int32_t
		{
			None = 0,
			Float = 1,
			Int = 2,
			Bool = 3,
			Vector3 = 4,
			String = 5,
			Enum = 6,
			Entity = 7,
		};

		Type type = Type::None;
		float f4[4] = {};     // Float (x), Vector3 (xyz)
		std::int64_t i64 = 0; // Int, Bool (0/1), Enum, Entity id
		std::string str;      // String
	};

	struct ScriptEntry
	{
		// C# script type name, resolved in the AetherGame assembly.
		std::string path;
		bool attached = false;
		// Per-entity field overrides applied to the script instance on attach and
		// persisted with the scene. Ordered for stable serialization.
		std::map<std::string, ScriptPropertyValue> properties;
	};

	struct ScriptComponent
	{
		// Entity-attached C# scripts. Each slot owns its attach state and field
		// overrides, so one entity can run several managed script instances.
		std::vector<ScriptEntry> scripts;
	};

	// Drives GPU-based skeletal animation for a skinned mesh entity.
	struct SkinnedMeshComponent
	{
		AnimationDatabase* animDb = nullptr;
		std::uint32_t skinIndex = 0;
		std::uint32_t jointCount = 0; // cached from animDb at spawn time
		std::uint32_t clipIndex = 0;
		float animTime = 0.f;
		float playbackSpeed = 1.f;
		std::uint32_t nodePoseOffset = 0;
		bool looping = true;
		// Runtime-loaded animation clips, kept on CPU for duration queries.
		// Cleared after compile_animations() bakes them into animDb.
		std::vector<assets::GltfAnimation> pendingExternalAnims;
	};

	// -- Animation blend -----------------------------------------------------

	// Drives cross-fade blending between two animation clips.
	// primaryClip is the currently playing clip (managed by SkinnedMeshComponent).
	// When a transition is requested via set_animation_blend(), secondaryClip
	// and transitionSpeed are set, and inTransition=true. The AnimationBlendSystem
	// advances blendWeight toward 0 each frame; when it reaches 0 the transition
	// completes and primaryClip is replaced by secondaryClip.
	struct AnimationBlendComponent
	{
		std::uint32_t primaryClip = 0;   // currently active clip index in animDb
		std::uint32_t secondaryClip = 0; // clip to transition to (0 = no transition)
		float blendWeight = 1.0f;        // 1.0 = fully primary, 0.0 = fully secondary
		float transitionSpeed = 4.0f;    // blend weight reduction per second
		bool inTransition = false;
	};

	// -- Root motion ---------------------------------------------------------

	// Tracks root bone motion state for physics-driven character movement.
	// Reads the Hips node's world position from the node global transforms buffer,
	// computes the delta since the previous frame, and applies that delta to the
	// entity's PhysicsStateComponent so the character physically moves through the
	// world rather than sliding in place.
	struct RootMotionComponent
	{
		std::uint32_t hipsNodeIdx = UINT32_MAX;       // node index of the root bone (e.g. Hips)
		glm::vec3 prevHipsWorldPos{0.0f, 0.0f, 0.0f}; // world position last frame
		glm::vec3 accumulatedDelta{0.0f, 0.0f, 0.0f}; // total delta since animation started
		bool applyToPhysics = true;                   // push delta to PhysicsStateComponent (physics-driven movement)
		bool applyToTransform = true;                 // push delta to TransformComponent (visual sync)
		bool enabled = true;
	};

	// -- Hierarchy panel editor tags ------------------------------------------

	// Present when the entity is hidden in the Scene View.
	// Toggled via the eye icon in the Hierarchy Panel row.
	struct HiddenTag
	{
		bool dummy = true;
	};

	// Present when the entity is *not* pickable via the scene-view ray-cast.
	// Toggled via the padlock icon in the Hierarchy Panel row.
	struct NotPickableTag
	{
		bool dummy = true;
	};
} // namespace aether
