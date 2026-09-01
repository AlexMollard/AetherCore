#include "debug/ProjectPanel.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/SceneSelection.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "debug/InspectorWidgets.hpp"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <string_view>
#include <system_error>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#endif

#include "debug/Icons.hpp"
#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "assets/TileAssetStore.hpp"
#include "editor/EditorProjectActions.hpp"
#include "io/FileUtil.hpp"
#include "editor/EditorProjectContext.hpp"
#include "project/ProjectStartupScene.hpp"
#include "editor/EditorProjectPublisher.hpp"
#include "layers/AppLayer.hpp"
#include "rendering/Renderer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"
#include "utils/TextIni.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::editor
{
	namespace
	{
		std::string DisplayPath(const std::filesystem::path& path)
		{
			return path.empty() ? std::string{} : path.lexically_normal().string();
		}

		// The part of a project folder's path that actually differs between rows.
		std::string ProjectRelative(const std::filesystem::path& path, const std::filesystem::path& root)
		{
			if (path.empty() || root.empty())
			{
				return DisplayPath(path);
			}
			std::error_code ec;
			const std::filesystem::path rel = std::filesystem::relative(path, root, ec);
			if (ec || rel.empty() || rel.generic_string().starts_with(".."))
			{
				return DisplayPath(path);
			}
			return rel.generic_string();
		}

		std::filesystem::path SettingsPath(const app::EditorProjectContext& project)
		{
			return project.projectFile;
		}

		std::string SceneNameFromPath(const std::filesystem::path& path)
		{
			std::string filename = path.filename().generic_string();
			constexpr std::string_view suffix = ".scene.toml";
			if (filename.ends_with(suffix))
			{
				filename.resize(filename.size() - suffix.size());
			}
			return filename;
		}

		bool FolderExists(const std::filesystem::path& path)
		{
			std::error_code ec;
			return std::filesystem::is_directory(path, ec);
		}

		void OpenFolderInShell(const std::filesystem::path& path)
		{
#ifdef _WIN32
			if (path.empty())
			{
				return;
			}
			ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
			static_cast<void>(path);
#endif
		}

		void MutedWrapped(const std::string& text)
		{
			if (text.empty())
			{
				return;
			}
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("%s", text.c_str());
			ImGui::PopTextWrapPos();
		}

		struct ButtonRow
		{
			float rightEdge = 0.0f;
			bool started = false;

			ButtonRow()
			{
				rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
			}

			void Item(const char* label)
			{
				const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
				if (started)
				{
					const float nextX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
					if (nextX + width <= rightEdge)
					{
						ImGui::SameLine();
					}
				}
				started = true;
			}
		};
	} // namespace

	void ProjectPanel::Refresh(const app::EditorProjectContext& project)
	{
		m_lastRoot = project.root;
		m_sceneNames.clear();
		m_status.clear();
		LoadProjectSettings(project);

		std::error_code ec;
		if (std::filesystem::is_directory(project.scenesDir, ec))
		{
			for (const auto& entry: std::filesystem::directory_iterator(project.scenesDir, ec))
			{
				if (ec || !entry.is_regular_file(ec))
				{
					continue;
				}
				// SceneNameFromPath returns the filename unchanged when it is not a scene, so
				// filter on the suffix rather than on an empty result.
				if (!entry.path().filename().generic_string().ends_with(".scene.toml"))
				{
					continue;
				}
				m_sceneNames.push_back(SceneNameFromPath(entry.path()));
			}
		}
		std::ranges::sort(m_sceneNames);
	}

	void ProjectPanel::EnsureStandardFolders(const app::EditorProjectContext& project)
	{
		for (const std::filesystem::path& path:
		        {project.assetsDir, project.assetsDir / "models", project.assetsDir / "materials", project.assetsDir / "textures", project.assetsDir / "animations", project.prefabsDir, project.root / "data", project.scenesDir, project.scriptsDir})
		{
			if (auto result = io::file_util::CreateDirectories(path); !result)
			{
				m_status = "Could not create " + DisplayPath(path) + ": " + result.error().message;
				return;
			}
		}
		m_status = "Project folders are ready.";
		Refresh(project);
	}

	void ProjectPanel::LoadProjectSettings(const app::EditorProjectContext& project)
	{
		m_startupScene = app::ReadProjectStartupScene(SettingsPath(project));
	}

	void ProjectPanel::DrawFolderRow(app::LayerContext& context, const std::filesystem::path& root, const char* label, const std::filesystem::path& path, const bool required)
	{
		const bool exists = FolderExists(path);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		// Only the folders a project cannot work without are a problem when absent. The
		// content folders are absent because that content has not been made yet - a 2D
		// game has no models and never will, and flagging three permanent amber warnings
		// at it just teaches you to stop reading the column.
		if (exists)
		{
			// Healthy folders say nothing. Nine rows all reading "Ready" is nine rows of
			// noise that bury the one row that is not.
			ImGui::TextDisabled("-");
		}
		else if (required)
		{
			ImGui::TextColored(chrome::kWarning, "Missing");
		}
		else
		{
			ImGui::TextDisabled("Unused");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("No %s in this project yet. Created when you add some.", label);
			}
		}
		ImGui::TableSetColumnIndex(2);
		// Relative to the project. Every one of these lives under the project root, which is
		// already printed above, so the absolute path spent the column repeating the same
		// prefix nine times and clipping the part that differs.
		ImGui::TextDisabled("%s", ProjectRelative(path, root).c_str());
		ImGui::SetItemTooltip("%s", DisplayPath(path).c_str());
		ImGui::TableSetColumnIndex(3);
		ImGui::PushID(label);
		ImGui::BeginDisabled(!exists);
		// Distinct per row: nine buttons sharing one id string are indistinguishable to
		// anything addressing the UI by name, even though PushID keeps ImGui itself happy.
		char openId[48];
		std::snprintf(openId, sizeof(openId), "##open_%s", label);
		if (chrome::GhostIconButton(ICON_FA_FOLDER_OPEN, openId, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
		{
			// Shows the folder in the editor's own browser rather than launching the OS file
			// manager. Two browsers side by side was the duplication; this makes the folder
			// list a way INTO the one that can actually do something with an asset.
			if (auto* selection = context.TryGet<SceneSelection>())
			{
				selection->RequestReveal(DisplayPath(path));
			}
		}
		ImGui::SetItemTooltip("Show in the File Explorer");
		ImGui::EndDisabled();
		ImGui::PopID();
	}

	void ProjectPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Project", VisiblePtr());

		const auto* project = context.TryGet<app::EditorProjectContext>();
		auto* actions = context.TryGet<EditorProjectActions>();

		if (project == nullptr || !project->IsLoaded())
		{
			chrome::PanelHeader("PROJECT");

			const char* icon = ICON_FA_CUBE;
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.32f));
			ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(icon).x) * 0.5f);
			ImGui::TextDisabled("%s", icon);
			const char* message = "No project is open.";
			ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(message).x) * 0.5f);
			ImGui::TextDisabled("%s", message);

			if (actions != nullptr && actions->openLauncher)
			{
				const char* button = ICON_FA_CUBE " Open Launcher";
				const float width = ImGui::CalcTextSize(button).x + ImGui::GetStyle().FramePadding.x * 2.0f + 16.0f;
				ImGui::Dummy(ImVec2(0.0f, 4.0f));
				ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - width) * 0.5f);
				if (chrome::OutlineButton(button, ImVec2(width, 0.0f)))
				{
					actions->openLauncher();
				}
			}

			ImGui::End();
			return;
		}

		if (project->root != m_lastRoot)
		{
			Refresh(*project);
		}

		const std::string sceneStat = std::to_string(m_sceneNames.size()) + (m_sceneNames.size() == 1 ? " scene" : " scenes");
		chrome::PanelHeader("PROJECT", sceneStat.c_str());
		ImGui::TextUnformatted(project->name.c_str());
		MutedWrapped(DisplayPath(project->root));
		if (!m_status.empty())
		{
			MutedWrapped(m_status);
		}
		ImGui::Dummy(ImVec2(0.0f, 4.0f));

		{
			ButtonRow toolbar;
			if (actions != nullptr && actions->openLauncher)
			{
				const char* label = ICON_FA_CUBE " Launcher";
				toolbar.Item(label);
				if (chrome::GhostButton(label))
				{
					actions->openLauncher();
				}
			}
			if (actions != nullptr && actions->reloadProject)
			{
				const char* label = ICON_FA_ROTATE " Reload";
				toolbar.Item(label);
				if (chrome::GhostButton(label))
				{
					actions->reloadProject();
					Refresh(*project);
				}
			}
			{
				const char* label = ICON_FA_FOLDER_OPEN " Open Root";
				toolbar.Item(label);
				if (chrome::GhostButton(label))
				{
					OpenFolderInShell(project->root);
				}
			}
		}

		// The scene a published game boots. Browsing scenes is the Hierarchy panel's job;
		// this is the one setting, so it is a combo rather than a second scene list.
		ImGui::SeparatorText("Startup Scene");
		if (m_sceneNames.empty())
		{
			ImGui::TextDisabled("No scenes found in %s", DisplayPath(project->scenesDir).c_str());
		}
		else
		{
			iw::LabelColumn("Boots");
			const std::string current = m_startupScene.empty() ? std::string("(none)") : m_startupScene;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::BeginCombo("##startupScene", current.c_str()))
			{
				for (const std::string& name: m_sceneNames)
				{
					const bool selected = name == m_startupScene;
					if (ImGui::Selectable(name.c_str(), selected))
					{
						std::string error;
						if (app::WriteProjectStartupScene(project->projectFile, name, error))
						{
							m_startupScene = name;
							m_status = "Startup scene set to '" + name + "'.";
						}
						else
						{
							m_status = error;
						}
					}
					if (selected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			ImGui::SetItemTooltip("The scene a published game boots. Also settable with the star in the Scenes list.");
		}

		ImGui::SeparatorText("Folders");
		if (ImGui::BeginTable("##projectFolders", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 82.0f);
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 64.0f);
			ImGui::TableSetupColumn("Path");
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + 4.0f);
			DrawFolderRow(context, project->root, "Assets", project->assetsDir, true);
			DrawFolderRow(context, project->root, "Models", project->assetsDir / "models", false);
			DrawFolderRow(context, project->root, "Materials", project->assetsDir / "materials", false);
			DrawFolderRow(context, project->root, "Textures", project->assetsDir / "textures", false);
			DrawFolderRow(context, project->root, "Animations", project->assetsDir / "animations", false);
			DrawFolderRow(context, project->root, "Scenes", project->scenesDir, true);
			DrawFolderRow(context, project->root, "Prefabs", project->prefabsDir, false);
			DrawFolderRow(context, project->root, "Data", project->root / "data", false);
			DrawFolderRow(context, project->root, "Scripts", project->scriptsDir, true);
			ImGui::EndTable();
		}
		ImGui::Dummy(ImVec2(0.0f, 4.0f));
		if (chrome::GhostButton(ICON_FA_FOLDER_OPEN " Repair Folders"))
		{
			EnsureStandardFolders(*project);
		}

		ImGui::End();
	}
} // namespace aether::editor
