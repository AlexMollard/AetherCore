#include "DayNightPanel.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include "layers/AppLayer.hpp"
#include "systems/DayNightSystem.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::app
{
	namespace
	{
		std::string FormatTime(float hours)
		{
			hours = std::fmod(hours, 24.0f);
			if (hours < 0.0f)
			{
				hours += 24.0f;
			}

			const int totalMinutes = static_cast<int>(std::round(hours * 60.0f)) % (24 * 60);
			const int hour = totalMinutes / 60;
			const int minute = totalMinutes % 60;
			return std::format("{:02d}:{:02d}", hour, minute);
		}
	} // namespace

	void DayNightPanel::OnImGui(LayerContext& context)
	{
		ImGui::Begin("Day / Night", VisiblePtr());
		if (auto dayNight = context.TryGet<DayNightSystem>())
		{
			bool enabled = dayNight->IsEnabled();
			if (ImGui::Checkbox("Cycle enabled", &enabled))
			{
				dayNight->SetEnabled(enabled);
				m_manualMode = !enabled;
			}

			if (ImGui::Checkbox("Manual mode", &m_manualMode))
			{
				dayNight->SetEnabled(!m_manualMode);
			}

			float timeOfDay = dayNight->GetTimeOfDay();
			const std::string timeLabel = FormatTime(timeOfDay);
			ImGui::Text("Time: %s", timeLabel.c_str());
			if (ImGui::SliderFloat("Time of day", &timeOfDay, 0.0f, 24.0f, "%.2f h"))
			{
				dayNight->SetTimeOfDay(timeOfDay);
			}

			float timeSpeed = dayNight->GetTimeSpeed();
			ImGui::BeginDisabled(m_manualMode);
			if (ImGui::SliderFloat("Time speed", &timeSpeed, 0.0f, 300.0f, "%.1f sec/sec"))
			{
				dayNight->SetTimeSpeed(timeSpeed);
			}
			ImGui::EndDisabled();

			if (ImGui::Button("Noon"))
			{
				dayNight->SetTimeOfDay(12.0f);
			}
			ImGui::SameLine();
			if (ImGui::Button("Midnight"))
			{
				dayNight->SetTimeOfDay(0.0f);
			}
			ImGui::SameLine();
			if (ImGui::Button("Sunrise"))
			{
				dayNight->SetTimeOfDay(6.0f);
			}

			const glm::vec3 sunDirection = dayNight->GetSunDirection();
			ImGui::SeparatorText("Sun");
			ImGui::Text("Direction: %.2f, %.2f, %.2f", sunDirection.x, sunDirection.y, sunDirection.z);
			ImGui::Text("Elevation: %.1f deg", glm::degrees(std::asin(std::clamp(sunDirection.y, -1.0f, 1.0f))));
		}
		else
		{
			ImGui::TextUnformatted("DayNightSystem is not registered.");
		}
		ImGui::End();
	}

	void DayNightPanel::LoadSettings(TomlConfig& config, LayerContext& context)
	{
		m_manualMode = config.GetBool("debug.daynightmanual", m_manualMode);
		if (auto dayNight = context.TryGet<DayNightSystem>())
		{
			dayNight->SetEnabled(!m_manualMode);
			dayNight->SetTimeOfDay(config.GetFloat("debug.daynighttime", dayNight->GetTimeOfDay()));
			dayNight->SetTimeSpeed(config.GetFloat("debug.daynightspeed", dayNight->GetTimeSpeed()));
		}
	}

	void DayNightPanel::SaveSettings(TomlConfig& config, LayerContext& context) const
	{
		config.Set("debug.daynightmanual", m_manualMode);
		if (auto dayNight = context.TryGet<DayNightSystem>())
		{
			config.Set("debug.daynighttime", dayNight->GetTimeOfDay());
			config.Set("debug.daynightspeed", dayNight->GetTimeSpeed());
		}
	}
} // namespace aether::app
