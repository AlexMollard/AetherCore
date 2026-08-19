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

		bool MatchesFilter(const std::string_view key, const char* filter)
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
			return lower(std::string(key)).find(lower(std::string(filter))) != std::string::npos;
		}
	} // namespace

	void SettingsPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Settings", VisiblePtr());

		auto* settingsService = context.TryGet<aether::SettingsService>();
		if (settingsService == nullptr)
		{
			ImGui::TextUnformatted("Settings service unavailable.");
			ImGui::End();
			return;
		}

		chrome::PanelHeader("SETTINGS");

		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##settingsFilter", ICON_FA_MAGNIFYING_GLASS "  Filter settings...", m_filter, sizeof(m_filter));
		ImGui::Spacing();

		std::string currentSection;
		ForEachSettingField(settingsService->Values(),
		        [&](std::string_view key, auto& field)
		        {
			        if (!MatchesFilter(key, m_filter))
			        {
				        return;
			        }
			        const auto [section, name] = SplitSettingKey(key);
			        const std::string sectionLabel(section);
			        if (sectionLabel != currentSection)
			        {
				        currentSection = sectionLabel;
				        ImGui::SeparatorText(currentSection.c_str());
			        }

			        const SettingInfo& info = SettingMetadata(key);
			        const std::string label(name);
			        ImGui::PushID(label.c_str());

			        // Leave room for the per-field reset, so long labels do not push it off.
			        const float resetWidth = ImGui::GetFrameHeight();
			        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.45f));
			        const bool changed = DrawSettingWidget(("##" + label).c_str(), field, info);

			        ImGui::SameLine();
			        ImGui::AlignTextToFramePadding();
			        ImGui::TextUnformatted(label.c_str());
			        // Every non-obvious knob in here already had a paragraph explaining it on
			        // the struct field; none of it reached the panel, which showed bare names.
			        if (!info.description.empty() && ImGui::IsItemHovered() && ImGui::BeginTooltip())
			        {
				        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
				        ImGui::TextUnformatted(std::string(info.description).c_str());
				        if (info.restartRequired)
				        {
					        ImGui::TextColored(chrome::kWarning, "Takes effect on the next launch.");
				        }
				        ImGui::PopTextWrapPos();
				        ImGui::EndTooltip();
			        }
			        if (info.restartRequired)
			        {
				        ImGui::SameLine();
				        ImGui::TextColored(chrome::kMuted, ICON_FA_ROTATE);
			        }

			        using FieldType = std::decay_t<decltype(field)>;
			        FieldType defaultValue{};
			        const bool hasDefault = DefaultSettingValue<FieldType>(key, defaultValue);
			        if (hasDefault && field != defaultValue)
			        {
				        ImGui::SameLine(ImGui::GetContentRegionMax().x - resetWidth);
				        if (chrome::GhostIconButton(ICON_FA_ROTATE_LEFT, "##reset", ImVec2(resetWidth, resetWidth)))
				        {
					        field = defaultValue;
					        settingsService->ApplyField(key);
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
			        }
		        });

		ImGui::Separator();
		ImGui::TextDisabled("Saved on exit. " ICON_FA_ROTATE " marks a setting read once at startup.");

		ImGui::End();
	}
} // namespace aether::editor
