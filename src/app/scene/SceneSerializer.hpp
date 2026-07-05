#pragma once

#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "material/EffectParams.hpp"
#include "material/MaterialAsset.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"

namespace aether
{
	class AssetManager;
	class EffectParamBuffer;
	class MaterialRegistry;
	class PipelineCache;
	class PrimitiveMeshes;
	class TextureRegistry;
	class World;
} // namespace aether

namespace aether::app::effects
{
	class EffectManager;
} // namespace aether::app::effects

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
	};

	struct SceneDescription
	{
		std::string name;
		std::vector<EntityRecord> entities;
	};

	// ── Capture / TOML ──────────────────────────────────────────────────────────

	// Snapshot every live entity. Registries are used read-only (material
	// describe + texture-path lookup).
	SceneDescription CaptureScene(World& world, const MaterialRegistry& materials, const TextureRegistry& textures);

	std::string WriteToml(const SceneDescription& scene);
	std::optional<SceneDescription> ParseToml(std::string_view text);

	// ── Files ───────────────────────────────────────────────────────────────────
	// Dev builds write into the repo (AETHER_SCENES_SOURCE_DIR, committable);
	// otherwise the engine-settings directory convention applies.

	std::string ScenesDirectory();
	bool SaveSceneFile(const std::string& sceneName, const SceneDescription& scene);
	std::optional<SceneDescription> ReadSceneFile(const std::string& sceneName);
	std::vector<std::string> ListSceneFiles();

	// ── Apply (load) ────────────────────────────────────────────────────────────

	struct ApplySceneDeps
	{
		AssetManager* assets = nullptr;                  // model load + registries access
		PrimitiveMeshes* primitives = nullptr;           // primitive mesh resolve
		effects::EffectManager* effectManager = nullptr; // effect-by-name re-apply
		EffectParamBuffer* effectParams = nullptr;
		scripting::SceneContext* sceneContext = nullptr; // model cache + sceneEntities registration
	};

	// Instantiates the description into the world (asset resolution degrades
	// gracefully when a dep is missing - entities and value components still
	// build, which is what the doctests exercise). Returns the created entities
	// in record order.
	std::vector<Entity> ApplyScene(const SceneDescription& scene, World& world, const ApplySceneDeps& deps);

	// Replace-all load: destroys every live entity (on_destroy hooks release
	// physics bodies / material slots / effect slots), applies the file and
	// re-registers sceneEntities so F5 script reload still cleans up.
	bool LoadSceneFile(const std::string& sceneName, World& world, const ApplySceneDeps& deps);
} // namespace aether::app::scene
