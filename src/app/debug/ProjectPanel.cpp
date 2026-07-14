#include "debug/ProjectPanel.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/InspectorWidgets.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

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

		ImVec4 StatusColor(bool succeeded)
		{
			return succeeded ? chrome::kSuccess : chrome::kError;
		}

		void StatusText(const std::string& message, bool succeeded)
		{
			if (message.empty())
			{
				return;
			}
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(StatusColor(succeeded), "%s", message.c_str());
			ImGui::PopTextWrapPos();
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

		m_publishProductName = config.GetString("publish.productname", m_publishProductName);
		m_publishPlatformName = config.GetString("publish.platformname", m_publishPlatformName);
		m_publishOutputRoot = config.GetString("publish.outputroot", m_publishOutputRoot);
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

		config.Set("publish.productName", m_publishProductName);
		config.Set("publish.platformName", m_publishPlatformName);
		config.Set("publish.outputRoot", m_publishOutputRoot);
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
		m_publishProductName = defaults.productName;
		m_publishPlatformName = defaults.platformName;
		m_publishOutputRoot = DisplayPath(defaults.outputRoot);
		m_publishCleanOutput = defaults.cleanOutput;
		m_publishBuildScripts = defaults.buildProjectScripts;
		m_publishUsePackageTemplate = defaults.usePackageTemplate;
		m_publishVerifyOutput = defaults.verifyOutput;
		m_publishSyncEditorPak = defaults.syncEditorRuntimeProjectPak;
		m_publishOpenAfter = true;
	}

	void ProjectPanel::DrawFolderRow(const char* label, const std::filesystem::path& path)
	{
		const bool exists = FolderExists(path);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		ImGui::TextColored(exists ? chrome::kSuccess : chrome::kWarning, "%s", exists ? "Ready" : "Missing");
		ImGui::TableSetColumnIndex(2);
		ImGui::TextDisabled("%s", DisplayPath(path).c_str());
		ImGui::TableSetColumnIndex(3);
		ImGui::PushID(label);
		ImGui::BeginDisabled(!exists);
		if (chrome::GhostIconButton(ICON_FA_FOLDER_OPEN, "##open", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
		{
			OpenFolderInShell(path);
		}
		ImGui::EndDisabled();
		ImGui::PopID();
	}

	void ProjectPanel::DrawSceneTable()
	{
		if (!ImGui::BeginTable("##projectScenes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
		{
			return;
		}

		ImGui::TableSetupColumn("Scene");
		ImGui::TableSetupColumn("Startup", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + 8.0f);
		ImGui::TableSetupColumn("Path");
		ImGui::TableHeadersRow();

		for (const SceneEntry& scene: m_scenes)
		{
			ImGui::PushID(scene.path.generic_string().c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
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
		ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
		if (!ImGui::BeginPopupModal("Publish Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			return;
		}

		ImGui::SeparatorText("Output");
		iw::PropInputText("Product", m_publishProductName);
		iw::PropInputText("Platform", m_publishPlatformName);

		iw::LabelColumn("Output Root");
		const float resetWidth = ImGui::GetFrameHeight();
		ImGui::SetNextItemWidth(-(resetWidth + ImGui::GetStyle().ItemSpacing.x));
		ImGui::InputText("##outputRoot", &m_publishOutputRoot);
		ImGui::SameLine();
		if (chrome::GhostIconButton(ICON_FA_ROTATE, "##resetRoot", ImVec2(resetWidth, ImGui::GetFrameHeight())))
		{
			const EditorProjectPublishOptions defaults = MakeDefaultEditorProjectPublishOptions(project);
			m_publishOutputRoot = DisplayPath(defaults.outputRoot);
		}

		const std::filesystem::path outputRoot = m_publishOutputRoot;
		const std::string platform = m_publishPlatformName.empty() ? std::string{"Windows"} : m_publishPlatformName;
		const std::string product = m_publishProductName.empty() ? project.name : m_publishProductName;
		const std::filesystem::path finalFolder = outputRoot / platform / product;
		iw::LabelColumn("Destination");
		MutedWrapped(DisplayPath(finalFolder));

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

		StatusText(m_publishStatus, m_publishSucceeded);

		ImGui::Separator();
		const auto* actions = context.TryGet<EditorProjectActions>();
		const bool canPublish = actions != nullptr && actions->publishProject && !m_publishProductName.empty() && !m_publishPlatformName.empty() && !m_publishOutputRoot.empty();

		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float buttonWidth = (ImGui::GetContentRegionAvail().x - 2.0f * spacing) / 3.0f;
		if (chrome::OutlineButton(ICON_FA_FLOPPY_DISK " Save Defaults", ImVec2(buttonWidth, 0.0f)))
		{
			SavePublishSettings(project);
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!canPublish);
		if (chrome::PrimaryButton(ICON_FA_ROCKET " Publish", ImVec2(buttonWidth, 0.0f)))
		{
			if (m_dirtySettings)
			{
				SaveProjectSettings(project);
			}
			SavePublishSettings(project);

			EditorProjectPublishOptions options;
			options.outputRoot = outputRoot;
			options.productName = m_publishProductName;
			options.platformName = m_publishPlatformName;
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
		if (chrome::GhostButton("Cancel", ImVec2(buttonWidth, 0.0f)))
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

		const std::string sceneStat = std::to_string(m_scenes.size()) + (m_scenes.size() == 1 ? " scene" : " scenes");
		chrome::PanelHeader("PROJECT", sceneStat.c_str());
		ImGui::TextUnformatted(project->name.c_str());
		MutedWrapped(DisplayPath(project->root));
		ImGui::Dummy(ImVec2(0.0f, 2.0f));

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
			{
				const char* label = ICON_FA_FOLDER_OPEN " Repair Folders";
				toolbar.Item(label);
				if (chrome::GhostButton(label))
				{
					EnsureStandardFolders(*project);
				}
			}
		}
		MutedWrapped(m_status);

		if (actions != nullptr)
		{
			const auto& visualStudios = actions->visualStudioInstallations;
			const auto selected = std::ranges::find(visualStudios, m_visualStudioInstall, &VisualStudioInstallation::installPath);
			if (selected == visualStudios.end() && !visualStudios.empty())
			{
				const auto compatible = std::ranges::find_if(visualStudios, [](const VisualStudioInstallation& installation) { return installation.supportsDotNet10 && installation.hasDebuggerAutomation; });
				m_visualStudioInstall = (compatible != visualStudios.end() ? compatible : visualStudios.begin())->installPath;
			}
			const auto current = std::ranges::find(visualStudios, m_visualStudioInstall, &VisualStudioInstallation::installPath);

			ImGui::SeparatorText("Scripting");
			iw::LabelColumn("C# Debugger");
			const char* debugLabel = ICON_FA_BUG " Debug C#";
			const float debugWidth = ImGui::CalcTextSize(debugLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			ImGui::SetNextItemWidth(-(debugWidth + ImGui::GetStyle().ItemSpacing.x));
			const char* preview = current != visualStudios.end() ? current->displayName.c_str() : "No Visual Studio IDE found";
			if (ImGui::BeginCombo("##scriptDebugger", preview))
			{
				for (const VisualStudioInstallation& installation: visualStudios)
				{
					const bool isSelected = installation.installPath == m_visualStudioInstall;
					const std::string label = installation.displayName + (installation.supportsDotNet10 ? "" : " (.NET 10 unsupported)") + (installation.hasDebuggerAutomation ? "" : " (debugger automation unavailable)");
					if (ImGui::Selectable(label.c_str(), isSelected))
					{
						m_visualStudioInstall = installation.installPath;
					}
					if (isSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			const bool canDebugScripts = actions->debugScripts && current != visualStudios.end() && current->supportsDotNet10 && current->hasDebuggerAutomation;
			ImGui::BeginDisabled(!canDebugScripts);
			if (chrome::PrimaryButton(debugLabel))
			{
				const EditorProjectActionResult result = actions->debugScripts(m_visualStudioInstall);
				m_status = result.message;
			}
			ImGui::EndDisabled();
		}

		ImGui::SeparatorText("Folders");
		if (ImGui::BeginTable("##projectFolders", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 82.0f);
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 64.0f);
			ImGui::TableSetupColumn("Path");
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() + 4.0f);
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

		{
			ButtonRow row;
			const char* clearLabel = ICON_FA_XMARK " Clear Startup";
			row.Item(clearLabel);
			ImGui::BeginDisabled(m_startupScene.empty());
			if (chrome::GhostButton(clearLabel))
			{
				m_startupScene.clear();
				m_dirtySettings = true;
			}
			ImGui::EndDisabled();

			const char* saveLabel = ICON_FA_FLOPPY_DISK " Save Project Settings";
			row.Item(saveLabel);
			ImGui::BeginDisabled(!m_dirtySettings);
			if (chrome::OutlineButton(saveLabel))
			{
				SaveProjectSettings(*project);
			}
			ImGui::EndDisabled();
		}

		ImGui::SeparatorText("Build & Publish");
		if (actions != nullptr)
		{
			ButtonRow row;

			const char* packLabel = ICON_FA_BOX_OPEN " Pack Project";
			row.Item(packLabel);
			ImGui::BeginDisabled(!actions->packProject);
			if (chrome::GhostButton(packLabel))
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

			if (actions->rebuildEnginePak)
			{
				const char* label = ICON_FA_GEAR " Rebuild Engine Pak";
				row.Item(label);
				if (chrome::GhostButton(label))
				{
					const EditorProjectActionResult result = actions->rebuildEnginePak();
					m_packSucceeded = result.succeeded;
					m_packStatus = result.message;
				}
			}

			if (actions->recompileShaders)
			{
				const char* label = ICON_FA_BOLT " Recompile Shaders";
				row.Item(label);
				if (chrome::GhostButton(label))
				{
					const EditorProjectActionResult result = actions->recompileShaders();
					m_shaderSucceeded = result.succeeded;
					m_shaderStatus = result.message;
				}
			}

			if (!m_lastPackPath.empty())
			{
				const char* label = ICON_FA_FOLDER_OPEN " Output";
				row.Item(label);
				ImGui::BeginDisabled(!FolderExists(m_lastPackPath.parent_path()));
				if (chrome::GhostButton(label))
				{
					OpenFolderInShell(m_lastPackPath.parent_path());
				}
				ImGui::EndDisabled();
			}

			const char* publishLabel = ICON_FA_ROCKET " Publish...";
			row.Item(publishLabel);
			ImGui::BeginDisabled(!actions->publishProject);
			if (chrome::PrimaryButton(publishLabel))
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
			MutedWrapped(DisplayPath(m_lastPackPath));
			MutedWrapped(FileSummary(m_lastPackPath));
		}
		StatusText(m_packStatus, m_packSucceeded);
		StatusText(m_shaderStatus, m_shaderSucceeded);

		if (!m_lastPublishPath.empty())
		{
			ImGui::BeginDisabled(!FolderExists(m_lastPublishPath));
			if (chrome::GhostButton(ICON_FA_FOLDER_OPEN " Published Build"))
			{
				OpenFolderInShell(m_lastPublishPath);
			}
			ImGui::EndDisabled();
			MutedWrapped(DisplayPath(m_lastPublishPath));
		}
		StatusText(m_publishStatus, m_publishSucceeded);

		ImGui::End();
	}
} // namespace aether::editor
