#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::app
{
	struct EditorProjectContext;
}

namespace aether::editor
{
	struct EditorProjectActionResult
	{
		bool succeeded = false;
		std::string message;
		std::string remediation; // what to do about a failure; empty on success
		std::filesystem::path outputPath;
	};

	struct VisualStudioInstallation
	{
		std::filesystem::path installPath;
		std::string displayName;
		int majorVersion = 0;
		bool supportsDotNet10 = false;
		bool hasDebuggerAutomation = false;
	};

	using EditorProjectPublishProgress = std::function<void(float completion, std::string_view stage)>;

	struct EditorProjectActions
	{
		std::function<void()> openLauncher;
		std::function<void()> reloadProject;
		std::function<EditorProjectActionResult(const app::EditorProjectContext&)> packProject;
		std::function<EditorProjectActionResult(const app::EditorProjectContext&, const EditorProjectPublishProgress&)> publishProject;
		std::vector<VisualStudioInstallation> visualStudioInstallations;
		std::function<EditorProjectActionResult(const std::filesystem::path& visualStudioInstall)> debugScripts;
		std::function<EditorProjectActionResult()> rebuildEnginePak;
		std::function<EditorProjectActionResult()> recompileShaders;
	};
} // namespace aether::editor
