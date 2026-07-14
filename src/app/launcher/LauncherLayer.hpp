#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "layers/AppLayer.hpp"
#include "editor/ControlServer.hpp"
#include "launcher/LauncherProcess.hpp"
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
		~LauncherLayer() override;

		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;

		[[nodiscard]] std::span<const EditorProjectContext> RecentProjects() const;
		[[nodiscard]] bool IsLaunchingEditor() const noexcept;

	private:
		// Both spawn a separate Editor for the chosen project (CreateProject scaffolds
		// it first). Errors surface in the hub via m_windowState.error.
		void OpenProject(const std::filesystem::path& root);
		void CreateProject(const std::filesystem::path& root, std::string_view name);
		// Spawns the Editor for `root`, assigning it the next MCP control port (see
		// m_controlBasePort) so the AetherCore MCP can drive it.
		void SpawnEditorFor(const std::filesystem::path& root);
		void StartControlServer();
		void StopControlServer();
		void RememberRecent(const std::filesystem::path& root);
		void PersistSettings();

		// Per-project preview thumbnail (<root>/.aether/preview.png, written by the
		// editor on save). Loaded once per project and cached; the card grid shows it
		// or a placeholder. LoadPreview is a no-op when the file is absent or fails.
		void LoadPreview(const EditorProjectContext& project);
		[[nodiscard]] std::uint64_t PreviewTextureFor(const EditorProjectContext& project) const;
		void ReleasePreviews();

		ProjectLauncherWindow m_window;
		ProjectLauncherWindowState m_windowState;
		std::vector<EditorProjectContext> m_recentProjects;
		EditorProjectContext m_currentProject; // always empty: the launcher opens nothing in-process
		TomlConfig m_config;

		Texture m_logoTexture;
		std::uint64_t m_logoTextureId = 0;
		ServiceContainer* m_services = nullptr;
		std::unique_ptr<editor::ControlServer> m_controlServer;

		// Loaded preview thumbnails, keyed by normalized project root. A cached entry
		// with textureId == 0 records "no preview" so it is not retried every frame.
		struct PreviewEntry
		{
			Texture texture;
			std::uint64_t textureId = 0;
		};

		std::unordered_map<std::string, PreviewEntry> m_previews;

		// MCP integration: the Launcher owns the configured port while its hub is
		// visible. It stops its endpoint immediately before spawning an Editor, which
		// then inherits the same port. This keeps aether-ctl/MCP attached to 8787 (or
		// the configured override) throughout the handoff without a bind collision.
		int m_controlBasePort = 0;
		std::optional<launcher::EditorLaunch> m_pendingEditor;
		double m_editorStartupSeconds = 0.0;
	};
} // namespace aether::app
