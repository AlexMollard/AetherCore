#include "PerformancePanel.hpp"

#include <algorithm>
#include <format>

#include <imgui.h>

#include "layers/AppLayer.hpp"
#include "Color.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "utils/Profiler.hpp"

namespace aether::app
{
	void PerformancePanel::PushFrameSample(float frameMs)
	{
		m_frameSamples[m_frameSampleHead] = frameMs;
		m_frameSampleHead = (m_frameSampleHead + 1) % kFrameSampleCount;
		m_frameSampleCount = std::min(m_frameSampleCount + 1, kFrameSampleCount);
	}

	void PerformancePanel::OnUpdate(LayerContext& context)
	{
		PushFrameSample(static_cast<float>(context.deltaTimeSeconds * 1000.0));
	}

	void PerformancePanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		const float curMs = m_frameSampleCount > 0 ? m_frameSamples[(m_frameSampleHead + m_frameSamples.size() - 1) % m_frameSamples.size()] : static_cast<float>(context.deltaTimeSeconds * 1000.0);
		float totalMs = 0.0f;
		float minMs = curMs;
		float maxMs = curMs;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		for (std::size_t i = 0; i < m_frameSampleCount; ++i)
		{
			const std::size_t idx = (m_frameSampleHead + m_frameSamples.size() - m_frameSampleCount + i) % m_frameSamples.size();
			const float sample = m_frameSamples[idx];
			m_orderedSamples[i] = sample;
			totalMs += sample;
			minMs = std::min(minMs, sample);
			maxMs = std::max(maxMs, sample);
		}

		const float avgMs = m_frameSampleCount > 0 ? totalMs / static_cast<float>(m_frameSampleCount) : curMs;
		const float curFps = curMs > 0.0f ? 1000.0f / curMs : 0.0f;
		const float avgFps = avgMs > 0.0f ? 1000.0f / avgMs : 0.0f;

		if (m_frameSampleCount > 0)
		{
			m_sorted = m_orderedSamples;
			std::sort(m_sorted.begin(), m_sorted.begin() + static_cast<std::ptrdiff_t>(m_frameSampleCount));
			const std::size_t count = m_frameSampleCount;
			p95Ms = m_sorted[static_cast<std::size_t>(static_cast<float>(count) * 0.95f) % count];
			p99Ms = m_sorted[static_cast<std::size_t>(static_cast<float>(count) * 0.99f) % count];
		}

		m_titleAccum += static_cast<float>(context.deltaTimeSeconds);
		if (m_titleFps == 0.0f || m_titleAccum >= kTitleUpdateInterval)
		{
			m_titleFps = avgFps;
			m_titleMs = avgMs;
			m_titleAccum = 0.0f;
		}

		ImGui::Begin(std::format("Performance  |  {:.0f} FPS  |  {:.2f} ms###Performance", m_titleFps, m_titleMs).c_str(), VisiblePtr());

		// Compact 4-column stats grid
		if (ImGui::BeginTable("PerfStats", 4, ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted("Frame");
			ImGui::SameLine();
			ImGui::Text("#%u", context.frameIndex);

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted("FPS ");
			ImGui::SameLine();
			ImGui::TextColored(FpsColor(curFps), "%.1f", curFps);
			ImGui::SameLine();
			ImGui::TextDisabled("(%.1f)", avgFps);

			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted("Delta");
			ImGui::SameLine();
			ImGui::TextColored(MsColor(curMs), "%.2fms", curMs);

			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted("Render CPU");
			ImGui::SameLine();
			float renderCpuMs = -1.0f;
			if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
			{
				renderCpuMs = imgui->GetLastRenderCpuTimeMs();
			}
			if (renderCpuMs >= 0.0f)
			{
				ImGui::TextColored(MsColor(renderCpuMs), "%.3fms", renderCpuMs);
			}
			else
			{
				ImGui::TextDisabled("N/A");
			}

			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted("Avg");
			ImGui::SameLine();
			ImGui::TextColored(MsColor(avgMs), "%.2fms", avgMs);

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted("Min");
			ImGui::SameLine();
			ImGui::TextColored(ImVec4{colors::Success.r, colors::Success.g, colors::Success.b, colors::Success.a}, "%.2fms", minMs);

			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted("Max");
			ImGui::SameLine();
			ImGui::TextColored(MsColor(maxMs), "%.2fms", maxMs);

			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted("P95 ");
			ImGui::SameLine();
			ImGui::TextColored(MsColor(p95Ms), "%.2f", p95Ms);
			ImGui::SameLine();
			ImGui::TextUnformatted("  P99 ");
			ImGui::SameLine();
			ImGui::TextColored(MsColor(p99Ms), "%.2f", p99Ms);

			ImGui::EndTable();
		}

		// Frame time distribution histogram
		if (m_frameSampleCount > 0)
		{
			constexpr int kBucketCount = 8;
			constexpr float kBucketThresholds[kBucketCount] = {2.0f, 4.0f, 8.333f, 12.0f, 16.667f, 33.333f, 50.0f, 100.0f};
			const ImU32 kBucketColors[kBucketCount] = {
			        ToU32(colors::HistFastest),
			        ToU32(colors::HistFast),
			        ToU32(colors::HistFair),
			        ToU32(colors::HistOkay),
			        ToU32(colors::HistSlow),
			        ToU32(colors::HistSlower),
			        ToU32(colors::HistBad),
			        ToU32(colors::HistTerrible),
			};
			constexpr const char* kBucketLabels[kBucketCount] = {"<2", "<4", "<8.33", "<12", "<16.67", "<33.33", "<50", ">50"};

			int bucketCounts[kBucketCount] = {};
			for (std::size_t i = 0; i < m_frameSampleCount; ++i)
			{
				const float ms = m_orderedSamples[i];
				for (int b = 0; b < kBucketCount; ++b)
				{
					if (ms < kBucketThresholds[b])
					{
						++bucketCounts[b];
						break;
					}
				}
			}

			const int maxBucket = *std::ranges::max_element(bucketCounts);
			const float barWidth = (ImGui::GetContentRegionAvail().x - static_cast<float>(kBucketCount) * 4.0f) / static_cast<float>(kBucketCount);
			if (barWidth > 0.0f)
			{
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const ImVec2 origin = ImGui::GetCursorScreenPos();
				const float barMaxHeight = 60.0f;
				const float barMinHeight = 4.0f;

				for (int b = 0; b < kBucketCount; ++b)
				{
					const float t = maxBucket > 0 ? static_cast<float>(bucketCounts[b]) / static_cast<float>(maxBucket) : 0.0f;
					const float h = barMinHeight + t * (barMaxHeight - barMinHeight);
					const ImVec2 bMin(origin.x + static_cast<float>(b) * (barWidth + 4.0f), origin.y + barMaxHeight - h);
					const ImVec2 bMax(bMin.x + barWidth, origin.y + barMaxHeight);
					dl->AddRectFilled(bMin, bMax, kBucketColors[b], 3.0f);
					dl->AddRect(bMin, bMax, ToU32(colors::Border), 3.0f);

					const float labelY = origin.y + barMaxHeight + 2.0f;
					const char* label = kBucketLabels[b];
					const ImVec2 labelSize = ImGui::CalcTextSize(label);
					dl->AddText(ImVec2(bMin.x + (barWidth - labelSize.x) * 0.5f, labelY), ToU32(colors::TextSecondary), label);
				}
			}

			ImGui::Dummy(ImVec2(0.0f, 60.0f + 20.0f));
		}

		// Frame time plot filling remaining panel space
		{
			const float minPlot = 0.0f;
			const float maxPlot = std::max(33.333f, maxMs * 1.1f);
			const float plotHeight = std::max(40.0f, ImGui::GetContentRegionAvail().y);

			ImGui::PlotLines("##FrameTime", m_orderedSamples.data(), static_cast<int>(m_frameSampleCount), 0, nullptr, minPlot, maxPlot, ImVec2(-1.0f, plotHeight));
		}

		ImGui::End();
	}

	void PerformancePanel::LoadSettings(TomlConfig& /*config*/, LayerContext& /*context*/)
	{
	}

	void PerformancePanel::SaveSettings(TomlConfig& /*config*/, LayerContext& /*context*/) const
	{
	}
} // namespace aether::app
