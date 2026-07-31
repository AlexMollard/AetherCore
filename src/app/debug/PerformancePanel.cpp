#include "PerformancePanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "layers/AppLayer.hpp"
#include "utils/FrameStats.hpp"
#include "utils/Profiler.hpp"

namespace aether::editor
{
	namespace
	{
		ImVec4 SmoothnessColor(const Smoothness smoothness)
		{
			switch (smoothness)
			{
				case Smoothness::Stuttering:
					return chrome::kError;
				case Smoothness::Alternating:
					return chrome::kWarning;
				case Smoothness::Even:
					break;
			}
			return chrome::kSuccess;
		}

		const char* DominantPhase(const FrameTiming& frame)
		{
			struct Entry
			{
				const char* name;
				float ms;
			};
			const Entry entries[] = {
			        {"game work", frame.gameWorkMs},
			        {"in-flight wait", frame.inFlightWaitMs},
			        {"pacer wait", frame.pacerWaitMs},
			        {"render exec", frame.renderExecMs},
			        {"present wait", frame.presentWaitMs},
			};
			const Entry* worst = &entries[0];
			for (const Entry& entry: entries)
			{
				if (entry.ms > worst->ms)
				{
					worst = &entry;
				}
			}
			return worst->name;
		}

		float MeanOf(const std::vector<FrameTiming>& frames, float FrameTiming::*field)
		{
			if (frames.empty())
			{
				return 0.0f;
			}
			float total = 0.0f;
			for (const FrameTiming& frame: frames)
			{
				total += frame.*field;
			}
			return total / static_cast<float>(frames.size());
		}
	} // namespace

	void PerformancePanel::DrawVerdict(const FrameStats& stats) const
	{
		const float fps = stats.avgMs > 0.0f ? 1000.0f / stats.avgMs : 0.0f;
		ImGui::Text("%.0f fps", static_cast<double>(fps));
		ImGui::SameLine();
		ImGui::TextDisabled("avg %.2f ms", static_cast<double>(stats.avgMs));
		ImGui::SameLine();
		const std::string label(SmoothnessLabel(stats.smoothness));
		ImGui::TextColored(SmoothnessColor(stats.smoothness), "%s", label.c_str());

		ImGui::TextDisabled("min %.2f   median %.2f   p95 %.2f   p99 %.2f   max %.2f ms",
		        static_cast<double>(stats.minMs), static_cast<double>(stats.medianMs),
		        static_cast<double>(stats.p95Ms), static_cast<double>(stats.p99Ms), static_cast<double>(stats.maxMs));
	}

	void PerformancePanel::DrawPacingStrip() const
	{
		if (m_frames.empty())
		{
			return;
		}
		ImGui::SeparatorText("Pacing");
		std::vector<float> wall;
		wall.reserve(m_frames.size());
		for (const FrameTiming& frame: m_frames)
		{
			wall.push_back(frame.wallMs);
		}
		const float scale = std::max(*std::ranges::max_element(wall), 1.0f);
		ImGui::PlotHistogram("##pacing", wall.data(), static_cast<int>(wall.size()), 0, nullptr, 0.0f, scale, ImVec2(-FLT_MIN, 72.0f));
		ImGui::TextDisabled("each column is one frame; even heights mean even delivery");
	}

	void PerformancePanel::DrawPhaseBreakdown() const
	{
		ImGui::SeparatorText("Where the time goes (mean ms)");
		if (ImGui::BeginTable("##phases", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			const struct
			{
				const char* label;
				float FrameTiming::*field;
			} rows[] = {
			        {"Game work", &FrameTiming::gameWorkMs},
			        {"In-flight wait", &FrameTiming::inFlightWaitMs},
			        {"Pacer wait", &FrameTiming::pacerWaitMs},
			        {"Render exec", &FrameTiming::renderExecMs},
			        {"Present wait", &FrameTiming::presentWaitMs},
			};
			for (const auto& row: rows)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.label);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", static_cast<double>(MeanOf(m_frames, row.field)));
			}
			ImGui::EndTable();
		}
	}

	void PerformancePanel::DrawSimVsReal() const
	{
		if (m_frames.empty())
		{
			return;
		}
		ImGui::SeparatorText("Simulation vs real");
		std::size_t clamped = 0;
		float lostMs = 0.0f;
		for (const FrameTiming& frame: m_frames)
		{
			if (frame.wallMs > frame.simDtMs + 0.01f)
			{
				++clamped;
				lostMs += frame.wallMs - frame.simDtMs;
			}
		}
		if (clamped == 0)
		{
			ImGui::TextColored(chrome::kSuccess, "Simulation received the full frame time.");
			return;
		}
		ImGui::TextColored(chrome::kWarning, "%zu of %zu frames were clamped, losing %.1f ms of simulation time.",
		        clamped, m_frames.size(), static_cast<double>(lostMs));
		ImGui::TextDisabled("The world advances slower than the clock; motion falls behind.");
	}

	void PerformancePanel::DrawStutterList(const FrameStats& stats) const
	{
		ImGui::SeparatorText("Worst frames");
		const float threshold = stats.medianMs * 1.5f;
		int shown = 0;
		for (auto it = m_frames.rbegin(); it != m_frames.rend() && shown < 6; ++it)
		{
			if (it->wallMs <= threshold)
			{
				continue;
			}
			ImGui::Text("#%llu  %.2f ms", static_cast<unsigned long long>(it->frameIndex), static_cast<double>(it->wallMs));
			ImGui::SameLine();
			ImGui::TextDisabled("mostly %s", DominantPhase(*it));
			++shown;
		}
		if (shown == 0)
		{
			ImGui::TextDisabled("No frame exceeded 1.5x the median.");
		}
	}

	void PerformancePanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		const auto* timeline = context.TryGet<FrameTimeline>();
		if (timeline == nullptr)
		{
			ImGui::Begin("Performance###Performance", VisiblePtr());
			chrome::PanelHeader("PERFORMANCE");
			// Deliberately no fallback to context.deltaTimeSeconds: reporting the clamped
			// simulation delta as if it were frame time is the bug this panel was rewritten
			// to remove, and a silent fallback would quietly reintroduce it.
			ImGui::TextColored(chrome::kError, "No frame timeline available.");
			ImGui::End();
			return;
		}

		m_frames.resize(kDisplayFrames);
		const std::size_t count = timeline->Snapshot(m_frames);
		m_frames.resize(count);

		const FrameStats stats = ComputeFrameStats(m_frames);

		m_titleAccum += ImGui::GetIO().DeltaTime;
		if (m_titleFps == 0.0f || m_titleAccum >= kTitleUpdateInterval)
		{
			m_titleFps = stats.avgMs > 0.0f ? 1000.0f / stats.avgMs : 0.0f;
			m_titleMs = stats.avgMs;
			m_titleAccum = 0.0f;
		}

		char title[96]{};
		std::snprintf(title, sizeof(title), "Performance  |  %.0f FPS  |  %.2f ms###Performance",
		        static_cast<double>(m_titleFps), static_cast<double>(m_titleMs));

		ImGui::Begin(title, VisiblePtr());
		chrome::PanelHeader("PERFORMANCE");
		DrawVerdict(stats);
		DrawPacingStrip();
		DrawPhaseBreakdown();
		DrawSimVsReal();
		DrawStutterList(stats);
		ImGui::End();
	}
} // namespace aether::editor
