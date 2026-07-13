#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "debug/ProjectLauncherWindow.hpp"
#include "editor/EditorProjectActions.hpp"
#include "editor/EditorProjectContext.hpp"
#include "material/Texture.hpp"
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
		// Opens the project at `root`. `reloadScene` drives the newly opened
		// project's startup scene into the live world (tearing down any scene from
		// the previously open project); it is left false only for the boot-time
		// "reopen last project" path, where ScriptedSceneLayer performs the initial
		// startup-scene load once it attaches.
		void OpenProject(std::filesystem::path root, bool reloadScene = true);
		void CreateProject(std::filesystem::path root, std::string_view name);
		void RefreshServices();
		// Ticked every frame by DebugLayer: polls the async project-script build
		// started on project open and reloads the assembly on success (or surfaces
		// the build error). Cheap no-op when no build is pending.
		void UpdateScriptBuild();

		[[nodiscard]] bool IsProjectLoaded() const noexcept;
		[[nodiscard]] bool IsLauncherOpen() const noexcept;
		[[nodiscard]] bool HasCurrentProject() const;
		[[nodiscard]] std::uint64_t LogoTextureId() const noexcept;
		[[nodiscard]] const EditorProjectContext& CurrentProject() const noexcept;
		[[nodiscard]] EditorProjectContext& CurrentProject() noexcept;

	private:
		void ConfigureActions();
		void AddRecentProject(std::filesystem::path root, std::string name);
		// Drives the current project's startup scene (from the just-reloaded
		// settings) into the live world through the shared ReplaceScene path,
		// clearing any previously loaded scene.
		void LoadProjectStartupScene();
		// Kicks off an async build of the open project's C# game scripts
		// (scripts/AetherGame.csproj) - the runtime replacement for CMake's old
		// build-time game-scripts staging. UpdateScriptBuild() polls it to completion
		// and reloads the assembly. No-op if the project has no scripts or the build
		// lacks dotnet support.
		void BuildAndReloadProjectScripts();

		EditorProjectContext m_currentProject;
		// True while an async project-script build kicked off by OpenProject is still
		// being polled to completion by UpdateScriptBuild.
		bool m_scriptBuildPending = false;
		EditorProjectActions m_actions;
		ServiceContainer* m_services = nullptr;
		std::vector<EditorProjectContext> m_recentProjects;
		ProjectLauncherWindow m_launcher;
		ProjectLauncherWindowState m_launcherState;
		Texture m_logoTexture;
		std::uint64_t m_logoTextureId = 0;
		bool m_projectLoaded = false;
		bool m_launcherOpen = true;
	};
} // namespace aether::app
