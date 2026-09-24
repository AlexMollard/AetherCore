#pragma once

#include <filesystem>
#include <vector>

#include "editor/EditorProjectActions.hpp"

namespace aether::editor
{
	[[nodiscard]] std::vector<VisualStudioInstallation> FindVisualStudioInstallations();

	[[nodiscard]] EditorProjectActionResult OpenVisualStudioAndAttachScriptDebugger(const std::filesystem::path& scriptsProject, const std::filesystem::path& visualStudioInstall);
} // namespace aether::editor
