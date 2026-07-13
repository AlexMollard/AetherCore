#include "launcher/LauncherLayer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <string>

#include "assets/AssetManager.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "io/PlatformPaths.hpp"
#include "launcher/LauncherProcess.hpp"
#include "platform/Window.hpp"
#include "project/ProjectCommon.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"
#include "utils/TextIni.hpp"

namespace aether::app
{
	namespace
	{
		constexpr std::string_view kEditorLogoPath = "engine://branding/aethercore-icon-white.png";

		// The launcher persists its recent-projects list separately from the editor's
		// EditorState.toml: the hub owns the recent list; the editor is spawned for a
		// single project via --project and never reads it.
		std::filesystem::path LauncherStatePath()
		{
			return io::PlatformPaths::GetUserConfigDir() / "LauncherState.toml";
		}

		// Base MCP control port forwarded to spawned editors, from AETHER_CONTROL_PORT:
		//   unset              -> 8787 (zero-setup: launcher-spawned editors are MCP-ready)
		//   a valid port 1-65535-> that value
		//   "0"/"off"/"none"/"disabled"/"false" -> 0 (disabled; editors start no endpoint)
		int ResolveControlBasePort()
		{
			constexpr int kDefaultPort = 8787;
			const char* env = std::getenv("AETHER_CONTROL_PORT");
			if (env == nullptr || *env == '\0')
			{
				return kDefaultPort;
			}
			std::string lowered;
			for (const char* p = env; *p != '\0'; ++p)
			{
				lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
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
	} // namespace

	void LauncherLayer::OnAttach(LayerContext& context)
	{
		m_services = &context.services;

		// The launcher is a single-window hub in its own OS process, so ImGui
		// multi-viewport (tear-out windows) is unnecessary. Worse, with the shared
		// overlay's ConfigViewportsNoAutoMerge, the floating hub would be promoted to
		// its own platform window - leaving the MAIN swapchain empty (the engine's
		// ScreenshotService, which captures that swapchain, would only see the clear
		// colour). Disabling viewports keeps the hub in the main window.
		if (auto* imgui = context.services.TryGet<ImguiSubsystem>())
		{
			imgui->SetViewportsEnabled(false);
		}

		// The hub layout is responsive down to its documented minimum; stop the OS
		// window from shrinking below it.
		if (auto* window = context.services.TryGet<Window>())
		{
			window->SetMinimumSize(kProjectLauncherMinWidth, kProjectLauncherMinHeight);
		}

		// Optional hub logo. AssetManager + the Dear ImGui overlay both exist in a
		// UiShell runtime (AssetSubsystem is initialized; the overlay is installed by
		// the launcher entry point), so the load path mirrors the editor's.
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

		// Seed the hub's open/create text fields with a sensible default location.
		const std::filesystem::path cwd = project::NormalizePath(std::filesystem::current_path());
		std::snprintf(m_windowState.openPath.data(), m_windowState.openPath.size(), "%s", project::DisplayPath(cwd).c_str());
		std::snprintf(m_windowState.newPath.data(), m_windowState.newPath.size(), "%s", project::DisplayPath(cwd / "AetherProject").c_str());
		std::snprintf(m_windowState.newName.data(), m_windowState.newName.size(), "%s", "AetherProject");

		// MCP integration: decide the control port forwarded to spawned editors.
		m_controlBasePort = ResolveControlBasePort();
		if (m_controlBasePort > 0)
		{
			AE_INFO(LogCategory::App, "Launcher: spawned editors get an MCP control port (base {}; the Nth editor gets {}+N). Set AETHER_CONTROL_PORT=off to disable.", m_controlBasePort, m_controlBasePort);
		}
		else
		{
			AE_INFO(LogCategory::App, "Launcher: MCP control-port forwarding disabled (AETHER_CONTROL_PORT); spawned editors start no control endpoint.");
		}
	}

	void LauncherLayer::OnDetach(LayerContext& /*context*/)
	{
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

	void LauncherLayer::LoadPreview(const EditorProjectContext& project)
	{
		if (m_services == nullptr)
		{
			return;
		}
		const std::string key = project::NormalizePath(project.root).string();
		if (m_previews.contains(key))
		{
			return; // already loaded (or known-absent)
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
		m_previews.emplace(key, std::move(entry)); // cache even a 0 id so we don't retry
	}

	std::uint64_t LauncherLayer::PreviewTextureFor(const EditorProjectContext& project) const
	{
		const auto it = m_previews.find(project::NormalizePath(project.root).string());
		return it != m_previews.end() ? it->second.textureId : 0;
	}

	void LauncherLayer::ReleasePreviews()
	{
		auto* imgui = m_services != nullptr ? m_services->TryGet<ImguiSubsystem>() : nullptr;
		for (auto& [key, entry]: m_previews)
		{
			if (entry.textureId != 0 && imgui != nullptr)
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(entry.textureId));
			}
			entry.texture.Destroy();
		}
		m_previews.clear();
	}

	void LauncherLayer::OnImGui(LayerContext& /*context*/)
	{
		ProjectLauncherWindowModel model;
		model.projectLoaded = false; // the launcher never loads a project in-process
		model.hasCurrentProject = false;
		model.logoTextureId = m_logoTextureId;
		model.currentProject = &m_currentProject;
		model.recentProjects = std::span<const EditorProjectContext>(m_recentProjects.data(), m_recentProjects.size());
		model.previewTextureId = [this](const EditorProjectContext& project) { return PreviewTextureFor(project); };
		model.modifiedLabel = [](const EditorProjectContext& project) { return project::LastModifiedLabel(project.root); };

		ProjectLauncherWindowActions actions;
		actions.openProject = [this](std::filesystem::path root) { OpenProject(root); };
		actions.createProject = [this](std::filesystem::path root, std::string_view name) { CreateProject(root, name); };
		actions.browseFolder = []() { return project::PickProjectFolder(); };
		actions.browseProjectFile = []() { return project::PickProjectFile(); };
		actions.closeLauncher = []() {}; // no-op: the hub IS the launcher; quit via the OS window
		actions.saveSettings = [this]() { PersistSettings(); };

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

	void LauncherLayer::CreateProject(const std::filesystem::path& root, std::string_view name)
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
		if (!project::WriteProjectDescriptor(resolved, projectName, m_windowState.error))
		{
			return;
		}
		RememberRecent(resolved);
		SpawnEditorFor(resolved);
	}

	void LauncherLayer::SpawnEditorFor(const std::filesystem::path& root)
	{
		// Hand each spawned editor a distinct control port (base + N) so several open
		// editors never fight over one port; 0 when forwarding is disabled.
		const int controlPort = m_controlBasePort > 0 ? m_controlBasePort + m_spawnCount++ : 0;
		launcher::SpawnEditor(root, controlPort);
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
		LoadPreview(entry); // may not exist yet (editor writes it on save) - cached as absent then
		m_recentProjects.insert(m_recentProjects.begin(), std::move(entry));
		if (m_recentProjects.size() > static_cast<std::size_t>(project::kMaxRecentProjects))
		{
			m_recentProjects.resize(static_cast<std::size_t>(project::kMaxRecentProjects));
		}
		PersistSettings();
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
