#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"

#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::app::scene
{
	std::string NewScene(World& world, const ApplySceneDeps& deps, SceneKind kind)
	{
		// Use the kind-specific template ("default"/"default2d") only if it actually
		// matches the requested kind. A project can shadow those names with a scene of
		// the OTHER kind (e.g. a 2D game whose own "default" scene is 2D), which would
		// otherwise make "new 3D scene" produce a 2D world - and a 2D world keeps the
		// editor camera orthographic, so the new 3D scene opened on the 2D view camera.
		if (const auto desc = ReadSceneFile(kind == SceneKind::Scene2D ? "default2d" : "default"); desc.has_value() && desc->kind == kind)
		{
			ReplaceScene(*desc, world, deps);
			return desc->name.empty() ? std::string{"Untitled"} : desc->name;
		}

		SceneDescription empty;
		empty.name = "Untitled";
		empty.kind = kind;
		empty.features = DefaultSceneFeatures(kind);
		ReplaceScene(empty, world, deps);
		return empty.name;
	}

	bool QuickSave(World& world, const std::string& sceneName, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer)
	{
		if (sceneName.empty())
		{
			return false;
		}
		return SaveSceneFile(sceneName, CaptureScene(world, materials, textures, renderer));
	}

	bool SwitchScene(const std::string& sceneName, World& world, const ApplySceneDeps& deps, OnLoadFailure onFailure)
	{
		if (!sceneName.empty() && LoadSceneFile(sceneName, world, deps))
		{
			return true;
		}
		if (onFailure == OnLoadFailure::KeepCurrent)
		{
			AE_WARN(LogCategory::App, "SwitchScene: could not load '{}'; leaving the current scene untouched.", sceneName);
			return false;
		}
		SceneDescription empty;
		empty.kind = world.GetSceneKind();
		empty.features = world.GetSceneFeatures();
		ReplaceScene(empty, world, deps);
		return false;
	}
} // namespace aether::app::scene
