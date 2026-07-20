#pragma once

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "editor/EditorProjectActions.hpp"
#include "editor/EditorProjectContext.hpp"

namespace aether::editor
{
	class ProjectPanel final : public DebugPanel
	{
	public:
		~ProjectPanel() override;

		std::string_view GetName() const override
		{
			return "Project";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return true;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		struct SceneEntry
		{
			std::string name;
			std::filesystem::path path;
		};

		struct PublishTask
		{
			std::atomic<float> completion = 0.0f;
			std::mutex mutex;
			std::string stage;
		};

		void Refresh(const app::EditorProjectContext& project);
		void EnsureStandardFolders(const app::EditorProjectContext& project);
		void LoadProjectSettings(const app::EditorProjectContext& project);
		void SaveProjectSettings(const app::EditorProjectContext& project);
		void LoadPublishSettings(const app::EditorProjectContext& project);
		void SavePublishSettings(const app::EditorProjectContext& project);
		void ResetPublishSettings(const app::EditorProjectContext& project);
		void DrawFolderRow(const char* label, const std::filesystem::path& path);
		void DrawSceneTable();
		void DrawPublishDialog(app::LayerContext& context, const app::EditorProjectContext& project);

		std::filesystem::path m_lastRoot;
		std::vector<SceneEntry> m_scenes;
		std::string m_startupScene;
		std::string m_status;
		std::filesystem::path m_visualStudioInstall;
		std::filesystem::path m_lastPackPath;
		std::string m_packStatus;
		bool m_packSucceeded = false;
		std::string m_shaderStatus;
		bool m_shaderSucceeded = false;
		std::filesystem::path m_lastPublishPath;
		std::string m_publishStatus;
		bool m_publishSucceeded = false;
		std::string m_publishProductName;
		std::string m_publishPlatformName;
		std::string m_publishOutputRoot;
		bool m_publishCleanOutput = true;
		bool m_publishBuildScripts = true;
		bool m_publishUsePackageTemplate = true;
		bool m_publishVerifyOutput = true;
		bool m_publishSyncEditorPak = true;
		bool m_publishOpenAfter = true;
		std::future<EditorProjectActionResult> m_publishFuture;
		std::shared_ptr<PublishTask> m_publishTask;
		bool m_dirtySettings = false;
	};
} // namespace aether::editor
