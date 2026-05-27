#include "DebugLayer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <regex>

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#endif

#include "utils/DebugGui.hpp"

#include "AetherCore.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "platform/Input.hpp"
#include "utils/Logger.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/Renderer.hpp"
#include "scripting/ScriptingSubsystem.hpp"

namespace aether::app
{
	namespace
	{
		glm::vec4 FpsColor(float fps) noexcept
		{
			if (fps >= 55.f)
			{
				return { 0.40f, 0.72f, 0.46f, 1.f };
			}
			if (fps >= 30.f)
			{
				return { 0.86f, 0.71f, 0.30f, 1.f };
			}
			return { 0.80f, 0.33f, 0.30f, 1.f };
		}

		glm::vec4 MsColor(float ms) noexcept
		{
			if (ms <= 16.667f)
			{
				return { 0.40f, 0.72f, 0.46f, 1.f };
			}
			if (ms <= 25.f)
			{
				return { 0.86f, 0.71f, 0.30f, 1.f };
			}
			return { 0.80f, 0.33f, 0.30f, 1.f };
		}

		ImVec4 ToImVec4(glm::vec4 c)
		{
			return { c.r, c.g, c.b, c.a };
		}

	} // namespace

	// ── Stat helpers ──────────────────────────────────────────────────────────

	const char* DebugLayer::GetTonemapModeName(aether::TonemapMode mode)
	{
		switch (mode)
		{
			case aether::TonemapMode::Reinhard:
				return "Reinhard";
			case aether::TonemapMode::AcesFilmic:
				return "ACES Filmic";
			case aether::TonemapMode::Uncharted2:
				return "Uncharted2";
			default:
				return "Unknown";
		}
	}

	float DebugLayer::GetAverageFrameTimeMs() const
	{
		if (m_frameHistoryCount == 0)
		{
			return 0.f;
		}
		float total = 0.f;
		for (std::size_t i = 0; i < m_frameHistoryCount; ++i)
		{
			total += m_frameTimesMs[i];
		}
		return total / static_cast<float>(m_frameHistoryCount);
	}

	float DebugLayer::GetMinFrameTimeMs() const
	{
		if (m_frameHistoryCount == 0)
		{
			return 0.f;
		}
		float v = m_frameTimesMs[0];
		for (std::size_t i = 1; i < m_frameHistoryCount; ++i)
		{
			v = std::min(v, m_frameTimesMs[i]);
		}
		return v;
	}

	float DebugLayer::GetMaxFrameTimeMs() const
	{
		if (m_frameHistoryCount == 0)
		{
			return 0.f;
		}
		float v = m_frameTimesMs[0];
		for (std::size_t i = 1; i < m_frameHistoryCount; ++i)
		{
			v = std::max(v, m_frameTimesMs[i]);
		}
		return v;
	}

	void DebugLayer::DrawFrameTimeGraph() const
	{
		// Build a flat float array in chronological order for ImGui::PlotHistogram.
		std::array<float, kFrameHistorySize> ordered{};
		for (std::size_t i = 0; i < m_frameHistoryCount; ++i)
		{
			const std::size_t idx = (m_frameHistoryHead + kFrameHistorySize - m_frameHistoryCount + i) % kFrameHistorySize;
			ordered[i] = m_frameTimesMs[idx];
		}

		std::array<char, 32> overlay{};
		std::snprintf(overlay.data(), overlay.size(), "%.2f ms", GetAverageFrameTimeMs());

		ImGui::PlotHistogram("##ft", ordered.data(), static_cast<int>(m_frameHistoryCount), 0, overlay.data(), 0.f, 33.333f, ImVec2(ImGui::GetContentRegionAvail().x, 72.f));
	}

	// ── Script error toast ────────────────────────────────────────────────────

	void DebugLayer::ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine)
	{
		outPath.clear();
		outLine = 0;

		// daScript error format is multi-line:
		//   Compile error in D:\AetherCore\resources\scripts\sandbox.das:
		//   error[20000]: syntax error...
		//   D:\AetherCore\resources\scripts\sandbox.das:30:0
		// The location line (path:line:col) appears on a separate line.
		// Search for the pattern "*.das:NUMBER:NUMBER" which indicates the actual location.
		
		// Find all occurrences of ".das:" followed by digits (the location pattern)
		std::size_t searchPos = 0;
		while (searchPos < error.size())
		{
			const auto dasPos = error.find(".das:", searchPos);
			if (dasPos == std::string::npos)
				break;
			
			// Check if this is followed by a number (line number)
			const std::size_t colonPos = dasPos + 4;
			if (colonPos >= error.size() || !std::isdigit(static_cast<unsigned char>(error[colonPos])))
			{
				searchPos = dasPos + 1;
				continue;
			}
			
			// Scan backwards to find the start of the path
			std::size_t start = dasPos;
			while (start > 0 && error[start - 1] != ' ' && error[start - 1] != '\n' && error[start - 1] != '\r')
			{
				--start;
			}
			
			// Extract the path including .das
			outPath = error.substr(start, dasPos + 4 - start);
			
			// Parse the line number after .das:
			std::size_t lineStart = colonPos + 1;
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
			
			// Found a valid location, stop searching
			if (outLine > 0)
				break;
			
			searchPos = dasPos + 1;
		}
	}

	void DebugLayer::OpenInVSCode(const std::string& filePath, int line)
	{
		if (filePath.empty())
		{
			return;
		}

		std::string resolved = filePath;

		// Resolve relative paths against the project root.
		if (!std::filesystem::path(filePath).is_absolute())
		{
			std::error_code ec;
			auto candidate = std::filesystem::weakly_canonical(
				std::filesystem::current_path() / ".." / ".." / filePath, ec);
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

		// Try VS Code first.
		int ret = system(("where code >nul 2>&1 && " + cmd).c_str());
		if (ret == 0)
		{
			return;
		}

		// Fallback: open with default editor.
		ShellExecuteA(nullptr, "open", resolved.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
		// Linux/macOS fallback.
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
		auto* scripting = context.TryGet<scripting::ScriptingSubsystem>();
		if (!scripting)
		{
			return;
		}

		// If errors were explicitly cleared (e.g. successful reload), dismiss
		// all toasts immediately.
		if (scripting->ConsumeErrorsCleared())
		{
			m_errorToasts.clear();
		}

		auto errors = scripting->PollPendingErrors();
		if (!errors.empty())
		{
			// New errors arrived - replace old toasts instead of stacking.
			m_errorToasts.clear();
		}

		for (auto& err : errors)
		{
			// Split multi-error compiler output into individual errors.
			// daslang compiler outputs all errors as one string, each starting with "error[NNNNN]:"
			std::vector<std::string> individualErrors;
			std::regex errorPattern(R"(error\[\d+\]:)");
			auto begin = std::sregex_iterator(err.begin(), err.end(), errorPattern);
			auto end = std::sregex_iterator();
			
			if (begin == end)
			{
				// No error[NNNNN]: pattern found, treat as single error
				individualErrors.push_back(err);
			}
			else
			{
				// Split by error[NNNNN]: pattern
				std::size_t lastPos = 0;
				for (auto it = begin; it != end; ++it)
				{
					std::smatch match = *it;
					if (it == begin)
					{
						// First error - include everything from start to next error
						lastPos = match.position();
					}
					else
					{
						// Extract previous error block
						std::string prevError = err.substr(lastPos, match.position() - lastPos);
						individualErrors.push_back(prevError);
						lastPos = match.position();
					}
				}
				// Add the last error block
				individualErrors.push_back(err.substr(lastPos));
			}

			for (const auto& singleErr : individualErrors)
			{
				if (singleErr.empty()) continue;

				ScriptErrorToast toast;
				toast.message = singleErr;

				// Extract summary (first non-empty line).
				std::size_t pos = 0;
				while (pos < singleErr.size())
				{
					auto lineEnd = singleErr.find('\n', pos);
					if (lineEnd == std::string::npos) lineEnd = singleErr.size();
					std::string line = singleErr.substr(pos, lineEnd - pos);

					// Trim leading whitespace.
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

	void DebugLayer::DrawErrorToasts(LayerContext& context)
	{
		if (m_errorToasts.empty())
			return;

		const double now = context.elapsedTimeSeconds;

		if (m_errorToasts.empty())
			return;

		ImGuiViewport* vp    = ImGui::GetMainViewport();
		const float toastW   = std::min(520.f, vp->WorkSize.x * 0.7f);
		const float margin   = 16.f;
		const float anchorX  = vp->WorkPos.x + vp->WorkSize.x - margin;
		float       yOffset  = vp->WorkPos.y + vp->WorkSize.y - margin;

		constexpr std::size_t kMaxVisibleToasts = 3;
		constexpr float kToastStackGap = 4.f;

		const std::size_t visibleCount = std::min(m_errorToasts.size(), kMaxVisibleToasts);
		const std::size_t hiddenCount = m_errorToasts.size() - visibleCount;

		// Draw visible toasts (newest first, from back of deque)
		for (std::size_t i = 0; i < visibleCount; ++i)
		{
			ScriptErrorToast& toast = m_errorToasts[m_errorToasts.size() - 1 - i];

			char winId[64];
			std::snprintf(winId, sizeof(winId), "##toast_%p", &toast);

			ImGui::SetNextWindowPos(ImVec2(anchorX, yOffset), ImGuiCond_Always, ImVec2(1.f, 1.f));
			ImGui::SetNextWindowSize(ImVec2(toastW, 0.f), ImGuiCond_Always);

			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.14f, 0.04f, 0.04f, 1.f));
			ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.48f, 0.12f, 0.12f, 1.f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   5.f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(12.f, 10.f));

			constexpr ImGuiWindowFlags kFlags =
				ImGuiWindowFlags_NoTitleBar         |
				ImGuiWindowFlags_NoResize           |
				ImGuiWindowFlags_NoMove             |
				ImGuiWindowFlags_NoScrollbar        |
				ImGuiWindowFlags_NoScrollWithMouse  |
				ImGuiWindowFlags_NoSavedSettings    |
				ImGuiWindowFlags_NoDocking          |
				ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoNav;

			if (ImGui::Begin(winId, nullptr, kFlags))
			{
				// --- Header: title left, filename:line right ---
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.32f, 0.32f, 1.f));
				ImGui::TextUnformatted("SCRIPT ERROR");
				ImGui::PopStyleColor();

				if (!toast.filePath.empty())
				{
					const char* p  = toast.filePath.c_str();
					const char* sl = std::strrchr(p, '/');
					const char* bs = std::strrchr(p, '\\');
					const char* fn = (sl > bs ? sl : bs) ? (sl > bs ? sl : bs) + 1 : p;

					char loc[128];
					std::snprintf(loc, sizeof(loc), "%s:%d", fn, toast.line);

					const float locW = ImGui::CalcTextSize(loc).x;
					ImGui::SameLine();
					ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - locW);
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.36f, 0.36f, 1.f));
					ImGui::TextUnformatted(loc);
					ImGui::PopStyleColor();
				}

				ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.38f, 0.10f, 0.10f, 1.f));
				ImGui::Separator();
				ImGui::PopStyleColor();

				// --- Summary ---
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.87f, 0.80f, 1.f));
				ImGui::TextWrapped("%s", toast.summary.c_str());
				ImGui::PopStyleColor();
				ImGui::Spacing();

				// --- Callstack / full message (always visible) ---
				ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.09f, 0.09f, 1.f));
				ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(0.22f, 0.17f, 0.17f, 1.f));
				ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.f);

				if (ImGui::BeginChild("##stack", ImVec2(0.f, 120.f), true, ImGuiWindowFlags_HorizontalScrollbar))
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.62f, 0.57f, 1.f));
					ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 2.f));
					ImGui::TextUnformatted(toast.message.c_str());
					ImGui::PopStyleVar();
					ImGui::PopStyleColor();

					// Only auto-scroll if already at the bottom.
					if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f)
						ImGui::SetScrollHereY(1.f);
				}
				ImGui::EndChild();
				ImGui::PopStyleVar();
				ImGui::PopStyleColor(2);

				ImGui::Spacing();

				// --- Buttons ---
				ImGui::PushID(&toast);
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(9.f, 4.f));
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6.f, 4.f));

				// Pushes a consistent dark-red button style; caller pops 4 colors.
				auto PushRedBtn = [](float brightness) {
					const float b = brightness;
					ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.28f * b, 0.07f * b, 0.07f * b, 1.f));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f * b, 0.12f * b, 0.12f * b, 1.f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f * b, 0.18f * b, 0.18f * b, 1.f));
					ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.92f,     0.62f,     0.62f,     1.f));
				};

				PushRedBtn(1.f);
				if (ImGui::Button(toast.copied ? "Copied!" : "Copy"))
				{
					ImGui::SetClipboardText(toast.message.c_str());
					toast.copied = true;
					toast.copyFeedbackTime = now;
				}
				ImGui::PopStyleColor(4);

				if (toast.copied && (now - toast.copyFeedbackTime) > 1.5)
					toast.copied = false;

				ImGui::SameLine();

				PushRedBtn(1.f);
				if (ImGui::Button("Open in VS Code"))
					OpenInVSCode(toast.filePath, toast.line);
				ImGui::PopStyleColor(4);

				// Dismiss flush-right with exact calculated width.
				const float dismissW = ImGui::CalcTextSize("Dismiss").x
									+ ImGui::GetStyle().FramePadding.x * 2.f;
				ImGui::SameLine();
				ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - dismissW);

				PushRedBtn(1.3f); // slightly brighter to differentiate as destructive
				if (ImGui::Button("Dismiss"))
					toast.dismissed = true;
				ImGui::PopStyleColor(4);

				ImGui::PopStyleVar(3);
				ImGui::PopID();

				// --- Progress bar ---
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.58f, 0.14f, 0.14f, 1.f));
				ImGui::PushStyleColor(ImGuiCol_FrameBg,       ImVec4(0.10f, 0.03f, 0.03f, 1.f));
				ImGui::PopStyleColor(2);
			}
			ImGui::End();

			ImGui::PopStyleVar(3);
			ImGui::PopStyleColor(2);

			yOffset -= (ImGui::GetWindowHeight() / 1.6f) + kToastStackGap;
		}

		// Draw "X more errors" indicator above oldest visible toast
		if (hiddenCount > 0)
		{
			const float indicatorHeight = 60.f;
			yOffset -= indicatorHeight + kToastStackGap;

			char indicatorId[64];
			std::snprintf(indicatorId, sizeof(indicatorId), "##more_errors_%zu", hiddenCount);

			ImGui::SetNextWindowPos(ImVec2(anchorX, yOffset), ImGuiCond_Always, ImVec2(1.f, 1.f));
			ImGui::SetNextWindowSize(ImVec2(toastW, indicatorHeight));
			ImGui::SetNextWindowBgAlpha(0.97f);

			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.14f, 0.04f, 0.04f, 1.f));
			ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.48f, 0.12f, 0.12f, 1.f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   5.f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(12.f, 10.f));

			constexpr ImGuiWindowFlags kIndicatorFlags =
				ImGuiWindowFlags_NoTitleBar         |
				ImGuiWindowFlags_NoResize           |
				ImGuiWindowFlags_NoMove             |
				ImGuiWindowFlags_NoScrollbar        |
				ImGuiWindowFlags_NoScrollWithMouse  |
				ImGuiWindowFlags_NoSavedSettings    |
				ImGuiWindowFlags_NoDocking          |
				ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoNav;

			if (ImGui::Begin(indicatorId, nullptr, kIndicatorFlags))
			{
				// Center: "⚠ X more errors"
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.32f, 0.32f, 1.f));
				
				char moreText[64];
				std::snprintf(moreText, sizeof(moreText), "⚠  %zu more error%s", hiddenCount, hiddenCount > 1 ? "s" : "");
				
				const float textW = ImGui::CalcTextSize(moreText).x;
				const float textX = (toastW - ImGui::GetStyle().WindowPadding.x * 2.f - textW) * 0.5f;
				ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + textX);
				ImGui::TextUnformatted(moreText);
				
				ImGui::PopStyleColor();

				// Right: "Dismiss All" button
				const float dismissAllW = ImGui::CalcTextSize("Dismiss All").x + ImGui::GetStyle().FramePadding.x * 2.f;
				ImGui::SameLine();
				ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - dismissAllW);

				auto PushRedBtn = [](float brightness) {
					const float b = brightness;
					ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.28f * b, 0.07f * b, 0.07f * b, 1.f));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f * b, 0.12f * b, 0.12f * b, 1.f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f * b, 0.18f * b, 0.18f * b, 1.f));
					ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.92f,     0.62f,     0.62f,     1.f));
				};

				PushRedBtn(1.3f);
				if (ImGui::Button("Dismiss All"))
				{
					m_errorToasts.clear();
				}
				ImGui::PopStyleColor(4);
			}
			ImGui::End();

			ImGui::PopStyleVar(3);
			ImGui::PopStyleColor(2);
		}
	}

	// ── AppLayer overrides ────────────────────────────────────────────────────

	void DebugLayer::OnUpdate(LayerContext& context)
	{
		if (context.Get<Input>().IsKeyPressed(aether::Key::F1))
		{
			m_visible = !m_visible;
		}

		if (context.Get<Input>().IsKeyPressed(aether::Key::F5))
		{
			if (auto* scripting = context.TryGet<scripting::ScriptingSubsystem>())
			{
				scripting->RequestReload();
			}
		}

		
		if (context.Get<Input>().IsKeyPressed(aether::Key::F))
		{
			const bool enabled = !context.Get<Renderer>().IsFxaaEnabled();
			context.Get<Renderer>().SetFxaaEnabled(enabled);
			AE_INFO(aether::LogCategory::App, "FXAA: {}", enabled ? "on" : "off");
		}

		if (context.Get<Input>().IsKeyPressed(aether::Key::T))
		{
			const auto next = static_cast<aether::TonemapMode>((static_cast<int>(context.Get<Renderer>().GetTonemapMode()) + 1) % 3);
			context.Get<Renderer>().SetTonemapMode(next);
			const char* names[] = { "Reinhard", "ACES Filmic", "Uncharted2" };
			AE_INFO(aether::LogCategory::App, "Tonemap: {}", names[static_cast<int>(next)]);
		}

		PollScriptErrors(context);

		const float frameMs = static_cast<float>(context.deltaTimeSeconds * 1000.0);
		m_frameTimesMs[m_frameHistoryHead] = frameMs;
		m_frameHistoryHead = (m_frameHistoryHead + 1) % kFrameHistorySize;
		m_frameHistoryCount = std::min(m_frameHistoryCount + 1, kFrameHistorySize);
	}

	void DebugLayer::OnGui(LayerContext& context)
	{
		// Error toasts are always visible, regardless of m_visible.
		DrawErrorToasts(context);

		if (!m_visible)
		{
			return;
		}

		// Keep the panel anchored to the top-right with a fixed initial size.
		const ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.f, 12.f), ImGuiCond_FirstUseEver, ImVec2(1.f, 0.f));
		ImGui::SetNextWindowSize(ImVec2(420.f, 0.f), ImGuiCond_FirstUseEver);

		ImGui::Begin("DEBUG OVERLAY", &m_visible, ImGuiWindowFlags_NoCollapse);

		std::array<char, 128> buf{};

		// ── PERFORMANCE ────────────────────────────────────────────────────────
		ImGui::SeparatorText("PERFORMANCE");

		const float curMs = m_frameHistoryCount > 0 ? m_frameTimesMs[(m_frameHistoryHead + kFrameHistorySize - 1) % kFrameHistorySize] : static_cast<float>(context.deltaTimeSeconds * 1000.0);
		const float avgMs = GetAverageFrameTimeMs();
		const float minMs = GetMinFrameTimeMs();
		const float maxMs = GetMaxFrameTimeMs();
		const float curFps = curMs > 0.f ? 1000.f / curMs : 0.f;
		const float avgFps = avgMs > 0.f ? 1000.f / avgMs : 0.f;

		ImGui::Columns(2, "perf", false);

		std::snprintf(buf.data(), buf.size(), "#%llu", static_cast<unsigned long long>(context.frameIndex));
		ImGui::Text("Frame");
		ImGui::NextColumn();
		ImGui::TextUnformatted(buf.data());
		ImGui::NextColumn();

		std::snprintf(buf.data(), buf.size(), "%.1f", curFps);
		ImGui::Text("FPS");
		ImGui::NextColumn();
		ImGui::TextColored(ToImVec4(FpsColor(curFps)), "%s", buf.data());
		ImGui::NextColumn();

		std::snprintf(buf.data(), buf.size(), "%.2f ms", curMs);
		ImGui::Text("Delta");
		ImGui::NextColumn();
		ImGui::TextColored(ToImVec4(MsColor(curMs)), "%s", buf.data());
		ImGui::NextColumn();

		std::snprintf(buf.data(), buf.size(), "%.1f", avgFps);
		ImGui::Text("Avg FPS");
		ImGui::NextColumn();
		ImGui::TextColored(ToImVec4(FpsColor(avgFps)), "%s", buf.data());
		ImGui::NextColumn();

		std::snprintf(buf.data(), buf.size(), "%.2f ms", minMs);
		ImGui::Text("Min");
		ImGui::NextColumn();
		ImGui::TextColored(ToImVec4({ 0.40f, 0.72f, 0.46f, 1.f }), "%s", buf.data());
		ImGui::NextColumn();

		std::snprintf(buf.data(), buf.size(), "%.2f ms", maxMs);
		ImGui::Text("Max");
		ImGui::NextColumn();
		ImGui::TextColored(ToImVec4(MsColor(maxMs)), "%s", buf.data());
		ImGui::NextColumn();

		ImGui::Columns(1);

		ImGui::Spacing();
		ImGui::TextDisabled("Frame Time (0 - 33 ms)  /  ref: 60fps 30fps");
		DrawFrameTimeGraph();

		// ── RENDERER ──────────────────────────────────────────────────────────
		ImGui::SeparatorText("RENDERER");
		ImGui::Columns(2, "rend", false);

		ImGui::Text("Tonemap");
		ImGui::NextColumn();
		ImGui::TextUnformatted(GetTonemapModeName(context.Get<Renderer>().GetTonemapMode()));
		ImGui::NextColumn();

		const bool fxaa = context.Get<Renderer>().IsFxaaEnabled();
		ImGui::Text("FXAA");
		ImGui::NextColumn();
		ImGui::TextColored(fxaa ? ImVec4(0.4f, 0.72f, 0.46f, 1.f) : ImVec4(0.5f, 0.6f, 0.69f, 1.f), fxaa ? "On" : "Off");
		ImGui::NextColumn();

		const VkExtent2D ext = context.Get<Swapchain>().GetExtent();
		std::snprintf(buf.data(), buf.size(), "%u x %u", ext.width, ext.height);
		ImGui::Text("Resolution");
		ImGui::NextColumn();
		ImGui::TextUnformatted(buf.data());
		ImGui::NextColumn();

		ImGui::Columns(1);

		// ── CAMERA ────────────────────────────────────────────────────────────
		ImGui::SeparatorText("CAMERA");

		const aether::Camera* cam = context.Get<CameraManager>().TryGetMainCamera();
		if (cam)
		{
			ImGui::Columns(2, "cam", false);
			const glm::vec3 pos = cam->GetPosition();
			std::snprintf(buf.data(), buf.size(), "%.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
			ImGui::Text("Position");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();

			std::snprintf(buf.data(), buf.size(), "%.0f deg", cam->GetFovDegrees());
			ImGui::Text("FOV");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();

			std::snprintf(buf.data(), buf.size(), "%.2f", cam->GetNearPlane());
			ImGui::Text("Near");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();

			std::snprintf(buf.data(), buf.size(), "%.0f", cam->GetFarPlane());
			ImGui::Text("Far");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();
			ImGui::Columns(1);
		}
		else
		{
			ImGui::TextDisabled("No active camera");
		}

		// ── LIGHTING ──────────────────────────────────────────────────────────
		ImGui::SeparatorText("LIGHTING");

		{
			ImGui::Columns(2, "light", false);

			std::snprintf(buf.data(), buf.size(), "%zu", context.Get<Renderer>().GetPointLights().size());
			ImGui::Text("Point Lights");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();

			std::snprintf(buf.data(), buf.size(), "%zu", context.Get<Renderer>().GetSpotLights().size());
			ImGui::Text("Spot Lights");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();

			std::snprintf(buf.data(), buf.size(), "%.2f", context.Get<Renderer>().GetDirectionalLightIntensity());
			ImGui::Text("Sun Intensity");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();

			ImGui::Columns(1);
		}

		// ── SCRIPTING ─────────────────────────────────────────────────────────
		if (context.TryGet<scripting::ScriptingSubsystem>())
		{
			ImGui::SeparatorText("SCRIPTING");

			if (ImGui::Button("Reload Script [F5]"))
			{
				context.TryGet<scripting::ScriptingSubsystem>()->RequestReload();
			}
		}

		ImGui::End();
	}
} // namespace aether::app
