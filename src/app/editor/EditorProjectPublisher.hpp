#pragma once

#include <filesystem>
#include <string>

#include "editor/EditorProjectActions.hpp"

namespace aether::app
{
	struct EditorProjectContext;
}

namespace aether::editor
{
	struct EditorProjectPublishConfig
	{
		std::filesystem::path executableDir;
		std::filesystem::path packageTemplateDir;
		std::filesystem::path dotnetExe;
		std::string managedConfig;
		std::string managedConfigDir;
		std::filesystem::path managedSdkProject;
		std::string runtimeExecutableName;
	};

	[[nodiscard]] EditorProjectPublishConfig MakeDefaultEditorProjectPublishConfig();
	[[nodiscard]] EditorProjectPublishOptions MakeDefaultEditorProjectPublishOptions(const app::EditorProjectContext& project);
	[[nodiscard]] EditorProjectActionResult PackProject(const app::EditorProjectContext& project, const EditorProjectPublishConfig& config);
	[[nodiscard]] EditorProjectActionResult PublishProject(const app::EditorProjectContext& project, const EditorProjectPublishConfig& config, const EditorProjectPublishOptions& options);
} // namespace aether::editor
