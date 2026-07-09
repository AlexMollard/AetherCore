#include "debug/EditorProjectManager.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <optional>
#include <string_view>

#ifdef _WIN32
#	include <Windows.h>
#	include <shobjidl.h>
#	undef CopyFile // Windows.h defines CopyFile as CopyFileA/CopyFileW macro, conflicts with file_util::CopyFile
#endif

#include "editor/EditorProjectPublisher.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"
#include "utils/SettingsService.hpp"
#include "utils/TextIni.hpp"

using namespace std::string_view_literals;

namespace aether::app
{
	namespace
	{
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
			return io::file_util::Exists(DescriptorPath(root)) || io::file_util::Exists(LegacyDescriptorPath(root));
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
	}

	void EditorProjectManager::Attach(ServiceContainer& services)
	{
		m_services = &services;
		ConfigureActions();
		services.Register<EditorProjectActions>(m_actions);
		services.Register<EditorProjectContext>(m_currentProject);
	}

	void EditorProjectManager::Detach()
	{
		m_services = nullptr;
		m_actions = {};
	}

	void EditorProjectManager::ConfigureActions()
	{
		m_actions.openLauncher = [this]()
		{
			OpenLauncher();
		};
		m_actions.reloadProject = [this]()
		{
			if (!m_currentProject.root.empty())
			{
				OpenProject(m_currentProject.root);
			}
		};
		m_actions.packProject = [](const EditorProjectContext& project)
		{
			return PackProject(project, MakeDefaultEditorProjectPublishConfig());
		};
		m_actions.publishProject = [](const EditorProjectContext& project, const EditorProjectPublishOptions& options)
		{
			return PublishProject(project, MakeDefaultEditorProjectPublishConfig(), options);
		};
	}

	void EditorProjectManager::LoadSettings(TomlConfig& config)
	{
		m_launcherState.openLastProject = config.GetBool("launcher.open_last", false);
		m_currentProject.root = NormalizePath(config.GetString("launcher.current.path"));
		m_currentProject.name = config.GetString("launcher.current.name");
		if (!m_currentProject.root.empty() && m_currentProject.name.empty())
		{
			m_currentProject.name = ReadProjectName(m_currentProject.root);
		}

		m_recentProjects.clear();
		for (int i = 0; i < kMaxRecentProjects; ++i)
		{
			const std::string key = std::format("launcher.recent_{}", i);
			EditorProjectContext project;
			project.root = NormalizePath(config.GetString(key + ".path"));
			project.name = config.GetString(key + ".name");
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
		std::snprintf(m_launcherState.openPath.data(), m_launcherState.openPath.size(), "%s", DisplayPath(cwd).c_str());
		std::snprintf(m_launcherState.newPath.data(), m_launcherState.newPath.size(), "%s", DisplayPath(cwd / "AetherProject").c_str());
		std::snprintf(m_launcherState.newName.data(), m_launcherState.newName.size(), "%s", "AetherProject");

		if (m_launcherState.openLastProject && HasCurrentProject())
		{
			OpenProject(m_currentProject.root);
		}
	}

	void EditorProjectManager::SaveSettings(TomlConfig& config)
	{
		config.Set("launcher.open_last", m_launcherState.openLastProject);
		config.Set("launcher.current.path", DisplayPath(m_currentProject.root));
		config.Set("launcher.current.name", m_currentProject.name);
		for (int i = 0; i < kMaxRecentProjects; ++i)
		{
			const std::string key = std::format("launcher.recent_{}", i);
			if (i < static_cast<int>(m_recentProjects.size()))
			{
				config.Set(key + ".path", DisplayPath(m_recentProjects[static_cast<std::size_t>(i)].root));
				config.Set(key + ".name", m_recentProjects[static_cast<std::size_t>(i)].name);
			}
			else
			{
				config.Set(key + ".path", std::string_view{});
				config.Set(key + ".name", std::string_view{});
			}
		}
	}

	void EditorProjectManager::AddRecentProject(std::filesystem::path root, std::string name)
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

	bool EditorProjectManager::HasCurrentProject() const
	{
		return !m_currentProject.root.empty() && HasProjectDescriptor(m_currentProject.root);
	}

	void EditorProjectManager::OpenProject(std::filesystem::path root)
	{
		m_launcherState.error.clear();
		root = ResolveProjectRoot(std::move(root));
		if (root.empty())
		{
			m_launcherState.error = "Choose a project folder.";
			return;
		}
		auto projectResult = ReadProjectDescriptor(root);
		if (!projectResult)
		{
			m_launcherState.error = "No .project/aether.project found in that folder.";
			return;
		}
		m_currentProject = *std::move(projectResult);
		m_currentProject.loaded = true;
		io::FileSystem::Mount("project", m_currentProject.root);
		scene::SetProjectSceneDirectories(m_currentProject.scenesDir, m_currentProject.prefabsDir);
		RefreshServices();
		AddRecentProject(m_currentProject.root, m_currentProject.name);
		m_projectLoaded = true;
		m_launcherOpen = false;
		AE_INFO(LogCategory::App, "Opened editor project '{}' at {}", m_currentProject.name, DisplayPath(m_currentProject.root));
	}

	void EditorProjectManager::RefreshServices()
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

	void EditorProjectManager::CreateProject(std::filesystem::path root, std::string_view name)
	{
		m_launcherState.error.clear();
		root = ResolveProjectRoot(std::move(root));
		std::string projectName(name);
		projectName = text::TrimAscii(std::move(projectName));
		if (projectName.empty())
		{
			projectName = FallbackProjectName(root);
		}
		if (root.empty())
		{
			m_launcherState.error = "Choose a project folder.";
			return;
		}
		if (!WriteProjectDescriptor(root, projectName, m_launcherState.error))
		{
			return;
		}
		OpenProject(root);
	}

	void EditorProjectManager::DrawLauncher()
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
			CloseLauncher();
		};
		actions.saveSettings = []() {};

		m_launcher.Draw(m_launcherState, model, actions);
	}

	void EditorProjectManager::OpenLauncher()
	{
		m_launcherOpen = true;
	}

	void EditorProjectManager::CloseLauncher()
	{
		m_launcherOpen = false;
	}

	bool EditorProjectManager::IsProjectLoaded() const noexcept
	{
		return m_projectLoaded;
	}

	bool EditorProjectManager::IsLauncherOpen() const noexcept
	{
		return m_launcherOpen;
	}

	const EditorProjectContext& EditorProjectManager::CurrentProject() const noexcept
	{
		return m_currentProject;
	}

	EditorProjectContext& EditorProjectManager::CurrentProject() noexcept
	{
		return m_currentProject;
	}
} // namespace aether::app
