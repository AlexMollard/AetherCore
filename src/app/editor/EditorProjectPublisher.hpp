#pragma once

#include <filesystem>
#include <string>

#include "editor/EditorProjectActions.hpp"

namespace aether::app
{
	struct EditorProjectContext;

	struct EditorProjectPublishConfig
	{
		std::filesystem::path assetPackerExe;
		std::filesystem::path executableDir;
		std::filesystem::path packageTemplateDir;
		std::filesystem::path dotnetExe;
		std::string managedConfig;
		std::string managedConfigDir;
		std::filesystem::path managedSdkProject;
		std::string runtimeExecutableName;
	};

	[[nodiscard]] EditorProjectPublishConfig MakeDefaultEditorProjectPublishConfig();
	[[nodiscard]] EditorProjectPublishOptions MakeDefaultEditorProjectPublishOptions(const EditorProjectContext& project);
	[[nodiscard]] std::string MakeProjectScriptCsprojText(const std::filesystem::path& managedSdkProject);
	[[nodiscard]] EditorProjectActionResult PackProject(const EditorProjectContext& project, const EditorProjectPublishConfig& config);
	[[nodiscard]] EditorProjectActionResult PublishProject(const EditorProjectContext& project, const EditorProjectPublishConfig& config, const EditorProjectPublishOptions& options);
} // namespace aether::app
