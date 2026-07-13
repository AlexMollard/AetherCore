#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "editor/EditorProjectContext.hpp"

namespace aether::app
{
	struct ProjectLauncherWindowState
	{
		bool openLastProject = false;
		std::array<char, 260> openPath{};
		std::array<char, 260> newPath{};
		std::array<char, 96> newName{};
		std::string error;
	};

	struct ProjectLauncherWindowModel
	{
		bool projectLoaded = false;
		bool hasCurrentProject = false;
		std::uint64_t logoTextureId = 0;
		const EditorProjectContext* currentProject = nullptr;
		std::span<const EditorProjectContext> recentProjects;
	};

	struct ProjectLauncherWindowActions
	{
		std::function<void(std::filesystem::path)> openProject;
		std::function<void(std::filesystem::path, std::string_view)> createProject;
		std::function<std::optional<std::filesystem::path>()> browseFolder;
		std::function<std::optional<std::filesystem::path>()> browseProjectFile;
		std::function<void()> closeLauncher;
		std::function<void()> saveSettings;
	};

	class ProjectLauncherWindow final
	{
	public:
		void Draw(ProjectLauncherWindowState& state, const ProjectLauncherWindowModel& model, const ProjectLauncherWindowActions& actions);
	};
} // namespace aether::app
