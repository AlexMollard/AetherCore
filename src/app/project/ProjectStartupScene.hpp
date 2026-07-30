#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether::app
{
	// The startup scene is project data, not a user preference. It lives in the project's
	// ProjectSettings.toml so it travels with the project, reaches a published build and
	// means the same thing on every machine - a per-user copy would boot one scene here
	// and an empty world on a fresh clone. Everything that reads or writes it goes through
	// this header, so there is exactly one store for it.

	// The scene name recorded in the project file. Empty when unset or unreadable.
	[[nodiscard]] std::string ReadProjectStartupScene(const std::filesystem::path& projectFile);

	// Writes app.startupScene into the project file, preserving every other key. Refuses
	// rather than clobbering when the file exists but cannot be read or parsed.
	bool WriteProjectStartupScene(const std::filesystem::path& projectFile, std::string_view sceneName, std::string& error);

	// Whether the project actually carries this scene. The authored .scene.toml is the
	// truth: the .scene.bin beside it is a rebuildable cache and is absent on a fresh clone.
	[[nodiscard]] bool ProjectHasScene(const std::filesystem::path& scenesDir, std::string_view sceneName);

	// A startup scene must name a scene the project has. A dangling name boots an empty
	// world, and a published game has no editor to notice, so this is an error - never a
	// warning - on every path that would bake or persist it.
	bool ValidateProjectStartupScene(const std::filesystem::path& scenesDir, std::string_view sceneName, std::string& error);
} // namespace aether::app
