#include "DebugLayer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include <glm/glm.hpp>

#include "AetherCore.hpp"
#include "Logger.hpp"
#include "PostProcessStack.hpp"
#include "UIRenderer.hpp"
#include "UiLayout.hpp"

namespace aether::app
{
	const char* DebugLayer::GetTonemapModeName(aether::TonemapMode mode)
	{
		switch (mode)
		{
		case aether::TonemapMode::Reinhard:   return "Reinhard";
		case aether::TonemapMode::AcesFilmic: return "ACES Filmic";
		case aether::TonemapMode::Uncharted2: return "Uncharted2";
		default:                              return "Unknown";
		}
	}

	float DebugLayer::GetAverageFrameTimeMs() const
	{
		if (m_frameHistoryCount == 0)
		{
			return 0.0f;
		}

		float totalMs = 0.0f;
		for (std::size_t i = 0; i < m_frameHistoryCount; ++i)
		{
			totalMs += m_frameTimesMs[i];
		}

		return totalMs / static_cast<float>(m_frameHistoryCount);
	}

	void DebugLayer::DrawDebugLine(aether::UIRenderer& ui, std::string_view text, float y) const
	{
		ui.DrawText(
			text,
			aether::UiPoint{
				.anchor = { 1.0f, 0.0f },
				.offsetPx = { -344.0f, y },
			},
			18.0f,
			glm::vec4(0.92f, 0.95f, 0.97f, 1.0f));
	}

	void DebugLayer::DrawFrameTimeGraph(aether::UIRenderer& ui, float x, float y, float width, float height) const
	{
		ui.DrawQuad(
			aether::UiRect{
				.anchorMin = { 1.0f, 0.0f },
				.anchorMax = { 1.0f, 0.0f },
				.offsetMinPx = { x, y },
				.offsetMaxPx = { x + width, y + height },
			},
			glm::vec4(0.05f, 0.07f, 0.09f, 0.90f));

		if (m_frameHistoryCount == 0)
		{
			return;
		}

		constexpr float graphMaxMs = 33.333f;
		const float barWidth = std::max(1.0f, width / static_cast<float>(kFrameHistorySize));

		for (std::size_t i = 0; i < m_frameHistoryCount; ++i)
		{
			const std::size_t historyIndex = (m_frameHistoryHead + kFrameHistorySize - m_frameHistoryCount + i) % kFrameHistorySize;
			const float frameMs = m_frameTimesMs[historyIndex];
			const float normalizedHeight = std::clamp(frameMs / graphMaxMs, 0.05f, 1.0f);
			const float barHeight = normalizedHeight * height;
			const float barX = x + static_cast<float>(i) * barWidth;
			const glm::vec4 barColor = frameMs <= 16.667f
				? glm::vec4(0.42f, 0.78f, 0.52f, 0.95f)
				: (frameMs <= 25.0f ? glm::vec4(0.91f, 0.74f, 0.33f, 0.95f) : glm::vec4(0.86f, 0.38f, 0.34f, 0.95f));

			ui.DrawQuad(
				aether::UiRect{
					.anchorMin = { 1.0f, 0.0f },
					.anchorMax = { 1.0f, 0.0f },
					.offsetMinPx = { barX, y + height - barHeight },
					.offsetMaxPx = { barX + std::max(1.0f, barWidth - 1.0f), y + height },
				},
				barColor);
		}
	}

	void DebugLayer::OnAttach(LayerContext& context)
	{
		m_frameTimesMs.fill(0.0f);
		m_frameHistoryHead = 0;
		m_frameHistoryCount = 0;
		INFO(LogCategory::App, "Debug layer attached.");
		(void)context;
	}

	void DebugLayer::OnDetach(LayerContext& context)
	{
		INFO(LogCategory::App, "Debug layer detached.");
		(void)context;
	}

	void DebugLayer::OnUpdate(LayerContext& context)
	{
		const float frameMs = static_cast<float>(context.deltaTimeSeconds * 1000.0);
		m_frameTimesMs[m_frameHistoryHead] = frameMs;
		m_frameHistoryHead = (m_frameHistoryHead + 1) % kFrameHistorySize;
		m_frameHistoryCount = std::min(m_frameHistoryCount + 1, kFrameHistorySize);
	}

	void DebugLayer::OnGui(LayerContext& context)
	{
		if (context.ui == nullptr || context.renderer == nullptr)
		{
			return;
		}

		context.ui->DrawQuad(
			aether::UiRect{
				.anchorMin = { 1.0f, 0.0f },
				.anchorMax = { 1.0f, 0.0f },
				.offsetMinPx = { -396.0f, 12.0f },
				.offsetMaxPx = { -12.0f, 270.0f },
			},
			glm::vec4(0.08f, 0.11f, 0.14f, 0.82f));

		context.ui->DrawText(
			"Debug Overlay",
			aether::UiPoint{
				.anchor = { 1.0f, 0.0f },
				.offsetPx = { -380.0f, 42.0f },
			},
			28.0f,
			glm::vec4(0.95f, 0.90f, 0.68f, 1.0f));

		std::array<char, 128> line{};
		const float currentFrameMs = m_frameHistoryCount > 0
			? m_frameTimesMs[(m_frameHistoryHead + kFrameHistorySize - 1) % kFrameHistorySize]
			: static_cast<float>(context.deltaTimeSeconds * 1000.0);
		const float averageFrameMs = GetAverageFrameTimeMs();
		const float fps = currentFrameMs > 0.0f ? 1000.0f / currentFrameMs : 0.0f;
		const float averageFps = averageFrameMs > 0.0f ? 1000.0f / averageFrameMs : 0.0f;

		std::snprintf(line.data(), line.size(), "Frame: %llu", static_cast<unsigned long long>(context.frameIndex));
		DrawDebugLine(*context.ui, line.data(), 82.0f);

		std::snprintf(line.data(), line.size(), "Delta: %.2f ms | FPS: %.1f (avg %.1f)", currentFrameMs, fps, averageFps);
		DrawDebugLine(*context.ui, line.data(), 106.0f);

		std::snprintf(line.data(), line.size(), "Tonemap: %s", GetTonemapModeName(context.engine.GetTonemapMode()));
		DrawDebugLine(*context.ui, line.data(), 130.0f);

		std::snprintf(line.data(), line.size(), "FXAA: %s", context.renderer->IsFxaaEnabled() ? "On" : "Off");
		DrawDebugLine(*context.ui, line.data(), 154.0f);

		DrawDebugLine(*context.ui, "Frame Time Graph (0-33 ms)", 186.0f);
		DrawFrameTimeGraph(*context.ui, -380.0f, 202.0f, 340.0f, 44.0f);
	}
}
