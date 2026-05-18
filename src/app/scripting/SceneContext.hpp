#pragma once

#include <deque>
#include <string>
#include <vector>

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
	// Per-script-layer context: engine services + bookkeeping for scene cleanup.
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

		// All entities that should be destroyed on scene unload/reload.
		std::vector<aether::Entity> sceneEntities;

		// Loaded model data.  Must outlive the mesh entities that reference it.
		// std::deque does not invalidate references on push_back.
		std::deque<aether::LoadedModel> loadedModels;

		// Names of C++ systems registered via register_system().
		// Unregistered from World on scene unload/reload.
		std::vector<std::string> registeredSystems;
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
