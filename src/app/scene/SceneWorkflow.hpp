#pragma once
#include <string>
#include "scene/SceneSerializer.hpp"

namespace aether
{
	class World;
}

namespace aether::app::scene
{
	// Loads the "default" scene template via ReplaceScene. Returns the scene name
	// ("Untitled") on success, empty string on failure.
	std::string NewScene(World& world, const ApplySceneDeps& deps);

	// Captures the world and writes it to `sceneName`. Returns false if the name
	// is empty (caller should fall back to Save-As) or the write fails.
	bool QuickSave(World& world, const std::string& sceneName, const MaterialRegistry& materials, const TextureRegistry& textures, const Renderer* renderer);

	// Switches the live scene to `sceneName` via ReplaceScene, tearing the
	// current scene down first. When the name is empty or no such scene file
	// exists, the world is cleared instead so a previous scene never lingers
	// (e.g. when the active project changes). Returns true when a scene file was
	// loaded, false when the world was cleared.
	bool SwitchScene(const std::string& sceneName, World& world, const ApplySceneDeps& deps);
} // namespace aether::app::scene
