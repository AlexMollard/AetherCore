#include "DebugLayer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace std::string_view_literals;

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>

#ifdef _WIN32
#	include <Windows.h>
#	include <shobjidl.h>
#	include <shellapi.h>
#	undef CopyFile // Windows.h defines CopyFile as CopyFileA/CopyFileW macro, conflicts with file_util::CopyFile
#endif

#include "debug/ConsolePanel.hpp"
#include "debug/DayNightPanel.hpp"
#include "debug/OpenInEditor.hpp"
#include "debug/DevToolsPanel.hpp"
#include "debug/FileExplorerPanel.hpp"
#include "debug/Icons.hpp"
#include "debug/HierarchyPanel.hpp"
#include "debug/InspectorPanel.hpp"
#include "debug/LightingPanel.hpp"
#include "debug/PerformancePanel.hpp"
#include "debug/PostProcessingPanel.hpp"
#include "debug/ProjectPanel.hpp"
#include "debug/RenderGraphPanel.hpp"
#include "debug/SettingsPanel.hpp"
#include "debug/TonemapPanel.hpp"
#include "debug/TextureInspectorPanel.hpp"
#include "debug/UiCanvasPanel.hpp"
#include "debug/ViewportPanel.hpp"
#include "AetherCore.hpp"
#include "PlaySession.hpp"
#include "assets/AssetManager.hpp"
#include "editor/EditorProjectPublisher.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "mesh/Mesh.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "platform/Input.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/Components.hpp"
#include "PlayState.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/FuzzyMatch.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/SettingsService.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/TextIni.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::app
{
	namespace
	{
		// 12 wireframe edges of a local AABB under an arbitrary affine transform
		// (AddDebugBox's quat form cannot represent non-uniform scale or shear).
		void AppendObbEdges(std::vector<DebugVertex>& out, const glm::mat4& m, const glm::vec3& mn, const glm::vec3& mx, const glm::vec4& color)
		{
			glm::vec3 corners[8];
			for (int i = 0; i < 8; ++i)
			{
				const glm::vec3 local{(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z};
				corners[i] = glm::vec3(m * glm::vec4(local, 1.0f));
			}
			static constexpr int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
			for (const auto& edge: kEdges)
			{
				AddDebugLine(out, corners[edge[0]], corners[edge[1]], color);
			}
		}

		// Font-Awesome glyph shown next to each window in the Window menu. Falls back
		// to a neutral dot for anything unmapped.
		const char* WindowMenuIcon(std::string_view panelName)
		{
			if (panelName == "Scene Outliner")
			{
				return ICON_FA_SITEMAP;
			}
			if (panelName == "Inspector")
			{
				return ICON_FA_MAGNIFYING_GLASS;
			}
			if (panelName == "File Explorer")
			{
				return ICON_FA_FOLDER_OPEN;
			}
			if (panelName == "Project")
			{
				return ICON_FA_CUBE;
			}
			if (panelName == "Viewport")
			{
				return ICON_FA_EYE;
			}
			if (panelName == "UI Canvas")
			{
				return ICON_FA_IMAGE;
			}
			if (panelName == "Render Graph")
			{
				return ICON_FA_DIAGRAM_PROJECT;
			}
			if (panelName == "Post Processing")
			{
				return ICON_FA_WAND_MAGIC_SPARKLES;
			}
			if (panelName == "Tonemap")
			{
				return ICON_FA_PALETTE;
			}
			if (panelName == "Lighting")
			{
				return ICON_FA_LIGHTBULB;
			}
			if (panelName == "Day / Night")
			{
				return ICON_FA_CLOUD_SUN;
			}
			if (panelName == "TextureInspector")
			{
				return ICON_FA_IMAGE;
			}
			if (panelName == "Console")
			{
				return ICON_FA_CODE;
			}
			if (panelName == "Performance")
			{
				return ICON_FA_GAUGE_HIGH;
			}
			if (panelName == "DevTools")
			{
				return ICON_FA_BUG;
			}
			if (panelName == "Settings")
			{
				return ICON_FA_GEARS;
			}
			return ICON_FA_CIRCLE;
		}

		// Slugifies a panel name into a stable config key ("Render Graph" ->
		// "debug.window.render_graph") for persisting per-panel visibility.
		std::string PanelVisibilityKey(std::string_view panelName)
		{
			std::string key = "debug.window.";
			for (const char c: panelName)
			{
				key += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
			}
			return key;
		}

		constexpr int kMaxRecentProjects = 8;
		constexpr std::string_view kProjectDirectory = ".project";
		constexpr std::string_view kProjectDescriptor = "aether.project";

		std::filesystem::path NormalizePath(std::filesystem::path path)
		{
			std::error_code ec;
			if (path.empty())
			{
				return {};
			}
			path = std::filesystem::absolute(path, ec);
			if (ec)
			{
				return path.lexically_normal();
			}
			const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
			return ec ? path.lexically_normal() : canonical;
		}

		std::string DisplayPath(const std::filesystem::path& path)
		{
			return path.empty() ? std::string{} : path.lexically_normal().string();
		}

		std::filesystem::path ProjectDirectoryPath(const std::filesystem::path& root)
		{
			return root / kProjectDirectory;
		}

		std::filesystem::path DescriptorPath(const std::filesystem::path& root)
		{
			return ProjectDirectoryPath(root) / kProjectDescriptor;
		}

		std::filesystem::path LegacyDescriptorPath(const std::filesystem::path& root)
		{
			return root / kProjectDescriptor;
		}

		std::filesystem::path ExistingDescriptorPath(const std::filesystem::path& root)
		{
			const std::filesystem::path descriptor = DescriptorPath(root);
			if (io::file_util::Exists(descriptor))
			{
				return descriptor;
			}
			const std::filesystem::path legacyDescriptor = LegacyDescriptorPath(root);
			if (io::file_util::Exists(legacyDescriptor))
			{
				return legacyDescriptor;
			}
			return descriptor;
		}

		std::filesystem::path ResolveProjectRoot(std::filesystem::path path)
		{
			path = NormalizePath(std::move(path));
			if (path.empty())
			{
				return {};
			}
			if (path.filename() == kProjectDescriptor)
			{
				path = path.parent_path();
			}
			if (path.filename() == kProjectDirectory)
			{
				path = path.parent_path();
			}
			return path;
		}

		bool HasProjectDescriptor(const std::filesystem::path& root)
		{
			if (io::file_util::Exists(DescriptorPath(root)))
			{
				return true;
			}
			return io::file_util::Exists(LegacyDescriptorPath(root));
		}

		std::string FallbackProjectName(const std::filesystem::path& root)
		{
			const std::string name = root.filename().string();
			return name.empty() ? "Aether Project" : name;
		}

		std::filesystem::path ResolveProjectPath(const std::filesystem::path& root, std::string_view value, std::string_view fallback)
		{
			std::filesystem::path path = value.empty() ? std::filesystem::path(fallback) : std::filesystem::path(std::string(value));
			if (path.is_relative())
			{
				path = root / path;
			}
			return NormalizePath(std::move(path));
		}

		Expected<EditorProjectContext> ReadProjectDescriptor(const std::filesystem::path& root)
		{
			EditorProjectContext project;
			project.root = NormalizePath(root);
			project.name = FallbackProjectName(project.root);
			project.assetsDir = ResolveProjectPath(project.root, {}, "assets");
			project.scenesDir = ResolveProjectPath(project.root, {}, "scenes");
			project.prefabsDir = ResolveProjectPath(project.root, {}, "assets/prefabs");
			project.scriptsDir = ResolveProjectPath(project.root, {}, "scripts");
			project.settingsDir = ResolveProjectPath(project.root, {}, "settings");

			auto descriptorText = io::file_util::ReadText(ExistingDescriptorPath(root));
			if (!descriptorText)
			{
				AE_UNEXPECTED(AetherError::Engine("No project descriptor found."));
			}

			std::string assetsPath;
			std::string scenesPath;
			std::string prefabsPath;
			std::string scriptsPath;
			std::string settingsPath;
			try
			{
				text::ParseToml(*descriptorText,
				        [&](const text::IniEntry& entry)
				        {
					        if (entry.fullKey == "project.name")
					        {
						        project.name = text::StripQuotes(entry.value);
					        }
					        else if (entry.fullKey == "paths.assets")
					        {
						        assetsPath = text::StripQuotes(entry.value);
					        }
					        else if (entry.fullKey == "paths.scenes")
					        {
						        scenesPath = text::StripQuotes(entry.value);
					        }
					        else if (entry.fullKey == "paths.prefabs")
					        {
						        prefabsPath = text::StripQuotes(entry.value);
					        }
					        else if (entry.fullKey == "paths.scripts")
					        {
						        scriptsPath = text::StripQuotes(entry.value);
					        }
					        else if (entry.fullKey == "paths.settings")
					        {
						        settingsPath = text::StripQuotes(entry.value);
					        }
				        });
			}
			catch (...)
			{
				return project;
			}

			if (project.name.empty())
			{
				project.name = FallbackProjectName(project.root);
			}
			project.assetsDir = ResolveProjectPath(project.root, assetsPath, "assets");
			project.scenesDir = ResolveProjectPath(project.root, scenesPath, "scenes");
			const std::string inferredPrefabsPath = !prefabsPath.empty() ? prefabsPath : (!assetsPath.empty() ? assetsPath + "/prefabs" : std::string{});
			project.prefabsDir = ResolveProjectPath(project.root, inferredPrefabsPath, "assets/prefabs");
			project.scriptsDir = ResolveProjectPath(project.root, scriptsPath, "scripts");
			project.settingsDir = ResolveProjectPath(project.root, settingsPath, "settings");
			return project;
		}

		std::string ReadProjectName(const std::filesystem::path& root)
		{
			auto result = ReadProjectDescriptor(root);
			return result.has_value() ? result->name : FallbackProjectName(root);
		}

		std::string EscapeTomlString(std::string_view value)
		{
			std::string out;
			for (const char c: value)
			{
				if (c == '\\' || c == '"')
				{
					out += '\\';
				}
				out += c;
			}
			return out;
		}

		bool SeedProjectTemplateFiles(const std::filesystem::path& root, std::string& error)
		{
			auto CopyTemplateFile = [&](const std::filesystem::path& srcRoot, const char* relPath, const std::filesystem::path& dest) -> bool
			{
				if (io::file_util::Exists(dest))
				{
					return true;
				}
				if (auto dirResult = io::file_util::CreateDirectories(dest.parent_path()); !dirResult)
				{
					error = "Could not create project folder: " + dirResult.error().message;
					return false;
				}
				if (auto copyResult = io::file_util::CopyFile(srcRoot / relPath, dest); !copyResult)
				{
					error = "Could not copy project template file '" + std::string(relPath) + "': " + copyResult.error().message;
					return false;
				}
				return true;
			};

			const std::filesystem::path scriptsProject = root / "scripts" / "AetherGame.csproj";
			if (!io::file_util::Exists(scriptsProject))
			{
				const EditorProjectPublishConfig publishConfig = MakeDefaultEditorProjectPublishConfig();
				if (auto writeResult = io::file_util::WriteText(scriptsProject, MakeProjectScriptCsprojText(publishConfig.managedSdkProject)); !writeResult)
				{
					error = "Could not write project scripts file: " + writeResult.error().message;
					return false;
				}
			}

			return CopyTemplateFile(AETHER_DEFAULT_SETTINGS_DIR, "engine.toml", root / "settings" / "engine.toml") && CopyTemplateFile(AETHER_SCENES_SOURCE_DIR, "default.scene.toml", root / "scenes" / "default.scene.toml");
		}

		std::string ReadProjectStartupScene(const EditorProjectContext& project)
		{
			const std::filesystem::path settingsPath = project.settingsDir / "engine.toml";
			auto content = io::file_util::ReadText(settingsPath);
			if (!content)
			{
				return {};
			}
			TomlConfig config;
			try
			{
				config.Load(*content);
			}
			catch (...)
			{
				AE_WARN(LogCategory::App, "Failed to parse project settings file: {}", settingsPath.string());
				return {};
			}
			return config.GetString("app.startupscene");
		}

		bool WriteProjectDescriptor(const std::filesystem::path& root, std::string_view name, std::string& error)
		{
			if (auto dirResult = io::file_util::CreateDirectories(root); !dirResult)
			{
				error = "Could not create project directory: " + dirResult.error().message;
				return false;
			}
			if (auto dirResult = io::file_util::CreateDirectories(ProjectDirectoryPath(root)); !dirResult)
			{
				error = "Could not create project metadata directory: " + dirResult.error().message;
				return false;
			}

			for (std::string_view dir: {"assets"sv, "assets/models"sv, "assets/materials"sv, "assets/textures"sv, "assets/animations"sv, "assets/prefabs"sv, "data"sv, "scenes"sv, "scripts"sv, "settings"sv})
			{
				if (auto dirResult = io::file_util::CreateDirectories(root / std::filesystem::path(dir)); !dirResult)
				{
					error = "Could not create project folder: " + dirResult.error().message;
					return false;
				}
			}

			const std::string descriptor = "# AetherCore project descriptor\n\n"
			                               "[project]\n"
			                               "version = 1\n"
			                               "name = \""
			                               + EscapeTomlString(name)
			                               + "\"\n"
			                                 "\n[paths]\n"
			                                 "assets = \"assets\"\n"
			                                 "scenes = \"scenes\"\n"
			                                 "prefabs = \"assets/prefabs\"\n"
			                                 "scripts = \"scripts\"\n"
			                                 "settings = \"settings\"\n";
			if (auto writeResult = io::file_util::WriteText(DescriptorPath(root), descriptor); !writeResult)
			{
				error = "Could not write aether.project.";
				return false;
			}
			return SeedProjectTemplateFiles(root, error);
		}

#ifdef _WIN32
		std::optional<std::filesystem::path> PickProjectFolder()
		{
			const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			const bool uninitialize = SUCCEEDED(coInit);

			IFileDialog* dialog = nullptr;
			HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
			if (FAILED(hr) || dialog == nullptr)
			{
				if (uninitialize)
				{
					CoUninitialize();
				}
				return std::nullopt;
			}

			DWORD options = 0;
			if (SUCCEEDED(dialog->GetOptions(&options)))
			{
				dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
			}
			dialog->SetTitle(L"Select AetherCore Project Folder");

			std::optional<std::filesystem::path> selected;
			if (SUCCEEDED(dialog->Show(nullptr)))
			{
				IShellItem* item = nullptr;
				if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
				{
					PWSTR rawPath = nullptr;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath != nullptr)
					{
						selected = std::filesystem::path(rawPath);
						CoTaskMemFree(rawPath);
					}
					item->Release();
				}
			}

			dialog->Release();
			if (uninitialize)
			{
				CoUninitialize();
			}
			return selected;
		}
#endif
	} // namespace

	void DebugLayer::ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine)
	{
		outPath.clear();
		outLine = 0;

		// .NET exception stack traces read "... in <path>\Script.cs:line 42".
		// Anchor on the ".cs:" marker, then take the preceding path and the line
		// number after an optional "line " token so the toast can offer to open
		// the offending script.
		std::size_t searchPos = 0;
		while (searchPos < error.size())
		{
			const auto extPos = error.find(".cs:", searchPos);
			if (extPos == std::string::npos)
			{
				break;
			}

			// Path: walk back from ".cs" to the preceding whitespace.
			std::size_t start = extPos;
			while (start > 0 && error[start - 1] != ' ' && error[start - 1] != '\n' && error[start - 1] != '\r')
			{
				--start;
			}
			outPath = error.substr(start, extPos + 3 - start); // include ".cs"

			// Skip the ':' and an optional "line " token before the number.
			std::size_t lineStart = extPos + 4;
			if (error.compare(lineStart, 5, "line ") == 0)
			{
				lineStart += 5;
			}

			std::size_t lineEnd = lineStart;
			while (lineEnd < error.size() && std::isdigit(static_cast<unsigned char>(error[lineEnd])))
			{
				++lineEnd;
			}

			if (lineEnd > lineStart)
			{
				try
				{
					outLine = std::stoi(error.substr(lineStart, lineEnd - lineStart));
				}
				catch (...)
				{
					outLine = 0;
				}
			}

			if (outLine > 0)
			{
				break;
			}

			outPath.clear();
			searchPos = extPos + 1;
		}
	}

	void DebugLayer::OpenInVSCode(const std::string& filePath, int line)
	{
		OpenInEditor(filePath, line);
	}

	void DebugLayer::PollScriptErrors(LayerContext& context)
	{
		auto scripting = context.TryGet<scripting::CSharpScriptingSubsystem>();
		if (!scripting)
		{
			return;
		}

		if (scripting->ConsumeErrorsCleared())
		{
			m_errorToasts.clear();
		}

		auto errors = scripting->PollPendingErrors();
		if (!errors.empty())
		{
			m_errorToasts.clear();
		}

		for (auto& err: errors)
		{
			std::vector<std::string> individualErrors;
			static const std::regex errorPattern(R"(error\[\d+\]:)");
			auto begin = std::sregex_iterator(err.begin(), err.end(), errorPattern);
			auto end = std::sregex_iterator();

			if (begin == end)
			{
				individualErrors.push_back(err);
			}
			else
			{
				std::size_t lastPos = 0;
				for (auto it = begin; it != end; ++it)
				{
					const std::smatch& match = *it;
					if (it == begin)
					{
						lastPos = match.position();
					}
					else
					{
						individualErrors.push_back(err.substr(lastPos, match.position() - lastPos));
						lastPos = match.position();
					}
				}
				individualErrors.push_back(err.substr(lastPos));
			}

			for (const auto& singleErr: individualErrors)
			{
				if (singleErr.empty())
				{
					continue;
				}

				ScriptErrorToast toast;
				toast.message = singleErr;

				std::size_t pos = 0;
				while (pos < singleErr.size())
				{
					auto lineEnd = singleErr.find('\n', pos);
					if (lineEnd == std::string::npos)
					{
						lineEnd = singleErr.size();
					}
					std::string line = singleErr.substr(pos, lineEnd - pos);

					std::size_t first = line.find_first_not_of(" \t");
					if (first != std::string::npos && !line.empty())
					{
						toast.summary = line.substr(first);
						break;
					}
					pos = lineEnd + 1;
				}

				ParseErrorLocation(singleErr, toast.filePath, toast.line);
				m_errorToasts.push_back(std::move(toast));
			}
		}
	}

	void DebugLayer::LoadSettings(LayerContext&)
	{
		if (m_debugConfig.LoadFile("debug"))
		{
			AE_INFO(LogCategory::App, "Debug settings loaded");
		}
		LoadLauncherSettings();
	}

	void DebugLayer::SaveSettings(LayerContext&)
	{
		if (m_debugConfig.SaveIfDirty("debug", "Debug layer settings"))
		{
			AE_INFO(LogCategory::App, "Debug settings saved");
		}
	}

	void DebugLayer::PersistSettings(LayerContext& context)
	{
		for (auto& panel: m_panels)
		{
			panel->SaveSettings(m_debugConfig, context);
			m_debugConfig.Set(PanelVisibilityKey(panel->GetName()), panel->IsVisible());
		}
		SaveLauncherSettings();
		SaveSettings(context);
	}

	void DebugLayer::LoadLauncherSettings()
	{
		m_projectLauncherState.openLastProject = m_debugConfig.GetBool("launcher.open_last", false);
		m_currentProject.root = NormalizePath(m_debugConfig.GetString("launcher.current.path"));
		m_currentProject.name = m_debugConfig.GetString("launcher.current.name");
		if (!m_currentProject.root.empty() && m_currentProject.name.empty())
		{
			m_currentProject.name = ReadProjectName(m_currentProject.root);
		}

		m_recentProjects.clear();
		for (int i = 0; i < kMaxRecentProjects; ++i)
		{
			const std::string key = std::format("launcher.recent_{}", i);
			EditorProjectContext project;
			project.root = NormalizePath(m_debugConfig.GetString(key + ".path"));
			project.name = m_debugConfig.GetString(key + ".name");
			if (project.root.empty())
			{
				continue;
			}
			if (project.name.empty())
			{
				project.name = ReadProjectName(project.root);
			}
			if (std::ranges::none_of(m_recentProjects, [&](const EditorProjectContext& existing) { return NormalizePath(existing.root) == project.root; }))
			{
				m_recentProjects.push_back(std::move(project));
			}
		}

		const std::filesystem::path cwd = NormalizePath(std::filesystem::current_path());
		std::snprintf(m_projectLauncherState.openPath.data(), m_projectLauncherState.openPath.size(), "%s", DisplayPath(cwd).c_str());
		std::snprintf(m_projectLauncherState.newPath.data(), m_projectLauncherState.newPath.size(), "%s", DisplayPath(cwd / "AetherProject").c_str());
		std::snprintf(m_projectLauncherState.newName.data(), m_projectLauncherState.newName.size(), "%s", "AetherProject");

		if (m_projectLauncherState.openLastProject && HasCurrentProject())
		{
			OpenProject(m_currentProject.root);
		}
	}

	void DebugLayer::SaveLauncherSettings()
	{
		m_debugConfig.Set("launcher.open_last", m_projectLauncherState.openLastProject);
		m_debugConfig.Set("launcher.current.path", DisplayPath(m_currentProject.root));
		m_debugConfig.Set("launcher.current.name", m_currentProject.name);
		for (int i = 0; i < kMaxRecentProjects; ++i)
		{
			const std::string key = std::format("launcher.recent_{}", i);
			if (i < static_cast<int>(m_recentProjects.size()))
			{
				m_debugConfig.Set(key + ".path", DisplayPath(m_recentProjects[static_cast<std::size_t>(i)].root));
				m_debugConfig.Set(key + ".name", m_recentProjects[static_cast<std::size_t>(i)].name);
			}
			else
			{
				m_debugConfig.Set(key + ".path", std::string_view{});
				m_debugConfig.Set(key + ".name", std::string_view{});
			}
		}
	}

	void DebugLayer::AddRecentProject(std::filesystem::path root, std::string name)
	{
		root = ResolveProjectRoot(std::move(root));
		if (root.empty())
		{
			return;
		}
		if (name.empty())
		{
			name = ReadProjectName(root);
		}
		std::erase_if(m_recentProjects, [&](const EditorProjectContext& p) { return NormalizePath(p.root) == root; });
		EditorProjectContext project;
		project.root = std::move(root);
		project.name = std::move(name);
		m_recentProjects.insert(m_recentProjects.begin(), std::move(project));
		if (m_recentProjects.size() > kMaxRecentProjects)
		{
			m_recentProjects.resize(kMaxRecentProjects);
		}
	}

	bool DebugLayer::HasCurrentProject() const
	{
		return !m_currentProject.root.empty() && HasProjectDescriptor(m_currentProject.root);
	}

	void DebugLayer::OpenProject(std::filesystem::path root)
	{
		m_projectLauncherState.error.clear();
		root = ResolveProjectRoot(std::move(root));
		if (root.empty())
		{
			m_projectLauncherState.error = "Choose a project folder.";
			return;
		}
		auto projectResult = ReadProjectDescriptor(root);
		if (!projectResult)
		{
			m_projectLauncherState.error = "No .project/aether.project found in that folder.";
			return;
		}
		m_currentProject = *std::move(projectResult);
		m_currentProject.loaded = true;
		io::FileSystem::Mount("project", m_currentProject.root);
		scene::SetProjectSceneDirectories(m_currentProject.scenesDir, m_currentProject.prefabsDir);
		RefreshProjectServices();
		AddRecentProject(m_currentProject.root, m_currentProject.name);
		m_projectLoaded = true;
		m_launcherOpen = false;
		SaveLauncherSettings();
		AE_INFO(LogCategory::App, "Opened editor project '{}' at {}", m_currentProject.name, DisplayPath(m_currentProject.root));
	}

	void DebugLayer::RefreshProjectServices()
	{
		if (m_services == nullptr)
		{
			return;
		}

		m_services->Register<EditorProjectContext>(m_currentProject);
		if (auto* settings = m_services->TryGet<aether::SettingsService>())
		{
			const std::string startupScene = ReadProjectStartupScene(m_currentProject);
			if (!startupScene.empty())
			{
				settings->Values().app.startupScene = startupScene;
			}
		}
	}

	void DebugLayer::CreateProject(std::filesystem::path root, std::string_view name)
	{
		m_projectLauncherState.error.clear();
		root = ResolveProjectRoot(std::move(root));
		std::string projectName(name);
		projectName = text::TrimAscii(std::move(projectName));
		if (projectName.empty())
		{
			projectName = FallbackProjectName(root);
		}
		if (root.empty())
		{
			m_projectLauncherState.error = "Choose a project folder.";
			return;
		}
		if (!WriteProjectDescriptor(root, projectName, m_projectLauncherState.error))
		{
			return;
		}
		OpenProject(root);
	}

	void DebugLayer::DrawProjectLauncher(LayerContext&)
	{
		ProjectLauncherWindowModel model;
		model.projectLoaded = m_projectLoaded;
		model.hasCurrentProject = HasCurrentProject();
		model.currentProject = &m_currentProject;
		model.recentProjects = std::span<const EditorProjectContext>(m_recentProjects.data(), m_recentProjects.size());

		ProjectLauncherWindowActions actions;
		actions.openProject = [this](std::filesystem::path root)
		{
			OpenProject(std::move(root));
		};
		actions.createProject = [this](std::filesystem::path root, std::string_view name)
		{
			CreateProject(std::move(root), name);
		};
		actions.browseFolder = []() -> std::optional<std::filesystem::path>
		{
#ifdef _WIN32
			return PickProjectFolder();
#else
			return std::nullopt;
#endif
		};
		actions.closeLauncher = [this]()
		{
			m_launcherOpen = false;
		};
		actions.saveSettings = [this]()
		{
			SaveLauncherSettings();
		};

		m_projectLauncher.Draw(m_projectLauncherState, model, actions);
	}

	void DebugLayer::OnAttach(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		m_services = &context.services;
		LoadSettings(context);

		// Shared selection service: registered before panels attach so every
		// panel can resolve it for its whole lifetime.
		context.services.Register<SceneSelection>(m_selection);
		// Editor undo: panels push explicit points for keyboard-driven edits;
		// mouse gestures are covered by the per-click push in OnImGui.
		context.services.Register<UndoStack>(m_undoStack);
		m_projectActions.openLauncher = [this]()
		{
			m_launcherOpen = true;
		};
		m_projectActions.reloadProject = [this]()
		{
			if (!m_currentProject.root.empty())
			{
				OpenProject(m_currentProject.root);
			}
		};
		m_projectActions.packProject = [](const EditorProjectContext& project)
		{
			return PackProject(project, MakeDefaultEditorProjectPublishConfig());
		};
		m_projectActions.publishProject = [](const EditorProjectContext& project, const EditorProjectPublishOptions& options)
		{
			return PublishProject(project, MakeDefaultEditorProjectPublishConfig(), options);
		};
		context.services.Register<EditorProjectActions>(m_projectActions);
		context.services.Register<EditorProjectContext>(m_currentProject);

		m_panels.push_back(std::make_unique<RenderGraphPanel>());
		m_panels.push_back(std::make_unique<TextureInspectorPanel>());
		auto hierarchyPanel = std::make_unique<HierarchyPanel>();
		m_hierarchyPanel = hierarchyPanel.get();
		m_panels.push_back(std::move(hierarchyPanel));
		m_panels.push_back(std::make_unique<ProjectPanel>());
		m_panels.push_back(std::make_unique<FileExplorerPanel>());
		m_panels.push_back(std::make_unique<InspectorPanel>());
		m_panels.push_back(std::make_unique<UiCanvasPanel>());
		m_panels.push_back(std::make_unique<PerformancePanel>());
		m_panels.push_back(std::make_unique<ViewportPanel>());
		m_panels.push_back(std::make_unique<TonemapPanel>());
		m_panels.push_back(std::make_unique<PostProcessingPanel>());
		m_panels.push_back(std::make_unique<SettingsPanel>());
		m_panels.push_back(std::make_unique<DevToolsPanel>());
		m_panels.push_back(std::make_unique<ConsolePanel>());
		m_panels.push_back(std::make_unique<LightingPanel>());
		m_panels.push_back(std::make_unique<DayNightPanel>());
		for (auto& panel: m_panels)
		{
			panel->OnAttach(context);
		}

		// Load persisted named layout presets (Window > Layouts / command palette).
		ReloadLayoutPresets();
		for (auto& panel: m_panels)
		{
			panel->LoadSettings(m_debugConfig, context);
			panel->SetVisible(m_debugConfig.GetBool(PanelVisibilityKey(panel->GetName()), panel->DefaultVisible()));
		}
	}

	void DebugLayer::OnDetach(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		PersistSettings(context);
		for (auto& panel: m_panels)
		{
			panel->OnDetach(context);
		}
		m_panels.clear();
		m_hierarchyPanel = nullptr;
		context.services.Unregister<EditorProjectContext>();
		context.services.Unregister<EditorProjectActions>();
		context.services.Unregister<UndoStack>();
		context.services.Unregister<SceneSelection>();
		m_services = nullptr;
		m_projectActions = {};

		m_errorToasts.clear();
		m_dockspaceBuilt = false;
	}

	void DebugLayer::OnUpdate(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		const Input& input = context.Get<Input>();

		if (input.IsKeyPressed(aether::Key::F5))
		{
			if (auto scripting = context.TryGet<scripting::CSharpScriptingSubsystem>())
			{
				scripting->RequestReload();
			}
		}

		PollScriptErrors(context);

		if (!m_projectLoaded)
		{
			return;
		}

		// Entities can be destroyed by scripts/physics at any point; keep the
		// shared selection free of dangling ids before panels read it.
		m_selection.Prune(context.Get<World>());

		// Selection outlines: world-space wireframe boxes through the debug-line
		// pass (same submission path as the light gizmos).
		if (IsDebugRenderingEnabled() && !m_selection.All().empty())
		{
			if (auto* engine = context.TryGet<AetherCore>())
			{
				if (m_selection.ChangeSerial() != m_outlineSeenSerial)
				{
					m_outlineSeenSerial = m_selection.ChangeSerial();
					m_outlinePulseStart = context.elapsedTimeSeconds;
				}
				const float pulseT = m_outlinePulseStart >= 0.0 ? std::clamp(static_cast<float>((context.elapsedTimeSeconds - m_outlinePulseStart) / 0.5), 0.0f, 1.0f) : 1.0f;
				const float brightness = 1.6f - 0.6f * pulseT; // eases back to 1.0

				auto& verts = engine->GetPendingDebugVertices();
				World& world = context.Get<World>();
				const Entity primary = m_selection.Primary();
				for (const Entity e: m_selection.All())
				{
					const auto* tc = world.TryGet<TransformComponent>(e);
					if (tc == nullptr)
					{
						continue;
					}
					// Mesh bounds when present, else a small marker box so empty
					// entities are still visibly selected.
					glm::vec3 mn{-0.125f};
					glm::vec3 mx{0.125f};
					if (const auto* mc = world.TryGet<MeshComponent>(e); mc != nullptr && mc->mesh != nullptr && mc->mesh->GetAABBMin() != mc->mesh->GetAABBMax())
					{
						mn = mc->mesh->GetAABBMin();
						mx = mc->mesh->GetAABBMax();
					}
					const float alpha = (e == primary) ? 1.0f : 0.45f;
					const glm::vec4 gold{1.0f * brightness, 0.72f * brightness, 0.2f * brightness, alpha};
					AppendObbEdges(verts, tc->localToWorld, mn, mx, gold);
				}
			}
		}

		for (auto& panel: m_panels)
		{
			panel->OnUpdate(context);
		}
	}

	void DebugLayer::OnRenderTargetsInvalidated(LayerContext& context)
	{
		for (auto& panel: m_panels)
		{
			panel->OnRenderTargetsInvalidated(context);
		}
	}

	void DebugLayer::DrawStatusBar(LayerContext& context)
	{
		// A child that fills the row reserved below the DockSpace. Drawn inside the
		// (NoBackground) host window, so it gets its own menu-bar-coloured background.
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
		if (ImGui::BeginChild("##StatusBar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar))
		{
			ImGui::AlignTextToFramePadding();

			// Left: current scene + play state.
			if (HasCurrentProject())
			{
				ImGui::Text("  " ICON_FA_FOLDER_OPEN "  %s", m_currentProject.name.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
			}
			const char* sceneName = "-";
			if (const auto* scenes = context.TryGet<SceneSubsystem>(); scenes != nullptr && !scenes->GetCurrentScene().empty())
			{
				sceneName = scenes->GetCurrentScene().c_str();
			}
			ImGui::Text(ICON_FA_CUBE "  %s", sceneName);
			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
			const auto* playState = context.TryGet<PlayState>();
			const bool playing = playState != nullptr && playState->IsPlaying();
			ImGui::TextUnformatted(playing ? ICON_FA_PLAY "  Playing" : ICON_FA_STOP "  Editing");

			// Right (aligned): resolution, FPS, frame time.
			const ImGuiIO& io = ImGui::GetIO();
			gpu::Extent2D extent{};
			if (const auto* swapchain = context.TryGet<Swapchain>())
			{
				extent = swapchain->GetExtent();
			}
			const float frameMs = io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f;
			const std::string right = std::format(ICON_FA_GAUGE_HIGH "  {}x{}    {:.0f} FPS    {:.2f} ms  ", extent.width, extent.height, io.Framerate, frameMs);
			const float rightWidth = ImGui::CalcTextSize(right.c_str()).x;
			const float targetX = ImGui::GetWindowWidth() - rightWidth;
			if (targetX > ImGui::GetCursorPosX())
			{
				ImGui::SameLine(targetX);
			}
			ImGui::TextUnformatted(right.c_str());
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	void DebugLayer::DrawCommandPalette(LayerContext& context)
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_P, false))
		{
			m_paletteQuery[0] = '\0';
			m_paletteSelected = 0;
			ImGui::OpenPopup("##CommandPalette");
		}

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.16f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, vp->WorkSize.x - 40.0f), 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopup("##CommandPalette", ImGuiWindowFlags_NoMove))
		{
			return;
		}

		// Build the action list fresh each frame the palette is open (a dozen-ish entries).
		struct Action
		{
			std::string label;
			std::function<void()> run;
		};

		std::vector<Action> actions;
		for (auto& panel: m_panels)
		{
			DebugPanel* p = panel.get();
			actions.push_back({std::string("View: ") + std::string(p->GetName()), [p]() { *p->VisiblePtr() = !*p->VisiblePtr(); }});
		}
		if (auto* playState = context.TryGet<PlayState>())
		{
			actions.push_back({"Play: Toggle Play / Stop", [&context]() { TogglePlaySession(context); }});
		}
		actions.push_back({"Layout: Reset to Default", [this]() { m_resetLayout = true; }});
		for (const auto& preset: m_layoutPresets)
		{
			actions.push_back({std::string("Layout: ") + preset.name, [this, preset]() { ApplyLayoutPreset(preset); }});
		}

		if (ImGui::IsWindowAppearing())
		{
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		const bool submitted = ImGui::InputTextWithHint("##palettequery", "Type a command...", m_paletteQuery, sizeof(m_paletteQuery), ImGuiInputTextFlags_EnterReturnsTrue);

		struct Ranked
		{
			int score;
			std::size_t index;
		};

		std::vector<Ranked> ranked;
		for (std::size_t i = 0; i < actions.size(); ++i)
		{
			if (const auto s = FuzzyMatch(m_paletteQuery, actions[i].label))
			{
				ranked.push_back({*s, i});
			}
		}
		std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });

		const int count = static_cast<int>(ranked.size());
		if (count > 0)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
			{
				m_paletteSelected = (m_paletteSelected + 1) % count;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
			{
				m_paletteSelected = (m_paletteSelected + count - 1) % count;
			}
			m_paletteSelected = std::clamp(m_paletteSelected, 0, count - 1);
		}
		else
		{
			m_paletteSelected = 0;
		}

		ImGui::Separator();
		// One opaque highlight for the keyboard-selected row so the "selected"
		// (Header) tint and the "hovered" (HeaderHovered) tint don't stack into a
		// double band on the row under the cursor.
		ImVec4 paletteSel = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
		paletteSel.w = 1.0f;
		ImGui::PushStyleColor(ImGuiCol_Header, paletteSel);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, paletteSel);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, paletteSel);
		int runIndex = -1;
		for (int i = 0; i < count && i < 12; ++i)
		{
			const Action& action = actions[ranked[static_cast<std::size_t>(i)].index];
			if (ImGui::Selectable(action.label.c_str(), i == m_paletteSelected))
			{
				runIndex = static_cast<int>(ranked[static_cast<std::size_t>(i)].index);
			}
		}
		ImGui::PopStyleColor(3);
		if (submitted && count > 0)
		{
			runIndex = static_cast<int>(ranked[static_cast<std::size_t>(m_paletteSelected)].index);
		}

		if (runIndex >= 0)
		{
			actions[static_cast<std::size_t>(runIndex)].run();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void DebugLayer::ReloadLayoutPresets()
	{
		m_layoutPresets = LayoutPresetStore::LoadAll();
	}

	void DebugLayer::ApplyLayoutPreset(const LayoutPreset& preset)
	{
		m_pendingLayoutIni = preset.imguiIni;
		m_pendingLayoutVisibility = preset.visibility;
		m_pendingLayoutApply = true;
	}

	void DebugLayer::CaptureCurrentLayout(std::string name)
	{
		LayoutPreset preset;
		preset.name = std::move(name);
		std::size_t iniSize = 0;
		if (const char* ini = ImGui::SaveIniSettingsToMemory(&iniSize))
		{
			preset.imguiIni.assign(ini, iniSize);
		}
		preset.visibility.reserve(m_panels.size());
		for (const auto& panel: m_panels)
		{
			preset.visibility.emplace_back(std::string(panel->GetName()), panel->IsVisible());
		}
		if (LayoutPresetStore::Save(preset))
		{
			ReloadLayoutPresets();
		}
	}

	void DebugLayer::DeleteLayoutPreset(std::string_view name)
	{
		LayoutPresetStore::Remove(name);
		ReloadLayoutPresets();
	}

	DebugPanel* DebugLayer::FindPanelByName(std::string_view name) const
	{
		for (const auto& panel: m_panels)
		{
			if (panel->GetName() == name)
			{
				return panel.get();
			}
		}
		return nullptr;
	}

	void DebugLayer::SaveCurrentScene(LayerContext& context)
	{
		auto* scenes = context.TryGet<SceneSubsystem>();
		const std::string currentName = scenes != nullptr ? scenes->GetCurrentScene() : std::string{};

		bool saved = false;
		if (!currentName.empty())
		{
			if (auto* assets = context.TryGet<AssetManager>())
			{
				saved = scene::QuickSave(context.Get<World>(), currentName, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), context.TryGet<Renderer>());
			}
		}

		if (!saved && m_hierarchyPanel != nullptr)
		{
			// No scene name yet (or the quick-save failed) - fall back to the
			// named Save-As prompt instead of silently doing nothing.
			m_hierarchyPanel->RequestSaveAsPopup();
		}
	}

	void DebugLayer::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		// Once per ImGui frame, before any panel might call Manipulate.
		ImGuizmo::BeginFrame();

		// A layout preset queued last frame is applied here, before any window
		// Begin(), so ImGui reloads dock/window settings for this frame's panels.
		if (m_pendingLayoutApply)
		{
			ImGui::LoadIniSettingsFromMemory(m_pendingLayoutIni.c_str(), m_pendingLayoutIni.size());
			for (const auto& [name, visible]: m_pendingLayoutVisibility)
			{
				if (DebugPanel* panel = FindPanelByName(name))
				{
					panel->SetVisible(visible);
				}
			}
			m_pendingLayoutApply = false;
		}

		if (!m_projectLoaded || m_launcherOpen)
		{
			DrawProjectLauncher(context);
			PersistSettings(context);
			return;
		}

		// Fill the main viewport's backbuffer with an opaque editor background behind
		// every window. The passthrough dockspace otherwise exposes the swapchain,
		// which shows stale pixels where the central node is empty (e.g. all panels
		// torn out to other monitors). Every window - including the Viewport panel's
		// scene image - draws on top, so the normal docked view is unchanged. Gated to
		// frame 1+ like the menu/status bars: submitting ImGui geometry on frame 0,
		// before the first real UI frame, faults in this threaded frame-0 setup.
		if (m_dockspaceBuilt)
		{
			ImGuiViewport* mainViewport = ImGui::GetMainViewport();
			const ImU32 editorBg = ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32(0, 0, 0, 255);
			ImGui::GetBackgroundDrawList(mainViewport)->AddRectFilled(mainViewport->Pos, ImVec2(mainViewport->Pos.x + mainViewport->Size.x, mainViewport->Pos.y + mainViewport->Size.y), editorBg);
		}

		// ── Editor undo (edit mode only) ──────────────────────────────────────
		// Every LMB press records a pre-gesture snapshot (deduped against the
		// stack top), so a whole gizmo drag, slider drag or destructive click
		// coalesces into ONE undo step - no per-widget instrumentation.
		if (const auto* playState = context.TryGet<PlayState>(); playState != nullptr && !playState->IsPlaying())
		{
			const ImGuiIO& io = ImGui::GetIO();
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				m_undoStack.Push(context.Get<World>(), context.services);
			}
			if (io.KeyCtrl && !io.WantTextInput)
			{
				const bool zKey = ImGui::IsKeyPressed(ImGuiKey_Z, false);
				const bool redoCombo = ImGui::IsKeyPressed(ImGuiKey_Y, false) || (zKey && io.KeyShift);
				const bool undoCombo = zKey && !io.KeyShift;
				if (undoCombo || redoCombo)
				{
					const bool did = redoCombo ? m_undoStack.Redo(context.Get<World>(), context.services) : m_undoStack.Undo(context.Get<World>(), context.services);
					if (did)
					{
						// Restored entities have fresh ids.
						m_selection.Clear();
					}
				}
			}
		}

		// ── Quick save (Ctrl+S) ─────────────────────────────────────────────────
		// Mirrors File > Save; available in both Editing and Playing mode (same
		// as the Scene Outliner's Save button). Suppressed while a text field is
		// focused so typing 's' into a name box can't trigger a save.
		{
			const ImGuiIO& io = ImGui::GetIO();
			if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false))
			{
				SaveCurrentScene(context);
			}
		}

		if (!m_errorToasts.empty())
		{
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16.0f, viewport->WorkPos.y + viewport->WorkSize.y - 88.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - 32.0f, 68.0f), ImGuiCond_Always);
			ImGui::Begin("Script Errors", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
			const auto& toast = m_errorToasts.front();
			const auto& scriptErr = colors::Error;
			ImGui::TextColored(ImVec4(scriptErr.r, scriptErr.g, scriptErr.b, scriptErr.a), "Script Error%s", m_errorToasts.size() > 1 ? "s" : "");
			ImGui::SameLine();
			ImGui::TextUnformatted(toast.summary.c_str());
			if (!toast.filePath.empty())
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Open"))
				{
					OpenInVSCode(toast.filePath, toast.line);
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Dismiss All"))
			{
				m_errorToasts.clear();
			}
			ImGui::End();
		}

		// Root dockspace: invisible full-screen window for docking
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);
		// The menu bar is withheld on the very first frame: it shrinks the docked
		// Viewport by one row, and resizing the scene render target before the first
		// scene render has established it crashes the renderer. Letting frame 0 lay
		// out at full size, then adding the bar on frame 1, makes it an ordinary
		// (already-handled) resize.
		const bool showMenuBar = m_dockspaceBuilt;
		ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
		                             | ImGuiWindowFlags_NoBackground | (showMenuBar ? ImGuiWindowFlags_MenuBar : 0);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("DebugDockSpace", nullptr, hostFlags);
		ImGui::PopStyleVar(3);

		// Menu bar lives INSIDE the dockspace host window (the canonical Dear ImGui
		// dockspace pattern). A separate BeginMainMenuBar() shrinks the viewport
		// work-area, which collided with this full-viewport host and crashed the
		// renderer on the first frame.
		if (showMenuBar && ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Project Launcher..."))
				{
					m_launcherOpen = true;
				}
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_FA_PLUS "  New Scene"))
				{
					// A non-empty return is purely NewScene's success signal (it's the
					// template's cosmetic display name). A freshly created scene has no
					// file yet, so track it as UNNAMED - empty is the "unsaved" sentinel
					// SaveCurrentScene() checks to route Save/Ctrl+S to the Save-As prompt.
					const std::string name = scene::NewScene(context.Get<World>(), scene::MakeApplySceneDeps(context.services));
					if (!name.empty())
					{
						if (auto* scenes = context.TryGet<SceneSubsystem>())
						{
							scenes->SetCurrentScene("");
						}
						m_selection.Clear();
					}
				}
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open..."))
				{
					if (m_hierarchyPanel != nullptr)
					{
						m_hierarchyPanel->RequestOpenPopup();
					}
				}
				if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save", "Ctrl+S"))
				{
					SaveCurrentScene(context);
				}
				if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save As..."))
				{
					if (m_hierarchyPanel != nullptr)
					{
						m_hierarchyPanel->RequestSaveAsPopup();
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Window"))
			{
				// Renders one window's toggle (icon + name + checkmark) by resolving
				// its panel from m_panels.
				auto windowToggle = [this](std::string_view name)
				{
					for (auto& panel: m_panels)
					{
						if (panel->GetName() == name)
						{
							const std::string label = std::string(WindowMenuIcon(name)) + "  " + std::string(name);
							ImGui::MenuItem(label.c_str(), nullptr, panel->VisiblePtr());
							return;
						}
					}
				};

				struct MenuGroup
				{
					const char* icon;
					const char* label;
					std::vector<std::string_view> windows;
				};

				static const std::vector<MenuGroup> kGroups = {
				        {ICON_FA_CUBE, "Scene", {"Scene Outliner", "Project", "File Explorer", "Inspector", "Viewport", "UI Canvas"}},
				        {ICON_FA_PALETTE, "Rendering", {"Render Graph", "Post Processing", "Tonemap", "Lighting", "Day / Night", "TextureInspector"}},
				        {ICON_FA_GAUGE_HIGH, "Diagnostics", {"Performance", "Console", "DevTools"}},
				        {ICON_FA_GEARS, "Engine", {"Settings"}},
				};

				std::unordered_set<std::string_view> grouped;
				for (const auto& group: kGroups)
				{
					for (const auto& name: group.windows)
					{
						grouped.insert(name);
					}
				}

				for (const auto& group: kGroups)
				{
					const std::string groupLabel = std::string(group.icon) + "  " + group.label;
					if (ImGui::BeginMenu(groupLabel.c_str()))
					{
						for (const auto& name: group.windows)
						{
							windowToggle(name);
						}
						ImGui::EndMenu();
					}
				}

				// Safety net: any panel not assigned to a group still gets a toggle so
				// no window can become unreachable.
				const bool hasUngrouped = std::ranges::any_of(m_panels, [&](const auto& panel) { return !grouped.contains(panel->GetName()); });
				if (hasUngrouped && ImGui::BeginMenu(ICON_FA_CIRCLE "  Other"))
				{
					for (auto& panel: m_panels)
					{
						if (!grouped.contains(panel->GetName()))
						{
							windowToggle(panel->GetName());
						}
					}
					ImGui::EndMenu();
				}

				ImGui::Separator();
				if (ImGui::MenuItem("Show All Windows"))
				{
					for (auto& panel: m_panels)
					{
						panel->SetVisible(true);
					}
				}
				if (ImGui::MenuItem("Hide All Windows"))
				{
					for (auto& panel: m_panels)
					{
						panel->SetVisible(false);
					}
				}
				ImGui::Separator();
				if (ImGui::BeginMenu("Layouts"))
				{
					if (ImGui::MenuItem("Save Current As..."))
					{
						m_openSavePresetPopup = true;
					}
					ImGui::Separator();
					if (m_layoutPresets.empty())
					{
						ImGui::TextDisabled("(no saved layouts)");
					}
					for (const auto& preset: m_layoutPresets)
					{
						if (ImGui::MenuItem(preset.name.c_str()))
						{
							ApplyLayoutPreset(preset);
						}
					}
					if (!m_layoutPresets.empty())
					{
						ImGui::Separator();
						if (ImGui::BeginMenu("Delete"))
						{
							for (const auto& preset: m_layoutPresets)
							{
								if (ImGui::MenuItem(preset.name.c_str()))
								{
									DeleteLayoutPreset(preset.name);
								}
							}
							ImGui::EndMenu();
						}
					}
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Reset Layout"))
				{
					m_resetLayout = true;
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("View"))
			{
				bool debugRendering = IsDebugRenderingEnabled();
				if (ImGui::MenuItem("Debug Rendering", nullptr, &debugRendering))
				{
					SetDebugRenderingEnabled(debugRendering);
				}
				ImGui::EndMenu();
			}
			ImGui::EndMenuBar();
		}

		if (m_openSavePresetPopup)
		{
			ImGui::OpenPopup("Save Layout##popup");
			m_newPresetName[0] = '\0';
			m_openSavePresetPopup = false;
		}
		if (ImGui::BeginPopupModal("Save Layout##popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Preset name");
			if (ImGui::IsWindowAppearing())
			{
				ImGui::SetKeyboardFocusHere();
			}
			const bool entered = ImGui::InputText("##presetname", m_newPresetName, sizeof(m_newPresetName), ImGuiInputTextFlags_EnterReturnsTrue);
			const bool hasName = m_newPresetName[0] != '\0';
			ImGui::BeginDisabled(!hasName);
			if ((ImGui::Button("Save") || entered) && hasName)
			{
				CaptureCurrentLayout(m_newPresetName);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// V6: project file explorer sits under the outliner, with Inspector right
		// and Viewport/UI Canvas center. The id bump retires layouts where File
		// Explorer was absent or hidden by the old hierarchy asset browser.
		ImGuiID dockspace_id = ImGui::GetID("AetherDebugDockSpaceV6");
		const bool hasSavedDockspace = ImGui::DockBuilderGetNode(dockspace_id) != nullptr;
		// Reserve a row at the bottom of the dockspace for the status bar (frame 1+;
		// withheld on frame 0 for the same reason as the menu bar). Keeping it inside
		// the host window - rather than a separate BeginViewportSideBar, which
		// reserved viewport work-area and black-screened the render - matches the
		// menu bar's working approach.
		const bool showStatusBar = m_dockspaceBuilt;
		const float statusBarHeight = showStatusBar ? ImGui::GetFrameHeight() : 0.0f;
		// Zero the vertical item spacing between the DockSpace and the status bar so
		// the bar sits flush against the windows above it. Otherwise an ItemSpacing.y
		// strip is left uncovered and, with no swapchain clear, flashes stale
		// swapchain contents through the NoBackground host window.
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
		ImGui::DockSpace(dockspace_id, ImVec2(0.0f, -statusBarHeight), ImGuiDockNodeFlags_PassthruCentralNode);

		if (m_resetLayout || (!m_dockspaceBuilt && !hasSavedDockspace))
		{
			ImGui::DockBuilderRemoveNode(dockspace_id);
			ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

			ImGuiID remaining = dockspace_id;
			ImGuiID dock_left = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Left, 0.20f, nullptr, &remaining);
			ImGuiID dock_left_files = ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.42f, nullptr, &dock_left);
			ImGuiID dock_right = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Right, 0.27f, nullptr, &remaining);
			ImGuiID dock_right_tools = ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.38f, nullptr, &dock_right);
			ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Down, 0.28f, nullptr, &remaining);

			ImGui::DockBuilderDockWindow("Scene", dock_left);
			ImGui::DockBuilderDockWindow("Project", dock_left_files);
			ImGui::DockBuilderDockWindow("File Explorer", dock_left_files);
			ImGui::DockBuilderDockWindow("Viewport", remaining);
			ImGui::DockBuilderDockWindow("UI Canvas", remaining);
			ImGui::DockBuilderDockWindow("Inspector", dock_right);
			ImGui::DockBuilderDockWindow("Render Graph", dock_right_tools);
			ImGui::DockBuilderDockWindow("Debug", dock_right_tools);
			ImGui::DockBuilderDockWindow("Tonemap", dock_right_tools);
			ImGui::DockBuilderDockWindow("Post Processing", dock_right_tools);
			ImGui::DockBuilderDockWindow("Settings", dock_right_tools);
			ImGui::DockBuilderDockWindow("Performance", dock_bottom);
			ImGui::DockBuilderDockWindow("Console", dock_bottom);
			ImGui::DockBuilderDockWindow("Lighting", dock_bottom);
			ImGui::DockBuilderDockWindow("Day / Night", dock_bottom);
			ImGui::DockBuilderDockWindow("Textures", dock_bottom);

			ImGui::DockBuilderFinish(dockspace_id);
		}
		m_dockspaceBuilt = true;
		m_resetLayout = false;

		if (showStatusBar)
		{
			DrawStatusBar(context);
		}
		ImGui::PopStyleVar(); // ItemSpacing

		ImGui::End();

		// Every panel manages its own window (including Render Graph now); draw only
		// the ones the user has left visible.
		for (auto& panel: m_panels)
		{
			if (panel->IsVisible())
			{
				panel->OnImGui(context);
			}
		}

		DrawCommandPalette(context);

		PersistSettings(context);
	}
} // namespace aether::app
