#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditorProjectContext.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	class TomlConfig;
}

namespace aether::app::project
{
	// read, and scaffold projects on disk.

	inline constexpr int kMaxRecentProjects = 8;

	[[nodiscard]] std::filesystem::path NormalizePath(std::filesystem::path path);
	[[nodiscard]] std::string DisplayPath(const std::filesystem::path& path);
	[[nodiscard]] std::filesystem::path ProjectFilePath(const std::filesystem::path& root);
	[[nodiscard]] std::filesystem::path ResolveProjectRoot(std::filesystem::path path);
	[[nodiscard]] bool HasProjectDescriptor(const std::filesystem::path& root);
	[[nodiscard]] std::string FallbackProjectName(const std::filesystem::path& root);
	// per-project metadata dir so it never pollutes the asset tree.
	[[nodiscard]] std::filesystem::path PreviewImagePath(const std::filesystem::path& root);
	[[nodiscard]] std::string LastModifiedLabel(const std::filesystem::path& root);
	[[nodiscard]] Expected<EditorProjectContext> ReadProjectDescriptor(const std::filesystem::path& root);
	[[nodiscard]] std::string ReadProjectName(const std::filesystem::path& root);

	[[nodiscard]] bool WriteProjectDescriptor(const std::filesystem::path& root, std::string_view name, std::string& error);
	[[nodiscard]] std::string MakeProjectScriptCsprojText(const std::filesystem::path& managedSdkProject);

	[[nodiscard]] std::optional<std::filesystem::path> PickProjectFolder();
	[[nodiscard]] std::optional<std::filesystem::path> PickProjectFile();

	[[nodiscard]] std::vector<EditorProjectContext> LoadRecentProjects(TomlConfig& config);
	void SaveRecentProjects(TomlConfig& config, std::span<const EditorProjectContext> recents);
} // namespace aether::app::project
