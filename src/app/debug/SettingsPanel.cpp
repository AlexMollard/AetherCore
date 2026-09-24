#include "debug/SettingsPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

#include <imgui.h>

#include "debug/EditorChrome.hpp"
#include "Icons.hpp"
#include "layers/AppLayer.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Profiler.hpp"
#include "utils/SettingsService.hpp"

namespace aether::editor
{
	namespace
	{
		// The bounds in SettingInfo are the ones EngineSettingsIO::Sanitize enforces on
		// load. Applying them at the widget means a value the loader would silently clamp
		// cannot be entered in the first place - the old panel let you type framesInFlight
		// 99, showed it back, and quietly ran 3.
		template<class T>
		bool DrawSettingWidget(const char* label, T& field, const SettingInfo& info)
		{
			const bool bounded = info.minValue != info.maxValue;
			if constexpr (std::is_same_v<T, bool>)
			{
				return ImGui::Checkbox(label, &field);
			}
			else if constexpr (std::is_same_v<T, int>)
			{
				if (bounded)
				{
					const int lo = static_cast<int>(info.minValue);
					const int hi = static_cast<int>(info.maxValue);
					if (ImGui::SliderInt(label, &field, lo, hi))
					{
						field = std::clamp(field, lo, hi); // Ctrl+click types a raw value past the ends
						return true;
					}
					return false;
				}
				return ImGui::InputInt(label, &field);
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				if (bounded)
				{
					const auto lo = static_cast<float>(info.minValue);
					const auto hi = static_cast<float>(info.maxValue);
					if (ImGui::SliderFloat(label, &field, lo, hi))
					{
						field = std::clamp(field, lo, hi);
						return true;
					}
					return false;
				}
				return ImGui::InputFloat(label, &field);
			}
			else if constexpr (std::is_same_v<T, std::string>)
			{
				// A closed value set is a combo, not free text. Typed into a text box, a
				// misspelt window mode parsed as nothing and fell back to windowed, with
				// the wrong word still showing in the field.
				if (!info.choices.empty())
				{
					bool changed = false;
					if (ImGui::BeginCombo(label, field.c_str()))
					{
						for (const std::string_view choice: info.choices)
						{
							const std::string option(choice);
							const bool selected = field == option;
							if (ImGui::Selectable(option.c_str(), selected))
							{
								field = option;
								changed = true;
							}
							if (selected)
							{
								ImGui::SetItemDefaultFocus();
							}
						}
						ImGui::EndCombo();
					}
					return changed;
				}
				char buffer[256];
				std::snprintf(buffer, sizeof(buffer), "%s", field.c_str());
				if (ImGui::InputText(label, buffer, sizeof(buffer)))
				{
					field = buffer;
					return true;
				}
				return false;
			}
			else
			{
				static_assert(sizeof(T) == 0, "DrawSettingWidget missing a branch for a setting field type");
				return false;
			}
		}

		// Matches the description as well as the key, because what you remember about a
		// setting is usually what it does, not what it is called - "greyscale" finds
		// saturation, "beam" finds the volumetrics, and neither word is in a key.
		bool MatchesFilter(const std::string_view key, const std::string_view description, const char* filter)
		{
			if (filter[0] == '\0')
			{
				return true;
			}
			const auto lower = [](std::string text)
			{
				std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return text;
			};
			const std::string needle = lower(std::string(filter));
			return lower(std::string(key)).find(needle) != std::string::npos
			        || lower(std::string(description)).find(needle) != std::string::npos;
		}
	} // namespace

	void SettingsPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		// A settings window that opens as a 32-pixel stub is useless, and ImGui has no idea
		// what a sensible size for one is. FirstUseEver so a resized window stays resized.
		ImGui::SetNextWindowSize(ImVec2(560.0f, 720.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 240.0f), ImVec2(FLT_MAX, FLT_MAX));
		ImGui::Begin("Settings", VisiblePtr());

		auto* settingsService = context.TryGet<aether::SettingsService>();
		if (settingsService == nullptr)
		{
			ImGui::TextUnformatted("Settings service unavailable.");
			ImGui::End();
			return;
		}

		chrome::PanelHeader("SETTINGS");

		const bool hasProject = !settingsService->ProjectFile().empty();

		// One row of chrome, not four. Save and its state sit together because they are one
		// thought; the filter and the only-changed toggle share the next line because they
		// are the other. What the two homes mean lives behind the hint icon rather than
		// spending a permanent line to say something you need told once.
		{
			const bool dirty = settingsService->IsDirty();
			ImGui::BeginDisabled(!dirty);
			if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
			{
				settingsService->Save();
			}
			ImGui::EndDisabled();

			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(dirty ? chrome::kWarning : chrome::kMuted, dirty ? "Unsaved" : "Saved");

			ImGui::SameLine();
			ImGui::TextColored(chrome::kMuted, ICON_FA_CIRCLE_INFO);
			if (ImGui::IsItemHovered() && ImGui::BeginTooltip())
			{
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
				ImGui::TextUnformatted(hasProject
				        ? ICON_FA_BOX " marks a setting saved with the project, so it ships with the game. Everything else is saved for this machine only."
				        : "No project is open, so every change is saved for this machine only and none of it will ship.");
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}

			const std::string& saveError = settingsService->LastSaveError();
			if (!saveError.empty())
			{
				ImGui::TextColored(chrome::kWarning, ICON_FA_TRIANGLE_EXCLAMATION "  %s", saveError.c_str());
			}
		}

		const float toggleWidth = ImGui::CalcTextSize("Only changed").x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 2.0f;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - toggleWidth);
		ImGui::InputTextWithHint("##settingsFilter", ICON_FA_MAGNIFYING_GLASS "  Filter...", m_filter, sizeof(m_filter));
		ImGui::SameLine();
		ImGui::Checkbox("Only changed", &m_onlyModified);
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Show only settings that differ from their default - which is exactly what gets written to a file.");
		}
		ImGui::Separator();

		ImGui::BeginChild("##settingsList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

		std::string currentSection;
		bool sectionOpen = true;
		bool tableOpen = false;
		int shown = 0;

		// A table, so the controls line up in a column and the eye runs down one edge
		// instead of tracking a ragged one. Ends and restarts around each section header,
		// because a header inside a table row cannot span it cleanly.
		const auto closeTable = [&]()
		{
			if (tableOpen)
			{
				ImGui::EndTable();
				tableOpen = false;
			}
		};

		ForEachSettingField(settingsService->Values(),
		        [&](std::string_view key, auto& field)
		        {
			        const SettingInfo& info = SettingMetadata(key);

			        using FieldType = std::decay_t<decltype(field)>;
			        FieldType defaultValue{};
			        const bool hasDefault = DefaultSettingValue<FieldType>(key, defaultValue);
			        const bool modified = hasDefault && field != defaultValue;

			        if (!MatchesFilter(key, info.description, m_filter) || (m_onlyModified && !modified))
			        {
				        return;
			        }

			        const auto [section, name] = SplitSettingKey(key);
			        const std::string sectionLabel(section);
			        if (sectionLabel != currentSection)
			        {
				        closeTable();
				        currentSection = sectionLabel;
				        sectionOpen = ImGui::CollapsingHeader(currentSection.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
			        }
			        if (!sectionOpen)
			        {
				        return;
			        }

			        if (!tableOpen)
			        {
				        if (!ImGui::BeginTable(("##rows" + currentSection).c_str(), 3, ImGuiTableFlags_SizingFixedFit))
				        {
					        return;
				        }
				        tableOpen = true;
				        ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, ImGui::GetContentRegionAvail().x * 0.38f);
				        ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthStretch);
				        ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() * 1.6f);
			        }
			        ++shown;

			        ImGui::TableNextRow();
			        ImGui::PushID(std::string(name).c_str());

			        ImGui::TableSetColumnIndex(0);
			        ImGui::SetNextItemWidth(-FLT_MIN);
			        const bool changed = DrawSettingWidget("##v", field, info);

			        ImGui::TableSetColumnIndex(1);
			        ImGui::AlignTextToFramePadding();
			        // A changed setting is the one that ends up in a file and the one to
			        // suspect when a build looks wrong, so it reads at full strength and
			        // everything still at its default recedes.
			        ImGui::TextColored(modified ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : chrome::kMuted, "%s", std::string(name).c_str());

			        const bool project = aether::SettingsHomeFor(key) == aether::SettingsHome::Project;
			        if (ImGui::IsItemHovered() && ImGui::BeginTooltip())
			        {
				        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
				        if (!info.description.empty())
				        {
					        ImGui::TextUnformatted(std::string(info.description).c_str());
					        ImGui::Spacing();
				        }
				        ImGui::TextColored(chrome::kMuted, "%s",
				                project ? "Saved with the project - ships with the game."
				                        : "Saved for this machine only - not published.");
				        if (info.restartRequired)
				        {
					        ImGui::TextColored(chrome::kWarning, "Takes effect on the next launch.");
				        }
				        ImGui::PopTextWrapPos();
				        ImGui::EndTooltip();
			        }

			        // One marker column. Only project-homed settings are badged: "ships with
			        // the game" is the surprising half, and drawing a glyph on every row for
			        // the ordinary case is most of what made this list noisy. Restart lives
			        // in the tooltip for the same reason.
			        ImGui::TableSetColumnIndex(2);
			        if (modified)
			        {
				        if (chrome::GhostIconButton(ICON_FA_ROTATE_LEFT, "##reset", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
				        {
					        field = defaultValue;
					        settingsService->ApplyField(key);
					        settingsService->MarkDirty();
				        }
				        if (ImGui::IsItemHovered())
				        {
					        ImGui::SetTooltip("Reset to the default");
				        }
			        }
			        else if (project)
			        {
				        ImGui::AlignTextToFramePadding();
				        ImGui::TextColored(chrome::kMuted, ICON_FA_BOX);
			        }

			        ImGui::PopID();
			        if (changed)
			        {
				        settingsService->ApplyField(key);
				        settingsService->MarkDirty();
			        }
		        });

		closeTable();

		if (shown == 0)
		{
			ImGui::TextColored(chrome::kMuted, "Nothing matches that filter.");
		}

		ImGui::EndChild();
		ImGui::End();
	}
} // namespace aether::editor
