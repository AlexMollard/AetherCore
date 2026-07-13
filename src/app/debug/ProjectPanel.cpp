#include "debug/ProjectPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

#include <imgui.h>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#endif

#include "debug/Icons.hpp"
#include "editor/EditorProjectActions.hpp"
#include "io/FileUtil.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/EditorProjectPublisher.hpp"
#include "layers/AppLayer.hpp"
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

		std::filesystem::path SettingsPath(const app::EditorProjectContext& project)
		{
			return project.projectFile;
		}

		std::filesystem::path PublishSettingsPath(const app::EditorProjectContext& project)
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

		std::string FileSummary(const std::filesystem::path& path)
		{
			auto size = io::file_util::FileSize(path);
			if (!size)
			{
				return "Not built yet";
			}
			const std::uintmax_t bytes = *size;
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

		template<std::size_t N>
		void CopyToBuffer(std::array<char, N>& buffer, std::string_view text)
		{
			buffer.fill('\0');
			const std::size_t count = std::min(text.size(), N - 1);
			std::copy_n(text.data(), count, buffer.data());
		}

		template<std::size_t N>
		std::string BufferText(const std::array<char, N>& buffer)
		{
			return std::string(buffer.data());
		}
	} // namespace

	void ProjectPanel::Refresh(const app::EditorProjectContext& project)
	{
		m_lastRoot = project.root;
		m_scenes.clear();
		m_lastPackPath.clear();
		m_packStatus.clear();
		m_packSucceeded = false;
		m_shaderStatus.clear();
		m_shaderSucceeded = false;
		m_lastPublishPath.clear();
		m_publishStatus.clear();
		m_publishSucceeded = false;
		ResetPublishSettings(project);
		LoadPublishSettings(project);
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
		m_startupScene.clear();
		m_dirtySettings = false;

		auto text = io::file_util::ReadText(SettingsPath(project));
		if (!text)
		{
			return;
		}

		TomlConfig config;
		try
		{
			config.Load(*text);
		}
		catch (...)
		{
			m_status = "Could not parse project settings.";
			return;
		}
		m_startupScene = config.GetString("app.startupscene");
	}

	void ProjectPanel::SaveProjectSettings(const app::EditorProjectContext& project)
	{
		TomlConfig config;
		{
			auto text = io::file_util::ReadText(SettingsPath(project));
			if (text)
			{
				try
				{
					config.Load(*text);
				}
				catch (...)
				{
					m_status = "Could not parse project settings.";
					return;
				}
			}
		}

		config.Set("app.startupScene", m_startupScene);

		std::ostringstream buffer;
		config.Save(buffer, "AetherCore project file.");

		if (auto result = io::file_util::WriteText(SettingsPath(project), buffer.str()); !result)
		{
			m_status = "Could not write project settings.";
			return;
		}
		m_dirtySettings = false;
		m_status = "Project settings saved.";
	}

	void ProjectPanel::LoadPublishSettings(const app::EditorProjectContext& project)
	{
		auto text = io::file_util::ReadText(PublishSettingsPath(project));
		if (!text)
		{
			return;
		}

		TomlConfig config;
		try
		{
			config.Load(*text);
		}
		catch (...)
		{
			m_publishStatus = "Could not parse publish settings.";
			m_publishSucceeded = false;
			return;
		}

		CopyToBuffer(m_publishProductName, config.GetString("publish.productname", BufferText(m_publishProductName)));
		CopyToBuffer(m_publishPlatformName, config.GetString("publish.platformname", BufferText(m_publishPlatformName)));
		CopyToBuffer(m_publishOutputRoot, config.GetString("publish.outputroot", BufferText(m_publishOutputRoot)));
		m_publishCleanOutput = config.GetBool("publish.cleanoutput", m_publishCleanOutput);
		m_publishBuildScripts = config.GetBool("publish.buildscripts", m_publishBuildScripts);
		m_publishUsePackageTemplate = config.GetBool("publish.usepackagetemplate", m_publishUsePackageTemplate);
		m_publishVerifyOutput = config.GetBool("publish.verifyoutput", m_publishVerifyOutput);
		m_publishSyncEditorPak = config.GetBool("publish.synceditorpak", m_publishSyncEditorPak);
		m_publishOpenAfter = config.GetBool("publish.openafter", m_publishOpenAfter);
	}

	void ProjectPanel::SavePublishSettings(const app::EditorProjectContext& project)
	{
		TomlConfig config;
		{
			auto text = io::file_util::ReadText(PublishSettingsPath(project));
			if (text)
			{
				try
				{
					config.Load(*text);
				}
				catch (...)
				{
					m_publishStatus = "Could not parse publish settings.";
					m_publishSucceeded = false;
					return;
				}
			}
		}

		config.Set("publish.productName", BufferText(m_publishProductName));
		config.Set("publish.platformName", BufferText(m_publishPlatformName));
		config.Set("publish.outputRoot", BufferText(m_publishOutputRoot));
		config.Set("publish.cleanOutput", m_publishCleanOutput);
		config.Set("publish.buildScripts", m_publishBuildScripts);
		config.Set("publish.usePackageTemplate", m_publishUsePackageTemplate);
		config.Set("publish.verifyOutput", m_publishVerifyOutput);
		config.Set("publish.syncEditorPak", m_publishSyncEditorPak);
		config.Set("publish.openAfter", m_publishOpenAfter);

		std::ostringstream buffer;
		config.Save(buffer, "AetherCore project file.");

		if (auto result = io::file_util::WriteText(PublishSettingsPath(project), buffer.str()); !result)
		{
			m_publishStatus = "Could not write publish settings.";
			m_publishSucceeded = false;
			return;
		}
		m_publishStatus = "Publish defaults saved.";
		m_publishSucceeded = true;
	}

	void ProjectPanel::ResetPublishSettings(const app::EditorProjectContext& project)
	{
		const EditorProjectPublishOptions defaults = MakeDefaultEditorProjectPublishOptions(project);
		CopyToBuffer(m_publishProductName, defaults.productName);
		CopyToBuffer(m_publishPlatformName, defaults.platformName);
		CopyToBuffer(m_publishOutputRoot, DisplayPath(defaults.outputRoot));
		m_publishCleanOutput = defaults.cleanOutput;
		m_publishBuildScripts = defaults.buildProjectScripts;
		m_publishUsePackageTemplate = defaults.usePackageTemplate;
		m_publishVerifyOutput = defaults.verifyOutput;
		m_publishSyncEditorPak = defaults.syncEditorRuntimeProjectPak;
		m_publishOpenAfter = true;
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

	void ProjectPanel::DrawPublishDialog(app::LayerContext& context, const app::EditorProjectContext& project)
	{
		ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Publish Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			return;
		}

		ImGui::SeparatorText("Output");
		ImGui::SetNextItemWidth(260.0f);
		ImGui::InputText("Product", m_publishProductName.data(), m_publishProductName.size());
		ImGui::SetNextItemWidth(180.0f);
		ImGui::InputText("Platform", m_publishPlatformName.data(), m_publishPlatformName.size());
		ImGui::SetNextItemWidth(440.0f);
		ImGui::InputText("Output Root", m_publishOutputRoot.data(), m_publishOutputRoot.size());
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_ROTATE))
		{
			const EditorProjectPublishOptions defaults = MakeDefaultEditorProjectPublishOptions(project);
			CopyToBuffer(m_publishOutputRoot, DisplayPath(defaults.outputRoot));
		}

		const std::filesystem::path outputRoot = BufferText(m_publishOutputRoot);
		const std::string platform = BufferText(m_publishPlatformName).empty() ? std::string{"Windows"} : BufferText(m_publishPlatformName);
		const std::string product = BufferText(m_publishProductName).empty() ? project.name : BufferText(m_publishProductName);
		const std::filesystem::path finalFolder = outputRoot / platform / product;
		ImGui::TextDisabled("%s", DisplayPath(finalFolder).c_str());

		ImGui::SeparatorText("Build");
		if (ImGui::BeginTable("##publishSettings", 2, ImGuiTableFlags_SizingStretchSame))
		{
			ImGui::TableNextColumn();
			ImGui::Checkbox("Clean output", &m_publishCleanOutput);
			ImGui::Checkbox("Build scripts", &m_publishBuildScripts);
			ImGui::Checkbox("Use package template", &m_publishUsePackageTemplate);
			ImGui::TableNextColumn();
			ImGui::Checkbox("Verify package", &m_publishVerifyOutput);
			ImGui::Checkbox("Sync editor pak", &m_publishSyncEditorPak);
			ImGui::Checkbox("Open when done", &m_publishOpenAfter);
			ImGui::EndTable();
		}

		if (!m_publishStatus.empty())
		{
			const ImVec4 color = m_publishSucceeded ? ImVec4(0.55f, 0.85f, 0.62f, 1.0f) : ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
			ImGui::TextColored(color, "%s", m_publishStatus.c_str());
		}

		ImGui::Separator();
		const auto* actions = context.TryGet<EditorProjectActions>();
		const bool canPublish = actions != nullptr && actions->publishProject && !BufferText(m_publishProductName).empty() && !BufferText(m_publishPlatformName).empty() && !BufferText(m_publishOutputRoot).empty();
		if (chrome::OutlineButton(ICON_FA_FLOPPY_DISK " Save Defaults", ImVec2(140.0f, 0.0f)))
		{
			SavePublishSettings(project);
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!canPublish);
		if (chrome::PrimaryButton(ICON_FA_ROCKET " Publish", ImVec2(140.0f, 0.0f)))
		{
			if (m_dirtySettings)
			{
				SaveProjectSettings(project);
			}
			SavePublishSettings(project);

			EditorProjectPublishOptions options;
			options.outputRoot = outputRoot;
			options.productName = BufferText(m_publishProductName);
			options.platformName = BufferText(m_publishPlatformName);
			options.cleanOutput = m_publishCleanOutput;
			options.buildProjectScripts = m_publishBuildScripts;
			options.usePackageTemplate = m_publishUsePackageTemplate;
			options.verifyOutput = m_publishVerifyOutput;
			options.syncEditorRuntimeProjectPak = m_publishSyncEditorPak;

			const EditorProjectActionResult result = actions->publishProject(project, options);
			m_publishSucceeded = result.succeeded;
			m_publishStatus = result.message;
			m_lastPublishPath = result.outputPath;
			if (result.succeeded)
			{
				m_packSucceeded = true;
				m_packStatus = "Project packed as part of publish.";
				m_lastPackPath = result.outputPath / "data" / "project.pak";
				if (m_publishOpenAfter)
				{
					OpenFolderInShell(result.outputPath);
				}
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void ProjectPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Project", VisiblePtr());

		const auto* project = context.TryGet<app::EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			chrome::PanelHeader("PROJECT");
			ImGui::TextDisabled("No project is open.");
			if (auto* actions = context.TryGet<EditorProjectActions>())
			{
				if (actions->openLauncher && chrome::OutlineButton(ICON_FA_CUBE " Open Launcher"))
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

		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 bp = ImGui::GetCursorScreenPos();
			const float bandW = ImGui::GetContentRegionAvail().x;
			drawList->AddRectFilled(ImVec2(bp.x, bp.y + 3.0f), ImVec2(bp.x + 3.0f, bp.y + 30.0f), chrome::U32(chrome::kAccent));
			chrome::TextSized(drawList, 12.0f, ImVec2(bp.x + 10.0f, bp.y), chrome::kMuted, "PROJECT");
			chrome::TextSized(drawList, 17.0f, ImVec2(bp.x + 10.0f, bp.y + 14.0f), chrome::kText, project->name.c_str());
			ImGui::Dummy(ImVec2(0.0f, 34.0f));
			ImGui::TextDisabled("%s", DisplayPath(project->root).c_str());
			chrome::AccentHairline(drawList, ImGui::GetCursorScreenPos(), bandW, 0.30f);
			ImGui::Dummy(ImVec2(0.0f, 4.0f));
		}

		if (auto* actions = context.TryGet<EditorProjectActions>())
		{
			if (actions->openLauncher && chrome::GhostButton(ICON_FA_CUBE " Launcher"))
			{
				actions->openLauncher();
			}
			ImGui::SameLine();
			if (actions->reloadProject && chrome::GhostButton(ICON_FA_ROTATE " Reload"))
			{
				actions->reloadProject();
				Refresh(*project);
			}
		}
		ImGui::SameLine();
		if (chrome::GhostButton(ICON_FA_FOLDER_OPEN " Root"))
		{
			OpenFolderInShell(project->root);
		}
		ImGui::SameLine();
		if (chrome::GhostButton(ICON_FA_FOLDER_OPEN " Repair Folders"))
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
		if (chrome::GhostButton(ICON_FA_XMARK " Clear Startup"))
		{
			m_startupScene.clear();
			m_dirtySettings = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!m_dirtySettings);
		if (chrome::OutlineButton(ICON_FA_FLOPPY_DISK " Save Project Settings"))
		{
			SaveProjectSettings(*project);
		}
		ImGui::EndDisabled();

		ImGui::SeparatorText("Packaging");
		if (auto* actions = context.TryGet<EditorProjectActions>())
		{
			ImGui::BeginDisabled(!actions->packProject);
			if (chrome::GhostButton(ICON_FA_BOX_OPEN " Pack Project"))
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
			ImGui::SameLine();
			if (actions->rebuildEnginePak)
			{
				if (chrome::GhostButton(ICON_FA_GEAR " Rebuild Engine Pak"))
				{
					const EditorProjectActionResult result = actions->rebuildEnginePak();
					m_packSucceeded = result.succeeded;
					m_packStatus = result.message;
				}
				ImGui::SameLine();
			}
			if (actions->recompileShaders)
			{
				if (ImGui::Button(ICON_FA_BOLT "  Recompile Shaders"))
				{
					const EditorProjectActionResult result = actions->recompileShaders();
					m_shaderSucceeded = result.succeeded;
					m_shaderStatus = result.message;
				}
				ImGui::SameLine();
			}
			ImGui::BeginDisabled(!actions->publishProject);
			if (ImGui::Button(ICON_FA_ROCKET "  Publish..."))
			{
				ResetPublishSettings(*project);
				LoadPublishSettings(*project);
				m_publishStatus.clear();
				m_publishSucceeded = false;
				ImGui::OpenPopup("Publish Game");
			}
			ImGui::EndDisabled();
		}
		DrawPublishDialog(context, *project);
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
		if (!m_shaderStatus.empty())
		{
			const ImVec4 color = m_shaderSucceeded ? ImVec4(0.55f, 0.85f, 0.62f, 1.0f) : ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
			ImGui::TextColored(color, "%s", m_shaderStatus.c_str());
		}
		if (!m_lastPublishPath.empty())
		{
			ImGui::BeginDisabled(!FolderExists(m_lastPublishPath));
			if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Published Build"))
			{
				OpenFolderInShell(m_lastPublishPath);
			}
			ImGui::EndDisabled();
			ImGui::TextDisabled("%s", DisplayPath(m_lastPublishPath).c_str());
		}
		if (!m_publishStatus.empty())
		{
			const ImVec4 color = m_publishSucceeded ? ImVec4(0.55f, 0.85f, 0.62f, 1.0f) : ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
			ImGui::TextColored(color, "%s", m_publishStatus.c_str());
		}

		ImGui::End();
	}
} // namespace aether::editor
