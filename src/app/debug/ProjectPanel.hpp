#pragma once

#include <filesystem>
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
		std::string_view GetName() const override
		{
			return "Project";
		}

		// Part of the default layout: the project and its folders.
		[[nodiscard]] bool DefaultVisible() const override
		{
			return true;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		void Refresh(const app::EditorProjectContext& project);
		void EnsureStandardFolders(const app::EditorProjectContext& project);
		void LoadProjectSettings(const app::EditorProjectContext& project);
		void DrawFolderRow(const char* label, const std::filesystem::path& path, bool required);

		std::filesystem::path m_lastRoot;
		std::vector<std::string> m_sceneNames;
		std::string m_startupScene;
		std::string m_status;
	};
} // namespace aether::editor
