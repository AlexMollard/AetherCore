#pragma once

#include <filesystem>
#include <functional>
#include <string>
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

	struct EditorProjectPublishOptions
	{
		std::filesystem::path outputRoot;
		std::string productName;
		std::string platformName;
		bool cleanOutput = true;
		bool buildProjectScripts = true;
		bool usePackageTemplate = true;
		bool verifyOutput = true;
		bool syncEditorRuntimeProjectPak = true;
	};

	struct EditorProjectActions
	{
		std::function<void()> openLauncher;
		std::function<void()> reloadProject;
		std::function<EditorProjectActionResult(const app::EditorProjectContext&)> packProject;
		std::function<EditorProjectActionResult(const app::EditorProjectContext&, const EditorProjectPublishOptions&)> publishProject;
		std::vector<VisualStudioInstallation> visualStudioInstallations;
		std::function<EditorProjectActionResult(const std::filesystem::path& visualStudioInstall)> debugScripts;
		std::function<EditorProjectActionResult()> rebuildEnginePak;
		std::function<EditorProjectActionResult()> recompileShaders;
	};
} // namespace aether::editor
