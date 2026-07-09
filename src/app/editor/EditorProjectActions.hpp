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

	struct EditorProjectActions
	{
		std::function<void()> openLauncher;
		std::function<void()> reloadProject;
		std::function<EditorProjectActionResult(const EditorProjectContext&)> packProject;
	};
} // namespace aether::app
