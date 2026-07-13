#pragma once

#include <string>

namespace aether::editor
{
	// Open a source file (optionally at a line) in VS Code, falling back to the
	// OS default handler. Shared by the script-error toast and the log console.
	void OpenInEditor(const std::string& filePath, int line);
} // namespace aether::editor
