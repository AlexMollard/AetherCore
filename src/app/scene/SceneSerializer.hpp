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

	struct UICanvasRecord
	{
		std::uint8_t scaleMode = 0;
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
		bool pixelArt = false;
		std::string texturePath;
	};

	struct UITextRecord
	{
		std::string text;
		std::string fontName = "Roboto";
		float pixelSize = 24.f;
		glm::vec4 color{1.f};
		std::uint8_t hAlign = 0;
		std::uint8_t vAlign = 0;
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
		std::optional<UICanvasRecord> uiCanvas;
		std::optional<UIRectRecord> uiRect;
		std::optional<UIImageRecord> uiImage;
		std::optional<UITextRecord> uiText;
		// Pure data-only components (no asset resolution or side effects) are
		// captured/applied/serialized generically from the reflection registry - see
		// GenericComponentTypeNames(). Adding one needs only its AE_COMPONENT
		// declaration plus an entry in that list; no per-component serializer code.
		std::vector<GenericComponent> reflected;
		std::optional<ParticleEmitterComponent> particles;
		std::optional<PointLightComponent> pointLight;
		std::optional<SpotLightComponent> spotLight;
		std::optional<DayNightComponent> dayNight;
		std::optional<TileMapComponent> tileMap;
		// backingCamera is runtime state and is never serialized. mainCamera marks
		std::optional<CameraComponent> camera;
		bool mainCamera = false;
		std::optional<OrbitCameraComponent> orbitCamera;
		std::vector<ScriptRecord> scripts;
		std::optional<JointRecord> joint;
		// 2D physics: authored fields only (runtime body/shape/joint handles are
		// stripped at capture so play-stop restore never resurrects stale ids).
		std::optional<RigidBody2DComponent> rigidBody2D;
		std::optional<Collider2DComponent> collider2D;
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

	// v13 added Physics2D records (rigid_body_2d / collider_2d / joint_2d).
	// v14 added the Physics3D feature flag (feature-driven system activation).
	// v15 added the tile_map record and made Tilemaps a Scene2D default feature.
	inline constexpr int kSceneFormatVersion = 15;

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
		std::vector<LightRecord> lights;
		std::optional<EnvironmentRecord> environment;
		std::vector<AssetManifestEntry> assetManifest;
	};

	SceneDescription CaptureScene(World& world, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer = nullptr);

	SceneDescription CapturePrefab(World& world, Entity root, const MaterialRegistry& materials, const TextureRegistry& textures);

	SceneDescription CaptureSubtrees(World& world, const std::vector<Entity>& roots, const MaterialRegistry& materials, const TextureRegistry& textures);

	// Reflected component type names captured/applied/serialized generically (pure
	// data-only components). Capture, Apply and the TOML/binary codecs all key off
	// this one list, so adding such a component is a single entry here.
	const std::vector<std::string>& GenericComponentTypeNames();

	std::string WriteToml(const SceneDescription& scene);
	std::optional<SceneDescription> ParseToml(std::string_view text);

	// Binary scene/prefab format: the TOML document tree encoded as compact bytes
	// (magic + version header). Same content as WriteToml/ParseToml, but tokenizer-
	// free to load - the cooked runtime form. ReadSceneBinary fails closed
	// (std::nullopt) on a bad magic/version so callers can fall back to TOML.
	std::vector<std::byte> WriteSceneBinary(const SceneDescription& scene);
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

	Entity InstantiatePrefab(const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld);
} // namespace aether::app::scene
