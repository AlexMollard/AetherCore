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

		// Panel geometry — all x offsets from anchor top-right (1,0).
		constexpr glm::vec2 kAnchor{ 1.f, 0.f };
		constexpr float kPanelL = -432.0f;
		constexpr float kPanelR = -12.0f;
		constexpr float kPanelTop = 12.0f;
		constexpr float kPanelBot = 532.0f;
		constexpr float kInnerL = kPanelL + kPad;
		constexpr float kInnerR = kPanelR - kPad;

		// Two-column x positions
		constexpr float kColLKey = kInnerL;
		constexpr float kColLVal = -308.0f;
		constexpr float kColRKey = -204.0f;
		constexpr float kColRVal = -96.0f;

		// Graph-specific colours (not in OverlayStyle)
		constexpr glm::vec4 kColorGraphBg{ 0.04f, 0.05f, 0.07f, 0.97f };
		constexpr glm::vec4 kColorRef60{ 0.32f, 0.50f, 0.58f, 0.55f };
		constexpr glm::vec4 kColorRef30{ 0.58f, 0.38f, 0.28f, 0.45f };

		glm::vec4 FpsColor(float fps) noexcept
		{
			if (fps >= 55.0f)
				return kColorGood;
			if (fps >= 30.0f)
				return kColorWarn;
			return kColorBad;
		}

		glm::vec4 MsColor(float ms) noexcept
		{
			if (ms <= 16.667f)
				return kColorGood;
			if (ms <= 25.0f)
				return kColorWarn;
			return kColorBad;
		}
	} // namespace

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
			return 0.0f;
		float total = 0.0f;
		for (std::size_t i = 0; i < m_frameHistoryCount; ++i)
			total += m_frameTimesMs[i];
		return total / static_cast<float>(m_frameHistoryCount);
	}

	float DebugLayer::GetMinFrameTimeMs() const
	{
		if (m_frameHistoryCount == 0)
			return 0.0f;
		float v = m_frameTimesMs[0];
		for (std::size_t i = 1; i < m_frameHistoryCount; ++i)
			v = std::min(v, m_frameTimesMs[i]);
		return v;
	}

	float DebugLayer::GetMaxFrameTimeMs() const
	{
		if (m_frameHistoryCount == 0)
			return 0.0f;
		float v = m_frameTimesMs[0];
		for (std::size_t i = 1; i < m_frameHistoryCount; ++i)
			v = std::max(v, m_frameTimesMs[i]);
		return v;
	}

	void DebugLayer::DrawFrameTimeGraph(aether::UIRenderer& ui, float x, float y, float width, float height) const
	{
		constexpr float kGraphMaxMs = 33.333f;

		// Graph background
		ui.DrawRect(
		        aether::UiRect{
		                .anchorMin = {       1.f,        0.f },
		                .anchorMax = {       1.f,        0.f },
		                .offsetMinPx = {         x,          y },
		                .offsetMaxPx = { x + width, y + height },
        },
		        kColorGraphBg,
		        2.0f);

		// 33 ms reference line (30fps threshold) — near top of graph
		const float ref30Y = y + height * (1.0f - 33.333f / (kGraphMaxMs * 1.05f));
		ui.DrawLine(
		        aether::UiPoint{
		                .anchor = { 1.f,    0.f },
                          .offsetPx = {   x, ref30Y }
        },
		        aether::UiPoint{ .anchor = { 1.f, 0.f }, .offsetPx = { x + width, ref30Y } },
		        1.0f,
		        kColorRef30);

		// 16.67 ms reference line (60fps threshold) — mid graph
		const float ref60Y = y + height * 0.5f;
		ui.DrawLine(
		        aether::UiPoint{
		                .anchor = { 1.f,    0.f },
                          .offsetPx = {   x, ref60Y }
        },
		        aether::UiPoint{ .anchor = { 1.f, 0.f }, .offsetPx = { x + width, ref60Y } },
		        1.0f,
		        kColorRef60);

		if (m_frameHistoryCount == 0)
			return;

		const float barWidth = std::max(1.0f, width / static_cast<float>(kFrameHistorySize));
		for (std::size_t i = 0; i < m_frameHistoryCount; ++i)
		{
			const std::size_t idx = (m_frameHistoryHead + kFrameHistorySize - m_frameHistoryCount + i) % kFrameHistorySize;
			const float ms = m_frameTimesMs[idx];
			const float norm = std::clamp(ms / kGraphMaxMs, 0.02f, 1.0f);
			const float barH = norm * height;
			const float bx = x + static_cast<float>(i) * barWidth;
			ui.DrawRect(
			        aether::UiRect{
			                .anchorMin = { 1.f, 0.f },
			                .anchorMax = { 1.f, 0.f },
			                .offsetMinPx = { bx, y + height - barH },
			                .offsetMaxPx = { bx + std::max(1.0f, barWidth - 1.0f), y + height },
            },
			        MsColor(ms));
		}
	}

	void DebugLayer::OnAttach(LayerContext& context)
	{
		m_frameTimesMs.fill(0.0f);
		m_frameHistoryHead = 0;
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
		m_frameHistoryHead = (m_frameHistoryHead + 1) % kFrameHistorySize;
		m_frameHistoryCount = std::min(m_frameHistoryCount + 1, kFrameHistorySize);
	}

	void DebugLayer::OnGui(LayerContext& context)
	{
		if (context.ui == nullptr || context.renderer == nullptr || !m_visible)
			return;

		aether::UIRenderer& ui = *context.ui;
		std::array<char, 128> buf{};

		DrawPanel(ui, kAnchor, kPanelL, kPanelR, kPanelTop, kPanelBot);

		// Title
		ui.DrawText("DEBUG OVERLAY",
		        aether::UiPoint{
		                .anchor = kAnchor, .offsetPx = { kInnerL, kPanelTop + 26.0f }
        },
		        18.0f,
		        kColorTitle);

		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kPanelTop + 54.0f);

		constexpr float kPerfY = kPanelTop + 66.0f;
		DrawSectionHeader(ui, "PERFORMANCE", kAnchor, kPanelL, kInnerL, kPerfY);
		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kPerfY + 13.0f);

		const float curMs = m_frameHistoryCount > 0 ? m_frameTimesMs[(m_frameHistoryHead + kFrameHistorySize - 1) % kFrameHistorySize] : static_cast<float>(context.deltaTimeSeconds * 1000.0);
		const float avgMs = GetAverageFrameTimeMs();
		const float minMs = GetMinFrameTimeMs();
		const float maxMs = GetMaxFrameTimeMs();
		const float curFps = curMs > 0.f ? 1000.f / curMs : 0.f;
		const float avgFps = avgMs > 0.f ? 1000.f / avgMs : 0.f;

		constexpr float kR1 = kPerfY + 34.0f;
		std::snprintf(buf.data(), buf.size(), "#%llu", static_cast<unsigned long long>(context.frameIndex));
		DrawKV(ui, "Frame", buf.data(), kAnchor, kColLKey, kColLVal, kR1);
		std::snprintf(buf.data(), buf.size(), "%.2f ms", curMs);
		DrawKV(ui, "Delta", buf.data(), kAnchor, kColRKey, kColRVal, kR1, MsColor(curMs));

		constexpr float kR2 = kR1 + kRowH;
		std::snprintf(buf.data(), buf.size(), "%.1f", curFps);
		DrawKV(ui, "FPS", buf.data(), kAnchor, kColLKey, kColLVal, kR2, FpsColor(curFps));
		std::snprintf(buf.data(), buf.size(), "%.1f", avgFps);
		DrawKV(ui, "Avg FPS", buf.data(), kAnchor, kColRKey, kColRVal, kR2, FpsColor(avgFps));

		constexpr float kR3 = kR2 + kRowH;
		std::snprintf(buf.data(), buf.size(), "%.2f ms", minMs);
		DrawKV(ui, "Min", buf.data(), kAnchor, kColLKey, kColLVal, kR3, kColorGood);
		std::snprintf(buf.data(), buf.size(), "%.2f ms", maxMs);
		DrawKV(ui, "Max", buf.data(), kAnchor, kColRKey, kColRVal, kR3, MsColor(maxMs));

		constexpr float kGraphLabelY = kR3 + 22.0f;
		ui.DrawText("Frame Time  (0 - 33 ms)  /  ref: 60fps 30fps",
		        aether::UiPoint{
		                .anchor = kAnchor, .offsetPx = { kInnerL, kGraphLabelY }
        },
		        11.0f,
		        kColorLabel);

		constexpr float kGraphY = kGraphLabelY + 14.0f;
		constexpr float kGraphH = 72.0f;
		constexpr float kGraphW = kPanelR - kPanelL - 2.0f * kPad; // 388 px
		DrawFrameTimeGraph(ui, kInnerL, kGraphY, kGraphW, kGraphH);

		constexpr float kRendY = kGraphY + kGraphH + 22.0f;
		DrawSectionHeader(ui, "RENDERER", kAnchor, kPanelL, kInnerL, kRendY);
		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kRendY + 13.0f);

		constexpr float kRR1 = kRendY + 34.0f;
		DrawKV(ui, "Tonemap", GetTonemapModeName(context.engine.GetTonemapMode()), kAnchor, kColLKey, kColLVal, kRR1);
		DrawKV(ui, "FXAA", context.renderer->IsFxaaEnabled() ? "On" : "Off", kAnchor, kColRKey, kColRVal, kRR1, context.renderer->IsFxaaEnabled() ? kColorGood : kColorLabel);

		constexpr float kRR2 = kRR1 + kRowH;
		const VkExtent2D ext = context.engine.GetSwapchainExtent();
		std::snprintf(buf.data(), buf.size(), "%u x %u", ext.width, ext.height);
		DrawKV(ui, "Resolution", buf.data(), kAnchor, kColLKey, kColLVal, kRR2);

		constexpr float kCamY = kRR2 + 30.0f;
		DrawSectionHeader(ui, "CAMERA", kAnchor, kPanelL, kInnerL, kCamY);
		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kCamY + 13.0f);

		constexpr float kCR1 = kCamY + 34.0f;
		constexpr float kCR2 = kCR1 + kRowH;

		const aether::Camera* cam = context.cameras ? context.cameras->TryGetMainCamera() : nullptr;
		if (cam != nullptr)
		{
			const glm::vec3 pos = cam->GetPosition();
			std::snprintf(buf.data(), buf.size(), "%.1f, %.1f, %.1f", pos.x, pos.y, pos.z);
			DrawKV(ui, "Position", buf.data(), kAnchor, kColLKey, kColLVal, kCR1);

			std::snprintf(buf.data(), buf.size(), "%.0f deg", cam->GetFovDegrees());
			DrawKV(ui, "FOV", buf.data(), kAnchor, kColRKey, kColRVal, kCR1);

			std::snprintf(buf.data(), buf.size(), "%.2f", cam->GetNearPlane());
			DrawKV(ui, "Near", buf.data(), kAnchor, kColLKey, kColLVal, kCR2);

			std::snprintf(buf.data(), buf.size(), "%.0f", cam->GetFarPlane());
			DrawKV(ui, "Far", buf.data(), kAnchor, kColRKey, kColRVal, kCR2);
		}
		else
		{
			DrawValue(ui, "No active camera", kAnchor, kInnerL, kCR1, kColorLabel);
		}

		constexpr float kLightY = kCR2 + 30.0f;
		DrawSectionHeader(ui, "LIGHTING", kAnchor, kPanelL, kInnerL, kLightY);
		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kLightY + 13.0f);

		constexpr float kLR1 = kLightY + 34.0f;
		constexpr float kLR2 = kLR1 + kRowH;

		std::snprintf(buf.data(), buf.size(), "%zu", context.renderer->GetPointLights().size());
		DrawKV(ui, "Point Lights", buf.data(), kAnchor, kColLKey, kColLVal, kLR1);

		std::snprintf(buf.data(), buf.size(), "%zu", context.renderer->GetSpotLights().size());
		DrawKV(ui, "Spot Lights", buf.data(), kAnchor, kColRKey, kColRVal, kLR1);

		std::snprintf(buf.data(), buf.size(), "%.2f", context.renderer->GetDirectionalLightIntensity());
		DrawKV(ui, "Sun Intensity", buf.data(), kAnchor, kColLKey, kColLVal, kLR2);
	}
} // namespace aether::app
