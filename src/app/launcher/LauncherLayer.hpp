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
}

namespace aether::app
{
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
		void OpenProject(const std::filesystem::path& root);
		void CreateProject(const std::filesystem::path& root, std::string_view name);
		void SpawnEditorFor(const std::filesystem::path& root);
		void StartControlServer();
		void StopControlServer();
		void RememberRecent(const std::filesystem::path& root);
		void PersistSettings();

		void LoadPreview(const EditorProjectContext& project);
		[[nodiscard]] std::uint64_t PreviewTextureFor(const EditorProjectContext& project) const;
		void ReleasePreviews();

		ProjectLauncherWindow m_window;
		ProjectLauncherWindowState m_windowState;
		std::vector<EditorProjectContext> m_recentProjects;
		EditorProjectContext m_currentProject;
		TomlConfig m_config;

		Texture m_logoTexture;
		std::uint64_t m_logoTextureId = 0;
		ServiceContainer* m_services = nullptr;
		std::unique_ptr<editor::ControlServer> m_controlServer;

		struct PreviewEntry
		{
			Texture texture;
			std::uint64_t textureId = 0;
		};

		std::unordered_map<std::string, PreviewEntry> m_previews;

		int m_controlBasePort = 0;
		std::optional<launcher::EditorLaunch> m_pendingEditor;
		double m_editorStartupSeconds = 0.0;
	};
} // namespace aether::app
