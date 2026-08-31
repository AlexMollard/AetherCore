#include "PerformancePanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <implot.h>

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
		ImGui::SeparatorText("Frame timeline");

		// Stacked bands rather than one bar per frame. A single wall-time histogram tells you
		// THAT a frame was long; the stack tells you which phase made it long, which is the
		// only question worth asking of a profiler. ImGui's built-in PlotHistogram can draw
		// exactly one unlabelled series with no axes, so this is ImPlot's job.
		const int count = static_cast<int>(m_frames.size());
		std::vector<float> x(count);
		std::vector<float> wall(count);
		std::vector<float> zero(count, 0.0f);
		std::vector<float> cGame(count);
		std::vector<float> cRender(count);
		std::vector<float> cInFlight(count);
		std::vector<float> cPacer(count);
		std::vector<float> cPresent(count);
		for (int i = 0; i < count; ++i)
		{
			const FrameTiming& frame = m_frames[static_cast<std::size_t>(i)];
			x[i] = static_cast<float>(i);
			wall[i] = frame.wallMs;
			cGame[i] = frame.gameWorkMs;
			cRender[i] = cGame[i] + frame.renderExecMs;
			cInFlight[i] = cRender[i] + frame.inFlightWaitMs;
			cPacer[i] = cInFlight[i] + frame.pacerWaitMs;
			cPresent[i] = cPacer[i] + frame.presentWaitMs;
		}

		if (ImPlot::BeginPlot("##frames", ImVec2(-1.0f, 190.0f), ImPlotFlags_NoTitle | ImPlotFlags_Crosshairs))
		{
			ImPlot::SetupAxes("frame", "ms", ImPlotAxisFlags_NoGridLines, ImPlotAxisFlags_AutoFit);
			ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(count), ImPlotCond_Always);
			ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);

			ImPlotSpec band;
			band.FillAlpha = 0.55f;
			ImPlot::PlotShaded("Game work", x.data(), zero.data(), cGame.data(), count, band);
			ImPlot::PlotShaded("Render exec", x.data(), cGame.data(), cRender.data(), count, band);
			ImPlot::PlotShaded("In-flight wait", x.data(), cRender.data(), cInFlight.data(), count, band);
			ImPlot::PlotShaded("Pacer wait", x.data(), cInFlight.data(), cPacer.data(), count, band);
			ImPlot::PlotShaded("Present wait", x.data(), cPacer.data(), cPresent.data(), count, band);

			ImPlotSpec wallLine;
			wallLine.LineWeight = 1.6f;
			wallLine.LineColor = ImVec4(0.95f, 0.95f, 0.95f, 0.9f);
			ImPlot::PlotLine("Wall", x.data(), wall.data(), count, wallLine);

			// The line a frame has to stay under to hold 60 Hz. Having it drawn is the
			// difference between reading numbers and seeing whether you are inside budget.
			const double budgetMs = 1000.0 / 60.0;
			ImPlotSpec budget;
			budget.LineColor = ImVec4(0.90f, 0.35f, 0.25f, 0.8f);
			budget.Flags = ImPlotInfLinesFlags_Horizontal;
			ImPlot::PlotInfLines("60 Hz budget", &budgetMs, 1, budget);

			ImPlot::EndPlot();
		}
		ImGui::TextDisabled("hover to read a frame; bands stack to the wall time above them");
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
