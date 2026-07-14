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

	// A Visual Studio IDE found on this machine. Build Tools instances are not
	// included because they cannot open a project or attach a debugger.
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
		// Manual shader-recompile trigger (ShaderCompiler::CompileProject on the
		// current project, then refreshes the shaders:// overlay's project
		// layer). Compile-on-project-load is the other trigger; there is no
		// filesystem watch for .slang changes yet.
		std::function<EditorProjectActionResult()> recompileShaders;
	};
} // namespace aether::editor
