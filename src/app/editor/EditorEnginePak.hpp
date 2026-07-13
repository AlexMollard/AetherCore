#pragma once

#include <filesystem>
#include <optional>

#include "editor/EditorProjectActions.hpp"

namespace aether::editor
{
	// True in dev checkouts (AETHER_ENGINE_RESOURCES_DIR defined and present, and
	// - when this editor build was compiled with AETHER_SHADER_BUILD_DIR - that
	// compiled shader dir present too); false in a shipped editor with no engine
	// resources tree.
	[[nodiscard]] bool CanBakeEnginePak();

	// Absolute path to the engine resources source dir, or nullopt when unavailable.
	[[nodiscard]] std::optional<std::filesystem::path> EngineResourcesDir();

	// Bakes engine-owned runtime resources into `outputEnginePak` (staging the
	// engine asset subdirs so virtual paths match the CMake build, e.g. "fonts/..",
	// plus compiled shaders under "shaders/.." when this editor build has them).
	[[nodiscard]] EditorProjectActionResult BakeEnginePak(const std::filesystem::path& outputEnginePak);
} // namespace aether::editor
