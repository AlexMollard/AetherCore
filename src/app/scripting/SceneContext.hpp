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
	class IEngineRuntime;
	class ServiceContainer;
	struct DebugVertex;
} // namespace aether

namespace aether::app
{
	class DayNightSystem;
} // namespace aether::app

namespace aether::app::scripting
{
	// Per-script-layer context: engine services + bookkeeping for scene cleanup.
	struct SceneContext
	{
		aether::World* world = nullptr;
		aether::AssetManager* assets = nullptr;
		aether::CameraManager* cameras = nullptr;
		aether::Renderer* renderer = nullptr;
		aether::Input* input = nullptr;
		// Service container of the owning layer - lets script interop reach the
		// SceneSubsystem (prefab I/O) and build ApplySceneDeps for Instantiate.
		aether::ServiceContainer* services = nullptr;
		// Per-frame immediate debug-line accumulator (the engine's pending list,
		// extracted into the RenderFramePacket each frame). Backs Debug.DrawLine.
		std::vector<aether::DebugVertex>* debugVertices = nullptr;
		aether::app::DayNightSystem* dayNight = nullptr;
		aether::effects::EffectManager* effects = nullptr;
		aether::PhysicsSystem* physics = nullptr;
		// Used to quiesce the render thread around script-triggered GPU work
		// (e.g. animation compilation) so it can't race in-flight frames.
		aether::IEngineRuntime* engineRuntime = nullptr;
		gpu::CommandPool uploadPool = nullptr;
		float deltaTime = 0.0f;
		// Play-time accumulators for the scripting Time API (advanced once per frame
		// by ScriptComponentSystem while playing).
		float elapsedTime = 0.0f;
		std::uint64_t frameCount = 0;

		// All entities that should be destroyed on scene unload/reload.
		std::vector<aether::Entity> sceneEntities;

		// Loaded model data.  Must outlive the mesh entities that reference it.
		// std::deque does not invalidate references on push_back.
		std::deque<aether::LoadedModel> loadedModels;
		std::unordered_map<std::string, size_t> loadedModelMap;

		// -- Primitive mesh cache (for create_mesh / add_mesh) -----------------
		struct CachedMesh
		{
			const aether::Mesh* mesh = nullptr;
			aether::MaterialAsset materialAsset{};
			// Display name given to entities this mesh is attached to ("Cube", ...).
			std::string displayName;
			// Canonical primitive kind string as passed to create_mesh ("cube", ...);
			// recorded into MeshSourceComponent for scene serialization.
			std::string kindName;
		};

		aether::PrimitiveMeshes* primitives = nullptr;
		std::vector<CachedMesh> meshCache;
		// Authoring data for the primitive default; a registry slot is acquired
		// per entity at add_mesh time (deduped by the registry).
		aether::MaterialAsset defaultMaterial{};
		bool defaultMaterialInitialized = false;
	};

	// The active SceneContext for the current script call.
	// Set by ScriptedSceneLayer immediately before any das function invocation
	// and cleared immediately after.  Binding functions for non-World services
	// (camera, renderer, input) read from here.
	extern thread_local SceneContext* g_activeContext;

	// Accessor used inside binding functions. Never returns null during a script call.
	inline SceneContext& ActiveContext()
	{
		return *g_activeContext;
	}
} // namespace aether::app::scripting
