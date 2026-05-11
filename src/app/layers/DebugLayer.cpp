#include "DebugLayer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "utils/DebugGui.hpp"

#include "scene/AetherCore.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "platform/Input.hpp"
#include "utils/Logger.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/Renderer.hpp"

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

		void ColoredText(const char* label, const char* value, glm::vec4 valColor)
		{
			ImGui::TextUnformatted(label);
			ImGui::SameLine();
			ImGui::TextColored(ToImVec4(valColor), "%s", value);
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

	// ── AppLayer overrides ────────────────────────────────────────────────────

	void DebugLayer::OnUpdate(LayerContext& context)
	{
		if (context.input && context.input->IsKeyPressed(aether::Key::F1))
		{
			m_visible = !m_visible;
		}

		const float frameMs = static_cast<float>(context.deltaTimeSeconds * 1000.0);
		m_frameTimesMs[m_frameHistoryHead] = frameMs;
		m_frameHistoryHead = (m_frameHistoryHead + 1) % kFrameHistorySize;
		m_frameHistoryCount = std::min(m_frameHistoryCount + 1, kFrameHistorySize);
	}

	void DebugLayer::OnGui(LayerContext& context)
	{
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
		ImGui::TextUnformatted(GetTonemapModeName(context.engine.GetTonemapMode()));
		ImGui::NextColumn();

		const bool fxaa = context.renderer && context.renderer->IsFxaaEnabled();
		ImGui::Text("FXAA");
		ImGui::NextColumn();
		ImGui::TextColored(fxaa ? ImVec4(0.4f, 0.72f, 0.46f, 1.f) : ImVec4(0.5f, 0.6f, 0.69f, 1.f), fxaa ? "On" : "Off");
		ImGui::NextColumn();

		const VkExtent2D ext = context.engine.GetSwapchainExtent();
		std::snprintf(buf.data(), buf.size(), "%u x %u", ext.width, ext.height);
		ImGui::Text("Resolution");
		ImGui::NextColumn();
		ImGui::TextUnformatted(buf.data());
		ImGui::NextColumn();

		ImGui::Columns(1);

		// ── CAMERA ────────────────────────────────────────────────────────────
		ImGui::SeparatorText("CAMERA");

		const aether::Camera* cam = context.cameras ? context.cameras->TryGetMainCamera() : nullptr;
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

		if (context.renderer)
		{
			ImGui::Columns(2, "light", false);

			std::snprintf(buf.data(), buf.size(), "%zu", context.renderer->GetPointLights().size());
			ImGui::Text("Point Lights");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();

			std::snprintf(buf.data(), buf.size(), "%zu", context.renderer->GetSpotLights().size());
			ImGui::Text("Spot Lights");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();

			std::snprintf(buf.data(), buf.size(), "%.2f", context.renderer->GetDirectionalLightIntensity());
			ImGui::Text("Sun Intensity");
			ImGui::NextColumn();
			ImGui::TextUnformatted(buf.data());
			ImGui::NextColumn();

			ImGui::Columns(1);
		}

		ImGui::End();
	}
} // namespace aether::app
