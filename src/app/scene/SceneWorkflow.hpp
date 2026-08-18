#pragma once
#include <cstdint>
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

	// What a failed load should leave behind. The two callers genuinely disagree, which is
	// why this is a parameter rather than a policy baked into SwitchScene: opening a scene
	// must not cost you the one you had open (a corrupt file or a typo'd name would
	// otherwise wipe unsaved work and leave a save about to overwrite the real thing),
	// while switching PROJECTS must clear, or the old project's scene lingers in the new one.
	enum class OnLoadFailure : std::uint8_t
	{
		KeepCurrent,
		ClearWorld,
	};

	// Loads `sceneName` over the live world. Returns false if it could not be loaded, in
	// which case `onFailure` decides whether the current scene survives.
	bool SwitchScene(const std::string& sceneName, World& world, const ApplySceneDeps& deps, OnLoadFailure onFailure);
} // namespace aether::app::scene
