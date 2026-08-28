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
#include "debug/Icons.hpp"
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

		// Saving is explicit. It used to happen only on exit, which meant a crash lost the
		// edit and - worse - there was no moment at which the editor could tell you the
		// project file could not be written.
		{
			const bool dirty = settingsService->IsDirty();
			ImGui::BeginDisabled(!dirty);
			if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
			{
				settingsService->Save();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (dirty)
			{
				ImGui::TextColored(chrome::kWarning, "Unsaved changes");
			}
			else
			{
				ImGui::TextColored(chrome::kMuted, "Saved");
			}

			const std::string& saveError = settingsService->LastSaveError();
			if (!saveError.empty())
			{
				ImGui::TextColored(chrome::kWarning, ICON_FA_TRIANGLE_EXCLAMATION "  %s", saveError.c_str());
			}
		}

		// The split is the thing a newcomer most needs told: half these settings travel with
		// the project and half stay on this machine, and until now everything went to the
		// machine and quietly failed to ship.
		if (hasProject)
		{
			ImGui::TextColored(chrome::kMuted, ICON_FA_CIRCLE_INFO "  " ICON_FA_BOX " settings ship with the project; " ICON_FA_DESKTOP " stay on this machine.");
		}
		else
		{
			ImGui::TextColored(chrome::kWarning, ICON_FA_TRIANGLE_EXCLAMATION "  No project open - every change is saved for this machine only.");
		}

		ImGui::Spacing();
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##settingsFilter", ICON_FA_MAGNIFYING_GLASS "  Filter by name or description...", m_filter, sizeof(m_filter));
		ImGui::Checkbox("Only changed", &m_onlyModified);
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Show only settings that differ from their default - which is also exactly what gets written to a file.");
		}
		ImGui::Separator();

		ImGui::BeginChild("##settingsList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

		std::string currentSection;
		bool sectionOpen = true;
		int shown = 0;

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
				        currentSection = sectionLabel;
				        // Collapsible, so a long list can be folded down to the part you are
				        // working on. Open by default: a settings window that hides its
				        // contents on first open is worse than a long one.
				        sectionOpen = ImGui::CollapsingHeader(currentSection.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
			        }
			        if (!sectionOpen)
			        {
				        return;
			        }
			        ++shown;

			        const std::string label(name);
			        ImGui::PushID(label.c_str());

			        const float resetWidth = ImGui::GetFrameHeight();
			        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.40f));
			        const bool changed = DrawSettingWidget(("##" + label).c_str(), field, info);

			        ImGui::SameLine();
			        ImGui::AlignTextToFramePadding();
			        // A changed setting is worth spotting at a glance; it is the one that will
			        // end up in a file and the one to suspect when the build looks wrong.
			        if (modified)
			        {
				        ImGui::TextUnformatted(label.c_str());
			        }
			        else
			        {
				        ImGui::TextColored(chrome::kMuted, "%s", label.c_str());
			        }

			        if (!info.description.empty() && ImGui::IsItemHovered() && ImGui::BeginTooltip())
			        {
				        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
				        ImGui::TextUnformatted(std::string(info.description).c_str());
				        ImGui::Spacing();
				        ImGui::TextColored(chrome::kMuted, "%s",
				                aether::SettingsHomeFor(key) == aether::SettingsHome::Project
				                        ? "Saved with the project - ships with the game."
				                        : "Saved for this machine only - not published.");
				        if (info.restartRequired)
				        {
					        ImGui::TextColored(chrome::kWarning, "Takes effect on the next launch.");
				        }
				        ImGui::PopTextWrapPos();
				        ImGui::EndTooltip();
			        }

			        // Home and restart markers, right-aligned so the column of icons reads as
			        // a column rather than trailing each label at a different offset.
			        const bool project = aether::SettingsHomeFor(key) == aether::SettingsHome::Project;
			        ImGui::SameLine(ImGui::GetContentRegionMax().x - resetWidth * 3.0f);
			        ImGui::TextColored(chrome::kMuted, "%s", project ? ICON_FA_BOX : ICON_FA_DESKTOP);
			        ImGui::SameLine(ImGui::GetContentRegionMax().x - resetWidth * 2.0f);
			        if (info.restartRequired)
			        {
				        ImGui::TextColored(chrome::kMuted, ICON_FA_ROTATE);
			        }
			        else
			        {
				        ImGui::TextUnformatted(" ");
			        }

			        if (modified)
			        {
				        ImGui::SameLine(ImGui::GetContentRegionMax().x - resetWidth);
				        if (chrome::GhostIconButton(ICON_FA_ROTATE_LEFT, "##reset", ImVec2(resetWidth, resetWidth)))
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

			        ImGui::PopID();
			        if (changed)
			        {
				        settingsService->ApplyField(key);
				        settingsService->MarkDirty();
			        }
		        });

		if (shown == 0)
		{
			ImGui::TextColored(chrome::kMuted, "Nothing matches that filter.");
		}

		ImGui::EndChild();
		ImGui::End();
	}
} // namespace aether::editor
