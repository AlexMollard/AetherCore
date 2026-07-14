#pragma once

#include <string>

namespace aether::app
{
	struct EditorProjectContext;
}

namespace aether::editor
{
	// so without this a dragged model can never load in the editor.
	[[nodiscard]] bool EnsureModelBaked(const std::string& vfsModelPath, const app::EditorProjectContext& project, std::string& error);
} // namespace aether::editor
