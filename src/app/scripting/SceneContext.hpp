#pragma once

#include <deque>
#include <string>
#include <vector>

#include "gpu/GpuTypes.hpp"
#include "scene/Entity.hpp"
#include "scene/LoadedModel.hpp"
#include "material/EffectManager.hpp"
#include "material/MaterialAsset.hpp"

namespace aether
{
	class World;
	class AssetManager;
	class CameraManager;
	class Renderer;
	class Input;
	class GraphicsPipeline;
	class PrimitiveMeshes;
	class PhysicsSystem;
	class Physics2DSystem;
	class IEngineRuntime;
	class ServiceContainer;
	struct DebugVertex;
} // namespace aether

namespace aether::app
{
	class DayNightSystem;
}

namespace aether::app::scripting
{
	struct SceneContext
	{
		aether::World* world = nullptr;
		aether::AssetManager* assets = nullptr;
		aether::CameraManager* cameras = nullptr;
		aether::Renderer* renderer = nullptr;
		aether::Input* input = nullptr;
		aether::ServiceContainer* services = nullptr;
		std::vector<aether::DebugVertex>* debugVertices = nullptr;
		aether::app::DayNightSystem* dayNight = nullptr;
		aether::effects::EffectManager* effects = nullptr;
		aether::PhysicsSystem* physics = nullptr;
		aether::Physics2DSystem* physics2D = nullptr;
		// Used to quiesce the render thread around script-triggered GPU work
		aether::IEngineRuntime* engineRuntime = nullptr;
		gpu::CommandPool uploadPool = nullptr;
		float deltaTime = 0.0f;
		float elapsedTime = 0.0f;
		std::uint64_t frameCount = 0;

		std::vector<aether::Entity> sceneEntities;

		// Scene switch requested by a script (Scene.Load). Applied at the end
		// of the script update - a load tears down every entity, so it must
		// never run inside a script callback.
		std::string pendingSceneLoad;

		// Entities destroyed by scripts (Entity.Destroy). Deferred to the end
		// of the script update: an immediate destroy would free the component
		// storage the script runner is iterating.
		std::vector<aether::Entity> pendingDestroys;

		// Loaded model data.  Must outlive the mesh entities that reference it.
		std::deque<aether::LoadedModel> loadedModels;
		std::unordered_map<std::string, size_t> loadedModelMap;

		struct CachedMesh
		{
			const aether::Mesh* mesh = nullptr;
			aether::MaterialAsset materialAsset{};
			std::string displayName;
			std::string kindName;
		};

		aether::PrimitiveMeshes* primitives = nullptr;
		std::vector<CachedMesh> meshCache;
		aether::MaterialAsset defaultMaterial{};
		bool defaultMaterialInitialized = false;
	};

	extern thread_local SceneContext* g_activeContext;

	// Accessor used inside binding functions. Never returns null during a script call.
	inline SceneContext& ActiveContext()
	{
		return *g_activeContext;
	}
} // namespace aether::app::scripting
