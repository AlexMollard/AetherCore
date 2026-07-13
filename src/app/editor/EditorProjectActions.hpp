#pragma once

#include <filesystem>
#include <functional>
#include <string>

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
		std::function<EditorProjectActionResult()> rebuildEnginePak;
		// Manual shader-recompile trigger (ShaderCompiler::CompileProject on the
		// current project, then refreshes the shaders:// overlay's project
		// layer). Compile-on-project-load is the other trigger; there is no
		// filesystem watch for .slang changes yet.
		std::function<EditorProjectActionResult()> recompileShaders;
	};
} // namespace aether::editor
