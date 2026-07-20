#include "debug/EditorProjectManager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string_view>

#ifdef _WIN32
#	include <Windows.h>
#	include <shobjidl.h>
#	undef CopyFile
#endif

#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "FontProcessor.hpp"
#include "editor/EditorEnginePak.hpp"
#include "editor/EditorProjectPublisher.hpp"
#include "editor/ModelBake.hpp"
#include "editor/ShaderCompiler.hpp"
#include "editor/VisualStudioScriptDebug.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "io/DirectoryBackend.hpp"
#include "io/PlatformPaths.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "io/OverlayBackend.hpp"
#include "project/ProjectCommon.hpp"
#include "rendering/ScreenshotService.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"
#include "utils/SettingsService.hpp"
#include "utils/TextIni.hpp"

using namespace std::string_view_literals;

namespace aether::editor
{
	using namespace aether::app::project;

	namespace
	{
		constexpr std::string_view kEditorLogoPath = "engine://branding/aethercore-icon-white.png";

		// manual "Recompile Shaders" action - so the overlay never serves a
		void UpdateProjectShaderOverlay(const std::filesystem::path& projectRoot)
		{
			if (projectRoot.empty())
			{
				io::FileSystem::MountShaderOverlay(std::nullopt);
				return;
			}

			const std::filesystem::path shaderDir = ProjectShaderIntermediateDir(projectRoot);
			std::error_code ec;
			if (std::filesystem::is_directory(shaderDir, ec))
			{
				io::FileSystem::MountShaderOverlay(io::OverlayBackend::Layer{std::make_shared<io::DirectoryBackend>(shaderDir), ""});
			}
			else
			{
				io::FileSystem::MountShaderOverlay(std::nullopt);
			}

			if (const auto shaderGlob = io::FileSystem::Glob("shaders://**/*.spv"); shaderGlob.has_value())
			{
				AE_INFO(LogCategory::App, "shaders:// overlay resolves {} shader(s) after project shader compile.", shaderGlob->size());
			}
		}

		ShaderCompileResult CompileProjectShadersAndRefreshOverlay(const std::filesystem::path& projectRoot)
		{
			const ShaderCompileResult result = CompileProject(projectRoot);
			if (!result.ok)
			{
				AE_WARN(LogCategory::App, "Project shader compile had failures: {}", result.message);
			}
			else if (!result.message.empty())
			{
				AE_INFO(LogCategory::App, "Project shader compile: {}", result.message);
			}
			UpdateProjectShaderOverlay(projectRoot);
			return result;
		}

	} // namespace

	void EditorProjectManager::Attach(ServiceContainer& services)
	{
		m_services = &services;

		if (auto* assets = services.TryGet<AssetManager>())
		{
			auto logo = assets->CreateTexture(kEditorLogoPath);
			if (!logo)
			{
				AE_WARN(LogCategory::App, "Could not load editor logo '{}': {}", kEditorLogoPath, logo.error());
			}
			else if (auto* imgui = services.TryGet<ImguiSubsystem>())
			{
				const ImTextureID textureId = imgui->RegisterTexture(logo->GetView(), gpu::ImageLayout::ShaderReadOnly);
				if (textureId != ImTextureID_Invalid)
				{
					m_logoTexture = std::move(*logo);
					m_logoTextureId = static_cast<std::uint64_t>(textureId);
				}
			}
		}

		ConfigureActions();
		services.Register<EditorProjectActions>(m_actions);
		services.Register<app::EditorProjectContext>(m_currentProject);

		m_bakeHook.ensureBaked = [this](const std::string& vfsModelPath, std::string& error) -> bool
		{
			return editor::EnsureModelBaked(vfsModelPath, m_currentProject, error);
		};
		services.Register<app::scene::ModelBakeHook>(m_bakeHook);
	}

	void EditorProjectManager::Detach()
	{
		if (m_logoTextureId != 0 && m_services != nullptr)
		{
			if (auto* imgui = m_services->TryGet<ImguiSubsystem>())
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(m_logoTextureId));
			}
		}
		m_logoTextureId = 0;
		m_logoTexture.Destroy();
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
				OpenProject(m_currentProject.root, /*reloadScene=*/false);
			}
		};
		m_actions.packProject = [](const app::EditorProjectContext& project)
		{
			return PackProject(project, MakeDefaultEditorProjectPublishConfig());
		};
		m_actions.publishProject = [](const app::EditorProjectContext& project, const EditorProjectPublishOptions& options, const EditorProjectPublishProgress& progress)
		{
			return PublishProject(project, MakeDefaultEditorProjectPublishConfig(), options, progress);
		};
		m_actions.visualStudioInstallations = FindVisualStudioInstallations();
		m_actions.debugScripts = [this](const std::filesystem::path& visualStudioInstall) -> EditorProjectActionResult
		{
			if (!m_currentProject.IsLoaded())
			{
				return {.succeeded = false, .message = "No project is open."};
			}

			const std::filesystem::path scriptsProject = m_currentProject.scriptsDir / "AetherGame.csproj";
			if (auto* scripting = m_services != nullptr ? m_services->TryGet<app::scripting::CSharpScriptingSubsystem>() : nullptr)
			{
				scripting->BeginRebuildFromSource();
			}

			EditorProjectActionResult result = OpenVisualStudioAndAttachScriptDebugger(scriptsProject, visualStudioInstall);
			if (result.succeeded)
			{
				result.message += " Debug script build is queued in the background.";
			}
			return result;
		};
		if (CanBakeEnginePak())
		{
			m_actions.rebuildEnginePak = []
			{
				const EditorProjectPublishConfig config = MakeDefaultEditorProjectPublishConfig();
				return BakeEnginePak(config.executableDir / "data" / "engine.pak");
			};
		}
		if (CanCompileShaders())
		{
			m_actions.recompileShaders = [this]() -> EditorProjectActionResult
			{
				if (m_currentProject.root.empty())
				{
					return {.succeeded = false, .message = "No project is open."};
				}
				const ShaderCompileResult result = CompileProjectShadersAndRefreshOverlay(m_currentProject.root);
				return {.succeeded = result.ok, .message = result.message, .outputPath = ProjectShaderIntermediateDir(m_currentProject.root)};
			};
		}
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
			app::EditorProjectContext project;
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
			if (std::ranges::none_of(m_recentProjects, [&](const app::EditorProjectContext& existing) { return NormalizePath(existing.root) == project.root; }))
			{
				m_recentProjects.push_back(std::move(project));
			}
		}

		const std::filesystem::path cwd = NormalizePath(std::filesystem::current_path());
		std::snprintf(m_launcherState.openPath.data(), m_launcherState.openPath.size(), "%s", DisplayPath(cwd).c_str());
		std::snprintf(m_launcherState.newPath.data(), m_launcherState.newPath.size(), "%s", DisplayPath(cwd / "AetherProject").c_str());
		std::snprintf(m_launcherState.newName.data(), m_launcherState.newName.size(), "%s", "AetherProject");

		std::filesystem::path bootProject;
		if (const std::string env = io::PlatformPaths::ReadEnvironmentVariable("AETHER_PROJECT_DIR"); !env.empty())
		{
			bootProject = NormalizePath(env);
		}
		else if (m_launcherState.openLastProject && HasCurrentProject())
		{
			bootProject = m_currentProject.root;
		}

		if (!bootProject.empty())
		{
			OpenProject(bootProject, /*reloadScene=*/false);
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
		std::erase_if(m_recentProjects, [&](const app::EditorProjectContext& p) { return NormalizePath(p.root) == root; });
		app::EditorProjectContext project;
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

	void EditorProjectManager::BuildAndReloadProjectScripts()
	{
		if (m_services == nullptr)
		{
			return;
		}
		auto* scripting = m_services->TryGet<app::scripting::CSharpScriptingSubsystem>();
		if (scripting == nullptr)
		{
			return;
		}

		const std::filesystem::path scriptsProject = m_currentProject.scriptsDir / "AetherGame.csproj";
		std::error_code ec;
		if (!std::filesystem::exists(scriptsProject, ec))
		{
			scripting->SetScriptProject({}, {});
			return;
		}

		scripting->SetScriptProject(scriptsProject, m_currentProject.root / "Builds" / "Intermediate" / "managed");
		// Kick the dotnet build onto a worker thread so opening a project never blocks
		scripting->BeginRebuildFromSource();
		m_scriptBuildPending = true;
	}

	void EditorProjectManager::UpdateScriptBuild()
	{
		if (m_previewCaptureCountdown > 0 && --m_previewCaptureCountdown == 0)
		{
			CaptureProjectPreview();
		}

		if (!m_scriptBuildPending || m_services == nullptr)
		{
			return;
		}
		auto* scripting = m_services->TryGet<app::scripting::CSharpScriptingSubsystem>();
		if (scripting == nullptr)
		{
			m_scriptBuildPending = false;
			return;
		}

		if (const auto* play = m_services->TryGet<app::PlayState>(); play != nullptr && play->IsCompiling())
		{
			m_scriptBuildPending = false;
			return;
		}

		using BuildStatus = app::scripting::CSharpScriptingSubsystem::BuildStatus;
		std::string error;
		switch (scripting->PollRebuildStatus(error))
		{
			case BuildStatus::Running:
				return;
			case BuildStatus::Succeeded:
				scripting->ClearRebuild();
				if (scripting->IsAvailable())
				{
					scripting->ClearErrors();
					scripting->LoadScripts();
				}
				m_scriptBuildPending = false;
				AE_INFO(LogCategory::App, "Project C# scripts built and loaded.");
				return;
			case BuildStatus::Failed:
				scripting->ReportScriptError("Project script build failed:\n" + error);
				scripting->ClearRebuild();
				m_scriptBuildPending = false;
				return;
			case BuildStatus::Idle:
				m_scriptBuildPending = false;
				return;
		}
	}

	void EditorProjectManager::CaptureProjectPreview()
	{
		if (m_services == nullptr || m_currentProject.root.empty())
		{
			return;
		}
		auto* shot = m_services->TryGet<ScreenshotService>();
		if (shot == nullptr || !shot->IsInitialized())
		{
			return;
		}
		const auto textures = gpu::ResourceRegistry::ListDebugTextures();
		const auto it = std::ranges::find_if(textures, [](const gpu::DebugTextureInfo& t) { return t.debugName.contains("PostProcess") && t.debugName.contains("FinalColor"); });
		if (it == textures.end())
		{
			return;
		}
		void* image = gpu::ResourceRegistry::ResolveTextureImage(it->handle);
		if (image == nullptr)
		{
			return;
		}
		const std::filesystem::path previewPath = PreviewImagePath(m_currentProject.root);
		std::error_code ec;
		std::filesystem::create_directories(previewPath.parent_path(), ec);
		// Fire and forget: the render thread fulfils the request and writes the PNG.
		(void) shot->RequestImage(image, it->extent, it->format, it->aspect, gpu::ImageLayout::ShaderReadOnly, previewPath.string());
		AE_VERBOSE(LogCategory::App, "Captured project preview -> {}", previewPath.string());
	}

	void EditorProjectManager::OpenProject(std::filesystem::path root, bool reloadScene)
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
			m_launcherState.error = "No ProjectSettings.toml found there.";
			return;
		}
		m_currentProject = *std::move(projectResult);
		m_currentProject.loaded = true;
		io::FileSystem::Mount("project", m_currentProject.root);
		CompileProjectShadersAndRefreshOverlay(m_currentProject.root);
		BuildAndReloadProjectScripts();
		BakeProjectFonts();
		app::scene::SetProjectSceneDirectories(m_currentProject.scenesDir, m_currentProject.prefabsDir);
		RefreshServices();
		AddRecentProject(m_currentProject.root, m_currentProject.name);
		m_projectLoaded = true;
		m_launcherOpen = false;
		if (reloadScene)
		{
			LoadProjectStartupScene();
		}
		std::error_code previewEc;
		if (!std::filesystem::exists(PreviewImagePath(m_currentProject.root), previewEc))
		{
			m_previewCaptureCountdown = 90;
		}
		AE_INFO(LogCategory::App, "Opened editor project '{}' at {}", m_currentProject.name, DisplayPath(m_currentProject.root));
	}

	void EditorProjectManager::BakeProjectFonts() const
	{
		const std::filesystem::path fontsDir = m_currentProject.root / "assets" / "fonts";
		std::error_code ec;
		if (!std::filesystem::exists(fontsDir, ec))
		{
			return;
		}
		for (const auto& entry: std::filesystem::directory_iterator(fontsDir, ec))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}
			std::string ext = entry.path().extension().string();
			std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (ext != ".ttf" && ext != ".otf")
			{
				continue;
			}
			const std::filesystem::path meta = entry.path().parent_path() / (entry.path().stem().string() + ".fontmeta");
			std::error_code metaEc;
			if (std::filesystem::exists(meta, metaEc) && std::filesystem::last_write_time(meta, metaEc) >= entry.last_write_time(metaEc))
			{
				continue; // baked and up to date
			}
			const auto result = aether::assetpipeline::FontProcessor::BakeFont(entry.path(), fontsDir);
			if (result.success)
			{
				AE_INFO(LogCategory::App, "Project font baked: {} ({} glyphs, {}x{} atlas)", entry.path().filename().string(), result.glyphCount, result.atlasWidth, result.atlasHeight);
			}
			else
			{
				AE_WARN(LogCategory::App, "Project font bake failed for {}: {}", entry.path().filename().string(), result.error);
			}
		}
	}

	void EditorProjectManager::LoadProjectStartupScene()
	{
		if (m_services == nullptr)
		{
			return;
		}
		auto* world = m_services->TryGet<World>();
		if (world == nullptr)
		{
			return;
		}

		std::string sceneName;
		if (const auto* settings = m_services->TryGet<aether::SettingsService>())
		{
			sceneName = settings->Get().app.startupScene;
		}

		// startup scene - so switching projects never leaves the old scene live.
		const bool loaded = app::scene::SwitchScene(sceneName, *world, app::scene::MakeApplySceneDeps(*m_services));
		if (auto* scenes = m_services->TryGet<aether::SceneSubsystem>())
		{
			scenes->SetCurrentScene(loaded ? sceneName : std::string{});
		}
	}

	void EditorProjectManager::RefreshServices()
	{
		if (m_services == nullptr)
		{
			return;
		}

		m_services->Register<app::EditorProjectContext>(m_currentProject);
		if (auto* settings = m_services->TryGet<aether::SettingsService>())
		{
			auto loaded = EngineSettingsIO::LoadLayered("EngineSettings.toml", m_currentProject.projectFile);
			settings->Values() = loaded.values;
			settings->Base() = loaded.base;
			settings->ApplyAll();
		}
	}

	void EditorProjectManager::CreateProject(std::filesystem::path root, std::string_view name, ProjectTemplate projectTemplate)
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
		if (!WriteProjectDescriptor(root, projectName, m_launcherState.error, projectTemplate))
		{
			return;
		}
		OpenProject(root);
	}

	void EditorProjectManager::DrawLauncher()
	{
		app::ProjectLauncherWindowModel model;
		model.projectLoaded = m_projectLoaded;
		model.hasCurrentProject = HasCurrentProject();
		model.logoTextureId = m_logoTextureId;
		model.currentProject = &m_currentProject;
		model.recentProjects = std::span<const app::EditorProjectContext>(m_recentProjects.data(), m_recentProjects.size());

		app::ProjectLauncherWindowActions actions;
		actions.openProject = [this](std::filesystem::path root)
		{
			OpenProject(std::move(root));
		};
		actions.createProject = [this](std::filesystem::path root, std::string_view name, ProjectTemplate projectTemplate)
		{
			CreateProject(std::move(root), name, projectTemplate);
		};
		actions.browseFolder = []() -> std::optional<std::filesystem::path>
		{
#ifdef _WIN32
			return PickProjectFolder();
#else
			return std::nullopt;
#endif
		};
		actions.browseProjectFile = []() -> std::optional<std::filesystem::path>
		{
#ifdef _WIN32
			return PickProjectFile();
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

	std::uint64_t EditorProjectManager::LogoTextureId() const noexcept
	{
		return m_logoTextureId;
	}

	const app::EditorProjectContext& EditorProjectManager::CurrentProject() const noexcept
	{
		return m_currentProject;
	}

	app::EditorProjectContext& EditorProjectManager::CurrentProject() noexcept
	{
		return m_currentProject;
	}
} // namespace aether::editor
