#include "DebugLayer.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <regex>
#include <string_view>

using namespace std::string_view_literals;

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#endif

#include "debug/DayNightPanel.hpp"
#include "debug/DevToolsPanel.hpp"
#include "debug/HierarchyPanel.hpp"
#include "debug/InspectorPanel.hpp"
#include "debug/LightingPanel.hpp"
#include "debug/PerformancePanel.hpp"
#include "debug/PostProcessingPanel.hpp"
#include "debug/RenderGraphPanel.hpp"
#include "debug/TonemapPanel.hpp"
#include "debug/TextureInspectorPanel.hpp"
#include "debug/ViewportPanel.hpp"
#include "AetherCore.hpp"
#include "mesh/Mesh.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "platform/Input.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/Components.hpp"
#include "PlayState.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
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
		if (filePath.empty())
		{
			return;
		}

		std::string resolved = filePath;
		if (!std::filesystem::path(filePath).is_absolute())
		{
			std::error_code ec;
			auto candidate = std::filesystem::weakly_canonical(std::filesystem::current_path() / ".." / ".." / filePath, ec);
			if (!ec && std::filesystem::exists(candidate, ec))
			{
				resolved = candidate.string();
			}
		}

#ifdef _WIN32
		std::string cmd;
		if (line > 0)
		{
			cmd = "code -g \"" + resolved + ":" + std::to_string(line) + "\"";
		}
		else
		{
			cmd = "code \"" + resolved + "\"";
		}

		int ret = system(("where code >nul 2>&1 && " + cmd).c_str());
		if (ret == 0)
		{
			return;
		}

		ShellExecuteA(nullptr, "open", resolved.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
		std::string shellCmd = "code " + resolved;
		if (line > 0)
		{
			shellCmd += ":" + std::to_string(line);
		}
		system(shellCmd.c_str());
#endif
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
		if (!m_debugConfig.LoadFile("debug"))
		{
			return;
		}

		m_visible = m_debugConfig.GetBool("debug.visible", m_visible);

		AE_INFO(LogCategory::App, "Debug settings loaded");
	}

	void DebugLayer::SaveSettings(LayerContext&)
	{
		m_debugConfig.Set("debug.visible", m_visible);

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
		}
		SaveSettings(context);
	}

	void DebugLayer::OnAttach(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		LoadSettings(context);

		// Shared selection service: registered before panels attach so every
		// panel can resolve it for its whole lifetime.
		context.services.Register<SceneSelection>(m_selection);
		// Editor undo: panels push explicit points for keyboard-driven edits;
		// mouse gestures are covered by the per-click push in OnImGui.
		context.services.Register<UndoStack>(m_undoStack);

		m_panels.push_back(std::make_unique<RenderGraphPanel>());
		m_panels.push_back(std::make_unique<TextureInspectorPanel>());
		m_panels.push_back(std::make_unique<HierarchyPanel>());
		m_panels.push_back(std::make_unique<InspectorPanel>());
		m_panels.push_back(std::make_unique<PerformancePanel>());
		m_panels.push_back(std::make_unique<ViewportPanel>());
		m_panels.push_back(std::make_unique<TonemapPanel>());
		m_panels.push_back(std::make_unique<PostProcessingPanel>());
		m_panels.push_back(std::make_unique<DevToolsPanel>());
		m_panels.push_back(std::make_unique<LightingPanel>());
		m_panels.push_back(std::make_unique<DayNightPanel>());
		for (auto& panel: m_panels)
		{
			panel->OnAttach(context);
		}
		for (auto& panel: m_panels)
		{
			panel->LoadSettings(m_debugConfig, context);
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
		context.services.Unregister<UndoStack>();
		context.services.Unregister<SceneSelection>();

		m_errorToasts.clear();
		m_dockspaceBuilt = false;
	}

	void DebugLayer::OnUpdate(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		const Input& input = context.Get<Input>();

		if (input.IsKeyPressed(aether::Key::F1))
		{
			m_visible = !m_visible;
			SaveSettings(context);
		}

		if (input.IsKeyPressed(aether::Key::F5))
		{
			if (auto scripting = context.TryGet<scripting::CSharpScriptingSubsystem>())
			{
				scripting->RequestReload();
			}
		}

		PollScriptErrors(context);

		// Entities can be destroyed by scripts/physics at any point; keep the
		// shared selection free of dangling ids before panels read it.
		m_selection.Prune(context.Get<World>());

		// Selection outlines: world-space wireframe boxes through the debug-line
		// pass (same submission path as the light gizmos).
		if (m_visible && IsDebugRenderingEnabled() && !m_selection.All().empty())
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

	void DebugLayer::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		// Once per ImGui frame, before any panel might call Manipulate.
		ImGuizmo::BeginFrame();

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

		if (!m_visible)
		{
			if (auto* rendering = context.TryGet<RenderingSubsystem>())
			{
				rendering->GetPostProcessStack().SetHistogramCaptureEnabled(false);
			}
			context.Get<Input>().ClearMouseViewportTransform();
			return;
		}

		// Root dockspace: invisible full-screen window for docking
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
		                             | ImGuiWindowFlags_NoBackground;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("DebugDockSpace", nullptr, hostFlags);
		ImGui::PopStyleVar(3);

		// V3: classic editor arrangement - outliner left, Inspector right (over a
		// tabbed tool stack), utility tabs bottom, Viewport center. The id bump
		// retires saved V2 layouts so the new default actually applies once.
		ImGuiID dockspace_id = ImGui::GetID("AetherDebugDockSpaceV3");
		const bool hasSavedDockspace = ImGui::DockBuilderGetNode(dockspace_id) != nullptr;
		ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

		if (!m_dockspaceBuilt && !hasSavedDockspace)
		{
			ImGui::DockBuilderRemoveNode(dockspace_id);
			ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

			ImGuiID remaining = dockspace_id;
			ImGuiID dock_left = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Left, 0.20f, nullptr, &remaining);
			ImGuiID dock_right = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Right, 0.27f, nullptr, &remaining);
			ImGuiID dock_right_tools = ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.38f, nullptr, &dock_right);
			ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Down, 0.28f, nullptr, &remaining);

			ImGui::DockBuilderDockWindow("Scene", dock_left);
			ImGui::DockBuilderDockWindow("Viewport", remaining);
			ImGui::DockBuilderDockWindow("Inspector", dock_right);
			ImGui::DockBuilderDockWindow("Render Graph", dock_right_tools);
			ImGui::DockBuilderDockWindow("Debug", dock_right_tools);
			ImGui::DockBuilderDockWindow("Tonemap", dock_right_tools);
			ImGui::DockBuilderDockWindow("Post Processing", dock_right_tools);
			ImGui::DockBuilderDockWindow("Performance", dock_bottom);
			ImGui::DockBuilderDockWindow("Lighting", dock_bottom);
			ImGui::DockBuilderDockWindow("Day / Night", dock_bottom);
			ImGui::DockBuilderDockWindow("Textures", dock_bottom);

			ImGui::DockBuilderFinish(dockspace_id);
		}
		m_dockspaceBuilt = true;

		ImGui::End();

		// Panels that manage their own windows
		for (auto& panel: m_panels)
		{
			if (panel->GetName() != "Render Graph"sv)
			{
				panel->OnImGui(context);
			}
		}

		// Render Graph window wraps render graph panel content
		ImGui::Begin("Render Graph");
		for (auto& panel: m_panels)
		{
			if (panel->GetName() == "Render Graph"sv)
			{
				panel->OnImGui(context);
			}
		}
		ImGui::End();

		PersistSettings(context);
	}
} // namespace aether::app
