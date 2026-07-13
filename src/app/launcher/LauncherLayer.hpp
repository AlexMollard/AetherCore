#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "layers/AppLayer.hpp"
#include "debug/ProjectLauncherWindow.hpp"
#include "editor/EditorProjectContext.hpp"
#include "material/Texture.hpp"
#include "utils/TomlConfig.hpp"

namespace aether
{
	class ServiceContainer;
} // namespace aether

namespace aether::app
{
	// The project Launcher's sole layer: a Unity-Hub-style front end that lists
	// recent projects and, on open/create, spawns a SEPARATE full-engine Editor
	// process for the chosen project (launcher::SpawnEditor). It runs on a UiShell
	// engine - window + Vulkan + Dear ImGui only - and depends on nothing from the
	// editor: all project reading/scaffolding/persistence goes through the shared
	// ProjectCommon, so the slim Launcher target never links the editor.
	class LauncherLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;

	private:
		// Both spawn a separate Editor for the chosen project (CreateProject scaffolds
		// it first). Errors surface in the hub via m_windowState.error.
		void OpenProject(const std::filesystem::path& root);
		void CreateProject(const std::filesystem::path& root, std::string_view name);
		// Spawns the Editor for `root`, assigning it the next MCP control port (see
		// m_controlBasePort) so the AetherCore MCP can drive it.
		void SpawnEditorFor(const std::filesystem::path& root);
		void RememberRecent(const std::filesystem::path& root);
		void PersistSettings();

		ProjectLauncherWindow m_window;
		ProjectLauncherWindowState m_windowState;
		std::vector<EditorProjectContext> m_recentProjects;
		EditorProjectContext m_currentProject; // always empty: the launcher opens nothing in-process
		TomlConfig m_config;

		Texture m_logoTexture;
		std::uint64_t m_logoTextureId = 0;
		ServiceContainer* m_services = nullptr;

		// MCP integration: each spawned Editor gets AETHER_CONTROL_PORT so its
		// ControlServer auto-starts and the AetherCore MCP / aether-ctl can drive it.
		// m_controlBasePort is resolved once at attach from the AETHER_CONTROL_PORT env
		// (unset -> 8787 zero-setup default; a valid port -> that; "0"/"off"/"none" ->
		// disabled). Editor N gets base + N so multiple open editors never collide.
		int m_controlBasePort = 0;
		int m_spawnCount = 0;
	};
} // namespace aether::app
