#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"

namespace aether::app::scene
{
	std::string NewScene(World& world, const ApplySceneDeps& deps, SceneKind kind)
	{
		const auto desc = ReadSceneFile(kind == SceneKind::Scene2D ? "default2d" : "default");
		if (desc.has_value())
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

	bool SwitchScene(const std::string& sceneName, World& world, const ApplySceneDeps& deps)
	{
		if (!sceneName.empty() && LoadSceneFile(sceneName, world, deps))
		{
			return true;
		}
		SceneDescription empty;
		empty.kind = world.GetSceneKind();
		empty.features = world.GetSceneFeatures();
		ReplaceScene(empty, world, deps);
		return false;
	}
} // namespace aether::app::scene
