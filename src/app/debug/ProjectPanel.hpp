#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "editor/EditorProjectContext.hpp"

namespace aether::app
{
	class ProjectPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Project";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return true;
		}

		void OnImGui(LayerContext& context) override;

	private:
		struct SceneEntry
		{
			std::string name;
			std::filesystem::path path;
		};

		void Refresh(const EditorProjectContext& project);
		void EnsureStandardFolders(const EditorProjectContext& project);
		void LoadProjectSettings(const EditorProjectContext& project);
		void SaveProjectSettings(const EditorProjectContext& project);
		void LoadPublishSettings(const EditorProjectContext& project);
		void SavePublishSettings(const EditorProjectContext& project);
		void ResetPublishSettings(const EditorProjectContext& project);
		void DrawFolderRow(const char* label, const std::filesystem::path& path);
		void DrawSceneTable();
		void DrawPublishDialog(LayerContext& context, const EditorProjectContext& project);

		std::filesystem::path m_lastRoot;
		std::vector<SceneEntry> m_scenes;
		std::string m_startupScene;
		std::string m_status;
		std::filesystem::path m_lastPackPath;
		std::string m_packStatus;
		bool m_packSucceeded = false;
		std::string m_shaderStatus;
		bool m_shaderSucceeded = false;
		std::filesystem::path m_lastPublishPath;
		std::string m_publishStatus;
		bool m_publishSucceeded = false;
		std::array<char, 128> m_publishProductName{};
		std::array<char, 64> m_publishPlatformName{};
		std::array<char, 260> m_publishOutputRoot{};
		bool m_publishCleanOutput = true;
		bool m_publishBuildScripts = true;
		bool m_publishUsePackageTemplate = true;
		bool m_publishVerifyOutput = true;
		bool m_publishSyncEditorPak = true;
		bool m_publishOpenAfter = true;
		bool m_dirtySettings = false;
	};
} // namespace aether::app
