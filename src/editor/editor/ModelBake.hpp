#pragma once

#include <filesystem>
#include <string>

namespace aether::app
{
	struct EditorProjectContext;
}

namespace aether::editor
{
	// so without this a dragged model can never load in the editor.
	[[nodiscard]] bool EnsureModelBaked(const std::string& vfsModelPath, const app::EditorProjectContext& project, std::string& error);

	// Same, for callers that have the project's root but no open-project context - publish
	// bakes from a step, not from the editor's own project state.
	[[nodiscard]] bool EnsureModelBaked(const std::string& vfsModelPath, const std::filesystem::path& projectRoot, std::string& error);
} // namespace aether::editor
