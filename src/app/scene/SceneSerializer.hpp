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

	struct EntityRecord
	{
		std::uint32_t entityId = 0;
		std::string name;
		std::vector<std::string> tags;
		bool disabled = false;
		bool sprite = false;
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
		// Data-driven behaviors (BehaviorComponents.hpp) serialize as plain data;
		std::optional<BobComponent> bob;
		std::optional<SpinComponent> spin;
		std::optional<OrbitComponent> orbit;
		std::optional<MaterialPulseComponent> materialPulse;
		std::optional<ScalePulseComponent> scalePulse;
		std::optional<LookAtComponent> lookAt;
		std::optional<PointLightComponent> pointLight;
		std::optional<SpotLightComponent> spotLight;
		// backingCamera is runtime state and is never serialized. mainCamera marks
		std::optional<CameraComponent> camera;
		bool mainCamera = false;
		std::optional<OrbitCameraComponent> orbitCamera;
		std::vector<ScriptRecord> scripts;
		std::optional<JointRecord> joint;
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

	inline constexpr int kSceneFormatVersion = 9;

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
		std::vector<EntityRecord> entities;
		std::vector<LightRecord> lights;
		std::optional<EnvironmentRecord> environment;
		std::vector<AssetManifestEntry> assetManifest;
	};

	SceneDescription CaptureScene(World& world, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer = nullptr);

	SceneDescription CapturePrefab(World& world, Entity root, const MaterialRegistry& materials, const TextureRegistry& textures);

	SceneDescription CaptureSubtrees(World& world, const std::vector<Entity>& roots, const MaterialRegistry& materials, const TextureRegistry& textures);

	std::string WriteToml(const SceneDescription& scene);
	std::optional<SceneDescription> ParseToml(std::string_view text);

	void SetProjectSceneDirectories(std::filesystem::path scenesDir, std::filesystem::path prefabsDir);
	void ClearProjectSceneDirectories();

	std::string ScenesDirectory();
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

	void ReplaceScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps);

	std::vector<Entity> RestoreSceneInPlace(const SceneDescription& scene, World& world, const ApplySceneDeps& deps);

	// ReplaceScene from a scene file on disk.
	bool LoadSceneFile(const std::string& sceneName, World& world, const ApplySceneDeps& deps);

	Entity InstantiatePrefab(const SceneDescription& prefab, World& world, const ApplySceneDeps& deps, const glm::mat4& localToWorld);
} // namespace aether::app::scene
