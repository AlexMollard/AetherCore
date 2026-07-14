#pragma once

#include <filesystem>
#include <vector>

#include "editor/EditorProjectActions.hpp"

namespace aether::editor
{
	// Finds installed Visual Studio IDEs (not Build Tools). The editor exposes
	// these entries so the user can choose the IDE used for managed debugging.
	[[nodiscard]] std::vector<VisualStudioInstallation> FindVisualStudioInstallations();

	// Opens a project's managed scripts in the selected Visual Studio and asks
	// that same IDE instance to attach to this Editor process through EnvDTE
	// automation. Windows only; other platforms return a clear unsupported result.
	[[nodiscard]] EditorProjectActionResult OpenVisualStudioAndAttachScriptDebugger(const std::filesystem::path& scriptsProject, const std::filesystem::path& visualStudioInstall);
} // namespace aether::editor
