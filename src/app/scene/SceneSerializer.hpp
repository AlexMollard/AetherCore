#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "material/EffectParams.hpp"
#include "material/MaterialAsset.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/ModelBakeHook.hpp"

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
		// Material + body tunables (v8+): the Collider's friction/bounce/sensor and
		// the RigidBody's mass/damping/gravity/CCD/axis-locks survive a save/load.
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

	// Joint/constraint record. target is a file-local entity index (-1 = world),
	// resolved on apply the same way script entity references are.
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

	// UI records are self-contained plain data - they do NOT depend on the
	// aether::ui:: enums so this header stays decoupled from the UI module;
	// Apply (in the .cpp) maps the stored indices to the real ui:: types.
	struct UICanvasRecord
	{
		std::uint8_t scaleMode = 0; // 0 ConstantPixel, 1 ScaleWithReference
		glm::vec2 referenceResolution{1920.f, 1080.f};
		int sortBias = 0;
	};

	struct UIRectRecord
	{
		glm::vec2 anchorMin{0.5f, 0.5f};
		glm::vec2 anchorMax{0.5f, 0.5f};
		glm::vec2 offsetMin{-50.f, -50.f};
		glm::vec2 offsetMax{50.f, 50.f};
		glm::vec2 pivot{0.5f, 0.5f};
	};

	struct UIImageRecord
	{
		glm::vec4 color{1.f};
		float cornerRadius = 0.f;
		std::string texturePath; // re-resolved on apply; empty = solid color
	};

	struct UITextRecord
	{
		std::string text;
		std::string fontName = "Roboto";
		float pixelSize = 24.f;
		glm::vec4 color{1.f};
		std::uint8_t hAlign = 0; // ui::UIText::HAlign
		std::uint8_t vAlign = 0; // ui::UIText::VAlign
		bool wrap = true;
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

	struct EntityRecord
	{
		// Runtime-only source entity id used by editor play-mode snapshots to
		// restore in-place. TOML read/write deliberately ignores it.
		std::uint32_t entityId = 0;
		std::string name;
		std::vector<std::string> tags;
		// Entity explicitly disabled by the user (DisabledComponent): it and its
		// subtree stop rendering/updating. Persisted so a saved scene reloads in
		// the same active/inactive state.
		bool disabled = false;
		// Unity-style renderer markers. `sprite` == SpriteRendererComponent (2D
		// quad). `meshRenderer` == MeshRendererComponent; `meshRendererVisible`
		// is its visibility toggle. Kept minimal - the mesh/material identity is
		// already carried by MeshSourceComponent + the material record.
		bool sprite = false;
		bool meshRenderer = false;
		bool meshRendererVisible = true;
		bool meshRendererCastShadows = true;
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
		// UI components (v6+): entity carries a UI element (canvas root, anchored
		// rect, image fill and/or text label). Independent optionals - an entity
		// may carry any subset (e.g. a canvas root has uiCanvas + uiRect only).
		std::optional<UICanvasRecord> uiCanvas;
		std::optional<UIRectRecord> uiRect;
		std::optional<UIImageRecord> uiImage;
		std::optional<UITextRecord> uiText;
		// Data-driven behaviors (BehaviorComponents.hpp) serialize as plain data;
		// transient fields (bob base capture, pulse time) reset on apply.
		std::optional<BobComponent> bob;
		std::optional<SpinComponent> spin;
		std::optional<OrbitComponent> orbit;
		std::optional<MaterialPulseComponent> materialPulse;
		std::optional<ScalePulseComponent> scalePulse;
		std::optional<LookAtComponent> lookAt;
		// Entity lights (v3+): position/aim come from the TRS above.
		std::optional<PointLightComponent> pointLight;
		std::optional<SpotLightComponent> spotLight;
		// Entity camera (v8+): projection params; pose/aim come from the TRS above.
		// backingCamera is runtime state and is never serialized. mainCamera marks
		// the entity as the scene's main view (MainCameraComponent).
		std::optional<CameraComponent> camera;
		bool mainCamera = false;
		// Orbit-camera params (v8+): if present, the entity is an orbit/third-person
		// camera whose pose is driven from target/yaw/pitch/distance by CameraSystem.
		std::optional<OrbitCameraComponent> orbitCamera;
		// Entity script slots (v7+, ScriptComponent). Attach state is runtime.
		std::vector<ScriptRecord> scripts;
		// Physics joint (v8+): references another entity by file-local index.
		std::optional<JointRecord> joint;
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
	// v5 = C# script type names + serialized script_properties;
	// v6 = UI components (ui_canvas/ui_rect/ui_image/ui_text);
	// v7 = multiple entity script slots.
	inline constexpr int kSceneFormatVersion = 8;

	// One row of the persisted asset manifest: a stable AssetId (hex) and the
	// source it resolves to. Lets a loaded scene populate the asset catalog
	// deterministically, puts the AssetIds in the file, and is the seam for
	// future rename-stable references (the manifest becomes the id -> path
	// authority a rename tool updates).
	struct AssetManifestEntry
	{
		std::string id;       // AssetId hex
		std::string type;     // "mesh" | "texture"
		std::string path;     // primitive kind name or VFS path
		int subIndex = -1;    // model primitive index, else -1
		bool builtin = false; // path is a built-in primitive kind name
	};

	struct SceneDescription
	{
		std::string name;
		int version = kSceneFormatVersion;
		std::vector<EntityRecord> entities;
		std::vector<LightRecord> lights;
		std::optional<EnvironmentRecord> environment;
		// Referenced assets (meshes + textures), stable-id keyed. Written after the
		// entities; ignored by readers that predate it.
		std::vector<AssetManifestEntry> assetManifest;
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
	// Editor projects may override these directories after the launcher opens a
	// project. Without an override, dev builds use the source resources folders.
	void SetProjectSceneDirectories(std::filesystem::path scenesDir, std::filesystem::path prefabsDir);
	void ClearProjectSceneDirectories();

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
		AssetDatabase* assetDatabase = nullptr;          // catalog population for the asset picker

		// Editor-only: bake an unimported model on load so the scene resolves it.
		// Null in the shipped runtime (models are pre-baked) - see ModelBakeHook.
		std::function<bool(const std::string& vfsModelPath, std::string& error)> ensureModelBaked;
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

	// Editor play-mode restore: keeps captured entities alive and restores their
	// serialized components in-place, destroying entities created after the
	// snapshot. This preserves unsaved authored entity ids and script Entity
	// field references when leaving Play.
	std::vector<Entity> RestoreSceneInPlace(const SceneDescription& scene, World& world, const ApplySceneDeps& deps);

	// ReplaceScene from a scene file on disk.
	bool LoadSceneFile(const std::string& sceneName, World& world, const ApplySceneDeps& deps);

	// Additively instantiates a prefab and places its root at `localToWorld`
	// (the subtree keeps its internal offsets - delta-propagating re-root).
	// Returns the instantiated root entity, invalid if the prefab is empty.
	Entity InstantiatePrefab(const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld);
} // namespace aether::app::scene
