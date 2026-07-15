#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"

namespace aether::app::scene
{
	std::string NewScene(World& world, const ApplySceneDeps& deps)
	{
		const auto desc = ReadSceneFile("default");
		if (desc.has_value())
		{
			ReplaceScene(*desc, world, deps);
			return desc->name.empty() ? std::string{"Untitled"} : desc->name;
		}

		SceneDescription empty;
		empty.name = "Untitled";
		empty.kind = world.GetSceneKind();
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
		ReplaceScene(empty, world, deps);
		return false;
	}
} // namespace aether::app::scene
