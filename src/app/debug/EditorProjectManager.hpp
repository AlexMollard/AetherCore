#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "debug/ProjectLauncherWindow.hpp"
#include "editor/EditorProjectActions.hpp"
#include "editor/EditorProjectContext.hpp"
#include "utils/TomlConfig.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether::app
{
	class EditorProjectManager final
	{
	public:
		void Attach(ServiceContainer& services);
		void Detach();

		void LoadSettings(TomlConfig& config);
		void SaveSettings(TomlConfig& config);
		void DrawLauncher();

		void OpenLauncher();
		void CloseLauncher();
		void OpenProject(std::filesystem::path root);
		void CreateProject(std::filesystem::path root, std::string_view name);
		void RefreshServices();

		[[nodiscard]] bool IsProjectLoaded() const noexcept;
		[[nodiscard]] bool IsLauncherOpen() const noexcept;
		[[nodiscard]] bool HasCurrentProject() const;
		[[nodiscard]] const EditorProjectContext& CurrentProject() const noexcept;
		[[nodiscard]] EditorProjectContext& CurrentProject() noexcept;

	private:
		void ConfigureActions();
		void AddRecentProject(std::filesystem::path root, std::string name);

		EditorProjectContext m_currentProject;
		EditorProjectActions m_actions;
		ServiceContainer* m_services = nullptr;
		std::vector<EditorProjectContext> m_recentProjects;
		ProjectLauncherWindow m_launcher;
		ProjectLauncherWindowState m_launcherState;
		bool m_projectLoaded = false;
		bool m_launcherOpen = true;
	};
} // namespace aether::app
