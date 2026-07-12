#include "debug/ConsolePanel.hpp"
#include "debug/EditorChrome.hpp"

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
					return ImVec4(0.88f, 0.85f, 0.79f, 1.0f); // warm near-white (Night Amber, not blue)
				case LogLevel::Verbose:
				default:
					return ImVec4(0.59f, 0.55f, 0.50f, 1.0f); // warm muted gray
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

		// Toggleable "TAG N" chip in the tinted-ghost language: the severity tint
		// fills quietly while shown; filtered-off chips drop to faint text, no fill.
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

	void ConsolePanel::OnImGui(LayerContext& /*context*/)
	{
		ImGui::Begin(GetName().data(), VisiblePtr());

		std::vector<LogRingBuffer::Record> records;
		LogRingBuffer::Get().Snapshot(records);

		// Header counts + distinct categories in one pass over the snapshot.
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

		// ── Header band ──
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float bandW = ImGui::GetContentRegionAvail().x;
			drawList->AddRectFilled(ImVec2(p.x, p.y + 1.0f), ImVec2(p.x + 3.0f, p.y + 13.0f), chrome::U32(chrome::kAccent));
			chrome::TextSized(drawList, 12.0f, ImVec2(p.x + 10.0f, p.y), chrome::kMuted, "CONSOLE");
			char totalText[32]{};
			std::snprintf(totalText, sizeof(totalText), "%zu MESSAGES", records.size());
			const float totalW = chrome::MeasureSized(12.0f, totalText).x;
			chrome::TextSized(drawList, 12.0f, ImVec2(p.x + bandW - totalW, p.y), chrome::kFaint, totalText);
			ImGui::Dummy(ImVec2(0.0f, 16.0f));
			chrome::AccentHairline(drawList, ImGui::GetCursorScreenPos(), bandW, 0.30f);
			ImGui::Dummy(ImVec2(0.0f, 4.0f));
		}

		// ── Row 1: clickable level count badges ──
		LevelBadge("ERR", nErr, &m_showError, LevelColor(LogLevel::Error));
		ImGui::SameLine();
		LevelBadge("WRN", nWarn, &m_showWarn, LevelColor(LogLevel::Warn));
		ImGui::SameLine();
		LevelBadge("INF", nInfo, &m_showInfo, LevelColor(LogLevel::Info));
		ImGui::SameLine();
		LevelBadge("VRB", nVerbose, &m_showVerbose, LevelColor(LogLevel::Verbose));

		// ── Row 2: options + category filter + actions ──
		ImGui::Checkbox("Collapse", &m_collapse);
		ImGui::SameLine();
		ImGui::Checkbox("Time", &m_showTime);
		ImGui::SameLine();
		ImGui::Checkbox("Autoscroll", &m_autoScroll);
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
		if (chrome::GhostButton("Copy"))
		{
			std::string all;
			for (const auto& cr: display)
			{
				all += rowText(cr);
				all += '\n';
			}
			ImGui::SetClipboardText(all.c_str());
		}
		ImGui::SameLine();
		if (chrome::GhostButton("Save"))
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
		ImGui::SameLine();
		if (chrome::GhostButton("Clear"))
		{
			LogRingBuffer::Get().Clear();
		}

		// ── Filter box ──
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##logfilter", "Filter (message / category)...", m_filter, sizeof(m_filter));
		chrome::AccentHairline(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, 0.22f);
		ImGui::Dummy(ImVec2(0.0f, 3.0f));

		// ── Log rows ──
		ImGui::BeginChild("##loglines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
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
				ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(r.level));
				ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
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
} // namespace aether::app
