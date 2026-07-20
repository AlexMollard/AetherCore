#pragma once

#include <filesystem>
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
} // namespace aether::app::project
