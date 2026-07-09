#include "debug/ProjectPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <imgui.h>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#endif

#include "debug/Icons.hpp"
#include "editor/EditorProjectActions.hpp"
#include "editor/EditorProjectContext.hpp"
#include "layers/AppLayer.hpp"
#include "utils/Profiler.hpp"
#include "utils/TextIni.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::app
{
	namespace
	{
		std::string DisplayPath(const std::filesystem::path& path)
		{
			return path.empty() ? std::string{} : path.lexically_normal().string();
		}

		std::filesystem::path SettingsPath(const EditorProjectContext& project)
		{
			return project.settingsDir / "engine.toml";
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

		std::string FileSummary(const std::filesystem::path& path)
		{
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec))
			{
				return "Not built yet";
			}
			const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
			if (ec)
			{
				return "Built";
			}
			if (bytes >= 1024ull * 1024ull)
			{
				const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
				char buffer[64] = {};
				std::snprintf(buffer, sizeof(buffer), "%.1f MB", mb);
				return buffer;
			}
			return std::to_string(bytes / 1024ull) + " KB";
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
	} // namespace

	void ProjectPanel::Refresh(const EditorProjectContext& project)
	{
		m_lastRoot = project.root;
		m_scenes.clear();
		m_lastPackPath.clear();
		m_packStatus.clear();
		m_packSucceeded = false;
		LoadProjectSettings(project);

		std::error_code ec;
		if (std::filesystem::is_directory(project.scenesDir, ec))
		{
			for (const auto& entry: std::filesystem::directory_iterator(project.scenesDir, ec))
			{
				if (ec)
				{
					break;
				}
				if (!entry.is_regular_file(ec))
				{
					continue;
				}
				if (!entry.path().filename().generic_string().ends_with(".scene.toml"))
				{
					continue;
				}
				m_scenes.push_back(SceneEntry{
				        .name = SceneNameFromPath(entry.path()),
				        .path = entry.path(),
				});
			}
		}

		std::ranges::sort(m_scenes, {}, &SceneEntry::name);
	}

	void ProjectPanel::EnsureStandardFolders(const EditorProjectContext& project)
	{
		std::error_code ec;
		for (const std::filesystem::path& path: {project.assetsDir,
		             project.assetsDir / "models",
		             project.assetsDir / "materials",
		             project.assetsDir / "textures",
		             project.assetsDir / "animations",
		             project.prefabsDir,
		             project.root / "data",
		             project.scenesDir,
		             project.scriptsDir,
		             project.settingsDir})
		{
			std::filesystem::create_directories(path, ec);
			if (ec)
			{
				m_status = "Could not create " + DisplayPath(path) + ": " + ec.message();
				return;
			}
		}
		m_status = "Project folders are ready.";
		Refresh(project);
	}

	void ProjectPanel::LoadProjectSettings(const EditorProjectContext& project)
	{
		m_startupScene.clear();
		m_dirtySettings = false;

		std::ifstream in(SettingsPath(project), std::ios::binary);
		if (!in.is_open())
		{
			return;
		}

		std::stringstream buffer;
		buffer << in.rdbuf();
		try
		{
			text::ParseToml(buffer.str(),
			        [this](const text::IniEntry& entry)
			        {
				        if (entry.fullKey == "app.startupscene")
				        {
					        m_startupScene = text::StripQuotes(entry.value);
				        }
			        });
		}
		catch (...)
		{
			m_status = "Could not parse project settings.";
		}
	}

	void ProjectPanel::SaveProjectSettings(const EditorProjectContext& project)
	{
		TomlConfig config;
		{
			std::ifstream in(SettingsPath(project), std::ios::binary);
			if (in.is_open())
			{
				std::stringstream buffer;
				buffer << in.rdbuf();
				try
				{
					config.Load(buffer.str());
				}
				catch (...)
				{
					m_status = "Could not parse project settings.";
					return;
				}
			}
		}

		config.Set("app.startupScene", m_startupScene);

		std::error_code ec;
		std::filesystem::create_directories(project.settingsDir, ec);
		if (ec)
		{
			m_status = "Could not create settings folder: " + ec.message();
			return;
		}

		std::ofstream out(SettingsPath(project), std::ios::binary | std::ios::trunc);
		if (!out.is_open())
		{
			m_status = "Could not write project settings.";
			return;
		}

		config.Save(out, "AetherCore project settings");
		m_dirtySettings = false;
		m_status = "Project settings saved.";
	}

	void ProjectPanel::DrawFolderRow(const char* label, const std::filesystem::path& path)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		const bool exists = FolderExists(path);
		ImGui::TextColored(exists ? ImVec4(0.55f, 0.85f, 0.62f, 1.0f) : ImVec4(0.95f, 0.68f, 0.32f, 1.0f), "%s", exists ? "Ready" : "Missing");
		ImGui::TableSetColumnIndex(2);
		ImGui::TextDisabled("%s", DisplayPath(path).c_str());
		ImGui::TableSetColumnIndex(3);
		ImGui::BeginDisabled(!exists);
		ImGui::PushID(label);
		if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN))
		{
			OpenFolderInShell(path);
		}
		ImGui::PopID();
		ImGui::EndDisabled();
	}

	void ProjectPanel::DrawSceneTable()
	{
		if (!ImGui::BeginTable("##projectScenes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
		{
			return;
		}

		ImGui::TableSetupColumn("Scene");
		ImGui::TableSetupColumn("Startup", ImGuiTableColumnFlags_WidthFixed, 74.0f);
		ImGui::TableSetupColumn("Path");
		ImGui::TableHeadersRow();

		for (const SceneEntry& scene: m_scenes)
		{
			ImGui::PushID(scene.path.generic_string().c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(scene.name.c_str());
			ImGui::TableSetColumnIndex(1);
			const bool selected = m_startupScene == scene.name;
			if (ImGui::RadioButton("##startup", selected))
			{
				m_startupScene = scene.name;
				m_dirtySettings = true;
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::TextDisabled("%s", DisplayPath(scene.path).c_str());
			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	void ProjectPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Project", VisiblePtr());

		const auto* project = context.TryGet<EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			ImGui::TextDisabled("No project is open.");
			if (auto* actions = context.TryGet<EditorProjectActions>())
			{
				if (actions->openLauncher && ImGui::Button(ICON_FA_CUBE "  Open Launcher"))
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

		ImGui::TextUnformatted(project->name.c_str());
		ImGui::TextDisabled("%s", DisplayPath(project->root).c_str());

		if (auto* actions = context.TryGet<EditorProjectActions>())
		{
			if (actions->openLauncher && ImGui::Button(ICON_FA_CUBE "  Launcher"))
			{
				actions->openLauncher();
			}
			ImGui::SameLine();
			if (actions->reloadProject && ImGui::Button(ICON_FA_ROTATE "  Reload"))
			{
				actions->reloadProject();
				Refresh(*project);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Root"))
		{
			OpenFolderInShell(project->root);
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Repair Folders"))
		{
			EnsureStandardFolders(*project);
		}

		if (!m_status.empty())
		{
			ImGui::TextDisabled("%s", m_status.c_str());
		}

		ImGui::SeparatorText("Folders");
		if (ImGui::BeginTable("##projectFolders", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 82.0f);
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 64.0f);
			ImGui::TableSetupColumn("Path");
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 32.0f);
			DrawFolderRow("Assets", project->assetsDir);
			DrawFolderRow("Models", project->assetsDir / "models");
			DrawFolderRow("Materials", project->assetsDir / "materials");
			DrawFolderRow("Textures", project->assetsDir / "textures");
			DrawFolderRow("Animations", project->assetsDir / "animations");
			DrawFolderRow("Scenes", project->scenesDir);
			DrawFolderRow("Prefabs", project->prefabsDir);
			DrawFolderRow("Data", project->root / "data");
			DrawFolderRow("Scripts", project->scriptsDir);
			DrawFolderRow("Settings", project->settingsDir);
			ImGui::EndTable();
		}

		ImGui::SeparatorText("Startup Scene");
		if (m_scenes.empty())
		{
			ImGui::TextDisabled("No scenes found in the project scenes folder.");
		}
		else
		{
			DrawSceneTable();
		}

		const bool hasStartup = !m_startupScene.empty();
		ImGui::BeginDisabled(!hasStartup);
		if (ImGui::Button(ICON_FA_XMARK "  Clear Startup"))
		{
			m_startupScene.clear();
			m_dirtySettings = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!m_dirtySettings);
		if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save Project Settings"))
		{
			SaveProjectSettings(*project);
		}
		ImGui::EndDisabled();

		ImGui::SeparatorText("Packaging");
		if (auto* actions = context.TryGet<EditorProjectActions>())
		{
			ImGui::BeginDisabled(!actions->packProject);
			if (ImGui::Button(ICON_FA_BOX_OPEN "  Pack Project"))
			{
				if (m_dirtySettings)
				{
					SaveProjectSettings(*project);
				}
				const EditorProjectActionResult result = actions->packProject(*project);
				m_packSucceeded = result.succeeded;
				m_packStatus = result.message;
				m_lastPackPath = result.outputPath;
			}
			ImGui::EndDisabled();
		}
		if (!m_lastPackPath.empty())
		{
			ImGui::SameLine();
			ImGui::BeginDisabled(!FolderExists(m_lastPackPath.parent_path()));
			if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Output"))
			{
				OpenFolderInShell(m_lastPackPath.parent_path());
			}
			ImGui::EndDisabled();
			ImGui::TextDisabled("%s", DisplayPath(m_lastPackPath).c_str());
			ImGui::TextDisabled("%s", FileSummary(m_lastPackPath).c_str());
		}
		if (!m_packStatus.empty())
		{
			const ImVec4 color = m_packSucceeded ? ImVec4(0.55f, 0.85f, 0.62f, 1.0f) : ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
			ImGui::TextColored(color, "%s", m_packStatus.c_str());
		}

		ImGui::End();
	}
} // namespace aether::app
