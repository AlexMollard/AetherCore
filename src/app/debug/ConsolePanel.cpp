#include "debug/ConsolePanel.hpp"

#include <vector>

#include <imgui.h>

#include "utils/FuzzyMatch.hpp"
#include "utils/LogRingBuffer.hpp"

namespace aether::app
{
	namespace
	{
		ImVec4 LevelColor(LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Error:
					return ImVec4(0.96f, 0.45f, 0.45f, 1.0f);
				case LogLevel::Warn:
					return ImVec4(0.96f, 0.80f, 0.35f, 1.0f);
				case LogLevel::Info:
					return ImVec4(0.78f, 0.86f, 0.96f, 1.0f);
				case LogLevel::Verbose:
				default:
					return ImVec4(0.58f, 0.58f, 0.58f, 1.0f);
			}
		}

		const char* LevelTag(LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Error:
					return "ERR";
				case LogLevel::Warn:
					return "WRN";
				case LogLevel::Info:
					return "INF";
				case LogLevel::Verbose:
				default:
					return "VRB";
			}
		}
	} // namespace

	void ConsolePanel::OnImGui(LayerContext& /*context*/)
	{
		ImGui::Begin(GetName().data(), VisiblePtr());

		ImGui::Checkbox("Verbose", &m_showVerbose);
		ImGui::SameLine();
		ImGui::Checkbox("Info", &m_showInfo);
		ImGui::SameLine();
		ImGui::Checkbox("Warn", &m_showWarn);
		ImGui::SameLine();
		ImGui::Checkbox("Error", &m_showError);
		ImGui::SameLine();
		ImGui::Checkbox("Autoscroll", &m_autoScroll);
		ImGui::SameLine();
		if (ImGui::SmallButton("Clear"))
		{
			LogRingBuffer::Get().Clear();
		}
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##logfilter", "Filter...", m_filter, sizeof(m_filter));
		ImGui::Separator();

		const auto levelShown = [this](LogLevel level)
		{
			switch (level)
			{
				case LogLevel::Error:
					return m_showError;
				case LogLevel::Warn:
					return m_showWarn;
				case LogLevel::Info:
					return m_showInfo;
				case LogLevel::Verbose:
				default:
					return m_showVerbose;
			}
		};

		std::vector<LogRingBuffer::Record> records;
		LogRingBuffer::Get().Snapshot(records);

		ImGui::BeginChild("##loglines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
		for (const LogRingBuffer::Record& r: records)
		{
			if (!levelShown(r.level))
			{
				continue;
			}
			if (m_filter[0] != '\0' && !FuzzyMatch(m_filter, r.message).has_value() && !FuzzyMatch(m_filter, r.category).has_value())
			{
				continue;
			}
			ImGui::TextColored(LevelColor(r.level), "[%s] %s: %s", LevelTag(r.level), r.category.c_str(), r.message.c_str());
		}
		if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
		{
			ImGui::SetScrollHereY(1.0f);
		}
		ImGui::EndChild();

		ImGui::End();
	}
} // namespace aether::app
