#pragma once

#include <filesystem>
#include <optional>

#include "editor/EditorProjectActions.hpp"

namespace aether::editor
{
	[[nodiscard]] bool CanBakeEnginePak();

	[[nodiscard]] std::optional<std::filesystem::path> EngineResourcesDir();

	[[nodiscard]] EditorProjectActionResult BakeEnginePak(const std::filesystem::path& outputEnginePak);
} // namespace aether::editor
