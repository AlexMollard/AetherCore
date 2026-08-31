#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "material/EffectParams.hpp"
#include "material/MaterialAsset.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "particles/ParticleComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/ModelBakeHook.hpp"
#include "scene/reflection/Reflection.hpp"
#include "scene/SceneKind.hpp"

namespace aether
{
	class AssetManager;
	class AssetDatabase;
	class EffectParamBuffer;
	class MaterialRegistry;
	class PhysicsSystem;
	class PipelineCache;
	class PrimitiveMeshes;
	class Renderer;
	class ServiceContainer;
	class TextureRegistry;
	class World;
} // namespace aether

namespace aether::effects
{
	class EffectManager;
}

namespace aether::app::scripting
{
	struct SceneContext;
}

namespace aether::app::scene
{

	struct MaterialRecord
	{
		// When set, this material is a LINK to that asset and `asset` below holds the resolved
		// values. Only the keys named in `overrides` are written to the scene - everything
		// else is re-read from the asset on load, which is what makes an edit to the asset
		// reach every entity using it.
		std::string assetPath;
		std::vector<std::string> overrides;
		MaterialAsset asset{};
		std::string albedoPath;
		std::string normalPath;
		std::string metallicRoughnessPath;
		std::string occlusionPath;
		std::string emissivePath;
	};

	struct SkinnedRecord
	{
		std::uint32_t clipIndex = 0;
		float animTime = 0.0f;
		float playbackSpeed = 1.0f;
		bool looping = true;
	};

	struct PhysicsRecord
	{
		PhysicsShapeType shapeType = PhysicsShapeType::Box;
		glm::vec3 halfExtents{0.5f};
		float radius = 0.5f;
		float halfHeight = 0.5f;
		PhysicsMotionType motionType = PhysicsMotionType::Dynamic;
		glm::vec3 center{0.0f};
		float friction = 0.5f;
		float restitution = 0.0f;
		float mass = 0.0f;
		float linearDamping = 0.05f;
		float angularDamping = 0.05f;
		float gravityFactor = 1.0f;
		float maxLinearVelocity = 500.0f;
		float maxAngularVelocity = 47.124f;
		bool isSensor = false;
		bool continuousCollision = false;
		bool allowSleeping = true;
		glm::bvec3 lockPosition{false};
		glm::bvec3 lockRotation{false};
	};

	struct JointRecord
	{
		JointType type = JointType::Fixed;
		int targetIndex = -1;
		glm::vec3 anchor{0.0f};
		glm::vec3 axis{0.0f, 1.0f, 0.0f};
		float minLimit = 0.0f;
		float maxLimit = 0.0f;
		float distance = -1.0f;
		bool collideConnected = false;
	};

	// UI Canvas / UI Rect / UI Text are pure authored data, serialized generically via
	// reflection (no bespoke record). UI Image keeps a record for its texture path.
	struct UIImageRecord
	{
		glm::vec4 color{1.f};
		float cornerRadius = 0.f;
		bool pixelArt = false;
		std::string texturePath;
	};

	struct EffectRecord
	{
		std::string name;
		EffectParams params{};
	};

	struct ScriptRecord
	{
		std::string type;
		std::map<std::string, ScriptPropertyValue> properties;
	};

	// A component captured generically from the reflection registry: the reflected
	// component name plus a (fieldName, value) for each present field. Only fields
	// actually present are stored, so on apply an absent field keeps its default -
	// matching the old per-type behaviour. Serialized/applied with no per-component code.
	struct GenericComponent
	{
		std::string type;
		std::vector<std::pair<std::string, reflect::FieldValue>> fields;
	};

	struct EntityRecord
	{
		std::uint32_t entityId = 0;
		// Stable per-prefab id (assigned on prefab save). Prefab-instance overrides
		// and PrefabLink route by this guid, so reordering a prefab's entities never
		// misaligns overrides. 0 = unassigned (falls back to the entity index).
		std::uint64_t guid = 0;
		// Stable per-entity scene id (serialized as `node`), distinct from the prefab guid.
		// `parent` is resolved from the parent's nodeId when available, so hand-editing entity
		// order/membership in a .scene.toml does not corrupt parenting. 0 = unassigned (legacy).
		std::uint64_t nodeId = 0;
		// Transient: the parent's node id as read from `parent_node`, resolved to parentIndex after
		// the whole entity list is parsed. 0 = not present (fall back to the positional `parent`).
		std::uint64_t parentNodeId = 0;
		std::string name;
		std::vector<std::string> tags;
		bool disabled = false;
		std::optional<SpriteRendererComponent> sprite;
		std::optional<SpriteAnimatorComponent> spriteAnimator;
		bool meshRenderer = false;
		bool meshRendererVisible = true;
		bool meshRendererCastShadows = true;
		bool hasTransform = false;
		glm::vec3 position{0.0f};
		glm::vec3 eulerDeg{0.0f};
		glm::vec3 scale{1.0f};
		int parentIndex = -1;
		std::optional<MeshSourceComponent> mesh;
		std::optional<MaterialRecord> material;
		std::optional<SkinnedRecord> skinned;
		std::optional<PhysicsRecord> physics;
		std::optional<EffectRecord> effect;
		std::optional<UIImageRecord> uiImage;
		// Pure data-only components (no asset resolution or side effects) are
		// captured/applied/serialized generically from the reflection registry (every
		// ComponentType flagged genericSerialize). Adding one needs only its AE_COMPONENT
		// declaration plus AE_GENERIC_SERIALIZE(); no per-component serializer code.
		std::vector<GenericComponent> reflected;
		// Particle Emitter, Point/Spot lights, Day Night, Tile Map and Orbit Camera are
		// pure reflected data and live in `reflected` (genericSerialize) - no bespoke
		// record field. (Particle Emitter keeps its legacy "particles" on-disk key.)
		// backingCamera is runtime state and is never serialized. mainCamera marks
		std::optional<CameraComponent> camera;
		bool mainCamera = false;
		std::vector<ScriptRecord> scripts;
		std::optional<JointRecord> joint;
		// 2D physics: authored fields only (runtime body/shape/joint handles are
		// stripped at capture so play-stop restore never resurrects stale ids).
		std::optional<Joint2DComponent> joint2D;
		int joint2DTargetIndex = -1; // scene-local index, like JointRecord::targetIndex
	};

	struct LightRecord
	{
		bool isSpot = false;
		glm::vec3 position{0.0f};
		float radius = 1.0f;
		glm::vec3 color{1.0f};
		float intensity = 1.0f;
		bool castsShadow = false;
		glm::vec3 direction{0.0f, -1.0f, 0.0f};
		float innerAngleRad = 0.35f;
		float outerAngleRad = 0.60f;
	};

	struct EnvironmentRecord
	{
		glm::vec3 ambient{0.2f};
		glm::vec3 sunDirection{0.0f, -1.0f, 0.0f};
		float sunIntensity = 1.0f;
		glm::vec3 sunColor{1.0f};
		glm::vec3 skyHorizon{0.5f};
		glm::vec3 skyZenith{0.2f};
		glm::vec3 skyVoid{0.05f};
	};

	// A per-entity override inside a prefab instance: `guid` is the source prefab
	// entity's stable id (its index in the prefab), and `record` is the entity's
	// full authored state as it differs in this instance. Applied after the prefab
	// expands, replacing that entity's components. Whole-entity granularity: any
	// change to a prefab entity in a scene freezes that entity's components here.
	struct PrefabEntityOverride
	{
		std::uint64_t guid = 0;
		// Only the top-level keys that differ from the source prefab entity, as TOML
		// table text (e.g. "position = [...]\n[sprite]\ntint = [...]"). Non-overridden
		// keys - including ones the prefab changes *later* - come from the prefab at
		// apply time, so an override no longer freezes the whole entity's other fields.
		std::string partialToml;
	};

	// A prefab entity's stable id: its assigned guid, or (index + 1) for a legacy
	// prefab whose entities were never re-saved with guids. Overrides and PrefabLink
	// route by this, so both the apply and capture sides must agree on it.
	[[nodiscard]] inline std::uint64_t EffectiveGuid(const EntityRecord& rec, std::size_t index)
	{
		return rec.guid != 0 ? rec.guid : static_cast<std::uint64_t>(index + 1);
	}

	// A linked prefab instance placed in a scene: the scene stores this reference
	// (prefab path + root transform + per-entity overrides) instead of the expanded
	// entities, so editing the prefab propagates to every scene that instances it.
	struct PrefabInstanceRecord
	{
		std::string prefabPath;
		std::string name; // hierarchy display name (defaults to the prefab's root name)
		glm::vec3 position{0.0f};
		glm::vec3 eulerDeg{0.0f};
		glm::vec3 scale{1.0f};
		std::vector<PrefabEntityOverride> overrides; // applied after expansion
		// Prefab entities (by stable guid) deleted in this instance; removed on load.
		std::vector<std::uint64_t> removedGuids;
		// Entities added to this instance beyond the prefab (self-contained subtrees,
		// internal parentIndex; roots attach to the instance root). Kept across reloads.
		std::vector<EntityRecord> addedEntities;
	};

	// v13 added Physics2D records (rigid_body_2d / collider_2d / joint_2d).
	// v14 added the Physics3D feature flag (feature-driven system activation).
	// v15 added the tile_map record and made Tilemaps a Scene2D default feature.
	// v16 added prefab_instances (linked prefab instances).
	inline constexpr int kSceneFormatVersion = 16;

	struct AssetManifestEntry
	{
		std::string id;
		std::string type;
		std::string path;
		int subIndex = -1;
		bool builtin = false;
	};

	struct SceneDescription
	{
		std::string name;
		int version = kSceneFormatVersion;
		SceneKind kind = SceneKind::Scene3D;
		SceneFeatureFlags features = DefaultSceneFeatures(SceneKind::Scene3D);
		std::vector<EntityRecord> entities;
		std::vector<PrefabInstanceRecord> prefabInstances;
		std::vector<LightRecord> lights;
		std::optional<EnvironmentRecord> environment;
		std::vector<AssetManifestEntry> assetManifest;
	};

	// Assign a fresh, stable guid to every entity that lacks one (max existing + 1).
	// Existing guids are preserved, so reordering never remaps them. Called on save.
	void AssignPrefabGuids(SceneDescription& prefab);

	// A fresh random 64-bit scene-node id (never 0). Used to stamp entities that lack a stable
	// SceneNodeComponent so scene parenting can reference nodes by id instead of array position.
	std::uint64_t GenerateSceneNodeId();

	SceneDescription CaptureScene(World& world, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer = nullptr);

	SceneDescription CapturePrefab(World& world, Entity root, const MaterialRegistry& materials, const TextureRegistry& textures);

	// A SpriteAnimator drives the renderer's frame (spriteId/uvRect/pixelSize/pivot)
	// every editor-preview frame. That is runtime state, not authored data, so both
	// capture (to keep saves stable) and the undo diff (to ignore preview animation)
	// canonicalize those fields to a fixed value when an entity has both components.
	void CanonicalizeAnimatedSpriteFrame(EntityRecord& record);

	SceneDescription CaptureSubtrees(World& world, const std::vector<Entity>& roots, const MaterialRegistry& materials, const TextureRegistry& textures);

	// includeSceneHeader=false omits the [scene] block (kind/features/version) - used
	// for prefab fragments, which are header-less by convention. Writing a header onto
	// a header-less prefab would wrongly stamp it kind='3d' with 3D features.
	std::string WriteToml(const SceneDescription& scene, bool includeSceneHeader = true);
	std::optional<SceneDescription> ParseToml(std::string_view text);

	// True when a scene's top-level text declares no main-camera entity AND no prefab
	// instances (which could carry one). Publish uses this to warn - non-fatally, since
	// a script may create the camera at runtime - that a shipped startup scene would
	// otherwise boot with a default camera.
	bool SceneTextHasNoCameraSource(std::string_view sceneToml);

	// Field-level prefab overrides. ComputePrefabOverrideToml returns the TOML text of
	// only the top-level keys where `live` differs from `prefab` (empty if identical);
	// callers should canonicalize transforms first so precision noise is not an
	// override. MergePrefabOverride overlays that partial text onto a fresh copy of the
	// prefab record, so keys the instance did not override always track the prefab.
	std::string ComputePrefabOverrideToml(const EntityRecord& live, const EntityRecord& prefab);
	EntityRecord MergePrefabOverride(const EntityRecord& prefab, const std::string& partialToml);

	// Serialize once into BOTH the text and cooked-binary forms, sharing a single
	// document-tree build (cheaper than calling WriteToml + WriteSceneBinary).
	void SerializeScene(const SceneDescription& scene, std::string& outToml, std::vector<std::byte>& outBinary, bool includeSceneHeader = true);

	// Binary scene/prefab format: the TOML document tree encoded as compact bytes
	// (magic + version header). Same content as WriteToml/ParseToml, but tokenizer-
	// free to load - the cooked runtime form. ReadSceneBinary fails closed
	// (std::nullopt) on a bad magic/version so callers can fall back to TOML.
	std::vector<std::byte> WriteSceneBinary(const SceneDescription& scene, bool includeSceneHeader = true);
	std::optional<SceneDescription> ReadSceneBinary(const std::byte* data, std::size_t size);
	std::optional<SceneDescription> ReadSceneBinary(const std::vector<std::byte>& bytes);

	void SetProjectSceneDirectories(std::filesystem::path scenesDir, std::filesystem::path prefabsDir);
	void ClearProjectSceneDirectories();

	std::string ScenesDirectory();
	// Re-cook every project scene/prefab .toml to its .bin sibling (fresh cooked
	// binaries for the shipped pak). Returns how many were written.
	std::size_t CookProjectBinaries();
	bool SaveSceneFile(const std::string& sceneName, const SceneDescription& scene);
	std::optional<SceneDescription> ReadSceneFile(const std::string& sceneName);
	std::vector<std::string> ListSceneFiles();

	std::string PrefabsDirectory();
	bool SavePrefabFile(const std::string& prefabName, const SceneDescription& prefab);
	std::optional<SceneDescription> ReadPrefabFile(const std::string& prefabName);
	std::vector<std::string> ListPrefabFiles();

	struct ApplySceneDeps
	{
		AssetManager* assets = nullptr;
		PrimitiveMeshes* primitives = nullptr;
		effects::EffectManager* effectManager = nullptr;
		EffectParamBuffer* effectParams = nullptr;
		PipelineCache* pipelines = nullptr;
		scripting::SceneContext* sceneContext = nullptr;
		PhysicsSystem* physics = nullptr;
		Renderer* renderer = nullptr;
		AssetDatabase* assetDatabase = nullptr;

		std::function<bool(const std::string& vfsModelPath, std::string& error)> ensureModelBaked;
	};

	ApplySceneDeps MakeApplySceneDeps(ServiceContainer& services);

	std::vector<Entity> ApplyScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps);

	// Persistence (SceneTransient / DontDestroyOnLoad) is a GAMEPLAY behaviour:
	// only runtime scene switches spare marked subtrees. Authoring loads (the
	// editor's Open dialog, menus, control endpoint, play start/stop) always
	// reset the world completely.
	enum class SceneLoadMode
	{
		Authoring,
		GameplaySwitch,
	};

	void ReplaceScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, SceneLoadMode mode = SceneLoadMode::Authoring);

	std::vector<Entity> RestoreSceneInPlace(const SceneDescription& scene, World& world, const ApplySceneDeps& deps);

	// Restore a self-contained subtree snapshot (from CaptureSubtrees) onto the
	// exact entity ids it was captured under - reusing live handles, recreating any
	// missing ones via World::CreateWithId, and reattaching the subtree roots to
	// `attachParent` (invalid = scene root). Unlike RestoreSceneInPlace it never
	// touches entities outside the snapshot, so it is the primitive behind the
	// typed create/delete/edit undo commands. Returns the target entities, aligned
	// with scene.entities.
	std::vector<Entity> RestoreSubtreeInPlace(const SceneDescription& scene, World& world, const ApplySceneDeps& deps, Entity attachParent);

	// ReplaceScene from a scene file on disk.
	bool LoadSceneFile(const std::string& sceneName, World& world, const ApplySceneDeps& deps, SceneLoadMode mode = SceneLoadMode::Authoring);

	// outCreated (optional) receives the created entities aligned with prefab.entities
	// by index, so callers can map each spawned entity back to its source prefab entry.
	Entity InstantiatePrefab(const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld, std::vector<Entity>* outCreated = nullptr);

	// Create a LINKED prefab instance (unlike InstantiatePrefab, which copies): the
	// root is tagged (PrefabInstance + SceneTransient + PrefabLink subtree) so the
	// scene re-serializes it as a reference and edits to the prefab propagate to
	// every instance. `prefabName` is the prefab's save name (what a scene stores).
	Entity InstantiatePrefabInstance(const std::string& prefabName, const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld);
} // namespace aether::app::scene
