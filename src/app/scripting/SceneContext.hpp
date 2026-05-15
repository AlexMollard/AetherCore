#pragma once

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "scene/Entity.hpp"
#include "AetherCore.hpp" // for LoadedModel

namespace aether
{
	class World;
	class AssetManager;
	class CameraManager;
	class Renderer;
	class Input;
	class GraphicsPipeline;
} // namespace aether

namespace aether::app
{
	class SystemFactory;
}

namespace aether::app::scripting
{
	// Tracks a logical entity created by spawn().
	// Holds the accumulated transform and the mesh entities spawned by load_model().
	struct PendingEntity
	{
		glm::mat4 transform{ 1.0f };
		std::vector<aether::Entity> meshEntities;
	};

	// Per-script-layer context: all engine services + bookkeeping for cleanup.
	struct SceneContext
	{
		aether::World* world = nullptr;
		aether::AssetManager* assets = nullptr;
		aether::CameraManager* cameras = nullptr;
		aether::Renderer* renderer = nullptr;
		aether::Input* input = nullptr;
		aether::app::SystemFactory* systemFactory = nullptr;
		aether::GraphicsPipeline* defaultPipeline = nullptr;
		float deltaTime = 0.0f;
		std::string scriptPath;

		// Logical entities created by spawn().  Destroyed on scene unload/reload.
		// Key = entt entity id (uint32_t).
		std::unordered_map<uint32_t, PendingEntity> pendingEntities;

		// All entities that should be destroyed on scene unload/reload.
		// Includes both logical entities and spawned mesh entities.
		std::vector<aether::Entity> sceneEntities;

		// Loaded model data.  Must outlive the entities that reference it.
		// std::deque does not invalidate references on push_back.
		std::deque<aether::LoadedModel> loadedModels;

		// Names of C++ systems registered via register_system().
		// Unregistered from World on scene unload/reload.
		std::vector<std::string> registeredSystems;
	};

	// The active SceneContext for the current script call.
	// Set by ScriptedSceneLayer immediately before any das function invocation
	// and cleared immediately after.  All module binding functions read from here.
	extern thread_local SceneContext* g_activeContext;

	// Accessor used inside binding functions. Never returns null during a script call.
	inline SceneContext& ActiveContext()
	{
		return *g_activeContext;
	}
} // namespace aether::app::scripting
