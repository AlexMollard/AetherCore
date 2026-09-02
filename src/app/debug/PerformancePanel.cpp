#include "PerformancePanel.hpp"
#include "AetherCore.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "debug/Icons.hpp"

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

		// NOT a stacked breakdown. The phases are measured on different threads and overlap:
		// on an idle editor frame the in-flight wait and the present wait are both ~16 ms of
		// the same 16.6 ms frame, so stacking them draws a 33 ms frame that never happened.
		// What is honest per frame is the wall time, and the game work inside it - the part
		// that is actually yours to shrink. The overlapping waits are means in the table below.
		const int count = static_cast<int>(m_frames.size());
		std::vector<float> x(count);
		std::vector<float> wall(count);
		std::vector<float> game(count);
		float peakMs = 0.0f;
		for (int i = 0; i < count; ++i)
		{
			const FrameTiming& frame = m_frames[static_cast<std::size_t>(i)];
			x[i] = static_cast<float>(i);
			wall[i] = frame.wallMs;
			game[i] = frame.gameWorkMs;
			peakMs = std::max(peakMs, frame.wallMs);
		}

		// The axis top snaps to whole multiples of the frame budget rather than tracking the
		// peak continuously. A range that rescales every frame makes the trace appear to
		// breathe when nothing changed, which is most of why this read as unsteady.
		constexpr float kBudgetMs = 1000.0f / 60.0f;
		double yMax = kBudgetMs * 2.0;
		for (const double multiple: {1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0})
		{
			yMax = kBudgetMs * multiple;
			if (peakMs <= yMax * 0.92)
			{
				break;
			}
		}

		constexpr ImVec4 kWallColor{0.42f, 0.68f, 0.96f, 1.0f};
		constexpr ImVec4 kGameColor{0.98f, 0.76f, 0.31f, 1.0f};
		constexpr ImVec4 kBudgetColor{0.92f, 0.38f, 0.30f, 1.0f};

		// The legend is drawn below as plain text: ImPlot's own is a chunky box that has to
		// live either inside the plot (covering the trace) or outside it (eating the height
		// this strip does not have).
		if (ImPlot::BeginPlot("##frames", ImVec2(-1.0f, 120.0f),
		            ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect))
		{
			// The frame ordinal is a meaningless number to read; the axis says only
			// "older on the left". Dropping its ticks and label is most of the height saved.
			ImPlot::SetupAxes(nullptr, "ms", ImPlotAxisFlags_NoDecorations,
			        ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_LockMin | ImPlotAxisFlags_LockMax);
			ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(count), ImPlotCond_Always);
			ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, yMax, ImPlotCond_Always);

			ImPlotSpec wallFill;
			wallFill.FillColor = kWallColor;
			wallFill.FillAlpha = 0.22f;
			ImPlot::PlotShaded("Frame", x.data(), wall.data(), count, 0.0, wallFill);

			ImPlotSpec wallLine;
			wallLine.LineColor = kWallColor;
			wallLine.LineWeight = 1.5f;
			ImPlot::PlotLine("Frame", x.data(), wall.data(), count, wallLine);

			ImPlotSpec gameLine;
			gameLine.LineColor = kGameColor;
			gameLine.LineWeight = 1.25f;
			ImPlot::PlotLine("Game work", x.data(), game.data(), count, gameLine);

			const double budget = static_cast<double>(kBudgetMs);
			ImPlotSpec budgetLine;
			budgetLine.LineColor = kBudgetColor;
			budgetLine.LineWeight = 1.0f;
			budgetLine.Flags = ImPlotInfLinesFlags_Horizontal;
			ImPlot::PlotInfLines("60 Hz", &budget, 1, budgetLine);

			// Reading a spike off a trace is only useful if you can find out what it was.
			// ImPlot's own mouse text prints plot coordinates, which is the position of the
			// cursor rather than the frame under it - this reports the frame itself.
			if (ImPlot::IsPlotHovered())
			{
				const int hovered = std::clamp(static_cast<int>(ImPlot::GetPlotMousePos().x + 0.5), 0, count - 1);
				const FrameTiming& frame = m_frames[static_cast<std::size_t>(hovered)];
				ImGui::BeginTooltip();
				ImGui::Text("frame %llu", static_cast<unsigned long long>(frame.frameIndex));
				ImGui::Separator();
				ImGui::TextColored(kWallColor, "%.2f ms", static_cast<double>(frame.wallMs));
				ImGui::SameLine();
				ImGui::TextDisabled(frame.wallMs > kBudgetMs ? "over budget" : "in budget");
				ImGui::TextColored(kGameColor, "%.3f ms", static_cast<double>(frame.gameWorkMs));
				ImGui::SameLine();
				ImGui::TextDisabled("game work");
				ImGui::TextDisabled("in-flight %.2f   pacer %.2f   present %.2f",
				        static_cast<double>(frame.inFlightWaitMs), static_cast<double>(frame.pacerWaitMs),
				        static_cast<double>(frame.presentWaitMs));
				ImGui::EndTooltip();
			}

			ImPlot::EndPlot();
		}

		const auto key = [](const ImVec4& color, const char* label, const bool first)
		{
			if (!first)
			{
				ImGui::SameLine();
			}
			ImGui::TextColored(color, ICON_FA_MINUS);
			ImGui::SameLine(0.0f, 4.0f);
			ImGui::TextDisabled("%s", label);
		};
		key(kWallColor, "frame", true);
		key(kGameColor, "game work", false);
		key(kBudgetColor, "60 Hz budget", false);

		// The time direction belongs at the end of the axis it describes, not wedged into the
		// middle of the colour key where it read as one more series.
		const char* direction = "older to newer";
		const float directionWidth = ImGui::CalcTextSize(direction).x;
		ImGui::SameLine();
		if (const float slack = ImGui::GetContentRegionAvail().x - directionWidth; slack > 0.0f)
		{
			ImGui::Dummy(ImVec2(slack, 0.0f));
			ImGui::SameLine();
		}
		ImGui::TextDisabled("%s", direction);
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
			        {"Frame cap wait", &FrameTiming::pacerWaitMs},
			        // These three were absent, and they are most of an idle editor's frame. A
			        // breakdown that does not add up to the frame sends you hunting for time
			        // that was being displayed all along - which is exactly what happened when
			        // the same fields were missing from the log.
			        {"Latency pacer idle", &FrameTiming::pacerIdleMs},
			        {"Input staleness", &FrameTiming::inputStaleMs},
			        {"Loop tail", &FrameTiming::tailMs},
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

			// The producer's phases against the frame they have to fit inside. Render exec and
			// present wait belong to the render thread and overlap the producer, so they are
			// excluded from the sum rather than double-counted.
			const float accounted = MeanOf(m_frames, &FrameTiming::gameWorkMs) + MeanOf(m_frames, &FrameTiming::inFlightWaitMs)
			        + MeanOf(m_frames, &FrameTiming::pacerWaitMs) + MeanOf(m_frames, &FrameTiming::pacerIdleMs)
			        + MeanOf(m_frames, &FrameTiming::tailMs);
			const float wall = MeanOf(m_frames, &FrameTiming::wallMs);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("accounted / frame");
			ImGui::TableNextColumn();
			const bool balances = wall <= 0.0f || std::abs(wall - accounted) < wall * 0.1f;
			ImGui::TextColored(balances ? chrome::kMuted : chrome::kWarning, "%.3f / %.3f", static_cast<double>(accounted), static_cast<double>(wall));
			ImGui::EndTable();
		}
	}

	void PerformancePanel::DrawLatency(app::LayerContext& context) const
	{
		auto* engine = context.TryGet<AetherCore>();
		if (engine == nullptr)
		{
			return;
		}
		ImGui::SeparatorText("Input latency");

		// The only end-to-end number here: from the frame sampling input to that frame being
		// on screen, measured by the present-wait thread. Everything in the breakdown above is
		// a component of it, and none of them is a substitute for it.
		const float latchToFlip = engine->LatchToFlipMs();
		if (latchToFlip > 0.0f)
		{
			ImGui::Text("%.2f ms", static_cast<double>(latchToFlip));
			ImGui::SameLine();
			ImGui::TextDisabled("input to photons (measured)");
		}
		else
		{
			ImGui::TextDisabled("Not measured - needs VK_KHR_present_wait.");
		}

		// Focus decides whether the compositor throttles this window at all, so a reading
		// taken while it is in the background describes a machine nobody is looking at.
		if (!engine->IsWindowFocused())
		{
			ImGui::TextColored(chrome::kWarning, ICON_FA_TRIANGLE_EXCLAMATION "  Window is not focused - these numbers are not what you feel when using it.");
		}
		if (engine->IsIdleThrottled())
		{
			ImGui::TextDisabled("Idle throttled: the editor is deliberately running slowly because nothing is happening.");
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

		// An idle-throttled editor reads 10 FPS, which looks like a catastrophe rather than the
		// deliberate power saving it is. Say so in the title, where the number is.
		const auto* engine = context.TryGet<AetherCore>();
		const bool idle = engine != nullptr && engine->IsIdleThrottled();
		char title[128]{};
		std::snprintf(title, sizeof(title), "Performance  |  %.0f FPS%s  |  %.2f ms###Performance",
		        static_cast<double>(m_titleFps), idle ? " (idle)" : "", static_cast<double>(m_titleMs));

		ImGui::Begin(title, VisiblePtr());
		chrome::PanelHeader("PERFORMANCE");
		DrawVerdict(stats);
		DrawPacingStrip();
		DrawPhaseBreakdown();
		DrawLatency(context);
		DrawSimVsReal();
		DrawStutterList(stats);
		ImGui::End();
	}
} // namespace aether::editor
