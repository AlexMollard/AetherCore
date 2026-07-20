#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "animation/AnimationDatabase.hpp"
#include "assets/AssetId.hpp"
#include "assets/SpriteAnimationAsset.hpp"
#include "assets/AssetTypes.hpp"
#include "assets/GltfAsset.hpp"
#include "material/EffectParams.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialHandle.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class GraphicsPipeline;
	class Mesh;

	struct TransformComponent
	{
		glm::mat4 localToWorld{1.0f};
	};

	struct NameComponent
	{
		std::string name;
	};

	// through aether::ecs::SetParent (see scene/Hierarchy.hpp) - never mutate
	struct HierarchyComponent
	{
		Entity parent{};
		std::vector<Entity> children;
	};

	struct MeshComponent
	{
		const Mesh* mesh = nullptr;
		AssetId asset{};
		std::uint32_t resolvedGeneration = 0;
	};

	// The handle is authoritative for lifetime; gpuSlot is refreshed on assignment
	struct MaterialComponent
	{
		MaterialHandle handle{};
		std::uint32_t gpuSlot = 0xFFFFFFFFu;
	};

	struct MaterialInstanceComponent
	{
		MaterialAsset asset{};
	};

	struct EffectParamsComponent
	{
		std::uint32_t paramSlot = 0xFFFFFFFFu;
		EffectParams params{};
	};

	struct PipelineComponent
	{
		const GraphicsPipeline* pipeline = nullptr;
	};

	struct MeshSourceComponent
	{
		enum class Kind : std::uint8_t
		{
			Model,
			Primitive,
		};

		Kind kind = Kind::Primitive;
		std::string path;
		std::uint32_t primitiveIndex = 0;
	};

	struct MeshRendererComponent
	{
		bool visible = true;
		bool castShadows = true;
	};

	enum class SpriteBlendMode : std::uint8_t
	{
		Alpha = 0,
		Additive,
		Multiply,
		Opaque,
	};

	struct SpriteRendererComponent
	{
		std::string texturePath;
		std::string atlasPath;
		AssetObjectId spriteId{};
		glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f};
		glm::vec4 tint{1.0f};
		glm::vec2 pixelSize{100.0f};
		glm::vec2 pivot{0.5f};
		float pixelsPerUnit = 100.0f;
		std::int32_t sortingLayer = 0;
		std::int32_t orderInLayer = 0;
		SpriteBlendMode blendMode = SpriteBlendMode::Alpha;
		bool visible = true;
		bool flipX = false;
		bool flipY = false;
		bool pixelSnap = false;
		// Crisp nearest-neighbour sampling for pixel art (the shader snaps UVs to
		// texel centres). Atlas sprites also get this from the atlas' filter
		// recommendation; this opts a plain full-texture sprite in too.
		bool pixelArt = false;
	};

	struct SpriteAnimatorComponent
	{
		std::string animationPath;
		float speed = 1.0f;
		std::uint32_t startFrame = 0;
		SpriteAnimationLoopMode loopMode = SpriteAnimationLoopMode::Loop;
		bool useAssetLoopMode = true;
		bool autoplay = true;

		float frameTime = 0.0f;
		float fixedAccumulator = 0.0f;
		std::uint32_t currentFrame = 0;
		std::int32_t direction = 1;
		bool playing = false;
		bool initialized = false;
	};

	// References a TileMapAsset (cell data lives in its own binary file, never
	// scene TOML). Sorting offsets add onto each layer's own values.
	struct TileMapComponent
	{
		std::string tilemapPath;
		glm::vec4 tint{1.0f};
		std::int32_t sortingLayer = 0;
		std::int32_t orderInLayer = 0;
		std::uint32_t visibleLayerMask = 0xFFFFFFFFu;
		bool visible = true;
	};

	struct EffectRefComponent
	{
		std::string name;
	};

	struct SceneTransientComponent
	{
	};

	struct DisabledComponent
	{
	};

	// A prefab-instance root: this entity and its subtree were expanded from the
	// named prefab at scene load, and the scene re-serializes them as a single
	// reference (path + root transform + per-entity overrides) rather than as flat
	// entities. Editing the prefab propagates to every scene that instances it.
	// Overrides are re-derived at capture (live-vs-prefab diff), not stored here.
	struct PrefabInstanceComponent
	{
		std::string prefabPath;
	};

	// Marks an entity as part of a prefab instance's expanded subtree. `instanceRoot`
	// carries the PrefabInstanceComponent; `prefabGuid` is the source prefab entity's
	// stable id (its index in the prefab), used to route overrides / apply-revert.
	// Runtime-only, never a flat serialized entity - capture emits the reference.
	struct PrefabLinkComponent
	{
		Entity instanceRoot{};
		std::uint64_t prefabGuid = 0;
	};

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
			// str holds the ComponentCatalog name of the required component, so the
			Component = 8,
		};

		Type type = Type::None;
		float f4[4] = {};
		std::int64_t i64 = 0;
		std::string str;
	};

	struct ScriptEntry
	{
		std::string path;
		bool attached = false;
		std::map<std::string, ScriptPropertyValue> properties;
	};

	struct ScriptComponent
	{
		std::vector<ScriptEntry> scripts;
	};

	struct SkinnedMeshComponent
	{
		AnimationDatabase* animDb = nullptr;
		std::uint32_t skinIndex = 0;
		std::uint32_t jointCount = 0;
		std::uint32_t clipIndex = 0;
		float animTime = 0.f;
		float playbackSpeed = 1.f;
		std::uint32_t nodePoseOffset = 0;
		bool looping = true;
		std::vector<assets::GltfAnimation> pendingExternalAnims;
	};

	struct AnimationBlendComponent
	{
		std::uint32_t primaryClip = 0;
		std::uint32_t secondaryClip = 0;
		float blendWeight = 1.0f;
		float transitionSpeed = 4.0f;
		bool inTransition = false;
	};

	struct RootMotionComponent
	{
		std::uint32_t hipsNodeIdx = UINT32_MAX;
		glm::vec3 prevHipsWorldPos{0.0f, 0.0f, 0.0f};
		glm::vec3 accumulatedDelta{0.0f, 0.0f, 0.0f};
		bool applyToPhysics = true;
		bool applyToTransform = true;
		bool enabled = true;
	};

	struct HiddenTag
	{
		bool dummy = true;
	};

	struct NotPickableTag
	{
		bool dummy = true;
	};
} // namespace aether
