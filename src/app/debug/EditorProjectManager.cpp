#include "debug/EditorProjectManager.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string_view>

#ifdef _WIN32
#	include <Windows.h>
#	include <shobjidl.h>
#	undef CopyFile // Windows.h defines CopyFile as CopyFileA/CopyFileW macro, conflicts with file_util::CopyFile
#endif

#include "PlayState.hpp"
#include "assets/AssetManager.hpp"
#include "editor/EditorEnginePak.hpp"
#include "editor/EditorProjectPublisher.hpp"
#include "editor/ModelBake.hpp"
#include "editor/ShaderCompiler.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "io/DirectoryBackend.hpp"
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
	// Project descriptor/scaffold/picker/recents helpers live in the engine-agnostic
	// ProjectCommon (shared with the standalone Launcher). Pull its names into scope
	// so the unqualified calls below resolve to the shared implementations.
	using namespace aether::app::project;

	namespace
	{
		// kMaxRecentProjects comes from ProjectCommon (app::project::) via the using-directive above.
		constexpr std::string_view kEditorLogoPath = "engine://branding/aethercore-icon-white.png";

		// (Re)builds the shaders:// overlay's project layer for `projectRoot`:
		// prepends a DirectoryBackend over its compiled-shader intermediate dir
		// (ShaderCompiler::ProjectShaderIntermediateDir) when that dir exists,
		// so a project shader overrides an engine shader of the same name;
		// falls back to engine-only otherwise (project has no assets/shaders,
		// or this build has no slangc so nothing was ever compiled). Called
		// after every compile attempt - project load, project switch, and the
		// manual "Recompile Shaders" action - so the overlay never serves a
		// stale project layer.
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

			// Diagnostic: prove shaders:// actually resolves through the freshly
			// (re)mounted overlay - cheap enough to run on every project
			// load/switch/recompile, and useful for debugging a shader that fails
			// to resolve at runtime.
			if (const auto shaderGlob = io::FileSystem::Glob("shaders://**/*.spv"); shaderGlob.has_value())
			{
				AE_INFO(LogCategory::App, "shaders:// overlay resolves {} shader(s) after project shader compile.", shaderGlob->size());
			}
		}

		// Compiles `projectRoot`'s Slang shaders and refreshes the shaders://
		// overlay to match the result. Shared by OpenProject and the manual
		// "Recompile Shaders" action so both go through the same path.
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

		// Let scene load bake a not-yet-imported model against the live project.
		// Reads m_currentProject at call time, so it tracks project switches.
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
				// Reload re-reads the project descriptor/settings for the SAME
				// project; leave the working scene alone so in-editor edits aren't
				// discarded (project switching, which does swap scenes, goes through
				// the launcher's OpenProject instead).
				OpenProject(m_currentProject.root, /*reloadScene=*/false);
			}
		};
		m_actions.packProject = [](const app::EditorProjectContext& project)
		{
			return PackProject(project, MakeDefaultEditorProjectPublishConfig());
		};
		m_actions.publishProject = [](const app::EditorProjectContext& project, const EditorProjectPublishOptions& options)
		{
			return PublishProject(project, MakeDefaultEditorProjectPublishConfig(), options);
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

		// A project passed on the command line (--project, surfaced as
		// AETHER_PROJECT_DIR by main - the Launcher passes it when it spawns the
		// editor) boots straight into that project; otherwise fall back to the
		// persisted open-last-project behaviour.
		std::filesystem::path bootProject;
		if (const char* env = std::getenv("AETHER_PROJECT_DIR"); env != nullptr && *env != '\0')
		{
			bootProject = NormalizePath(env);
		}
		else if (m_launcherState.openLastProject && HasCurrentProject())
		{
			bootProject = m_currentProject.root;
		}

		if (!bootProject.empty())
		{
			// Boot-time reopen: ScriptedSceneLayer attaches after this and loads the
			// startup scene itself, so don't drive a (duplicate) scene load here.
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
			// No game scripts in this project: clear any prior project's build config
			// so switching away from a scripted project stops rebuilding it.
			scripting->SetScriptProject({}, {});
			return;
		}

		scripting->SetScriptProject(scriptsProject, m_currentProject.root / "Builds" / "Intermediate" / "managed");
		// Kick the dotnet build onto a worker thread so opening a project never blocks
		// the UI (a cold build can take tens of seconds). UpdateScriptBuild(), ticked
		// from DebugLayer, polls it to completion and reloads the assembly; the status
		// bar shows a "compiling C# scripts" progress bar until then. Scripts don't
		// tick in edit mode, so the scene can load before the build finishes.
		scripting->BeginRebuildFromSource();
		m_scriptBuildPending = true;
	}

	void EditorProjectManager::UpdateScriptBuild()
	{
		// One-shot preview capture, scheduled on opening a preview-less project so the
		// scene has a few frames to render before we grab it.
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

		// If the user pressed Play while the open-build was still running, the play
		// session adopts the in-flight build and drives it to Playing - stop tracking
		// it here so completion isn't double-handled.
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
				return; // keep the "compiling C# scripts" indicator up
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
		// The scene viewport's post-tonemap output - what the editor viewport shows.
		const auto textures = gpu::ResourceRegistry::ListDebugTextures();
		const auto it = std::ranges::find_if(textures, [](const gpu::DebugTextureInfo& t) { return t.debugName.find("PostProcess") != std::string::npos && t.debugName.find("FinalColor") != std::string::npos; });
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
		// Compile this project's Slang shaders (dev-only, no-op without slangc -
		// see ShaderCompiler::CanCompileShaders) and (re)mount shaders:// so the
		// project's compiled shaders take priority over engine shaders of the
		// same name. Runs on every OpenProject - including project switches, so
		// a switch away from a project drops its shader layer instead of
		// leaking it into the newly opened project.
		CompileProjectShadersAndRefreshOverlay(m_currentProject.root);
		// Build this project's C# game scripts and load them, so opening a project
		// makes its scripts live - the runtime replacement for the old CMake
		// build-time game-scripts staging. Runs on every OpenProject, including
		// switches, so a switch drops the previous project's scripts.
		BuildAndReloadProjectScripts();
		app::scene::SetProjectSceneDirectories(m_currentProject.scenesDir, m_currentProject.prefabsDir);
		RefreshServices();
		AddRecentProject(m_currentProject.root, m_currentProject.name);
		m_projectLoaded = true;
		m_launcherOpen = false;
		// Swap the live world over to this project's startup scene. Skipped only at
		// boot-time reopen, where the scene layer has not attached yet and performs
		// the initial load itself once it does.
		if (reloadScene)
		{
			LoadProjectStartupScene();
		}
		// Give a brand-new (preview-less) project a first thumbnail: capture once the
		// scene has had a moment to render. Existing previews refresh on save.
		std::error_code previewEc;
		if (!std::filesystem::exists(PreviewImagePath(m_currentProject.root), previewEc))
		{
			m_previewCaptureCountdown = 90;
		}
		AE_INFO(LogCategory::App, "Opened editor project '{}' at {}", m_currentProject.name, DisplayPath(m_currentProject.root));
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

		// SwitchScene tears down the previously loaded scene (ReplaceScene) before
		// applying the new one, and clears the world when the project has no
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
