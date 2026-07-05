#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "material/EffectParams.hpp"
#include "material/MaterialAsset.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"

namespace aether
{
	class AssetManager;
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
} // namespace aether::effects

namespace aether::app::scripting
{
	struct SceneContext;
} // namespace aether::app::scripting

namespace aether::app::scene
{
	// ── Pure records ────────────────────────────────────────────────────────────
	// A SceneDescription is the value-typed image of the world: everything either
	// serializes directly or reduces to stable identity (paths, kinds, names).
	// Capture/Apply translate between it and the live registry; Write/Parse
	// translate between it and TOML. The split keeps the file format and the
	// GPU-facing resolution independently testable.

	struct MaterialRecord
	{
		// Texture handles inside are runtime-only; the paths below re-acquire them.
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
	};

	struct EffectRecord
	{
		std::string name;
		EffectParams params{};
	};

	struct EntityRecord
	{
		std::string name;
		std::vector<std::string> tags;
		bool hasTransform = false;
		glm::vec3 position{0.0f};
		glm::vec3 eulerDeg{0.0f};
		glm::vec3 scale{1.0f};
		int parentIndex = -1; // file-local index into SceneDescription::entities
		std::optional<MeshSourceComponent> mesh;
		std::optional<MaterialRecord> material;
		std::optional<SkinnedRecord> skinned;
		std::optional<PhysicsRecord> physics;
		std::optional<EffectRecord> effect;
		// Data-driven behaviors (BehaviorComponents.hpp) serialize as plain data;
		// transient fields (bob base capture, pulse time) reset on apply.
		std::optional<BobComponent> bob;
		std::optional<SpinComponent> spin;
		std::optional<OrbitComponent> orbit;
		std::optional<MaterialPulseComponent> materialPulse;
		// Entity lights (v3+): position/aim come from the TRS above.
		std::optional<PointLightComponent> pointLight;
		std::optional<SpotLightComponent> spotLight;
		// Entity script path/type (v4+, ScriptComponent). Attach state is runtime.
		std::optional<std::string> script;
		// Serialized C# script field overrides (v5+). Empty when none.
		std::map<std::string, ScriptPropertyValue> scriptProperties;
	};

	// LEGACY (pre-v3): renderer-level light list. Still parsed so old files
	// load - ApplyScene migrates each record to a light ENTITY - but capture
	// and write emit per-entity light components instead.
	struct LightRecord
	{
		bool isSpot = false;
		glm::vec3 position{0.0f};
		float radius = 1.0f;
		glm::vec3 color{1.0f};
		float intensity = 1.0f;
		bool castsShadow = false;
		// Spot only:
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

	// Format version written to [scene]. Bump when records gain fields whose
	// absence silently degrades a loaded scene; ParseToml warns on older files.
	// v1 = Spec-3 (no behavior components, no lights/environment);
	// v2 = Spec-4 (behaviors, renderer-level [[lights]], environment);
	// v3 = lights are entities (per-entity point_light/spot_light tables);
	// v4 = entity script components (path-referenced scripts);
	// v5 = C# script type names + serialized script_properties.
	inline constexpr int kSceneFormatVersion = 5;

	struct SceneDescription
	{
		std::string name;
		int version = kSceneFormatVersion;
		std::vector<EntityRecord> entities;
		std::vector<LightRecord> lights;
		std::optional<EnvironmentRecord> environment;
	};

	// ── Capture / TOML ──────────────────────────────────────────────────────────

	// Snapshot every live entity, plus lights + environment when a Renderer is
	// supplied. Registries are used read-only (material describe + texture-path
	// lookup).
	SceneDescription CaptureScene(World& world, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer = nullptr);

	// Prefab = a scene description of ONE subtree (root record first,
	// parentIndex -1). No environment/lights sections and no transient
	// exclusion - a prefab captures exactly the subtree you point it at.
	SceneDescription CapturePrefab(World& world, Entity root, const MaterialRegistry& materials, const TextureRegistry& textures);

	// Multi-root variant (clipboard copy of a whole selection): each root's
	// subtree in parent-first order, roots recording parentIndex -1. Callers
	// pass selection ROOTS (no root inside another root's subtree).
	SceneDescription CaptureSubtrees(World& world, const std::vector<Entity>& roots, const MaterialRegistry& materials, const TextureRegistry& textures);

	std::string WriteToml(const SceneDescription& scene);
	std::optional<SceneDescription> ParseToml(std::string_view text);

	// ── Files ───────────────────────────────────────────────────────────────────
	// Dev builds write into the repo (AETHER_SCENES_SOURCE_DIR, committable);
	// otherwise the engine-settings directory convention applies.

	std::string ScenesDirectory();
	bool SaveSceneFile(const std::string& sceneName, const SceneDescription& scene);
	std::optional<SceneDescription> ReadSceneFile(const std::string& sceneName);
	std::vector<std::string> ListSceneFiles();

	// Prefab files: same TOML format, ".prefab.toml" under resources/prefabs.
	std::string PrefabsDirectory();
	bool SavePrefabFile(const std::string& prefabName, const SceneDescription& prefab);
	std::optional<SceneDescription> ReadPrefabFile(const std::string& prefabName);
	std::vector<std::string> ListPrefabFiles();

	// ── Apply (load) ────────────────────────────────────────────────────────────

	struct ApplySceneDeps
	{
		AssetManager* assets = nullptr;                  // model load + registries access
		PrimitiveMeshes* primitives = nullptr;           // primitive mesh resolve
		effects::EffectManager* effectManager = nullptr; // effect-by-name re-apply
		EffectParamBuffer* effectParams = nullptr;
		PipelineCache* pipelines = nullptr;              // effect pipeline resolve (headless-testable, unlike assets)
		scripting::SceneContext* sceneContext = nullptr; // model cache + sceneEntities registration
		PhysicsSystem* physics = nullptr;                // step-idle guard before replace-all destroys
		Renderer* renderer = nullptr;                    // lights + environment re-apply
	};

	// Resolves every apply dependency from the service container - the shared
	// path for editor panels, undo and any other tooling.
	ApplySceneDeps MakeApplySceneDeps(ServiceContainer& services);

	// Instantiates the description into the world (asset resolution degrades
	// gracefully when a dep is missing - entities and value components still
	// build, which is what the doctests exercise). Returns the created entities
	// in record order.
	std::vector<Entity> ApplyScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps);

	// Replace-all: waits out the async physics step, destroys every live entity
	// (on_destroy hooks release physics bodies / material slots / effect slots),
	// applies the description and re-registers sceneEntities so F5 script reload
	// still cleans up. Shared by LoadSceneFile and the editor's Stop-restore.
	void ReplaceScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps);

	// ReplaceScene from a scene file on disk.
	bool LoadSceneFile(const std::string& sceneName, World& world, const ApplySceneDeps& deps);

	// Additively instantiates a prefab and places its root at `localToWorld`
	// (the subtree keeps its internal offsets - delta-propagating re-root).
	// Returns the instantiated root entity, invalid if the prefab is empty.
	Entity InstantiatePrefab(const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld);
} // namespace aether::app::scene
