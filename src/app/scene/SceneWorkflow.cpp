#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"

namespace aether::app::scene
{
	std::string NewScene(World& world, const ApplySceneDeps& deps)
	{
		const auto desc = ReadSceneFile("default");
		if (!desc.has_value())
		{
			return {};
		}
		ReplaceScene(*desc, world, deps);
		return desc->name.empty() ? std::string{"Untitled"} : desc->name;
	}

	bool QuickSave(World& world, const std::string& sceneName, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer)
	{
		if (sceneName.empty())
		{
			return false;
		}
		return SaveSceneFile(sceneName, CaptureScene(world, materials, textures, renderer));
	}
} // namespace aether::app::scene
