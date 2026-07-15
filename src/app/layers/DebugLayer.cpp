#include "DebugLayer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <Windows.h>
#endif

using namespace std::string_view_literals;

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "debug/ConsolePanel.hpp"
#include "debug/ControlServerPanel.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/DayNightPanel.hpp"
#include "debug/DevToolsPanel.hpp"
#include "debug/FileExplorerPanel.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/Icons.hpp"
#include "debug/HierarchyPanel.hpp"
#include "debug/InspectorPanel.hpp"
#include "debug/LightingPanel.hpp"
#include "debug/PerformancePanel.hpp"
#include "debug/PostProcessingPanel.hpp"
#include "debug/ProjectPanel.hpp"
#include "debug/RenderGraphPanel.hpp"
#include "debug/SettingsPanel.hpp"
#include "debug/ThemePanel.hpp"
#include "debug/TonemapPanel.hpp"
#include "debug/TextureInspectorPanel.hpp"
#include "debug/SpriteAnimationPanel.hpp"
#include "debug/SpriteSlicerPanel.hpp"
#include "debug/UiCanvasPanel.hpp"
#include "debug/ViewportPanel.hpp"
#include "AetherCore.hpp"
#include "PlaySession.hpp"
#include "assets/AssetManager.hpp"
#include "io/PlatformPaths.hpp"
#include "mesh/Mesh.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "platform/Input.hpp"
#include "platform/Window.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/Components.hpp"
#include "PlayState.hpp"
#include "scene/ModelBakeHook.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/SceneWorkflow.hpp"
#include "scene/World.hpp"
#include "utils/SettingsService.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/FuzzyMatch.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::editor
{
	namespace
	{
#ifndef AETHER_LAUNCHER_EXE_NAME
#	define AETHER_LAUNCHER_EXE_NAME "Launcher.exe"
#endif
#ifndef AETHER_LAUNCHER_EXE_PATH
#	define AETHER_LAUNCHER_EXE_PATH ""
#endif

		bool SpawnStandaloneLauncher()
		{
#ifdef _WIN32
			const std::filesystem::path launcherExe = io::PlatformPaths::ResolveToolExecutable("AETHER_LAUNCHER_EXE", AETHER_LAUNCHER_EXE_PATH, AETHER_LAUNCHER_EXE_NAME);
			std::string command = "\"" + launcherExe.string() + "\"";
			STARTUPINFOA startupInfo{};
			startupInfo.cb = sizeof(startupInfo);
			PROCESS_INFORMATION processInfo{};
			const std::string workingDirectory = launcherExe.parent_path().string();
			if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startupInfo, &processInfo) == 0)
			{
				AE_ERROR(LogCategory::App, "Editor failed to start Launcher (GetLastError={})", GetLastError());
				return false;
			}
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
			return true;
#else
			AE_ERROR(LogCategory::App, "Starting the standalone Launcher is only implemented on Windows.");
			return false;
#endif
		}

		void AppendObbEdges(std::vector<DebugVertex>& out, const glm::mat4& m, const glm::vec3& mn, const glm::vec3& mx, const glm::vec4& color)
		{
			glm::vec3 corners[8];
			for (int i = 0; i < 8; ++i)
			{
				const glm::vec3 local{((i & 1) != 0) ? mx.x : mn.x, ((i & 2) != 0) ? mx.y : mn.y, ((i & 4) != 0) ? mx.z : mn.z};
				corners[i] = glm::vec3(m * glm::vec4(local, 1.0f));
			}
			static constexpr int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
			for (const auto& edge: kEdges)
			{
				AddDebugLine(out, corners[edge[0]], corners[edge[1]], color);
			}
		}

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
			if (panelName == "Sprite Slicer")
			{
				return ICON_FA_IMAGE;
			}
			if (panelName == "Sprite Animation")
			{
				return ICON_FA_FILM;
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
			if (panelName == "Theme")
			{
				return ICON_FA_PALETTE;
			}
			return ICON_FA_CIRCLE;
		}

		std::string PanelVisibilityKey(std::string_view panelName)
		{
			std::string key = "debug.window.";
			for (const char c: panelName)
			{
				key += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
			}
			return key;
		}

		std::filesystem::path EditorStatePath()
		{
			return io::PlatformPaths::GetUserConfigDir() / "EditorState.toml";
		}

	} // namespace

	void DebugLayer::LoadSettings(app::LayerContext&)
	{
		const auto path = EditorStatePath();
		if (!path.empty() && m_debugConfig.LoadFromPath(path))
		{
			AE_INFO(LogCategory::App, "Editor state loaded from {}", path.string());
		}
		m_projects.LoadSettings(m_debugConfig);
	}

	void DebugLayer::SaveSettings(app::LayerContext&)
	{
		if (!m_debugConfig.IsDirty())
		{
			return;
		}
		const auto path = EditorStatePath();
		if (!path.empty() && m_debugConfig.SaveToPath(path, "AetherCore editor state"))
		{
			m_debugConfig.MarkClean();
			AE_INFO(LogCategory::App, "Editor state saved to {}", path.string());
		}
	}

	void DebugLayer::PersistSettings(app::LayerContext& context)
	{
		for (auto& panel: m_panels)
		{
			panel->SaveSettings(m_debugConfig, context);
			m_debugConfig.Set(PanelVisibilityKey(panel->GetName()), panel->IsVisible());
		}
		m_projects.SaveSettings(m_debugConfig);

		// Persist the editor window size (guarded so an unchanged size never dirties
		if (m_editorWindowW > 0 && static_cast<int>(m_debugConfig.GetFloat("editor.window_width", -1.0f)) != m_editorWindowW)
		{
			m_debugConfig.Set("editor.window_width", static_cast<float>(m_editorWindowW));
		}
		if (m_editorWindowH > 0 && static_cast<int>(m_debugConfig.GetFloat("editor.window_height", -1.0f)) != m_editorWindowH)
		{
			m_debugConfig.Set("editor.window_height", static_cast<float>(m_editorWindowH));
		}

		SaveSettings(context);
	}

	void DebugLayer::OnAttach(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();
		m_projects.Attach(context.services);
		LoadSettings(context);

		int defW = 2560;
		int defH = 1440;
		if (auto* settings = context.services.TryGet<SettingsService>())
		{
			defW = settings->Get().window.width;
			defH = settings->Get().window.height;
		}
		m_editorWindowW = static_cast<int>(m_debugConfig.GetFloat("editor.window_width", static_cast<float>(defW)));
		m_editorWindowH = static_cast<int>(m_debugConfig.GetFloat("editor.window_height", static_cast<float>(defH)));

		// panel can resolve it for its whole lifetime.
		context.services.Register<SceneSelection>(m_selection);
		context.services.Register<UndoStack>(m_undoStack);
		m_panels.push_back(std::make_unique<RenderGraphPanel>());
		m_panels.push_back(std::make_unique<TextureInspectorPanel>());
		m_panels.push_back(std::make_unique<SpriteSlicerPanel>());
		m_panels.push_back(std::make_unique<SpriteAnimationPanel>());
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
		m_panels.push_back(std::make_unique<ThemePanel>());
		m_panels.push_back(std::make_unique<DevToolsPanel>());
		m_panels.push_back(std::make_unique<ConsolePanel>());
		m_panels.push_back(std::make_unique<LightingPanel>());
		m_panels.push_back(std::make_unique<DayNightPanel>());
		m_panels.push_back(std::make_unique<ControlServerPanel>());
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

		// main thread (same as OnImGui), so mutating visibility here is race-free.
		EditorWindowActions windowActions;
		windowActions.list = [this]()
		{
			std::vector<EditorWindowInfo> out;
			out.reserve(m_panels.size());
			for (const auto& panel: m_panels)
			{
				out.push_back({std::string(panel->GetName()), panel->IsVisible()});
			}
			return out;
		};
		windowActions.setVisible = [this](std::string_view name, bool visible) -> bool
		{
			const auto equalsIgnoreCase = [](std::string_view a, std::string_view b)
			{
				if (a.size() != b.size())
				{
					return false;
				}
				for (std::size_t i = 0; i < a.size(); ++i)
				{
					if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
					{
						return false;
					}
				}
				return true;
			};
			for (auto& panel: m_panels)
			{
				if (equalsIgnoreCase(panel->GetName(), name))
				{
					panel->SetVisible(visible);
					return true;
				}
			}
			return false;
		};
		windowActions.focusInspectorComponent = [this](std::string_view component)
		{
			if (DebugPanel* inspector = FindPanelByName("Inspector"))
			{
				inspector->SetVisible(true);
			}
			iw::InspectorFocusRequest() = std::string(component);
		};
		context.services.Register<EditorWindowActions>(m_windowActions = std::move(windowActions));
	}

	void DebugLayer::OnDetach(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		PersistSettings(context);
		for (auto& panel: m_panels)
		{
			panel->OnDetach(context);
		}
		context.services.Unregister<EditorWindowActions>();
		m_windowActions = {};
		m_panels.clear();
		m_hierarchyPanel = nullptr;
		context.services.Unregister<app::EditorProjectContext>();
		context.services.Unregister<app::scene::ModelBakeHook>();
		context.services.Unregister<EditorProjectActions>();
		context.services.Unregister<UndoStack>();
		context.services.Unregister<SceneSelection>();
		m_projects.Detach();

		m_scriptErrors.Clear();
		m_dockspaceBuilt = false;
	}

	void DebugLayer::OnUpdate(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();
		const Input& input = context.Get<Input>();

		if (input.IsKeyPressed(aether::Key::F5))
		{
			if (auto* scripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>())
			{
				scripting->RequestReload();
			}
		}

		m_scriptErrors.Poll(context);

		m_projects.UpdateScriptBuild();

		if (!m_projects.IsProjectLoaded())
		{
			return;
		}

		m_selection.Prune(context.Get<World>());

		const auto* updatePlayState = context.TryGet<app::PlayState>();
		const bool editing = updatePlayState == nullptr || !updatePlayState->IsPlaying();
		if (editing && IsDebugRenderingEnabled() && !m_selection.All().empty())
		{
			if (auto* engine = context.TryGet<AetherCore>())
			{
				if (m_selection.ChangeSerial() != m_outlineSeenSerial)
				{
					m_outlineSeenSerial = m_selection.ChangeSerial();
					m_outlinePulseStart = context.elapsedTimeSeconds;
				}
				const float pulseT = m_outlinePulseStart >= 0.0 ? std::clamp(static_cast<float>((context.elapsedTimeSeconds - m_outlinePulseStart) / 0.5), 0.0f, 1.0f) : 1.0f;
				const float brightness = 1.6f - 0.6f * pulseT;

				auto& verts = engine->GetPendingDebugVertices();
				World& world = context.Get<World>();
				const Entity primary = m_selection.Primary();
				for (const Entity e: m_selection.All())
				{
					if (world.Has<SpriteRendererComponent>(e))
					{
						continue;
					}
					const auto* tc = world.TryGet<TransformComponent>(e);
					if (tc == nullptr)
					{
						continue;
					}
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

	void DebugLayer::OnRenderTargetsInvalidated(app::LayerContext& context)
	{
		for (auto& panel: m_panels)
		{
			panel->OnRenderTargetsInvalidated(context);
		}
	}

	void DebugLayer::DrawStatusBar(app::LayerContext& context)
	{
		using namespace chrome;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
		if (ImGui::BeginChild("##StatusBar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar))
		{
			const auto* playState = context.TryGet<app::PlayState>();
			const bool playing = playState != nullptr && playState->IsPlaying();
			const bool compiling = playState != nullptr && playState->IsCompiling();

			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 barMin = ImGui::GetWindowPos();
			const float barW = ImGui::GetWindowWidth();

			if (playing || compiling)
			{
				AccentHairline(drawList, barMin, barW, playing ? 0.85f : 0.35f);
			}

			ImGui::AlignTextToFramePadding();

			const auto tick = [&](const ImVec4& color)
			{
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float h = ImGui::GetTextLineHeight();
				drawList->AddRectFilled(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + 3.0f, p.y + h - 1.0f), U32(color));
				ImGui::Dummy(ImVec2(9.0f, 0.0f));
				ImGui::SameLine();
			};
			const auto divider = [&]()
			{
				ImGui::SameLine(0.0f, 12.0f);
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float h = ImGui::GetTextLineHeight();
				drawList->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x, p.y + h - 1.0f), U32(WithAlpha(kStroke, 0.9f)), 1.0f);
				ImGui::Dummy(ImVec2(0.0f, 0.0f));
				ImGui::SameLine(0.0f, 12.0f);
			};

			ImGui::Dummy(ImVec2(2.0f, 0.0f));
			ImGui::SameLine();

			if (m_projects.HasCurrentProject())
			{
				tick(kAccent);
				ImGui::PushStyleColor(ImGuiCol_Text, kText);
				ImGui::Text(ICON_FA_FOLDER_OPEN "  %s", m_projects.CurrentProject().name.c_str());
				ImGui::PopStyleColor();
				divider();
			}

			const char* sceneName = "-";
			if (const auto* scenes = context.TryGet<SceneSubsystem>(); scenes != nullptr && !scenes->GetCurrentScene().empty())
			{
				sceneName = scenes->GetCurrentScene().c_str();
			}
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			ImGui::Text(ICON_FA_CUBE "  %s", sceneName);
			ImGui::PopStyleColor();

			{
				const char* label = playing ? ICON_FA_PLAY "  PLAYING" : (compiling ? ICON_FA_GEAR "  COMPILING" : ICON_FA_STOP "  EDITING");
				const ImVec2 textSize = ImGui::CalcTextSize(label);
				const ImVec4 color = (playing || compiling) ? kAccentHi : kMuted;
				const ImVec2 textPos(barMin.x + (barW - textSize.x) * 0.5f, barMin.y + (ImGui::GetWindowHeight() - textSize.y) * 0.5f);
				drawList->AddText(textPos, U32(color), label);
			}

			// scripts on a worker thread (project open / F5), show what's happening with
			if (const auto* buildScripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>(); buildScripting != nullptr && buildScripting->IsBuilding() && !compiling)
			{
				divider();
				tick(kAccent);
				ImGui::PushStyleColor(ImGuiCol_Text, kAccentHi);
				ImGui::TextUnformatted(ICON_FA_GEAR "  Compiling C# scripts");
				ImGui::PopStyleColor();
				ImGui::SameLine(0.0f, 10.0f);
				const float trackW = 120.0f;
				const float trackH = 3.0f;
				const float segW = trackW * 0.34f;
				const ImVec2 curPos = ImGui::GetCursorScreenPos();
				const float trackY = curPos.y + (ImGui::GetFrameHeight() - trackH) * 0.5f;
				const float radius = trackH * 0.5f;
				drawList->AddRectFilled(ImVec2(curPos.x, trackY), ImVec2(curPos.x + trackW, trackY + trackH), U32(WithAlpha(kAccent, 0.20f)), radius);
				const double sweep = ImGui::GetTime() * 0.8;
				const float u = static_cast<float>(sweep - std::floor(sweep));
				const float segX = curPos.x + u * (trackW - segW);
				drawList->AddRectFilled(ImVec2(segX, trackY), ImVec2(segX + segW, trackY + trackH), U32(kAccentHi), radius);
				ImGui::Dummy(ImVec2(trackW, 0.0f));
			}

			const ImGuiIO& io = ImGui::GetIO();
			gpu::Extent2D extent{};
			if (const auto* swapchain = context.TryGet<Swapchain>())
			{
				extent = swapchain->GetExtent();
			}
			const float frameMs = io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f;
			const std::string res = std::format("{}x{}", extent.width, extent.height);
			const std::string fps = std::format("{:.0f} FPS", io.Framerate);
			const std::string ms = std::format("{:.2f} MS", frameMs);
			const float gap = 18.0f;
			const float totalW = ImGui::CalcTextSize(ICON_FA_GAUGE_HIGH).x + 8.0f + ImGui::CalcTextSize(res.c_str()).x + gap + ImGui::CalcTextSize(fps.c_str()).x + gap + ImGui::CalcTextSize(ms.c_str()).x + 12.0f;
			const float targetX = ImGui::GetWindowWidth() - totalW;
			if (targetX > ImGui::GetCursorPosX())
			{
				ImGui::SameLine(targetX);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, kAccentHi);
			ImGui::TextUnformatted(ICON_FA_GAUGE_HIGH);
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0f, 8.0f);
			ImGui::PushStyleColor(ImGuiCol_Text, kFaint);
			ImGui::TextUnformatted(res.c_str());
			ImGui::SameLine(0.0f, gap);
			ImGui::PopStyleColor();
			ImGui::PushStyleColor(ImGuiCol_Text, kText);
			ImGui::TextUnformatted(fps.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0f, gap);
			ImGui::PushStyleColor(ImGuiCol_Text, kFaint);
			ImGui::TextUnformatted(ms.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	void DebugLayer::DrawCommandPalette(app::LayerContext& context)
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
		if (auto* playState = context.TryGet<app::PlayState>())
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

	bool DebugLayer::SaveCurrentScene(app::LayerContext& context)
	{
		auto* scenes = context.TryGet<SceneSubsystem>();
		const std::string currentName = scenes != nullptr ? scenes->GetCurrentScene() : std::string{};

		bool saved = false;
		if (!currentName.empty())
		{
			if (auto* assets = context.TryGet<AssetManager>())
			{
				saved = app::scene::QuickSave(context.Get<World>(), currentName, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), context.TryGet<Renderer>());
			}
		}

		if (saved)
		{
			ShowToast(std::string(ICON_FA_FLOPPY_DISK "  Saved  ") + currentName);
			m_projects.CaptureProjectPreview();
		}
		else if (m_hierarchyPanel != nullptr)
		{
			m_hierarchyPanel->RequestSaveAsPopup();
		}
		return saved;
	}

	void DebugLayer::SaveAndReturnToLauncher(app::LayerContext& context)
	{
		if (!SaveCurrentScene(context))
		{
			ShowToast(ICON_FA_CIRCLE_INFO "  Save the scene before returning to the launcher.", true);
			return;
		}
		if (!SpawnStandaloneLauncher())
		{
			ShowToast(ICON_FA_CIRCLE_INFO "  Could not open the project launcher.", true);
			return;
		}
		if (auto* window = context.services.TryGet<Window>())
		{
			window->RequestClose();
		}
	}

	void DebugLayer::CaptureEditorWindowSize(app::LayerContext& context)
	{
		auto* window = context.services.TryGet<Window>();
		if (window == nullptr)
		{
			return;
		}
		// launcher-size exclusion - the launcher never resizes the OS window anymore.
		const auto size = window->GetWindowSize();
		if (size.width >= 640 && size.height >= 480 && (size.width != m_editorWindowW || size.height != m_editorWindowH))
		{
			m_editorWindowW = size.width;
			m_editorWindowH = size.height;
		}
	}

	void DebugLayer::ShowToast(std::string text, bool isError)
	{
		m_toastText = std::move(text);
		m_toastStart = ImGui::GetTime();
		m_toastError = isError;
	}

	void DebugLayer::DrawToasts()
	{
		if (m_toastStart < 0.0)
		{
			return;
		}
		constexpr float kLifetime = 2.4f;
		constexpr float kFadeIn = 0.12f;
		constexpr float kFadeOut = 0.5f;
		const auto age = static_cast<float>(ImGui::GetTime() - m_toastStart);
		if (age > kLifetime)
		{
			m_toastStart = -1.0;
			return;
		}

		float alpha = 1.0f;
		if (age < kFadeIn)
		{
			alpha = age / kFadeIn;
		}
		else if (age > kLifetime - kFadeOut)
		{
			alpha = (kLifetime - age) / kFadeOut;
		}
		alpha = std::clamp(alpha, 0.0f, 1.0f);

		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
		constexpr float kFont = 14.0f;
		const ImVec2 textSize = chrome::MeasureSized(kFont, m_toastText.c_str());
		constexpr float padX = 18.0f;
		constexpr float padY = 10.0f;
		const float w = textSize.x + padX * 2.0f;
		const float h = textSize.y + padY * 2.0f;
		const float rise = (1.0f - alpha) * 8.0f;
		const float cx = vp->Pos.x + vp->Size.x * 0.5f;
		const float bottom = vp->Pos.y + vp->Size.y - ImGui::GetFrameHeight() - 18.0f + rise;
		const ImVec2 p0(cx - w * 0.5f, bottom - h);
		const ImVec2 p1(cx + w * 0.5f, bottom);
		const ImVec4 accent = m_toastError ? chrome::kError : chrome::kSuccess;

		constexpr float rounding = 9.0f;
		constexpr ImDrawFlags roundRight = ImDrawFlags_RoundCornersRight;
		dl->AddRectFilled(ImVec2(p0.x, p0.y + 3.0f), ImVec2(p1.x, p1.y + 3.0f), chrome::U32(chrome::WithAlpha(chrome::kBg, 0.55f * alpha)), rounding, roundRight);
		dl->AddRectFilled(p0, p1, chrome::U32(chrome::WithAlpha(chrome::kPanelHi, 0.98f * alpha)), rounding, roundRight);
		dl->AddRect(p0, p1, chrome::U32(chrome::WithAlpha(accent, 0.75f * alpha)), rounding, roundRight, 1.5f);
		dl->AddRectFilled(p0, ImVec2(p0.x + 3.0f, p1.y), chrome::U32(chrome::WithAlpha(accent, alpha)));
		chrome::TextSized(dl, kFont, ImVec2(p0.x + padX, p0.y + padY), chrome::WithAlpha(chrome::kText, alpha), m_toastText.c_str());
	}

	void DebugLayer::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		CaptureEditorWindowSize(context);

		ImGuizmo::BeginFrame();

		// A layout preset queued last frame is applied here, before any window
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

		if (!m_projects.IsProjectLoaded() || m_projects.IsLauncherOpen())
		{
			if (m_dockspaceBuilt)
			{
				ImGuiViewport* mainViewport = ImGui::GetMainViewport();
				const ImU32 backdrop = ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32(0, 0, 0, 255);
				ImGui::GetBackgroundDrawList(mainViewport)->AddRectFilled(mainViewport->Pos, ImVec2(mainViewport->Pos.x + mainViewport->Size.x, mainViewport->Pos.y + mainViewport->Size.y), backdrop);
			}
			m_projects.DrawLauncher();
			PersistSettings(context);
			return;
		}

		if (m_dockspaceBuilt)
		{
			ImGuiViewport* mainViewport = ImGui::GetMainViewport();
			const ImU32 editorBg = ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32(0, 0, 0, 255);
			ImGui::GetBackgroundDrawList(mainViewport)->AddRectFilled(mainViewport->Pos, ImVec2(mainViewport->Pos.x + mainViewport->Size.x, mainViewport->Pos.y + mainViewport->Size.y), editorBg);
		}

		if (const auto* playState = context.TryGet<app::PlayState>(); playState != nullptr && !playState->IsPlaying())
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
						m_selection.Clear();
					}
				}
			}
		}

		{
			const ImGuiIO& io = ImGui::GetIO();
			if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false))
			{
				SaveCurrentScene(context);
			}
		}

		m_scriptErrors.Draw();

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);
		const bool showMenuBar = m_dockspaceBuilt;
		const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus
		                                   | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground | (showMenuBar ? ImGuiWindowFlags_MenuBar : 0);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("DebugDockSpace", nullptr, hostFlags);
		ImGui::PopStyleVar(3);

		ImGui::PushStyleColor(ImGuiCol_MenuBarBg, chrome::kPanel);
		ImGui::PushStyleColor(ImGuiCol_Header, chrome::WithAlpha(chrome::kAccent, 0.20f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, chrome::WithAlpha(chrome::kAccent, 0.28f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, chrome::WithAlpha(chrome::kAccent, 0.36f));
		if (showMenuBar && ImGui::BeginMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Save & Return to Project Launcher..."))
				{
					SaveAndReturnToLauncher(context);
				}
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_FA_PLUS "  New Scene"))
				{
					const std::string name = app::scene::NewScene(context.Get<World>(), app::scene::MakeApplySceneDeps(context.services));
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
				        {ICON_FA_IMAGE, "2D", {"Sprite Slicer", "Sprite Animation"}},
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

			{
				using namespace chrome;
				const auto* playState = context.TryGet<app::PlayState>();
				const bool compiling = playState != nullptr && playState->IsCompiling();
				const char* chip = compiling ? ICON_FA_GEAR "  BUILD" : "AETHERCORE";
				const ImVec4 chipColor = compiling ? kAccentHi : kFaint;
				const float chipW = ImGui::CalcTextSize(chip).x;
				const float avail = ImGui::GetContentRegionAvail().x;
				if (avail > chipW + 16.0f)
				{
					ImGui::SameLine(ImGui::GetCursorPosX() + avail - chipW - 12.0f);
					ImGui::PushStyleColor(ImGuiCol_Text, chipColor);
					ImGui::TextUnformatted(chip);
					ImGui::PopStyleColor();
				}
			}
			ImGui::EndMenuBar();
		}
		ImGui::PopStyleColor(4);

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

		const ImGuiID dockspace_id = ImGui::GetID("AetherDebugDockSpaceV6");
		const bool hasSavedDockspace = ImGui::DockBuilderGetNode(dockspace_id) != nullptr;
		const bool showStatusBar = m_dockspaceBuilt;
		const float statusBarHeight = showStatusBar ? ImGui::GetFrameHeight() : 0.0f;
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
		ImGui::DockSpace(dockspace_id, ImVec2(0.0f, -statusBarHeight), ImGuiDockNodeFlags_PassthruCentralNode);

		if (m_resetLayout || (!m_dockspaceBuilt && !hasSavedDockspace))
		{
			ImGui::DockBuilderRemoveNode(dockspace_id);
			ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

			ImGuiID remaining = dockspace_id;
			ImGuiID dock_left = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Left, 0.20f, nullptr, &remaining);
			const ImGuiID dock_left_files = ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.42f, nullptr, &dock_left);
			ImGuiID dock_right = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Right, 0.27f, nullptr, &remaining);
			const ImGuiID dock_right_tools = ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.38f, nullptr, &dock_right);
			const ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Down, 0.28f, nullptr, &remaining);

			ImGui::DockBuilderDockWindow("Scene", dock_left);
			ImGui::DockBuilderDockWindow("Project", dock_left_files);
			ImGui::DockBuilderDockWindow("File Explorer", dock_left_files);
			ImGui::DockBuilderDockWindow("Viewport", remaining);
			ImGui::DockBuilderDockWindow("UI Canvas", remaining);
			ImGui::DockBuilderDockWindow("Sprite Slicer", remaining);
			ImGui::DockBuilderDockWindow("Sprite Animation", remaining);
			ImGui::DockBuilderDockWindow("Inspector", dock_right);
			ImGui::DockBuilderDockWindow("Render Graph", dock_right_tools);
			ImGui::DockBuilderDockWindow("Debug", dock_right_tools);
			ImGui::DockBuilderDockWindow("Tonemap", dock_right_tools);
			ImGui::DockBuilderDockWindow("Post Processing", dock_right_tools);
			ImGui::DockBuilderDockWindow("Settings", dock_right_tools);
			ImGui::DockBuilderDockWindow("Theme", dock_right_tools);
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
			DrawToasts();
		}
		ImGui::PopStyleVar();

		ImGui::End();

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
} // namespace aether::editor
