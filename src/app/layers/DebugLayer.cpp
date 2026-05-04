#include "DebugLayer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <glm/glm.hpp>

#include "AetherCore.hpp"
#include "Camera.hpp"
#include "CameraManager.hpp"
#include "Input.hpp"
#include "Logger.hpp"
#include "OverlayStyle.hpp"
#include "PostProcessStack.hpp"
#include "UiLayout.hpp"
#include "UIRenderer.hpp"

namespace aether::app
{
	namespace
	{
		using namespace overlay;

		// Panel geometry — all x offsets from the top-right anchor (1, 0).
		constexpr glm::vec2 kAnchor{ 1.f, 0.f };
		constexpr float kPanelL = -432.0f;
		constexpr float kPanelR = -12.0f;
		constexpr float kPanelTop = 12.0f;

		// Two-column x positions (shared by PanelBuilder ColumnDefs and the graph helper).
		constexpr float kColLKey = kPanelL + kPad; // = -416
		constexpr float kColLVal = -308.0f;
		constexpr float kColRKey = -204.0f;
		constexpr float kColRVal = -96.0f;

		// Graph-specific colours
		constexpr glm::vec4 kColorGraphBg{ 0.04f, 0.05f, 0.07f, 0.97f };
		constexpr glm::vec4 kColorRef60{ 0.32f, 0.50f, 0.58f, 0.55f };
		constexpr glm::vec4 kColorRef30{ 0.58f, 0.38f, 0.28f, 0.45f };

		glm::vec4 FpsColor(float fps) noexcept
		{
			if (fps >= 55.0f) return kColorGood;
			if (fps >= 30.0f) return kColorWarn;
			return kColorBad;
		}

		glm::vec4 MsColor(float ms) noexcept
		{
			if (ms <= 16.667f) return kColorGood;
			if (ms <= 25.0f)   return kColorWarn;
			return kColorBad;
		}
	} // namespace

	const char* DebugLayer::GetTonemapModeName(aether::TonemapMode mode)
	{
		switch (mode)
		{
			case aether::TonemapMode::Reinhard:    return "Reinhard";
			case aether::TonemapMode::AcesFilmic:  return "ACES Filmic";
			case aether::TonemapMode::Uncharted2:  return "Uncharted2";
			default:                               return "Unknown";
		}
	}

	float DebugLayer::GetAverageFrameTimeMs() const
	{
		if (m_frameHistoryCount == 0) return 0.0f;
		float total = 0.0f;
		for (std::size_t i = 0; i < m_frameHistoryCount; ++i)
			total += m_frameTimesMs[i];
		return total / static_cast<float>(m_frameHistoryCount);
	}

	float DebugLayer::GetMinFrameTimeMs() const
	{
		if (m_frameHistoryCount == 0) return 0.0f;
		float v = m_frameTimesMs[0];
		for (std::size_t i = 1; i < m_frameHistoryCount; ++i)
			v = std::min(v, m_frameTimesMs[i]);
		return v;
	}

	float DebugLayer::GetMaxFrameTimeMs() const
	{
		if (m_frameHistoryCount == 0) return 0.0f;
		float v = m_frameTimesMs[0];
		for (std::size_t i = 1; i < m_frameHistoryCount; ++i)
			v = std::max(v, m_frameTimesMs[i]);
		return v;
	}

	void DebugLayer::DrawFrameTimeGraph(aether::UIRenderer& ui, float x, float y,
	                                    float width, float height) const
	{
		constexpr float kGraphMaxMs = 33.333f;

		ui.DrawRect(
		        aether::UiRect{
		                .anchorMin   = { 1.f, 0.f },
		                .anchorMax   = { 1.f, 0.f },
		                .offsetMinPx = { x,         y          },
		                .offsetMaxPx = { x + width, y + height },
		        },
		        kColorGraphBg, 2.0f);

		const float ref30Y = y + height * (1.0f - 33.333f / (kGraphMaxMs * 1.05f));
		ui.DrawLine(
		        aether::UiPoint{ .anchor = { 1.f, 0.f }, .offsetPx = { x,         ref30Y } },
		        aether::UiPoint{ .anchor = { 1.f, 0.f }, .offsetPx = { x + width, ref30Y } },
		        1.0f, kColorRef30);

		const float ref60Y = y + height * 0.5f;
		ui.DrawLine(
		        aether::UiPoint{ .anchor = { 1.f, 0.f }, .offsetPx = { x,         ref60Y } },
		        aether::UiPoint{ .anchor = { 1.f, 0.f }, .offsetPx = { x + width, ref60Y } },
		        1.0f, kColorRef60);

		if (m_frameHistoryCount == 0)
			return;

		const float barWidth = std::max(1.0f, width / static_cast<float>(kFrameHistorySize));
		for (std::size_t i = 0; i < m_frameHistoryCount; ++i)
		{
			const std::size_t idx = (m_frameHistoryHead + kFrameHistorySize - m_frameHistoryCount + i) % kFrameHistorySize;
			const float ms   = m_frameTimesMs[idx];
			const float norm = std::clamp(ms / kGraphMaxMs, 0.02f, 1.0f);
			const float barH = norm * height;
			const float bx   = x + static_cast<float>(i) * barWidth;
			ui.DrawRect(
			        aether::UiRect{
			                .anchorMin   = { 1.f, 0.f },
			                .anchorMax   = { 1.f, 0.f },
			                .offsetMinPx = { bx,                                       y + height - barH },
			                .offsetMaxPx = { bx + std::max(1.0f, barWidth - 1.0f), y + height       },
			        },
			        MsColor(ms));
		}
	}

	void DebugLayer::OnAttach(LayerContext& context)
	{
		m_frameTimesMs.fill(0.0f);
		m_frameHistoryHead  = 0;
		m_frameHistoryCount = 0;
		INFO(LogCategory::App, "Debug layer attached.");
		(void) context;
	}

	void DebugLayer::OnDetach(LayerContext& context)
	{
		INFO(LogCategory::App, "Debug layer detached.");
		(void) context;
	}

	void DebugLayer::OnUpdate(LayerContext& context)
	{
		if (context.input && context.input->IsKeyPressed(aether::Key::F1))
			m_visible = !m_visible;

		const float frameMs = static_cast<float>(context.deltaTimeSeconds * 1000.0);
		m_frameTimesMs[m_frameHistoryHead] = frameMs;
		m_frameHistoryHead  = (m_frameHistoryHead + 1) % kFrameHistorySize;
		m_frameHistoryCount = std::min(m_frameHistoryCount + 1, kFrameHistorySize);
	}

	void DebugLayer::OnGui(LayerContext& context)
	{
		if (context.ui == nullptr || context.renderer == nullptr || !m_visible)
			return;

		aether::UIRenderer& ui = *context.ui;
		std::array<char, 128> buf{};

		// Two-column layout: left col for primary metric, right col for secondary.
		PanelBuilder panel(ui, kAnchor, kPanelL, kPanelR, kPanelTop,
		        { PanelBuilder::ColumnDef{ kColLKey, kColLVal },
		          PanelBuilder::ColumnDef{ kColRKey, kColRVal } });

		panel.Title("DEBUG OVERLAY").Section("PERFORMANCE");

		const float curMs = m_frameHistoryCount > 0
		        ? m_frameTimesMs[(m_frameHistoryHead + kFrameHistorySize - 1) % kFrameHistorySize]
		        : static_cast<float>(context.deltaTimeSeconds * 1000.0);
		const float avgMs  = GetAverageFrameTimeMs();
		const float minMs  = GetMinFrameTimeMs();
		const float maxMs  = GetMaxFrameTimeMs();
		const float curFps = curMs > 0.f ? 1000.f / curMs : 0.f;
		const float avgFps = avgMs > 0.f ? 1000.f / avgMs : 0.f;

		std::snprintf(buf.data(), buf.size(), "#%llu", static_cast<unsigned long long>(context.frameIndex));
		panel.KVCol(0, "Frame", buf.data());
		std::snprintf(buf.data(), buf.size(), "%.2f ms", curMs);
		panel.KVCol(1, "Delta", buf.data(), MsColor(curMs));
		panel.NextRow();

		std::snprintf(buf.data(), buf.size(), "%.1f", curFps);
		panel.KVCol(0, "FPS", buf.data(), FpsColor(curFps));
		std::snprintf(buf.data(), buf.size(), "%.1f", avgFps);
		panel.KVCol(1, "Avg FPS", buf.data(), FpsColor(avgFps));
		panel.NextRow();

		std::snprintf(buf.data(), buf.size(), "%.2f ms", minMs);
		panel.KVCol(0, "Min", buf.data(), kColorGood);
		std::snprintf(buf.data(), buf.size(), "%.2f ms", maxMs);
		panel.KVCol(1, "Max", buf.data(), MsColor(maxMs));
		panel.NextRow();

		// Graph label then graph as custom content blocks.
		constexpr float kGraphH = 72.0f;
		constexpr float kGraphW = kPanelR - kPanelL - 2.0f * kPad;

		panel.Custom(14.f, [&](float y) {
			ui.DrawText("Frame Time  (0 - 33 ms)  /  ref: 60fps 30fps",
			        aether::UiPoint{ .anchor = kAnchor, .offsetPx = { panel.InnerL(), y } },
			        11.0f, kColorLabel);
		});
		panel.Custom(kGraphH, [&](float y) {
			DrawFrameTimeGraph(ui, panel.InnerL(), y, kGraphW, kGraphH);
		});

		panel.Section("RENDERER");

		panel.KVCol(0, "Tonemap", GetTonemapModeName(context.engine.GetTonemapMode()));
		panel.KVCol(1, "FXAA",
		        context.renderer->IsFxaaEnabled() ? "On" : "Off",
		        context.renderer->IsFxaaEnabled() ? kColorGood : kColorLabel);
		panel.NextRow();

		const VkExtent2D ext = context.engine.GetSwapchainExtent();
		std::snprintf(buf.data(), buf.size(), "%u x %u", ext.width, ext.height);
		panel.KVCol(0, "Resolution", buf.data());
		panel.NextRow();

		panel.Section("CAMERA");

		const aether::Camera* cam = context.cameras ? context.cameras->TryGetMainCamera() : nullptr;
		if (cam != nullptr)
		{
			const glm::vec3 pos = cam->GetPosition();
			std::snprintf(buf.data(), buf.size(), "%.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
			panel.KVCol(0, "Position", buf.data());
			std::snprintf(buf.data(), buf.size(), "%.0f deg", cam->GetFovDegrees());
			panel.KVCol(1, "FOV", buf.data());
			panel.NextRow();

			std::snprintf(buf.data(), buf.size(), "%.2f", cam->GetNearPlane());
			panel.KVCol(0, "Near", buf.data());
			std::snprintf(buf.data(), buf.size(), "%.0f", cam->GetFarPlane());
			panel.KVCol(1, "Far", buf.data());
			panel.NextRow();
		}
		else
		{
			panel.Value("No active camera", kColorLabel);
		}

		panel.Section("LIGHTING");

		std::snprintf(buf.data(), buf.size(), "%zu", context.renderer->GetPointLights().size());
		panel.KVCol(0, "Point Lights", buf.data());
		std::snprintf(buf.data(), buf.size(), "%zu", context.renderer->GetSpotLights().size());
		panel.KVCol(1, "Spot Lights", buf.data());
		panel.NextRow();

		std::snprintf(buf.data(), buf.size(), "%.2f", context.renderer->GetDirectionalLightIntensity());
		panel.KV("Sun Intensity", buf.data());
	}
} // namespace aether::app
