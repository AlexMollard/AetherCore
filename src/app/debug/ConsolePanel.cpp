#include "debug/ConsolePanel.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/Icons.hpp"

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "debug/OpenInEditor.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "utils/FuzzyMatch.hpp"
#include "utils/LogCategory.hpp"
#include "utils/LogRingBuffer.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
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
					return ImVec4(0.88f, 0.85f, 0.79f, 1.0f);
				case LogLevel::Verbose:
				default:
					return ImVec4(0.59f, 0.55f, 0.50f, 1.0f);
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

		std::string Basename(const std::string& path)
		{
			if (path.empty())
			{
				return {};
			}
			return std::filesystem::path(path).filename().string();
		}

		std::string ToSingleLine(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			bool inBreak = false;
			for (const char c: text)
			{
				if (c == '\n' || c == '\r')
				{
					if (!inBreak)
					{
						out += "  |  ";
						inBreak = true;
					}
				}
				else
				{
					out += c;
					inBreak = false;
				}
			}
			return out;
		}

		void LevelBadge(const char* tag, int count, bool* shown, ImVec4 color)
		{
			char label[48];
			std::snprintf(label, sizeof(label), "%s %d", tag, count);
			ImGui::PushStyleColor(ImGuiCol_Button, *shown ? ImVec4(color.x, color.y, color.z, 0.16f) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.30f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 0.45f));
			ImGui::PushStyleColor(ImGuiCol_Text, *shown ? color : chrome::kFaint);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
			if (ImGui::Button(label))
			{
				*shown = !*shown;
			}
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(4);
		}
	} // namespace

	void ConsolePanel::ShowLatestProblem()
	{
		m_filter[0] = 0;
		RevealProblem();
	}

	void ConsolePanel::ShowScriptErrors()
	{
		// The prefix every script failure is logged with, so the Console shows those and
		// nothing else - a build error is not easier to find among a hundred info lines.
		std::snprintf(m_filter, sizeof(m_filter), "C# script error");
		RevealProblem();
	}

	void ConsolePanel::RevealProblem()
	{
		// Anything that could hide the entry we are about to jump to has to come off, or the
		// jump silently lands on nothing - which is exactly how "clicking the badge does
		// nothing" looked. The text filter is cleared for the same reason.
		m_showError = true;
		m_showWarn = true;
		// Following the tail would immediately undo the jump.
		m_autoScroll = false;
		m_revealProblem = true;
	}

	void ConsolePanel::OnImGui(app::LayerContext& /*context*/)
	{
		ImGui::Begin(GetName().data(), VisiblePtr());

		std::vector<LogRingBuffer::Record> records;
		LogRingBuffer::Get().Snapshot(records);

		int nErr = 0, nWarn = 0, nInfo = 0, nVerbose = 0;
		std::set<std::string> categories;
		for (const auto& r: records)
		{
			switch (r.level)
			{
				case LogLevel::Error:
					++nErr;
					break;
				case LogLevel::Warn:
					++nWarn;
					break;
				case LogLevel::Info:
					++nInfo;
					break;
				case LogLevel::Verbose:
				default:
					++nVerbose;
					break;
			}
			if (!r.category.empty())
			{
				categories.insert(r.category);
			}
		}

		LevelBadge("ERR", nErr, &m_showError, LevelColor(LogLevel::Error));
		ImGui::SameLine();
		LevelBadge("WRN", nWarn, &m_showWarn, LevelColor(LogLevel::Warn));
		ImGui::SameLine();
		LevelBadge("INF", nInfo, &m_showInfo, LevelColor(LogLevel::Info));
		ImGui::SameLine();
		LevelBadge("VRB", nVerbose, &m_showVerbose, LevelColor(LogLevel::Verbose));

		// Everything else shares the badges' row. Three rows of chrome above six visible log
		// lines is a panel that spends more height describing itself than showing the log.
		ImGui::SameLine();
		if (chrome::GhostButton(ICON_FA_SLIDERS "##consoleView"))
		{
			ImGui::OpenPopup("##consoleView");
		}
		ImGui::SetItemTooltip("View options");
		if (ImGui::BeginPopup("##consoleView"))
		{
			// Set once and then left alone, which is what makes them menu items rather than
			// something occupying the toolbar permanently.
			ImGui::Checkbox("Collapse repeats", &m_collapse);
			ImGui::Checkbox("Timestamps", &m_showTime);
			ImGui::Checkbox("Autoscroll", &m_autoScroll);
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		if (chrome::GhostButton("Categories"))
		{
			ImGui::OpenPopup("##catpopup");
		}
		if (ImGui::BeginPopup("##catpopup"))
		{
			if (ImGui::SmallButton("All"))
			{
				m_categoryHidden.clear();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("None"))
			{
				for (const auto& cat: categories)
				{
					m_categoryHidden[cat] = true;
				}
			}
			ImGui::Separator();
			if (categories.empty())
			{
				ImGui::TextDisabled("(no categories yet)");
			}
			for (const auto& cat: categories)
			{
				const auto it = m_categoryHidden.find(cat);
				bool shown = (it == m_categoryHidden.end()) || !it->second;
				if (ImGui::Checkbox(cat.c_str(), &shown))
				{
					m_categoryHidden[cat] = !shown;
				}
			}
			ImGui::EndPopup();
		}

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
		const auto categoryShown = [this](const std::string& cat)
		{
			const auto it = m_categoryHidden.find(cat);
			return (it == m_categoryHidden.end()) || !it->second;
		};

		// Filter (level + category + fuzzy text), then optionally collapse. Filter
		// first so collapse counts reflect only the visible lines.
		std::vector<LogRingBuffer::Record> filtered;
		filtered.reserve(records.size());
		for (auto& r: records)
		{
			if (!levelShown(r.level) || !categoryShown(r.category))
			{
				continue;
			}
			if (m_filter[0] != '\0' && !FuzzyMatch(m_filter, r.message).has_value() && !FuzzyMatch(m_filter, r.category).has_value())
			{
				continue;
			}
			filtered.push_back(r);
		}

		std::vector<CollapsedRecord> display;
		if (m_collapse)
		{
			display = CollapseConsecutive(filtered);
		}
		else
		{
			display.reserve(filtered.size());
			for (auto& r: filtered)
			{
				display.push_back(CollapsedRecord{r, 1});
			}
		}

		// Compose one visible row as text (shared by rendering, copy and save).
		const auto rowText = [this](const CollapsedRecord& cr)
		{
			const auto& r = cr.record;
			std::string s;
			if (m_showTime && !r.time.empty())
			{
				s += '[';
				s += r.time;
				s += "] ";
			}
			s += '[';
			s += LevelTag(r.level);
			s += "] ";
			s += r.category;
			s += ": ";
			s += r.message;
			if (cr.count > 1)
			{
				s += "  x" + std::to_string(cr.count);
			}
			if (r.line > 0)
			{
				s += "  (" + Basename(r.file) + ':' + std::to_string(r.line) + ')';
			}
			return s;
		};

		ImGui::SameLine();
		if (chrome::GhostButton(ICON_FA_ELLIPSIS "##consoleActions"))
		{
			ImGui::OpenPopup("##consoleActions");
		}
		ImGui::SetItemTooltip("Copy or save the log");
		if (ImGui::BeginPopup("##consoleActions"))
		{
		if (ImGui::MenuItem("Copy to clipboard"))
		{
			std::string all;
			for (const auto& cr: display)
			{
				all += rowText(cr);
				all += '\n';
			}
			ImGui::SetClipboardText(all.c_str());
		}
		if (ImGui::MenuItem("Save to a file"))
		{
			const auto dir = io::PlatformPaths::GetUserConfigDir();
			if (!dir.empty())
			{
				std::string content;
				for (const auto& cr: display)
				{
					content += std::string(rowText(cr)) + '\n';
				}
				const auto path = dir / "console-log.txt";
				if (io::file_util::WriteText(path, content))
				{
					AE_INFO(LogCategory::UI, "Console log saved to {}", path.string());
				}
			}
		}
			ImGui::EndPopup();
		}

		// Clear stays on the toolbar: it is the one action reached often enough to be worth
		// the width.
		ImGui::SameLine();
		if (chrome::GhostButton(ICON_FA_TRASH "##consoleClear"))
		{
			LogRingBuffer::Get().Clear();
		}
		ImGui::SetItemTooltip("Clear the log");

		// The filter takes what is left of the same row rather than a row of its own.
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##logfilter", "Filter (message / category)...", m_filter, sizeof(m_filter));
		chrome::AccentHairline(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, 0.22f);
		ImGui::Dummy(ImVec2(0.0f, 3.0f));

		// ── Log rows ──
		ImGui::BeginChild("##loglines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
		if (m_revealProblem)
		{
			m_revealProblem = false;
			// The newest error, or the newest warning when there is no error.
			int target = -1;
			for (int i = static_cast<int>(display.size()) - 1; i >= 0 && target < 0; --i)
			{
				if (display[static_cast<std::size_t>(i)].record.level == LogLevel::Error)
				{
					target = i;
				}
			}
			for (int i = static_cast<int>(display.size()) - 1; i >= 0 && target < 0; --i)
			{
				if (display[static_cast<std::size_t>(i)].record.level == LogLevel::Warn)
				{
					target = i;
				}
			}
			if (target >= 0)
			{
				// Rows are deliberately single-line and uniform, which is what makes this
				// arithmetic valid even though the clipper never builds the skipped ones.
				const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
				const float centred = static_cast<float>(target) * rowHeight - ImGui::GetContentRegionAvail().y * 0.5f;
				ImGui::SetScrollY(std::max(0.0f, centred));
			}
		}
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(display.size()));
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
			{
				const auto& cr = display[static_cast<std::size_t>(i)];
				const auto& r = cr.record;
				ImGui::PushID(i);

				const std::string label = rowText(cr);
				// Display each entry on one line so multi-line messages don't break the
				// clipper's uniform-row-height assumption (which corrupts scrolling).
				// `label` keeps the full text for the copy actions below.
				const std::string displayLabel = ToSingleLine(label);
				ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(r.level));
				ImGui::Selectable(displayLabel.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
				ImGui::PopStyleColor();

				if (r.line > 0)
				{
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						OpenInEditor(r.file, r.line);
					}
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					{
						ImGui::SetTooltip("Double-click to open %s:%d", Basename(r.file).c_str(), r.line);
					}
				}
				if (ImGui::BeginPopupContextItem("##rowctx"))
				{
					if (ImGui::MenuItem("Copy line"))
					{
						ImGui::SetClipboardText(label.c_str());
					}
					if (ImGui::MenuItem("Copy message"))
					{
						ImGui::SetClipboardText(r.message.c_str());
					}
					if (r.line > 0 && ImGui::MenuItem("Open in editor"))
					{
						OpenInEditor(r.file, r.line);
					}
					ImGui::EndPopup();
				}
				ImGui::PopID();
			}
		}
		if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
		{
			ImGui::SetScrollHereY(1.0f);
		}
		ImGui::EndChild();

		ImGui::End();
	}
} // namespace aether::editor
