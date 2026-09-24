#pragma once

#include <cstdint>
#include <functional>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "PlayState.hpp"
#include "debug/ProjectLauncherWindow.hpp"
#include "editor/EditorProjectActions.hpp"
#include "editor/EditorProjectContext.hpp"
#include "material/Texture.hpp"
#include "scene/ModelBakeHook.hpp"
#include "utils/TomlConfig.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether::editor
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
		void OpenProject(std::filesystem::path root, bool reloadScene = true);
		void CreateProject(std::filesystem::path root, std::string_view name, app::project::ProjectTemplate projectTemplate);

		// Invoked at the end of every successful OpenProject. The editor layer uses it to
		// apply the project's flavor (flavor panels + theme) once the context is loaded.
		void SetProjectOpenedHandler(std::function<void(const app::EditorProjectContext&)> handler)
		{
			m_projectOpened = std::move(handler);
		}

		void RefreshServices();
		void UpdateScriptBuild();

		// screenshot service. The capture itself is fulfilled by the render thread.
		void CaptureProjectPreview();

		[[nodiscard]] bool IsProjectLoaded() const noexcept;
		[[nodiscard]] bool IsLauncherOpen() const noexcept;
		[[nodiscard]] bool HasCurrentProject() const;
		[[nodiscard]] std::uint64_t LogoTextureId() const noexcept;
		[[nodiscard]] const app::EditorProjectContext& CurrentProject() const noexcept;
		[[nodiscard]] app::EditorProjectContext& CurrentProject() noexcept;

	private:
		void ConfigureActions();
		void AddRecentProject(std::filesystem::path root, std::string name);
		void LoadProjectStartupScene();
		void BuildAndReloadProjectScripts();

		// Bakes every TTF/OTF under assets/fonts to .fontcurves when
		// missing or older than the source - drop a font in, it just works.
		void BakeProjectFonts() const;

		app::EditorProjectContext m_currentProject;
		bool m_scriptBuildPending = false;
		int m_previewCaptureCountdown = 0;
		app::ProjectShaderRecompileHook m_shaderRecompileHook; // registered so Play recompiles shaders
		EditorProjectActions m_actions;
		std::function<void(const app::EditorProjectContext&)> m_projectOpened;
		app::scene::ModelBakeHook m_bakeHook;
		ServiceContainer* m_services = nullptr;
		std::vector<app::EditorProjectContext> m_recentProjects;
		app::ProjectLauncherWindow m_launcher;
		app::ProjectLauncherWindowState m_launcherState;
		Texture m_logoTexture;
		std::uint64_t m_logoTextureId = 0;
		bool m_projectLoaded = false;
		bool m_launcherOpen = true;
	};
} // namespace aether::editor
