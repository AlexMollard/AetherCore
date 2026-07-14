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
	// Default OS-window size for the standalone Launcher process (LauncherMain) and
	// the smallest window the responsive hub layout stays usable at (LauncherLayer
	// applies it as the OS window's minimum size). The hub itself is fluid - it
	// fills and re-flows to whatever window hosts it (see ProjectLauncherWindow::Draw).
	inline constexpr int kProjectLauncherDefaultWidth = 1920;
	inline constexpr int kProjectLauncherDefaultHeight = 1080;
	inline constexpr int kProjectLauncherMinWidth = 720;
	inline constexpr int kProjectLauncherMinHeight = 540;

	struct ProjectLauncherWindowState
	{
		bool openLastProject = false;
		std::array<char, 260> openPath{};
		std::array<char, 260> newPath{};
		std::array<char, 96> newName{};
		bool launching = false;
		std::string error;
	};

	struct ProjectLauncherWindowModel
	{
		bool projectLoaded = false;
		bool hasCurrentProject = false;
		std::uint64_t logoTextureId = 0;
		const EditorProjectContext* currentProject = nullptr;
		std::span<const EditorProjectContext> recentProjects;
		// ImGui texture id of a project's preview thumbnail, or 0 for none (the card
		// then draws a placeholder). Optional - unset means no previews at all.
		std::function<std::uint64_t(const EditorProjectContext&)> previewTextureId;
		// Short "last edited" label for a project (e.g. "3 days ago"), shown on its
		// card. Optional; return "" to omit the badge for a given project.
		std::function<std::string(const EditorProjectContext&)> modifiedLabel;
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
