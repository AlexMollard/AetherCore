#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace aether::app::project
{
	// Sanitize a display name into a safe folder name: trim ends, strip illegal
	// filename chars (\ / : * ? " < > |) and control chars, collapse internal
	// whitespace to single spaces. Returns "" when nothing usable remains, so
	// callers can treat that as invalid (the fallback name is NOT baked in here).
	[[nodiscard]] std::string SanitizeProjectFolderName(std::string_view name);

	// Compose the final project root: parent / SanitizeProjectFolderName(name),
	// normalized. Empty parent or empty sanitized name -> empty path.
	[[nodiscard]] std::filesystem::path ComposeNewProjectRoot(const std::filesystem::path& parent, std::string_view name);

	// Parent folder to propose for a new project, given the recent-project roots newest
	// first: the folder holding the newest one that still exists, else documentsDir, else
	// empty. Note "parent" - ComposeNewProjectRoot appends the name, and seeding this with
	// parent/name is what made the default create AetherProject/AetherProject.
	//
	// Deliberately not the working directory, which is what this used to be: in a dev build
	// that is the engine build tree, so new projects landed inside the engine, and from an
	// installed shortcut it is the install directory, which is usually not writable.
	// Reading the answer back off the recent list means "where do you keep your projects"
	// is not persisted a second time.
	[[nodiscard]] std::filesystem::path DefaultNewProjectParent(std::span<const std::filesystem::path> recentRoots, const std::filesystem::path& documentsDir);
} // namespace aether::app::project
