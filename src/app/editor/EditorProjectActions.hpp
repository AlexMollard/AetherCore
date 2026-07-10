#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace aether::app
{
	struct EditorProjectContext;

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
		std::function<EditorProjectActionResult(const EditorProjectContext&)> packProject;
		std::function<EditorProjectActionResult(const EditorProjectContext&, const EditorProjectPublishOptions&)> publishProject;
		std::function<EditorProjectActionResult()> rebuildEnginePak;
	};
} // namespace aether::app
