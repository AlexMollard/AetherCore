#pragma once
#include <string>
#include "scene/SceneSerializer.hpp"

namespace aether
{
	class World;
}

namespace aether::app::scene
{
	// Replaces the live scene with the shipped default for `kind`
	// ("default" / "default2d"), falling back to an empty scene of that kind.
	std::string NewScene(World& world, const ApplySceneDeps& deps, SceneKind kind = SceneKind::Scene3D);

	bool QuickSave(World& world, const std::string& sceneName, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer);

	// exists, the world is cleared instead so a previous scene never lingers
	bool SwitchScene(const std::string& sceneName, World& world, const ApplySceneDeps& deps);
} // namespace aether::app::scene
