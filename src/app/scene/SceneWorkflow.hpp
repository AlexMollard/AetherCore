#pragma once
#include <string>
#include "scene/SceneSerializer.hpp"

namespace aether
{
	class World;
}

namespace aether::app::scene
{
	std::string NewScene(World& world, const ApplySceneDeps& deps);

	bool QuickSave(World& world, const std::string& sceneName, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer);

	// exists, the world is cleared instead so a previous scene never lingers
	bool SwitchScene(const std::string& sceneName, World& world, const ApplySceneDeps& deps);
} // namespace aether::app::scene
