#pragma once

#include <array>
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

	enum class ProjectTemplate
	{
		Blank3D,
		Blank2D,
		Multiplayer2D,
		Multiplayer3D,
	};

	struct ProjectTemplateInfo
	{
		ProjectTemplate value;
		ProjectKind kind;
		std::string_view name;
		std::string_view description;
	};

	inline constexpr std::array<ProjectTemplateInfo, 4> kProjectTemplates{{
	        {ProjectTemplate::Blank3D, ProjectKind::Scene3D, "Blank 3D", "Perspective camera and the standard 3D starter scene."},
	        {ProjectTemplate::Blank2D, ProjectKind::Scene2D, "Blank 2D", "Orthographic camera and an empty XY-plane scene."},
	        {ProjectTemplate::Multiplayer2D, ProjectKind::Scene2D, "Multiplayer 2D", "Orthographic camera with room-code hosting and joining - no port forwarding needed."},
	        {ProjectTemplate::Multiplayer3D, ProjectKind::Scene3D, "Multiplayer 3D", "Perspective camera with room-code hosting and joining - no port forwarding needed."},
	}};

	// The single source of truth for which kind each template seeds - looked up rather
	// than a two-way ternary, so a template can be 2D or 3D independently of which
	// number it is in the list (Multiplayer2D and Blank3D are not adjacent).
	[[nodiscard]] constexpr ProjectKind ProjectKindForTemplate(ProjectTemplate projectTemplate)
	{
		for (const ProjectTemplateInfo& info: kProjectTemplates)
		{
			if (info.value == projectTemplate)
			{
				return info.kind;
			}
		}
		return ProjectKind::Scene3D;
	}

	// The [project].template string a template is written/read under (e.g. "blank_3d",
	// "multiplayer_2d") - the single source both WriteProjectDescriptor and the Launcher's
	// create_project control method key off, so a new template needs this list updated in
	// exactly one place.
	[[nodiscard]] std::string_view TemplateName(ProjectTemplate projectTemplate);
	[[nodiscard]] std::optional<ProjectTemplate> ProjectTemplateFromName(std::string_view name);

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

	[[nodiscard]] bool WriteProjectDescriptor(const std::filesystem::path& root, std::string_view name, std::string& error, ProjectTemplate projectTemplate = ProjectTemplate::Blank3D);
	[[nodiscard]] std::string MakeProjectScriptCsprojText(const std::filesystem::path& managedSdkProject);

	// A per-project Visual Studio solution (.slnx) that pairs the game scripts
	// project with the engine SDK project, so opening it resolves engine types in
	// the IDE. Generated locally on project open (not committed).
	[[nodiscard]] std::string MakeGameSolutionText(const std::filesystem::path& root, const std::filesystem::path& managedSdkProject);
	bool EnsureGameSolution(const std::filesystem::path& root, std::string& error);

	[[nodiscard]] std::optional<std::filesystem::path> PickProjectFolder();
	[[nodiscard]] std::optional<std::filesystem::path> PickProjectFile();

	// Open a folder (or a file's parent folder) in the OS file manager. No-op if
	// the path is empty or does not exist.
	void OpenPathInFileManager(const std::filesystem::path& path);

	[[nodiscard]] std::vector<EditorProjectContext> LoadRecentProjects(TomlConfig& config);
	void SaveRecentProjects(TomlConfig& config, std::span<const EditorProjectContext> recents);
} // namespace aether::app::project
