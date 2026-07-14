#include "debug/SettingsPanel.hpp"

#include <cstdio>
#include <string>
#include <type_traits>

#include <imgui.h>

#include "layers/AppLayer.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Profiler.hpp"
#include "utils/SettingsService.hpp"

namespace aether::editor
{
	namespace
	{
		template<class T>
		bool DrawSettingWidget(const char* label, T& field)
		{
			if constexpr (std::is_same_v<T, bool>)
			{
				return ImGui::Checkbox(label, &field);
			}
			else if constexpr (std::is_same_v<T, int>)
			{
				return ImGui::InputInt(label, &field);
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				return ImGui::InputFloat(label, &field);
			}
			else if constexpr (std::is_same_v<T, std::string>)
			{
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

		std::string currentSection;
		ForEachSettingField(settingsService->Values(),
		        [&](std::string_view key, auto& field)
		        {
			        const auto [section, name] = SplitSettingKey(key);
			        const std::string sectionLabel(section);
			        if (sectionLabel != currentSection)
			        {
				        currentSection = sectionLabel;
				        ImGui::SeparatorText(currentSection.c_str());
			        }

			        const std::string label(name);
			        if (DrawSettingWidget(label.c_str(), field))
			        {
				        settingsService->ApplyField(key);
			        }
		        });

		ImGui::Separator();
		ImGui::TextDisabled("Saved on exit. Async compute, startup scene and");
		ImGui::TextDisabled("autoplay take effect on next launch.");

		ImGui::End();
	}
} // namespace aether::editor
