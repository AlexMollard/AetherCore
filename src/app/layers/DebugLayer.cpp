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

#include "AetherCore.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "platform/Input.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiLayout.hpp"
#include "utils/Logger.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/Renderer.hpp"
#include "scripting/ScriptingSubsystem.hpp"
#include "vulkan/Swapchain.hpp"

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

	// ── Script error location ─────────────────────────────────────────────────

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
			std::vector<std::string> individualErrors;
			std::regex errorPattern(R"(error\[\d+\]:)");
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
					std::smatch match = *it;
					if (it == begin)
					{
						lastPos = match.position();
					}
					else
					{
						std::string prevError = err.substr(lastPos, match.position() - lastPos);
						individualErrors.push_back(prevError);
						lastPos = match.position();
					}
				}
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

	namespace
	{
		UiRect PxRect(float l, float t, float r, float b)
		{
			return UiRect{
				.anchorMin = { 0.f, 0.f },
				.anchorMax = { 0.f, 0.f },
				.offsetMinPx = { l, t },
				.offsetMaxPx = { r, b }
			};
		}
	} // namespace

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
		UIRenderer& ui = context.Get<UIRenderer>();
		const Input& input = context.Get<Input>();
		const VkExtent2D extent = context.Get<Swapchain>().GetExtent();
		const float sw = static_cast<float>(extent.width);
		const float sh = static_cast<float>(extent.height);

		const glm::vec4 green{ 0.40f, 0.72f, 0.46f, 1.f };
		const glm::vec4 yellow{ 0.86f, 0.71f, 0.30f, 1.f };
		const glm::vec4 red{ 0.80f, 0.33f, 0.30f, 1.f };
		const glm::vec4 white{ 0.93f, 0.93f, 0.93f, 1.f };
		const glm::vec4 dimmed{ 0.50f, 0.60f, 0.69f, 1.f };
		const glm::vec4 bg{ 0.08f, 0.08f, 0.11f, 0.92f };
		const glm::vec4 sectionFg{ 0.58f, 0.68f, 0.78f, 1.f };

		// ── Error notification bar (always visible) ──────────────────────────
		if (!m_errorToasts.empty())
		{
			constexpr float kBarHeight = 44.f;
			constexpr float kDismissW = 100.f;
			constexpr float kMargin = 16.f;
			const float barY = sh - kBarHeight - kMargin;

			// Background bar
			ui.DrawRect(PxRect(0.f, barY, sw, barY + kBarHeight), { 0.14f, 0.04f, 0.04f, 0.95f }, 4.f);

			// Error count text
			const std::size_t count = m_errorToasts.size();
			// Use the first error's summary or just "Script Errors"
			const std::string& summary = m_errorToasts[0].summary;
			std::array<char, 256> errText{};
			if (count == 1)
			{
				std::snprintf(errText.data(), errText.size(), "Script Error: %s", summary.c_str());
			}
			else
			{
				std::snprintf(errText.data(), errText.size(), "Script Errors (%zu): %s", count, summary.c_str());
			}

			const float textY = barY + (kBarHeight - 14.f) * 0.5f;
			ui.DrawText(errText.data(), UiPoint{ .anchor = { 0.f, 1.f }, .offsetPx = { kMargin, -kMargin - kBarHeight + (kBarHeight - 14.f) * 0.5f } }, 14.f, { 0.95f, 0.32f, 0.32f, 1.f });

			// Dismiss button
			const float dismissX = sw - kDismissW - kMargin;
			const float dismissTextX = sw - kDismissW - kMargin + 10.f;
			const float dismissY = barY + (kBarHeight - 24.f) * 0.5f;

			ui.DrawRect(PxRect(dismissX, dismissY, dismissX + kDismissW - 10.f, dismissY + 24.f), { 0.35f, 0.10f, 0.10f, 1.f }, 3.f);
			ui.DrawText("Dismiss All", UiPoint{ .anchor = { 0.f, 1.f }, .offsetPx = { dismissTextX, -kMargin - kBarHeight + (kBarHeight - 14.f) * 0.5f } }, 13.f, { 0.85f, 0.55f, 0.55f, 1.f });

			// Click detection for dismiss
			if (input.IsMouseButtonPressed(MouseButton::Left))
			{
				const glm::vec2 mp = input.GetMousePos();
				if (mp.x >= dismissX && mp.x <= dismissX + kDismissW - 10.f && mp.y >= dismissY && mp.y <= dismissY + 24.f)
				{
					m_errorToasts.clear();
				}
			}
		}

		if (!m_visible)
		{
			return;
		}

		// ── Debug overlay panel ──────────────────────────────────────────────
		const float panelX = sw - 420.f;
		const float panelY = 12.f;
		const float panelW = 408.f;
		const float col2X = panelX + 220.f;
		const float padX = 14.f;
		const float padY = 10.f;
		const float rowH = 18.f;
		const float sepH = 12.f;
		const float textSize = 13.f;

		std::array<char, 128> buf{};

		// Calculate panel height first (two-pass drawing)
		float contentH = padY;

		const float curMs = m_frameHistoryCount > 0 ? m_frameTimesMs[(m_frameHistoryHead + kFrameHistorySize - 1) % kFrameHistorySize] : static_cast<float>(context.deltaTimeSeconds * 1000.0);
		const float avgMs = GetAverageFrameTimeMs();
		const float minMs = GetMinFrameTimeMs();
		const float maxMs = GetMaxFrameTimeMs();
		const float curFps = curMs > 0.f ? 1000.f / curMs : 0.f;
		const float avgFps = avgMs > 0.f ? 1000.f / avgMs : 0.f;

		// Pass 1: measure
		auto addRow = [&]() { contentH += rowH; };
		auto addSep = [&]() { contentH += sepH; };
		contentH += rowH; // Frame
		contentH += rowH; // FPS
		contentH += rowH; // Delta
		contentH += rowH; // Avg FPS
		contentH += rowH; // Min
		contentH += rowH; // Max
		contentH += 76.f + 4.f; // histogram height + spacing
		addSep(); // separator before RENDERER
		contentH += rowH; // Tonemap
		contentH += rowH; // FXAA
		contentH += rowH; // Resolution
		addSep(); // separator before CAMERA
		contentH += rowH; // Position
		contentH += rowH; // FOV
		contentH += rowH; // Near
		contentH += rowH; // Far
		addSep(); // separator before LIGHTING
		contentH += rowH; // Point Lights
		contentH += rowH; // Spot Lights
		contentH += rowH; // Sun Intensity
		addSep(); // separator before SCRIPTING
		contentH += 30.f; // Reload button
		contentH += padY;

		const float panelH = contentH;

		// Background
		ui.DrawRect(PxRect(panelX, panelY, panelX + panelW, panelY + panelH), bg, 6.f);

		// Border accent
		ui.DrawRect(PxRect(panelX, panelY, panelX + 3.f, panelY + panelH), { 0.30f, 0.45f, 0.65f, 0.6f }, 6.f);

		float y = panelY + padY;

		auto drawLabelValue = [&](const char* label, const char* value, glm::vec4 valueColor)
		{
			ui.DrawText(label, UiPoint{ .anchor = { 0.f, 0.f }, .offsetPx = { panelX + padX, y } }, textSize, white);
			ui.DrawText(value, UiPoint{ .anchor = { 0.f, 0.f }, .offsetPx = { col2X, y } }, textSize, valueColor);
			y += rowH;
		};

		// ── PERFORMANCE ──────────────────────────────────────────────────────
		std::snprintf(buf.data(), buf.size(), "#%llu", static_cast<unsigned long long>(context.frameIndex));
		drawLabelValue("Frame", buf.data(), white);

		std::snprintf(buf.data(), buf.size(), "%.1f", curFps);
		drawLabelValue("FPS", buf.data(), FpsColor(curFps));

		std::snprintf(buf.data(), buf.size(), "%.2f ms", curMs);
		drawLabelValue("Delta", buf.data(), MsColor(curMs));

		std::snprintf(buf.data(), buf.size(), "%.1f", avgFps);
		drawLabelValue("Avg FPS", buf.data(), FpsColor(avgFps));

		std::snprintf(buf.data(), buf.size(), "%.2f ms", minMs);
		drawLabelValue("Min", buf.data(), green);

		std::snprintf(buf.data(), buf.size(), "%.2f ms", maxMs);
		drawLabelValue("Max", buf.data(), MsColor(maxMs));

		// Frame time histogram
		{
			const float chartX = panelX + padX;
			const float chartY = y;
			const float chartW = panelW - padX * 2.f;
			const float chartH = 76.f;

			ui.DrawRect(PxRect(chartX, chartY, chartX + chartW, chartY + chartH), { 0.12f, 0.12f, 0.15f, 1.f }, 3.f);

			if (m_frameHistoryCount > 0)
			{
				const float maxFrameTime = 33.333f;
				const float barW = chartW / static_cast<float>(kFrameHistorySize);
				for (std::size_t i = 0; i < kFrameHistorySize; ++i)
				{
					if (i >= m_frameHistoryCount)
					{
						continue;
					}
					const std::size_t idx = (m_frameHistoryHead + kFrameHistorySize - m_frameHistoryCount + i) % kFrameHistorySize;
					const float val = m_frameTimesMs[idx];
					const float barH = (std::min(val, maxFrameTime) / maxFrameTime) * chartH;
					const float bx = chartX + static_cast<float>(i) * barW;
					const float by = chartY + chartH - barH;

					glm::vec4 barColor{ 0.42f, 0.62f, 0.74f, 1.f };
					if (val > 25.f)
						barColor = { 0.80f, 0.33f, 0.30f, 1.f };
					else if (val > 16.667f)
						barColor = { 0.86f, 0.71f, 0.30f, 1.f };

					ui.DrawRect(PxRect(bx, by, bx + std::max(barW - 1.f, 1.f), chartY + chartH), barColor);
				}
			}

			// Reference lines at 16.667ms (60fps) and 33.333ms (30fps)
			const float refY60 = chartY + chartH - (16.667f / 33.333f * chartH);
			const float refY30 = chartY + chartH - (33.333f / 33.333f * chartH);
			ui.DrawLine(UiPoint{ .anchor = { 0.f, 0.f }, .offsetPx = { chartX, refY60 } }, UiPoint{ .anchor = { 0.f, 0.f }, .offsetPx = { chartX + chartW, refY60 } }, 1.f, { 0.40f, 0.72f, 0.46f, 0.4f });
			ui.DrawLine(UiPoint{ .anchor = { 0.f, 0.f }, .offsetPx = { chartX, refY30 } }, UiPoint{ .anchor = { 0.f, 0.f }, .offsetPx = { chartX + chartW, refY30 } }, 1.f, { 0.86f, 0.71f, 0.30f, 0.4f });

			// Bottom label
			std::snprintf(buf.data(), buf.size(), "Frame Time (0 - 33 ms)  |  avg: %.2f ms  |  ref: 60fps  30fps", avgMs);
			ui.DrawText(buf.data(), UiPoint{ .anchor = { 0.f, 0.f }, .offsetPx = { chartX, chartY + chartH + 2.f } }, 11.f, dimmed);

			y = chartY + chartH + 18.f;
		}

		// ── RENDERER ─────────────────────────────────────────────────────────
		y += 4.f;
		drawLabelValue("Tonemap", GetTonemapModeName(context.Get<Renderer>().GetTonemapMode()), white);

		const bool fxaa = context.Get<Renderer>().IsFxaaEnabled();
		drawLabelValue("FXAA", fxaa ? "On" : "Off", fxaa ? green : dimmed);

		const VkExtent2D ext = context.Get<Swapchain>().GetExtent();
		std::snprintf(buf.data(), buf.size(), "%u x %u", ext.width, ext.height);
		drawLabelValue("Resolution", buf.data(), white);

		// ── CAMERA ───────────────────────────────────────────────────────────
		y += 4.f;
		const aether::Camera* cam = context.Get<CameraManager>().TryGetMainCamera();
		if (cam)
		{
			const glm::vec3 pos = cam->GetPosition();
			std::snprintf(buf.data(), buf.size(), "%.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
			drawLabelValue("Position", buf.data(), white);

			std::snprintf(buf.data(), buf.size(), "%.0f deg", cam->GetFovDegrees());
			drawLabelValue("FOV", buf.data(), white);

			std::snprintf(buf.data(), buf.size(), "%.2f", cam->GetNearPlane());
			drawLabelValue("Near", buf.data(), white);

			std::snprintf(buf.data(), buf.size(), "%.0f", cam->GetFarPlane());
			drawLabelValue("Far", buf.data(), white);
		}
		else
		{
			y += rowH; // Position placeholder
			ui.DrawText("No active camera", UiPoint{ .anchor = { 0.f, 0.f }, .offsetPx = { panelX + padX, y - rowH * 3.f } }, textSize, dimmed);
		}

		// ── LIGHTING ─────────────────────────────────────────────────────────
		y += 4.f;
		std::snprintf(buf.data(), buf.size(), "%zu", context.Get<Renderer>().GetPointLights().size());
		drawLabelValue("Point Lights", buf.data(), white);

		std::snprintf(buf.data(), buf.size(), "%zu", context.Get<Renderer>().GetSpotLights().size());
		drawLabelValue("Spot Lights", buf.data(), white);

		std::snprintf(buf.data(), buf.size(), "%.2f", context.Get<Renderer>().GetDirectionalLightIntensity());
		drawLabelValue("Sun Intensity", buf.data(), white);

		// ── SCRIPTING ────────────────────────────────────────────────────────
		y += 4.f;
		if (context.TryGet<scripting::ScriptingSubsystem>())
		{
			const float btnX = panelX + padX;
			const float btnY = y;
			const float btnW = panelW - padX * 2.f;
			const float btnH = 28.f;

			// Button background
			const bool hovered = input.IsMouseButtonDown(MouseButton::Left) ?
				false :
				(input.GetMousePos().x >= btnX && input.GetMousePos().x <= btnX + btnW &&
					input.GetMousePos().y >= btnY && input.GetMousePos().y <= btnY + btnH);

			const glm::vec4 btnColor = hovered ? glm::vec4{ 0.25f, 0.30f, 0.40f, 1.f } : glm::vec4{ 0.20f, 0.24f, 0.30f, 1.f };
			ui.DrawRect(PxRect(btnX, btnY, btnX + btnW, btnY + btnH), btnColor, 4.f);

			// Button text centered
			constexpr const char* kBtnText = "Reload Script  [F5]";
			const float textW = ui.MeasureText(kBtnText, 13.f);
			const float textBX = btnX + (btnW - textW) * 0.5f;
			const float textBY = btnY + (btnH - textSize) * 0.5f;
			ui.DrawText(kBtnText, UiPoint{ .anchor = { 0.f, 0.f }, .offsetPx = { textBX, textBY } }, textSize, white);

			// Click detection
			if (input.IsMouseButtonPressed(MouseButton::Left) &&
				input.GetMousePos().x >= btnX && input.GetMousePos().x <= btnX + btnW &&
				input.GetMousePos().y >= btnY && input.GetMousePos().y <= btnY + btnH)
			{
				context.TryGet<scripting::ScriptingSubsystem>()->RequestReload();
			}
		}
	}
} // namespace aether::app
