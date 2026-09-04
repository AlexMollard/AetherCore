#include "launcher/LauncherLayer.hpp"

#include <climits>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <span>
#include <string>

#include "assets/AssetManager.hpp"
#include "editor/ControlMethods.hpp"
#include "editor/ControlServer.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "io/PlatformPaths.hpp"
#include "launcher/LauncherProcess.hpp"
#include "platform/Window.hpp"
#include "imgui/UiAutomationMethods.hpp"
#include "project/ProjectCommon.hpp"
#include "project/ProjectPaths.hpp"
#include "rendering/ScreenshotService.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"
#include "utils/TextIni.hpp"

namespace aether::app
{
	namespace
	{
		using nlohmann::json;

		constexpr std::string_view kEditorLogoPath = "engine://branding/aethercore-icon-white.png";

		json Obj(json properties = json::object(), const std::vector<std::string>& required = {})
		{
			json schema{{"type", "object"}, {"properties", std::move(properties)}};
			if (!required.empty())
			{
				schema["required"] = required;
			}
			return schema;
		}

		json StrProp()
		{
			return json{{"type", "string"}};
		}

		json ProjectTemplateProp()
		{
			return json{{"type", "string"}, {"enum", json::array({"blank_3d", "blank_2d"})}};
		}

		std::optional<project::ProjectTemplate> ProjectTemplateFromName(std::string_view name)
		{
			if (name == "blank_3d")
			{
				return project::ProjectTemplate::Blank3D;
			}
			if (name == "blank_2d")
			{
				return project::ProjectTemplate::Blank2D;
			}
			return std::nullopt;
		}

		// single project via --project and never reads it.
		std::filesystem::path LauncherStatePath()
		{
			return io::PlatformPaths::GetUserConfigDir() / "LauncherState.toml";
		}

		int ResolveControlBasePort()
		{
			constexpr int kDefaultPort = 8787;
			const std::string env = io::PlatformPaths::ReadEnvironmentVariable("AETHER_CONTROL_PORT");
			if (env.empty())
			{
				return kDefaultPort;
			}
			std::string lowered;
			for (const char character: env)
			{
				lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			}
			if (lowered == "off" || lowered == "none" || lowered == "disabled" || lowered == "false")
			{
				return 0;
			}
			try
			{
				const int port = std::stoi(lowered);
				return (port > 0 && port <= 65535) ? port : 0;
			}
			catch (const std::exception&)
			{
				return 0;
			}
		}

		std::vector<editor::ControlMethod> BuildLauncherControlMethods(LauncherLayer& launcher)
		{
			std::vector<editor::ControlMethod> methods;

			methods.push_back({"launcher.info",
			        "launcher_info",
			        "Live Launcher summary: recent-project count, handoff state, frame index, and fps.",
			        false,
			        Obj(),
			        [&launcher](const json&, editor::MethodContext& ctx) -> json
			        { return json{{"frame", ctx.frameIndex}, {"fps", ctx.fps}, {"recentProjectCount", launcher.RecentProjects().size()}, {"launchingEditor", launcher.IsLaunchingEditor()}}; }});

			methods.push_back({"launcher.projects",
			        "list_projects",
			        "List the Launcher's recent projects, including each project name and root path.",
			        false,
			        Obj(),
			        [&launcher](const json&, editor::MethodContext&) -> json
			        {
				        json projects = json::array();
				        for (const EditorProjectContext& project: launcher.RecentProjects())
				        {
					        projects.push_back(json{{"name", project.name}, {"root", project.root.string()}});
				        }
				        return json{{"projects", std::move(projects)}};
			        }});

			methods.push_back({"launcher.open_project",
			        "open_project",
			        "Open an existing AetherCore project and hand the control port from the Launcher to its Editor process.",
			        true,
			        Obj({{"root", StrProp()}}, {"root"}),
			        [&launcher](const json& params, editor::MethodContext&) -> json
			        {
				        const std::filesystem::path root = params.value("root", std::string{});
				        if (const std::string error = launcher.QueueOpenProjectForControl(root); !error.empty())
				        {
					        return json{{"error", error}};
				        }
				        return json{{"status", "queued"}, {"root", root.string()}};
			        }});

			methods.push_back({"launcher.create_project",
			        "create_project",
			        "Create a Blank 2D or Blank 3D project, then hand the control port from the Launcher to its Editor process.",
			        true,
			        Obj({{"root", StrProp()}, {"name", StrProp()}, {"template", ProjectTemplateProp()}}, {"root", "template"}),
			        [&launcher](const json& params, editor::MethodContext&) -> json
			        {
				        const std::filesystem::path root = params.value("root", std::string{});
				        const std::string name = params.value("name", std::string{});
				        const std::string templateName = params.value("template", std::string{});
				        const std::optional<project::ProjectTemplate> projectTemplate = ProjectTemplateFromName(templateName);
				        if (!projectTemplate.has_value())
				        {
					        return json{{"error", "template must be 'blank_3d' or 'blank_2d'"}};
				        }
				        if (const std::string error = launcher.QueueCreateProjectForControl(root, name, *projectTemplate); !error.empty())
				        {
					        return json{{"error", error}};
				        }
				        return json{{"status", "queued"}, {"root", root.string()}, {"name", name}, {"template", templateName}};
			        }});

			methods.push_back({"viewport.screenshot",
			        "screenshot",
			        "Capture the current Launcher hub frame to a .png and return its path. Pass 'path' or get a default under %LOCALAPPDATA%/AetherCore/screenshots.",
			        false,
			        Obj({{"path", StrProp()}}),
			        [](const json& params, editor::MethodContext& ctx) -> json
			        {
				        auto* screenshot = ctx.services.TryGet<ScreenshotService>();
				        if (screenshot == nullptr || !screenshot->IsInitialized())
				        {
					        return json{{"error", "no screenshot service"}};
				        }

				        std::string path = params.value("path", std::string{});
				        if (path.empty())
				        {
					        const std::filesystem::path dir = io::PlatformPaths::GetUserConfigDir() / "screenshots";
					        path = (dir / ("launcher_" + std::to_string(ctx.frameIndex) + ".png")).string();
				        }
				        std::future<std::string> result = screenshot->Request(path);
				        if (result.wait_for(std::chrono::seconds(8)) != std::future_status::ready)
				        {
					        return json{{"path", path}, {"status", "requested (still saving)"}};
				        }
				        const std::string saved = result.get();
				        return saved.empty() ? json{{"error", "capture failed"}} : json{{"path", saved}};
			        }});

			const auto recentsToJson = [&launcher]() -> json
			{
				json projects = json::array();
				for (const EditorProjectContext& p: launcher.RecentProjects())
				{
					projects.push_back(json{{"name", p.name}, {"root", p.root.string()}});
				}
				return projects;
			};

			methods.push_back({"launcher.remove_recent",
			        "remove_recent",
			        "Remove a project from the Launcher's recent list (works whether the project still exists or is missing).",
			        true,
			        Obj({{"root", StrProp()}}, {"root"}),
			        [&launcher, recentsToJson](const json& params, editor::MethodContext&) -> json
			        {
				        const std::filesystem::path root = params.value("root", std::string{});
				        const std::size_t before = launcher.RecentProjects().size();
				        launcher.RemoveRecent(root);
				        return json{{"removed", launcher.RecentProjects().size() != before}, {"projects", recentsToJson()}};
			        }});

			methods.push_back({"launcher.reveal_folder",
			        "reveal_folder",
			        "Open a recent project's folder in the OS file manager.",
			        false,
			        Obj({{"root", StrProp()}}, {"root"}),
			        [&launcher](const json& params, editor::MethodContext&) -> json
			        {
				        const std::filesystem::path root = params.value("root", std::string{});
				        launcher.RevealProjectFolder(root);
				        return json{{"status", "opened"}, {"root", root.string()}};
			        }});

			methods.push_back({"launcher.relocate_recent",
			        "relocate_recent",
			        "Re-point a moved project in the recent list to a new folder (headless: no folder picker).",
			        true,
			        Obj({{"old_root", StrProp()}, {"new_root", StrProp()}}, {"old_root", "new_root"}),
			        [&launcher, recentsToJson](const json& params, editor::MethodContext&) -> json
			        {
				        const std::filesystem::path oldRoot = params.value("old_root", std::string{});
				        const std::filesystem::path newRoot = params.value("new_root", std::string{});
				        std::string error;
				        if (!launcher.RelocateRecent(oldRoot, newRoot, error))
				        {
					        return json{{"error", error}};
				        }
				        return json{{"projects", recentsToJson()}};
			        }});

			editor::AppendUiAutomationMethods(methods);

			return methods;
		}
	} // namespace

	LauncherLayer::~LauncherLayer() = default;

	std::span<const EditorProjectContext> LauncherLayer::RecentProjects() const
	{
		return std::span<const EditorProjectContext>(m_recentProjects.data(), m_recentProjects.size());
	}

	bool LauncherLayer::IsLaunchingEditor() const noexcept
	{
		return m_pendingEditor.has_value();
	}

	std::string LauncherLayer::QueueOpenProjectForControl(const std::filesystem::path& root)
	{
		if (m_pendingControlAction.has_value() || m_pendingEditor.has_value())
		{
			return "the Launcher is already handing off to an Editor";
		}
		const std::filesystem::path resolved = project::ResolveProjectRoot(root);
		if (resolved.empty())
		{
			return "root must identify a project folder";
		}
		if (!project::HasProjectDescriptor(resolved))
		{
			return "no ProjectSettings.toml found under root";
		}
		m_pendingControlAction = PendingControlAction{.kind = PendingControlActionKind::Open, .root = resolved, .executeAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(250)};
		return {};
	}

	std::string LauncherLayer::QueueCreateProjectForControl(const std::filesystem::path& root, std::string_view name, project::ProjectTemplate projectTemplate)
	{
		if (m_pendingControlAction.has_value() || m_pendingEditor.has_value())
		{
			return "the Launcher is already handing off to an Editor";
		}
		const std::filesystem::path resolved = project::ResolveProjectRoot(root);
		if (resolved.empty())
		{
			return "root must identify a project folder";
		}
		m_pendingControlAction = PendingControlAction{
		        .kind = PendingControlActionKind::Create, .root = resolved, .name = text::TrimAscii(std::string(name)), .projectTemplate = projectTemplate, .executeAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(250)};
		return {};
	}

	void LauncherLayer::OnAttach(LayerContext& context)
	{
		m_services = &context.services;

		if (auto* imgui = context.services.TryGet<ImguiSubsystem>())
		{
			imgui->SetViewportsEnabled(false);
		}

		// The hub layout is responsive down to its documented minimum; stop the OS
		if (auto* window = context.services.TryGet<Window>())
		{
			window->SetMinimumSize(kProjectLauncherMinWidth, kProjectLauncherMinHeight);
		}

		if (auto* assets = context.services.TryGet<AssetManager>())
		{
			auto logo = assets->CreateTexture(kEditorLogoPath);
			if (!logo)
			{
				AE_WARN(LogCategory::App, "Could not load launcher logo '{}': {}", kEditorLogoPath, logo.error());
			}
			else if (auto* imgui = context.services.TryGet<ImguiSubsystem>())
			{
				const ImTextureID textureId = imgui->RegisterTexture(logo->GetView(), gpu::ImageLayout::ShaderReadOnly);
				if (textureId != ImTextureID_Invalid)
				{
					m_logoTexture = std::move(*logo);
					m_logoTextureId = static_cast<std::uint64_t>(textureId);
				}
			}
		}

		const auto path = LauncherStatePath();
		if (!path.empty() && m_config.LoadFromPath(path))
		{
			AE_INFO(LogCategory::App, "Launcher state loaded from {}", path.string());
		}
		m_recentProjects = project::LoadRecentProjects(m_config);
		for (const EditorProjectContext& recent: m_recentProjects)
		{
			LoadPreview(recent);
		}

		// Location is the PARENT folder - the dialog appends the name. Seeding it with
		// parent/name meant accepting every default created AetherProject/AetherProject.
		std::vector<std::filesystem::path> recentRoots;
		recentRoots.reserve(m_recentProjects.size());
		for (const EditorProjectContext& recent: m_recentProjects)
		{
			recentRoots.push_back(recent.root);
		}
		std::filesystem::path parent = project::DefaultNewProjectParent(recentRoots, io::PlatformPaths::GetUserDocumentsDir());
		if (parent.empty())
		{
			parent = project::NormalizePath(std::filesystem::current_path());
		}
		std::snprintf(m_windowState.openPath.data(), m_windowState.openPath.size(), "%s", project::DisplayPath(parent).c_str());
		std::snprintf(m_windowState.newPath.data(), m_windowState.newPath.size(), "%s", project::DisplayPath(parent).c_str());
		std::snprintf(m_windowState.newName.data(), m_windowState.newName.size(), "%s", "AetherProject");

		m_controlBasePort = ResolveControlBasePort();
		if (m_controlBasePort > 0)
		{
			StartControlServer();
			// Log the port actually bound: StartControlServer can fall back off the
			// documented one and has already warned when it did.
			if (m_controlServer != nullptr)
			{
				AE_INFO(LogCategory::App, "Launcher MCP endpoint uses port {}; spawned Editors inherit it after handoff. Set AETHER_CONTROL_PORT=off to disable.", m_controlServer->Port());
			}
		}
		else
		{
			AE_INFO(LogCategory::App, "Launcher: MCP control-port forwarding disabled (AETHER_CONTROL_PORT); spawned editors start no control endpoint.");
		}
	}

	void LauncherLayer::OnDetach(LayerContext& /*context*/)
	{
		StopControlServer();
		PersistSettings();
		ReleasePreviews();
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
	}

	void LauncherLayer::OnUpdate(LayerContext& context)
	{
		if (m_pendingControlAction.has_value() && std::chrono::steady_clock::now() >= m_pendingControlAction->executeAfter)
		{
			PendingControlAction action = std::move(*m_pendingControlAction);
			m_pendingControlAction.reset();
			if (action.kind == PendingControlActionKind::Create)
			{
				CreateProject(action.root, action.name, action.projectTemplate);
			}
			else
			{
				OpenProject(action.root);
			}
		}

		if (m_controlServer != nullptr && m_controlServer->IsRunning())
		{
			const double fps = context.deltaTimeSeconds > 0.0 ? 1.0 / context.deltaTimeSeconds : 0.0;
			m_controlServer->SetFrameInfo(context.frameIndex, fps);
			m_controlServer->DrainCommands();
		}

		if (!m_pendingEditor.has_value())
		{
			return;
		}

		switch (m_pendingEditor->Poll())
		{
			case launcher::EditorStartupState::Ready:
				AE_INFO(LogCategory::App, "Editor confirmed healthy; closing Launcher.");
				if (auto* window = context.services.TryGet<Window>())
				{
					window->RequestClose();
				}
				return;
			case launcher::EditorStartupState::Exited:
				m_pendingEditor.reset();
				m_editorStartupSeconds = 0.0;
				m_windowState.launching = false;
				m_windowState.error = "Editor exited while starting. The launcher is still open.";
				StartControlServer();
				return;
			case launcher::EditorStartupState::Pending:
				break;
		}

		m_editorStartupSeconds += context.deltaTimeSeconds;
		if (m_editorStartupSeconds >= 20.0 && m_windowState.launching)
		{
			// The editor process is still alive (an exit was handled above) and may bind
			// the handed-off control port at any moment, so the launcher must NOT take it
			// back: rebinding here races the editor for the port and, when the editor wins,
			// leaves the launcher with no MCP endpoint while an editor is running. Keep
			// tracking the process instead - it either reports healthy (the launcher
			// closes, per the handoff contract) or exits (that branch above then restarts
			// the endpoint on a port that is genuinely free again).
			m_editorStartupSeconds = 0.0;
			m_windowState.launching = false;
			m_windowState.error = "Editor is taking longer than expected to start. The launcher is still open; the control port stays handed off to the editor.";
			AE_WARN(LogCategory::App, "Editor did not confirm healthy startup within 20 seconds; keeping the Launcher open with the control port handed off (the editor process is still running).");
		}
	}

	void LauncherLayer::LoadPreview(const EditorProjectContext& project)
	{
		if (m_services == nullptr)
		{
			return;
		}
		const std::string key = project::NormalizePath(project.root).string();
		if (m_previews.contains(key))
		{
			return;
		}
		PreviewEntry entry;
		const std::filesystem::path previewPath = project::PreviewImagePath(project.root);
		std::error_code ec;
		if (std::filesystem::exists(previewPath, ec))
		{
			auto* assets = m_services->TryGet<AssetManager>();
			auto* imgui = m_services->TryGet<ImguiSubsystem>();
			if (assets != nullptr && imgui != nullptr)
			{
				if (auto texture = assets->CreateTextureFromDisk(previewPath))
				{
					const ImTextureID textureId = imgui->RegisterTexture(texture->GetView(), gpu::ImageLayout::ShaderReadOnly);
					if (textureId != ImTextureID_Invalid)
					{
						entry.texture = std::move(*texture);
						entry.textureId = static_cast<std::uint64_t>(textureId);
					}
				}
				else
				{
					AE_WARN(LogCategory::App, "Launcher: could not load project preview '{}': {}", previewPath.string(), texture.error());
				}
			}
		}
		m_previews.emplace(key, std::move(entry));
	}

	std::uint64_t LauncherLayer::PreviewTextureFor(const EditorProjectContext& project) const
	{
		const auto it = m_previews.find(project::NormalizePath(project.root).string());
		return it != m_previews.end() ? it->second.textureId : 0;
	}

	void LauncherLayer::ReleasePreview(const std::string& key)
	{
		const auto it = m_previews.find(key);
		if (it == m_previews.end())
		{
			return;
		}
		if (it->second.textureId != 0)
		{
			if (auto* imgui = m_services != nullptr ? m_services->TryGet<ImguiSubsystem>() : nullptr; imgui != nullptr)
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(it->second.textureId));
			}
		}
		it->second.texture.Destroy();
		m_previews.erase(it);
	}

	void LauncherLayer::ReleasePreviews()
	{
		while (!m_previews.empty())
		{
			ReleasePreview(m_previews.begin()->first);
		}
	}

	void LauncherLayer::OnImGui(LayerContext& /*context*/)
	{
		ProjectLauncherWindowModel model;
		model.projectLoaded = false; // the launcher never loads a project in-process
		model.hasCurrentProject = false;
		model.logoTextureId = m_logoTextureId;
		model.currentProject = &m_currentProject;
		model.recentProjects = std::span<const EditorProjectContext>(m_recentProjects.data(), m_recentProjects.size());
		model.previewTextureId = [this](const EditorProjectContext& project)
		{
			return PreviewTextureFor(project);
		};
		model.modifiedLabel = [](const EditorProjectContext& project)
		{
			return project::LastModifiedLabel(project.root);
		};

		ProjectLauncherWindowActions actions;
		actions.openProject = [this](const std::filesystem::path& root)
		{
			OpenProject(root);
		};
		actions.createProject = [this](const std::filesystem::path& parentDir, std::string_view name, project::ProjectTemplate projectTemplate)
		{
			const std::filesystem::path root = project::ComposeNewProjectRoot(parentDir, name);
			if (root.empty())
			{
				m_windowState.error = "Choose a location and a valid project name.";
				return;
			}
			CreateProject(root, name, projectTemplate);
		};
		actions.browseFolder = []()
		{
			return project::PickProjectFolder();
		};
		actions.browseProjectFile = []()
		{
			return project::PickProjectFile();
		};
		actions.closeLauncher = []() {};
		actions.saveSettings = [this]()
		{
			PersistSettings();
		};
		actions.removeRecent = [this](const std::filesystem::path& root) { RemoveRecent(root); };
		actions.revealProjectFolder = [this](const std::filesystem::path& root) { RevealProjectFolder(root); };
		actions.relocateRecent = [this](const std::filesystem::path& root) { RelocateRecent(root); };

		m_window.Draw(m_windowState, model, actions);
	}

	void LauncherLayer::OpenProject(const std::filesystem::path& root)
	{
		m_windowState.error.clear();
		const std::filesystem::path resolved = project::ResolveProjectRoot(root);
		if (resolved.empty())
		{
			m_windowState.error = "Choose a project folder.";
			return;
		}
		if (!project::HasProjectDescriptor(resolved))
		{
			m_windowState.error = "No ProjectSettings.toml found there.";
			return;
		}
		RememberRecent(resolved);
		SpawnEditorFor(resolved);
	}

	void LauncherLayer::CreateProject(const std::filesystem::path& root, std::string_view name, project::ProjectTemplate projectTemplate)
	{
		m_windowState.error.clear();
		const std::filesystem::path resolved = project::ResolveProjectRoot(root);
		if (resolved.empty())
		{
			m_windowState.error = "Choose a project folder.";
			return;
		}
		std::string projectName = text::TrimAscii(std::string(name));
		if (projectName.empty())
		{
			projectName = project::FallbackProjectName(resolved);
		}
		if (!project::WriteProjectDescriptor(resolved, projectName, m_windowState.error, projectTemplate))
		{
			return;
		}
		RememberRecent(resolved);
		SpawnEditorFor(resolved);
	}

	void LauncherLayer::SpawnEditorFor(const std::filesystem::path& root)
	{
		if (m_pendingEditor.has_value())
		{
			// A previously spawned editor is still running but never confirmed healthy
			// (the 20s branch in OnUpdate keeps it tracked). It still holds the handed-off
			// control port, and spawning a second editor now would hand the same port to
			// two children.
			m_windowState.error = "An editor started from this launcher is still running. Wait for it to finish starting, or close it, before opening another project.";
			return;
		}
		// Hand off the port the launcher actually bound - a fallback port when the
		// documented one was taken (see StartControlServer) - so the MCP client stays on
		// one port across the handoff (AGENTS.md).
		const int controlPort = m_controlServer != nullptr ? m_controlServer->Port() : m_controlBasePort;
		StopControlServer();
		// Open the editor where the launcher is, so the handoff lands in the same place on
		// screen instead of jumping - possibly to another monitor entirely.
		int centerX = INT_MIN;
		int centerY = INT_MIN;
		if (auto* window = m_services != nullptr ? m_services->TryGet<Window>() : nullptr)
		{
			window->GetDesktopCenter(centerX, centerY);
		}
		if (auto editor = launcher::SpawnEditor(root, controlPort, centerX, centerY))
		{
			m_pendingEditor = std::move(*editor);
			m_editorStartupSeconds = 0.0;
			m_windowState.launching = true;
			m_windowState.error.clear();
		}
		else
		{
			m_windowState.error = "Could not start the editor. Check the log for details.";
			StartControlServer();
		}
	}

	void LauncherLayer::StartControlServer()
	{
		if (m_controlBasePort <= 0 || m_services == nullptr || m_controlServer != nullptr)
		{
			return;
		}
		m_controlServer = std::make_unique<editor::ControlServer>(*m_services, [this]() { return BuildLauncherControlMethods(*this); }, "launcher");
		m_controlServer->Start(m_controlBasePort);
		if (m_controlServer->IsRunning())
		{
			return;
		}
		// ControlServer::Start already logged why the bind failed. Dropping the endpoint
		// here left the launcher uncontrollable whenever the documented port was taken (a
		// second launcher instance, a leftover editor); fall back to a nearby port and say
		// so loudly instead.
		const int maxPort = std::min(m_controlBasePort + 4, 65535);
		for (int candidate = m_controlBasePort + 1; candidate <= maxPort; ++candidate)
		{
			m_controlServer->Start(candidate);
			if (m_controlServer->IsRunning())
			{
				AE_WARN(LogCategory::App, "Launcher: MCP port {} was unavailable; the endpoint fell back to port {}. Point MCP clients at the fallback, or free the documented port.", m_controlBasePort, candidate);
				return;
			}
		}
		AE_WARN(LogCategory::App, "Launcher: could not start an MCP endpoint on any port from {} to {}; launcher control tools are offline for this session.", m_controlBasePort, maxPort);
		m_controlServer.reset();
	}

	void LauncherLayer::StopControlServer()
	{
		if (m_controlServer != nullptr)
		{
			m_controlServer->Stop();
			m_controlServer.reset();
		}
	}

	void LauncherLayer::RememberRecent(const std::filesystem::path& root)
	{
		const std::filesystem::path resolved = project::ResolveProjectRoot(root);
		if (resolved.empty())
		{
			return;
		}
		std::erase_if(m_recentProjects, [&](const EditorProjectContext& p) { return project::NormalizePath(p.root) == resolved; });
		EditorProjectContext entry;
		entry.root = resolved;
		entry.name = project::ReadProjectName(resolved);
		LoadPreview(entry);
		m_recentProjects.insert(m_recentProjects.begin(), std::move(entry));
		if (m_recentProjects.size() > static_cast<std::size_t>(project::kMaxRecentProjects))
		{
			// Same invariant as RemoveRecent: a recent dropped by the trim no longer
			// references its preview, so the texture must not linger in m_previews.
			std::vector<std::string> droppedKeys;
			for (std::size_t index = static_cast<std::size_t>(project::kMaxRecentProjects); index < m_recentProjects.size(); ++index)
			{
				droppedKeys.push_back(project::NormalizePath(m_recentProjects[index].root).string());
			}
			m_recentProjects.resize(static_cast<std::size_t>(project::kMaxRecentProjects));
			for (const std::string& key: droppedKeys)
			{
				ReleasePreview(key);
			}
		}
		PersistSettings();
	}

	void LauncherLayer::RemoveRecent(const std::filesystem::path& root)
	{
		const std::filesystem::path resolved = project::NormalizePath(project::ResolveProjectRoot(root));
		const std::size_t before = m_recentProjects.size();
		std::erase_if(m_recentProjects, [&](const EditorProjectContext& p) { return project::NormalizePath(p.root) == resolved; });
		if (m_recentProjects.size() == before)
		{
			return;
		}
		PersistSettings();
		// m_previews is keyed by the same normalized root (LoadPreview), and erase_if just
		// removed every recent referencing that key - so its preview texture and registered
		// ImTextureID are now unreferenced. Without this, one GPU image per removed project
		// accumulated until launcher exit.
		ReleasePreview(resolved.string());
	}

	void LauncherLayer::RelocateRecent(const std::filesystem::path& oldRoot)
	{
		m_windowState.error.clear();
		const auto picked = project::PickProjectFolder();
		if (!picked.has_value())
		{
			return; // user cancelled
		}
		std::string error;
		if (!RelocateRecent(oldRoot, project::ResolveProjectRoot(*picked), error))
		{
			m_windowState.error = error;
		}
	}

	bool LauncherLayer::RelocateRecent(const std::filesystem::path& oldRoot, const std::filesystem::path& newRoot, std::string& error)
	{
		const std::filesystem::path resolved = project::ResolveProjectRoot(newRoot);
		if (!project::HasProjectDescriptor(resolved))
		{
			error = "That folder has no ProjectSettings.toml.";
			return false;
		}
		RemoveRecent(oldRoot);
		RememberRecent(resolved); // moves to front + persists
		return true;
	}

	void LauncherLayer::RevealProjectFolder(const std::filesystem::path& root)
	{
		project::OpenPathInFileManager(project::ResolveProjectRoot(root));
	}

	void LauncherLayer::PersistSettings()
	{
		project::SaveRecentProjects(m_config, std::span<const EditorProjectContext>(m_recentProjects.data(), m_recentProjects.size()));
		if (!m_config.IsDirty())
		{
			return;
		}
		const auto path = LauncherStatePath();
		if (!path.empty() && m_config.SaveToPath(path, "AetherCore launcher state"))
		{
			m_config.MarkClean();
		}
	}
} // namespace aether::app
