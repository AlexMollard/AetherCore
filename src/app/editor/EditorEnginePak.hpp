#pragma once

#include <filesystem>
#include <optional>

#include "editor/EditorProjectActions.hpp"

namespace aether::app
{
	// True in dev checkouts (AETHER_ENGINE_RESOURCES_DIR defined and present);
	// false in a shipped editor with no engine resources tree.
	[[nodiscard]] bool CanBakeEnginePak();

	// Absolute path to the engine resources source dir, or nullopt when unavailable.
	[[nodiscard]] std::optional<std::filesystem::path> EngineResourcesDir();

	// Bakes engine-owned runtime resources into `outputEnginePak` (staging the
	// engine asset subdirs so virtual paths match the CMake build, e.g. "fonts/..").
	[[nodiscard]] EditorProjectActionResult BakeEnginePak(const std::filesystem::path& outputEnginePak);
} // namespace aether::app
